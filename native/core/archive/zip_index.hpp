#pragma once

#include "core/archive/archive.hpp"
#include "core/tasks/secure_fs.hpp"
#include <functional>

namespace nickel::archive::detail {
struct ZipEntry {
    std::string name;
    std::uint64_t size{};
    std::uint64_t compressed{};
    std::uint64_t offset{};
    std::uint32_t crc{};
    std::uint16_t method{};
    std::uint16_t flags{};
    bool directory{};
};
std::vector<ZipEntry> inspect_zip(tasks::detail::SecureFile& file, const Limits& limits,
                                  const std::function<bool()>& cancelled,
                                  tasks::detail::RelativePathPolicy policy = tasks::detail::RelativePathPolicy::DiskOutput);
std::uint32_t crc32_update(std::uint32_t state, std::span<const std::byte> bytes) noexcept;
} // namespace nickel::archive::detail
