#include "core/archive/zip_index.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace nickel::archive::detail {
namespace {
std::uint16_t u16(std::span<const std::byte> value, std::size_t offset) {
    if (offset + 2 > value.size()) throw std::runtime_error("truncated ZIP integer");
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(value[offset]) |
           (std::to_integer<unsigned>(value[offset + 1]) << 8));
}
std::uint32_t u32(std::span<const std::byte> value, std::size_t offset) {
    return u16(value, offset) | (static_cast<std::uint32_t>(u16(value, offset + 2)) << 16);
}
std::uint64_t u64(std::span<const std::byte> value, std::size_t offset) {
    return u32(value, offset) | (static_cast<std::uint64_t>(u32(value, offset + 4)) << 32);
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) throw std::runtime_error("ZIP integer overflow");
    return a + b;
}
void read_at(tasks::detail::SecureFile& file, std::uint64_t offset, std::span<std::byte> buffer) {
    if (offset > file.size() || buffer.size() > file.size() - offset) throw std::runtime_error("truncated ZIP structure");
    file.seek(offset);
    while (!buffer.empty()) {
        const auto count = file.read(buffer);
        if (!count) throw std::runtime_error("truncated ZIP structure");
        buffer = buffer.subspan(count);
    }
}
std::vector<std::byte> read_bytes(tasks::detail::SecureFile& file, std::uint64_t offset, std::size_t count) {
    std::vector<std::byte> out(count); read_at(file, offset, out); return out;
}
std::string text(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void checkpoint(const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) throw std::runtime_error("operation cancelled");
}
void extra_fields(std::span<const std::byte> extra, ZipEntry& entry,
                   bool need_size, bool need_compressed, bool need_offset,
                   std::string_view name) {
    bool zip64{};
    for (std::size_t at = 0; at < extra.size();) {
        if (extra.size() - at < 4) throw std::runtime_error("truncated ZIP extra field");
        const auto type = u16(extra, at), size = u16(extra, at + 2);
        at += 4;
        if (size > extra.size() - at) throw std::runtime_error("truncated ZIP extra value");
        const auto value = extra.subspan(at, size); at += size;
        if (type == 1) {
            if (zip64) throw std::runtime_error("duplicate ZIP64 extra field");
            zip64 = true; std::size_t position{};
            if (need_size) { entry.size = u64(value, position); position += 8; }
            if (need_compressed) { entry.compressed = u64(value, position); position += 8; }
            if (need_offset) { entry.offset = u64(value, position); position += 8; }
            if (value.size() >= position + 4 && u32(value, position) != 0) throw std::runtime_error("multi-disk ZIP64 rejected");
        } else if (type == 0x7075) {
            // A Unicode-path field must agree with the byte name, its CRC and the
            // version byte; disagreement is an ambiguous filename, not an override.
            if (value.size() < 5 || std::to_integer<unsigned>(value[0]) != 1 ||
                u32(value, 1) != (crc32_update(0xffffffffU, std::as_bytes(std::span(name.data(), name.size()))) ^ 0xffffffffU) ||
                text(value.subspan(5)) != name)
                throw std::runtime_error("ambiguous ZIP Unicode path field");
        } else if (type == 0x9901 || type == 0x0017 || type == 0x0014 || type == 0x000d)
            throw std::runtime_error("encrypted or linked ZIP extra field rejected");
    }
    if ((need_size || need_compressed || need_offset) && !zip64) throw std::runtime_error("ZIP64 metadata missing");
}
} // namespace

