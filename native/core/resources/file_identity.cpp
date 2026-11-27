#include "mods.hpp"
#include "utility.hpp"
#include "core/network/hash.hpp"

#include <algorithm>

#include <array>
#include <limits>

namespace nickel::resources {
namespace {
struct Murmur2 {
    explicit Murmur2(std::uint32_t filteredLength) : hash(1U ^ filteredLength) {}
    std::uint32_t hash;
    std::array<std::uint8_t, 4> tail{};
    unsigned tailLength = 0;
    static constexpr std::uint32_t multiplier = 0x5bd1e995U;
    static bool ignored(std::uint8_t byte) { return byte == 9 || byte == 10 || byte == 13 || byte == 32; }
    void add(std::uint8_t byte) {
        if (ignored(byte)) return;
        tail[tailLength++] = byte;
        if (tailLength == 4) {
            auto value = std::uint32_t(tail[0]) | std::uint32_t(tail[1]) << 8 | std::uint32_t(tail[2]) << 16 | std::uint32_t(tail[3]) << 24;
            value *= multiplier;
            value ^= value >> 24;
            value *= multiplier;
            hash = hash * multiplier ^ value;
            tailLength = 0;
        }
    }
    std::uint32_t finish() {
        if (tailLength >= 3) hash ^= std::uint32_t(tail[2]) << 16;
        if (tailLength >= 2) hash ^= std::uint32_t(tail[1]) << 8;
        if (tailLength >= 1) { hash ^= tail[0]; hash *= multiplier; }
        hash ^= hash >> 13;
        hash *= multiplier;
        hash ^= hash >> 15;
        return hash;
    }
};
} // namespace

bool validResourceFileName(std::string_view name) {
    if (name.empty() || name.size() > 240 || !validUtf8(name) || name == "." || name == ".." || name.back() == '.' || name.back() == ' ' || trim(name) != name) return false;
    for (const auto c : name) if (std::uint8_t(c) < 0x20 || c == 0x7f || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) return false;
    const auto stem = lower(name.substr(0, name.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "clock$" || stem == "conin$" || stem == "conout$") return false;
    if (stem.starts_with("com") || stem.starts_with("lpt")) {
        const auto index = stem.substr(3);
        if ((index.size() == 1 && index[0] >= '1' && index[0] <= '9') || index == "\xc2\xb9" || index == "\xc2\xb2" || index == "\xc2\xb3") return false;
    }
    return true;
}

std::optional<std::pair<std::string, ModState>> splitModFileName(std::string_view name) {
    if (!validResourceFileName(name)) return {};
    auto base = std::string(name);
    auto state = ModState::Enabled;
    if (lower(base).ends_with(".disabled")) { base.resize(base.size() - 9); state = ModState::Disabled; }
    else if (lower(base).ends_with(".old")) { base.resize(base.size() - 4); state = ModState::Old; }
    for (const std::string_view suffix : {".jar", ".zip", ".litemod"}) if (base.size() > suffix.size() && lower(base).ends_with(suffix)) return std::pair{base, state};
    return {};
}

bool validArchiveRelativePath(std::string_view name) {
    if (name.empty() || name.size() > 1024 || !validUtf8(name) || name.front() == '/' || name.find('\\') != std::string_view::npos || name.find(':') != std::string_view::npos) return false;
    while (!name.empty()) {
        const auto slash = name.find('/');
        const auto part = name.substr(0, slash);
        if (!validResourceFileName(part)) return false;
        if (slash == std::string_view::npos) break;
        name.remove_prefix(slash + 1);
    }
    return true;
}

std::uint32_t curseForgeFingerprint(std::span<const std::uint8_t> bytes) {
    std::uint64_t filtered = 0;
    for (const auto byte : bytes) if (!Murmur2::ignored(byte)) ++filtered;
    if (filtered > std::numeric_limits<std::uint32_t>::max()) fail("too_large", "Fingerprint input exceeds budget");
    Murmur2 fingerprint{std::uint32_t(filtered)};
    for (const auto byte : bytes) fingerprint.add(byte);
    return fingerprint.finish();
}

ResourceFileHashes hashResourceFile(const Path& path, const Cancellation& cancel, std::uint64_t maximumBytes) {
    cancel.check();
    detail::FileReader file(path, std::min<std::uint64_t>(maximumBytes, std::numeric_limits<std::uint32_t>::max()));
    using transport::HashAlgorithm;
    using transport::detail::IncrementalHash;
    IncrementalHash sha1(HashAlgorithm::Sha1), sha256(HashAlgorithm::Sha256), sha512(HashAlgorithm::Sha512);
    std::array<std::uint8_t, 65536> buffer{};
    std::uint32_t filteredLength = 0;
    std::uint64_t firstSize = 0;
    for (auto count = file.read(buffer, cancel); count; count = file.read(buffer, cancel)) {
        const auto bytes = std::as_bytes(std::span(buffer).first(count));
        sha1.update(bytes);
        sha256.update(bytes);
        sha512.update(bytes);
        firstSize += count;
        for (std::size_t i = 0; i < count; ++i) if (!Murmur2::ignored(buffer[i])) ++filteredLength;
    }
    ResourceFileHashes result{firstSize, sha1.finish(), sha512.finish(), sha256.finish(), 0};
    file.rewind();
    Murmur2 fingerprint(filteredLength);
    IncrementalHash secondSha256(HashAlgorithm::Sha256);
    std::uint64_t secondSize = 0;
    for (auto count = file.read(buffer, cancel); count; count = file.read(buffer, cancel)) {
        secondSha256.update(std::as_bytes(std::span(buffer).first(count)));
        secondSize += count;
        for (std::size_t i = 0; i < count; ++i) fingerprint.add(buffer[i]);
    }
    file.verifyUnchanged();
    cancel.check();
    if (firstSize != file.size() || firstSize != secondSize || !transport::detail::digest_equal(result.sha256, secondSha256.finish()))
        fail("conflict", "Resource changed between identity passes");
    result.curseForgeFingerprint = fingerprint.finish();
    return result;
}

std::uint32_t curseForgeFingerprintFile(const Path& path, const Cancellation& cancel, std::uint64_t maximumBytes) {
    return hashResourceFile(path, cancel, maximumBytes).curseForgeFingerprint;
}

} // namespace nickel::resources
