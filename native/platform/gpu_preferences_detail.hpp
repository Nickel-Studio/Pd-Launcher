#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace nickel::foundation_detail {

inline constexpr std::size_t gpu_preference_max_bytes = 64 * 1024;

struct GpuRegistryValue {
    std::uint32_t type = 0;
    std::vector<std::uint8_t> bytes;
    bool operator==(const GpuRegistryValue&) const = default;
};

// Pure REG_SZ transformation; deliberately contains no registry/path backend.
GpuRegistryValue high_performance_gpu_value(const std::optional<GpuRegistryValue>& original);

} // namespace nickel::foundation_detail
