#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace nickel {

struct PhysicalMemory {
    std::optional<std::uint64_t> total_mb;
    std::optional<std::uint64_t> available_mb;
};

PhysicalMemory physical_memory() noexcept;
PhysicalMemory parse_linux_meminfo(std::string_view text) noexcept;

} // namespace nickel