std::uint32_t crc32_update(std::uint32_t state, std::span<const std::byte> bytes) noexcept {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> out{};
        for (std::uint32_t at = 0; at < out.size(); ++at) {
            auto value = at;
            for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
            out[at] = value;
        }
        return out;
    }();
    for (const auto byte : bytes) state = (state >> 8) ^ table[(state ^ std::to_integer<unsigned>(byte)) & 255];
    return state;
}
std::vector<ZipEntry> inspect_zip(tasks::detail::SecureFile& file, const Limits& limits,
                                  const std::function<bool()>& cancelled, tasks::detail::RelativePathPolicy policy) {
    checkpoint(cancelled);
    const auto file_size = file.size();
    if (file_size < 22 || file_size > limits.max_archive_bytes) throw std::runtime_error("invalid ZIP archive size");
    const auto tail_size = static_cast<std::size_t>(std::min<std::uint64_t>(file_size, 22 + 65535 + 20));
    auto tail = read_bytes(file, file_size - tail_size, tail_size);
    std::optional<std::size_t> eocd;
    // The end record must finish exactly at EOF; trailing bytes are rejected rather than
    // scanned past, so a concatenated archive cannot append a second index.
    for (std::size_t at = tail.size() - 22;; --at) {
        if (u32(tail, at) == 0x06054b50 && at + 22 + u16(tail, at + 20) == tail.size()) { eocd = at; break; }
        if (!at) break;
    }
    if (!eocd) throw std::runtime_error("ZIP end record missing or trailing data");
    const auto record = std::span(tail).subspan(*eocd, 22);
    if (u16(record, 4) || u16(record, 6) || u16(record, 8) != u16(record, 10)) throw std::runtime_error("multi-disk ZIP rejected");
    std::uint64_t entries = u16(record, 10), directory_size = u32(record, 12), directory_offset = u32(record, 16);
    auto directory_end = file_size - tail_size + *eocd;
    if (entries == 0xffff || directory_size == 0xffffffff || directory_offset == 0xffffffff) {
        if (directory_end < 20) throw std::runtime_error("ZIP64 locator missing");
        std::array<std::byte, 20> locator{}; read_at(file, directory_end - 20, locator);
        if (u32(locator, 0) != 0x07064b50 || u32(locator, 4) || u32(locator, 16) != 1)
            throw std::runtime_error("invalid ZIP64 locator");
        const auto offset = u64(locator, 8);
        std::array<std::byte, 56> end{}; read_at(file, offset, end);
        if (u32(end, 0) != 0x06064b50 || u64(end, 4) < 44 || u64(end, 4) > 4096 ||
            add(offset, add(12, u64(end, 4))) != directory_end - 20 || u32(end, 16) || u32(end, 20) ||
            u64(end, 24) != u64(end, 32)) throw std::runtime_error("invalid ZIP64 end record");
        entries = u64(end, 32); directory_size = u64(end, 40); directory_offset = u64(end, 48);
        directory_end = offset;
    }
    if (entries > limits.max_entries || add(directory_offset, directory_size) != directory_end)
        throw std::runtime_error("ZIP central directory bounds invalid");
    std::vector<ZipEntry> out;
    out.reserve(static_cast<std::size_t>(entries));
    std::unordered_set<std::string> names;
    std::uint64_t at = directory_offset, total{};
    for (std::uint64_t index = 0; index < entries; ++index) {
        checkpoint(cancelled);
        std::array<std::byte, 46> header{}; read_at(file, at, header);
        if (u32(header, 0) != 0x02014b50) throw std::runtime_error("invalid ZIP central entry");
        const auto name_size = u16(header, 28), extra_size = u16(header, 30), comment_size = u16(header, 32);
        if (!name_size || name_size > limits.max_path_bytes || u16(header, 34)) throw std::runtime_error("invalid ZIP path or disk");
        auto variable = read_bytes(file, add(at, 46), name_size + extra_size + comment_size);
        const auto raw_name = text(std::span(variable).first(name_size));
        ZipEntry entry;
        entry.name = tasks::detail::validate_relative(raw_name, limits.max_depth, limits.max_path_bytes, policy);
        entry.flags = u16(header, 8); entry.method = u16(header, 10); entry.crc = u32(header, 16);
        entry.compressed = u32(header, 20); entry.size = u32(header, 24); entry.offset = u32(header, 42);
        if ((entry.flags & (1 | 0x20 | 0x40 | 0x2000)) || (entry.method != 0 && entry.method != 8))
            throw std::runtime_error("encrypted or unsupported ZIP entry");
        if (!(entry.flags & 0x800) && std::any_of(raw_name.begin(), raw_name.end(), [](unsigned char c) { return c >= 0x80; }))
            throw std::runtime_error("ambiguous ZIP filename encoding");
        const auto attributes = u32(header, 38);
        const auto mode = (attributes >> 16) & 0170000;
        // Legacy Maven writers use -1 for an unavailable Unix directory mode.
        const bool legacy_directory = (u16(header, 4) >> 8) == 3 && (attributes >> 16) == 0xffff &&
            (attributes & 0x10) && raw_name.ends_with('/') && entry.size == 0 && entry.crc == 0;
        if (mode && mode != 0100000 && mode != 0040000 && !legacy_directory)
            throw std::runtime_error("ZIP link or special file rejected");
        entry.directory = raw_name.ends_with('/') || mode == 0040000 || (attributes & 0x10);
        if (entry.directory && (entry.size || entry.crc)) throw std::runtime_error("ZIP directory carries data");
        extra_fields(std::span(variable).subspan(name_size, extra_size), entry,
                     entry.size == 0xffffffff, entry.compressed == 0xffffffff, entry.offset == 0xffffffff, raw_name);
        if (entry.size > limits.max_entry_bytes || add(total, entry.size) > limits.max_total_bytes ||
            (entry.size > 1024 * 1024 && limits.max_expansion_ratio &&
             entry.size / std::max<std::uint64_t>(entry.compressed, 1) > limits.max_expansion_ratio))
            throw std::runtime_error("ZIP expansion quota exceeded");
        total = add(total, entry.size);
        if (!names.insert(tasks::detail::case_key(entry.name)).second) throw std::runtime_error("ZIP duplicate or case conflict");
        out.push_back(std::move(entry));
        at = add(at, 46 + name_size + extra_size + comment_size);
        if (at > directory_end) throw std::runtime_error("ZIP central entry extends beyond directory");
    }
    if (at != directory_end) throw std::runtime_error("ZIP unindexed central entries rejected");
    // Reconcile both header tables: local entries must tile the file contiguously
    // (no gaps or overlaps), so hidden or disguised payloads are rejected.
    auto physical = out;
    std::sort(physical.begin(), physical.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    std::uint64_t expected_offset{};
    for (const auto& entry : physical) {
        checkpoint(cancelled);
        if (entry.offset != expected_offset) throw std::runtime_error("ZIP hidden, overlapping, or self-extracting local entry");
        std::array<std::byte, 30> local{}; read_at(file, entry.offset, local);
        if (u32(local, 0) != 0x04034b50 || u16(local, 6) != entry.flags || u16(local, 8) != entry.method)
            throw std::runtime_error("ZIP local and central headers disagree");
        const auto name_size = u16(local, 26), extra_size = u16(local, 28);
        if (!name_size || name_size > limits.max_path_bytes) throw std::runtime_error("ZIP local path exceeds limit");
        const auto variable = read_bytes(file, add(entry.offset, 30), name_size + extra_size);
        const auto name = text(std::span(variable).first(name_size));
        if (tasks::detail::validate_relative(name, limits.max_depth, limits.max_path_bytes, policy) != entry.name ||
            name.ends_with('/') != entry.directory) throw std::runtime_error("ZIP local path mismatch");
        ZipEntry local_entry = entry;
        local_entry.size = u32(local, 22); local_entry.compressed = u32(local, 18);
        extra_fields(std::span(variable).subspan(name_size, extra_size), local_entry,
                     local_entry.size == 0xffffffff, local_entry.compressed == 0xffffffff, false, name);
        if (!(entry.flags & 8) && (u32(local, 14) != entry.crc || local_entry.size != entry.size || local_entry.compressed != entry.compressed))
            throw std::runtime_error("ZIP local size or CRC mismatch");
        expected_offset = add(add(entry.offset, 30 + name_size + extra_size), entry.compressed);
        if (entry.flags & 8) {
            std::array<std::byte, 4> signature{}; read_at(file, expected_offset, signature);
            const bool signed_descriptor = u32(signature, 0) == 0x08074b50;
            const bool zip64 = entry.size >= 0xffffffff || entry.compressed >= 0xffffffff ||
                u32(local, 18) == 0xffffffff || u32(local, 22) == 0xffffffff;
            const auto bytes = zip64 ? 20U : 12U;
            const auto descriptor = read_bytes(file, expected_offset + (signed_descriptor ? 4 : 0), bytes);
            if (u32(descriptor, 0) != entry.crc ||
                (zip64 ? u64(descriptor, 4) : u32(descriptor, 4)) != entry.compressed ||
                (zip64 ? u64(descriptor, 12) : u32(descriptor, 8)) != entry.size)
                throw std::runtime_error("ZIP data descriptor mismatch");
            expected_offset = add(expected_offset, bytes + (signed_descriptor ? 4 : 0));
        }
        if (expected_offset > directory_offset) throw std::runtime_error("ZIP data overlaps central directory");
    }
    if (expected_offset != directory_offset) throw std::runtime_error("ZIP hidden local data rejected");
    return out;
}
} // namespace nickel::archive::detail
