#include "icons.hpp"
#include "domain_io.hpp"
#include "settings.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"
#include "core/media/images.hpp"
#include "core/packs/internal.hpp"

#include <algorithm>
#include <array>

namespace nickel::instances {
namespace {
using NativeFile = packs::detail::NativeFile;
constexpr std::string_view icon_prefix = ".nickel-icons/";
constexpr std::array<std::uint8_t, 8> png_signature{137, 80, 78, 71, 13, 10, 26, 10};

struct FileBytes {
    std::vector<std::uint8_t> bytes;
    std::string sha256;
};

FileBytes read_complete(NativeFile& file, std::size_t maximum, std::stop_token cancel) {
    detail::check_cancelled(cancel);
    const auto size = file.size();
    if (!size || size > maximum) throw Error("limit", "Instance icon exceeds its complete-file byte limit");
    FileBytes result;
    result.bytes.resize(static_cast<std::size_t>(size));
    foundation_detail::Sha256 digest;
    std::size_t offset = 0;
    while (offset < result.bytes.size()) {
        detail::check_cancelled(cancel);
        const auto count = std::min(packs::detail::copy_buffer_bytes, result.bytes.size() - offset);
        auto chunk = std::span(result.bytes).subspan(offset, count);
        if (file.read_at(offset, chunk.data(), count) != count)
            throw Error("source_changed", "Selected instance icon ended before its pinned size");
        digest.update(chunk);
        offset += count;
    }
    std::uint8_t extra = 0;
    if (file.read_at(size, &extra, 1)) throw Error("source_changed", "Selected instance icon grew during its bounded read");
    file.unchanged();
    result.sha256 = digest.finish();
    detail::check_cancelled(cancel);
    return result;
}

void verify_complete(NativeFile& file, std::uint64_t size, std::string_view expected,
                     std::stop_token cancel) {
    file.unchanged();
    if (file.size() != size) throw Error("source_changed", "Selected instance icon size changed before publication");
    foundation_detail::Sha256 digest;
    std::array<std::uint8_t, packs::detail::copy_buffer_bytes> buffer{};
    std::uint64_t offset = 0;
    while (offset < size) {
        detail::check_cancelled(cancel);
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
        if (file.read_at(offset, buffer.data(), count) != count)
            throw Error("source_changed", "Selected instance icon changed before publication");
        digest.update(std::span(buffer).first(count));
        offset += count;
    }
    file.unchanged();
    if (digest.finish() != expected) throw Error("source_changed", "Selected instance icon content changed before publication");
    detail::check_cancelled(cancel);
}

std::uint32_t big_endian(std::span<const std::uint8_t> bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16)
        | (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}

std::uint32_t png_crc(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xffffffffU;
}

void validate_managed_png(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 57 || bytes.size() > managed_icon_max_bytes
        || !std::equal(png_signature.begin(), png_signature.end(), bytes.begin()))
        throw Error("invalid_icon", "Managed instance icon is not a bounded PNG");
    std::size_t offset = png_signature.size();
    constexpr std::array<std::string_view, 3> chunks{"IHDR", "IDAT", "IEND"};
    for (const auto type : chunks) {
        if (bytes.size() - offset < 12) throw Error("invalid_icon", "Managed instance icon has an incomplete PNG chunk");
        const auto length = big_endian(bytes.subspan(offset, 4));
        if (length > bytes.size() - offset - 12
            || !std::equal(type.begin(), type.end(), bytes.begin() + offset + 4))
            throw Error("invalid_icon", "Managed instance icon contains unapproved PNG chunks");
        const auto data = bytes.subspan(offset + 8, length);
        if (png_crc(bytes.subspan(offset + 4, length + 4)) != big_endian(bytes.subspan(offset + 8 + length, 4)))
            throw Error("invalid_icon", "Managed instance icon PNG checksum differs");
        if (type == "IHDR" && (length != 13 || big_endian(data.first(4)) != 32
            || big_endian(data.subspan(4, 4)) != 32 || data[8] != 8 || data[9] != 6
            || data[10] || data[11] || data[12]))
            throw Error("invalid_icon", "Managed instance icon must be a static 32x32 RGBA8 PNG");
        if ((type == "IDAT" && !length) || (type == "IEND" && length))
            throw Error("invalid_icon", "Managed instance icon has invalid PNG framing");
        offset += static_cast<std::size_t>(length) + 12;
    }
    if (offset != bytes.size()) throw Error("invalid_icon", "Managed instance icon contains trailing content");
}

std::optional<std::string_view> managed_digest(std::string_view reference) {
    if (!reference.starts_with(icon_prefix)) return {};
    if (reference.size() != icon_prefix.size() + 64 + 4 || !reference.ends_with(".png"))
        throw Error("invalid_icon", "Managed instance icon reference is not an approved relative token");
    const auto digest = reference.substr(icon_prefix.size(), 64);
    if (!std::all_of(digest.begin(), digest.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw Error("invalid_icon", "Managed instance icon reference contains an invalid digest");
    return digest;
}

Json icon_settings(const fs::path& root, std::string_view id, const fs::path& directory) {
    auto current = detail::read_optional_json(directory / settings_file, settings_max_bytes, true);
    if (current && !current->is_object()) throw Error("invalid_settings", "Existing version settings are not an object");
    auto value = current ? std::move(*current) : read_local_settings(root, id).value;
    return value.is_object() ? std::move(value) : Json::object();
}

struct UnpublishedIcon {
    fs::path path;
    std::string digest;
    bool created = false;
    bool retained = false;
    ~UnpublishedIcon() {
        if (created && !retained) {
            try { (void)foundation_detail::remove_file_if_matches(path, digest); } catch (...) {}
        }
    }
};
} // namespace

std::string set_instance_icon(const fs::path& root, std::string_view id, const fs::path& selected,
                              media::ImageService& images, std::stop_token cancel) {
    detail::check_cancelled(cancel);
    if (selected.empty() || !selected.is_absolute()) throw Error("unsafe_path", "Selected instance icon must be an absolute regular file");
    const auto directory = checked_instance_directory(root, id);
    foundation_detail::DirectoryGuard instance_pin(directory);
    {
        detail::DirectoryLock lock(root);
        instance_pin.verify();
        if (is_pending(directory)) throw Error("pending_instance", "Cannot edit pending instance");
    }
    NativeFile source(selected, NativeFile::Mode::read);
    auto input = read_complete(source, selected_icon_max_bytes, cancel);
    const auto source_size = input.bytes.size();
    media::EncodedImage encoded{std::move(input.bytes), {}, media::ImageKind::avatar};
    auto icon = images.normalize_icon(encoded, cancel);
    if (icon.mime != "image/png" || icon.kind != media::ImageKind::avatar)
        throw Error("invalid_icon", "Media service did not return a sanitized instance PNG");
    validate_managed_png(icon.bytes);
    verify_complete(source, source_size, input.sha256, cancel);
    foundation_detail::Sha256 digest;
    digest.update(icon.bytes);
    const auto hash = digest.finish();
    auto reference = std::string(icon_prefix) + hash + ".png";
    const auto target = directory / path_from_utf8(reference);

    detail::DirectoryLock lock(root);
    instance_pin.verify();
    detail::check_cancelled(cancel);
    if (is_pending(directory)) throw Error("pending_instance", "Cannot edit pending instance");
    auto value = icon_settings(root, id, directory);
    value["icon"] = reference;
    const auto settings = value.dump(2);
    if (settings.size() > settings_max_bytes) throw Error("limit", "Icon settings exceed their file quota");
    foundation_detail::DirectoryGuard icon_directory(target.parent_path(), true);
    UnpublishedIcon publication{target, hash};
    if (detail::path_exists(target)) {
        NativeFile existing(target, NativeFile::Mode::read);
        auto bytes = read_complete(existing, managed_icon_max_bytes, cancel);
        if (bytes.sha256 != hash || bytes.bytes != icon.bytes)
            throw Error("conflict", "Managed instance icon destination contains different bytes");
    } else {
        detail::check_cancelled(cancel);
        detail::atomic_write(target, {reinterpret_cast<const char*>(icon.bytes.data()), icon.bytes.size()}, false);
        publication.created = true;
    }
    NativeFile published(target, NativeFile::Mode::read);
    auto approved = read_complete(published, managed_icon_max_bytes, cancel);
    if (approved.sha256 != hash || approved.bytes != icon.bytes)
        throw Error("source_changed", "Managed instance icon changed before reference publication");
    verify_complete(source, source_size, input.sha256, cancel);
    published.unchanged();
    instance_pin.verify();
    icon_directory.verify();
    detail::check_cancelled(cancel);
    // A flush failure can follow a successful atomic replacement. Retain first;
    // remove a new image only when a bounded read proves it is not referenced.
    publication.retained = true;
    try { detail::atomic_write(directory / settings_file, settings); }
    catch (...) {
        try {
            const auto stored = detail::read_optional_json(directory / settings_file, settings_max_bytes, true);
            if (!stored || (stored->is_object() && detail::string_at(*stored, "icon") != std::optional<std::string>(reference)))
                publication.retained = false;
        } catch (...) {}
        throw;
    }
    return reference;
}

std::optional<InstanceIcon> read_instance_icon(const fs::path& root, std::string_view id,
                                              std::stop_token cancel) {
    detail::check_cancelled(cancel);
    const auto directory = checked_instance_directory(root, id);
    foundation_detail::DirectoryGuard instance_pin(directory);
    detail::DirectoryLock lock(root);
    instance_pin.verify();
    if (is_pending(directory)) throw Error("pending_instance", "Cannot read pending instance icon");
    const auto settings = icon_settings(root, id, directory);
    const auto icon = detail::string_at(settings, "icon");
    if (!icon) return {};
    const auto expected = managed_digest(*icon);
    if (!expected) return {};
    NativeFile file(directory / path_from_utf8(*icon), NativeFile::Mode::read);
    auto stored = read_complete(file, managed_icon_max_bytes, cancel);
    if (stored.sha256 != *expected) throw Error("source_changed", "Managed instance icon no longer matches its approved digest");
    validate_managed_png(stored.bytes);
    instance_pin.verify();
    detail::check_cancelled(cancel);
    return InstanceIcon{*icon, std::move(stored.bytes)};
}

} // namespace nickel::instances
