#pragma once

#include "core/instances/instances.hpp"
#include <cstdint>
#include <limits>

namespace nickel::diagnostics::detail {
inline void verify_report_space(std::uint64_t available, std::uint64_t needed) {
    constexpr std::uint64_t reserve = 1024 * 1024;
    if (available == std::numeric_limits<std::uint64_t>::max()
        || available < needed || available - needed < reserve)
        throw instances::Error("insufficient_space", "Insufficient verified space for a safe error-report snapshot");
}
} // namespace nickel::diagnostics::detail
