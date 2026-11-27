#pragma once

#include <cstddef>

namespace test::allocation_fault {
// Thread-local, one-shot failure keeps process reader threads and cleanup unaffected.
void arm(std::ptrdiff_t successfulAllocations) noexcept;
void disarm() noexcept;
bool triggered() noexcept;
}
