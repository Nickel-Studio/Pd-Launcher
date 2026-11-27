#include "allocation_fault.hpp"

#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
thread_local std::ptrdiff_t remaining = -1;
thread_local bool fired = false;

void checkpoint() {
    if (remaining < 0) return;
    if (remaining-- == 0) {
        remaining = -1;
        fired = true;
        throw std::bad_alloc();
    }
}

void* allocate(std::size_t bytes) {
    checkpoint();
    if (auto pointer = std::malloc(bytes ? bytes : 1)) return pointer;
    throw std::bad_alloc();
}

void* allocate_aligned(std::size_t bytes, std::size_t alignment) {
    checkpoint();
#ifdef _WIN32
    auto pointer = _aligned_malloc(bytes ? bytes : 1, alignment);
#else
    void* pointer = nullptr;
    if (posix_memalign(&pointer, alignment, bytes ? bytes : 1) != 0) pointer = nullptr;
#endif
    if (!pointer) throw std::bad_alloc();
    return pointer;
}

void free_aligned(void* pointer) noexcept {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
}

namespace test::allocation_fault {
void arm(std::ptrdiff_t successfulAllocations) noexcept { remaining = successfulAllocations; fired = false; }
void disarm() noexcept { remaining = -1; }
bool triggered() noexcept { return fired; }
}

void* operator new(std::size_t bytes) { return allocate(bytes); }
void* operator new[](std::size_t bytes) { return allocate(bytes); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept { try { return allocate(bytes); } catch (...) { return nullptr; } }
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept { try { return allocate(bytes); } catch (...) { return nullptr; } }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }

void* operator new(std::size_t bytes, std::align_val_t alignment) { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); }
void* operator new[](std::size_t bytes, std::align_val_t alignment) { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); }
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); } catch (...) { return nullptr; } }
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); } catch (...) { return nullptr; } }
void operator delete(void* pointer, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { free_aligned(pointer); }
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { free_aligned(pointer); }
