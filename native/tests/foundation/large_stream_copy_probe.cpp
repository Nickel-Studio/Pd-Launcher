#include "core/common/error.hpp"
#include "core/common/json.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/path_guard.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <malloc.h>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace {
constexpr std::size_t allocation_ceiling = 1024 * 1024;
constexpr std::size_t live_allocation_ceiling = 4 * 1024 * 1024;
constexpr std::uint64_t resident_ceiling = 128 * 1024 * 1024;
std::atomic<bool> measuring{false};
std::atomic<std::size_t> largest{0};
std::atomic<std::uint64_t> requests{0};
std::atomic<std::size_t> live_bytes{0};
std::atomic<std::size_t> peak_live_bytes{0};
std::atomic<std::size_t> tracked_count{0};
struct Allocation { void* address = nullptr; std::size_t size = 0; };
std::array<Allocation, 4096> allocations{};
std::atomic_flag allocation_lock = ATOMIC_FLAG_INIT;
struct AllocationLock {
    AllocationLock() { while (allocation_lock.test_and_set(std::memory_order_acquire)) {} }
    ~AllocationLock() { allocation_lock.clear(std::memory_order_release); }
};
void record_allocation(std::size_t size) {
    if (!measuring.load()) return;
    ++requests;
    auto previous = largest.load();
    while (previous < size && !largest.compare_exchange_weak(previous, size)) {}
    if (size > allocation_ceiling) throw std::bad_alloc();
}
void retain_allocation(void* address, std::size_t size) {
    if (!measuring.load()) return;
    AllocationLock lock;
    const auto live = live_bytes.load();
    if (size > live_allocation_ceiling - live) throw std::bad_alloc();
    // A fixed registry also catches a whole-file buffer split into small allocations.
    for (auto& allocation : allocations) {
        if (allocation.address) continue;
        allocation = {address, size};
        ++tracked_count;
        live_bytes = live + size;
        peak_live_bytes = std::max(peak_live_bytes.load(), live + size);
        return;
    }
    throw std::bad_alloc();
}
void release_allocation(void* address) noexcept {
    if (!address || tracked_count.load() == 0) return;
    AllocationLock lock;
    for (auto& allocation : allocations) {
        if (allocation.address != address) continue;
        live_bytes -= allocation.size;
        --tracked_count;
        allocation = {};
        return;
    }
}
void* aligned_allocate(std::size_t size, std::size_t alignment) {
#ifdef _WIN32
    return _aligned_malloc(size ? size : 1, alignment);
#else
    void* value = nullptr;
    return ::posix_memalign(&value, alignment, size ? size : 1) == 0 ? value : nullptr;
#endif
}
void aligned_release(void* value) noexcept {
#ifdef _WIN32
    _aligned_free(value);
#else
    std::free(value);
#endif
}
}
void* operator new(std::size_t size) {
    record_allocation(size);
    auto* value = std::malloc(size ? size : 1);
    if (!value) throw std::bad_alloc();
    try { retain_allocation(value, size); }
    catch (...) { std::free(value); throw; }
    return value;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    record_allocation(size);
    auto* value = aligned_allocate(size, static_cast<std::size_t>(alignment));
    if (!value) throw std::bad_alloc();
    try { retain_allocation(value, size); }
    catch (...) { aligned_release(value); throw; }
    return value;
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { try { return ::operator new(size); } catch (...) { return nullptr; } }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { try { return ::operator new[](size); } catch (...) { return nullptr; } }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return ::operator new(size, alignment); } catch (...) { return nullptr; } }
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return ::operator new[](size, alignment); } catch (...) { return nullptr; } }
void operator delete(void* value) noexcept { release_allocation(value); std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete(void* value, std::align_val_t) noexcept { release_allocation(value); aligned_release(value); }
void operator delete[](void* value, std::align_val_t alignment) noexcept { ::operator delete(value, alignment); }
void operator delete(void* value, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(value, alignment); }
void operator delete[](void* value, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(value, alignment); }
void operator delete(void* value, const std::nothrow_t&) noexcept { ::operator delete(value); }
void operator delete[](void* value, const std::nothrow_t&) noexcept { ::operator delete(value); }
void operator delete(void* value, std::align_val_t alignment, const std::nothrow_t&) noexcept { ::operator delete(value, alignment); }
void operator delete[](void* value, std::align_val_t alignment, const std::nothrow_t&) noexcept { ::operator delete(value, alignment); }

namespace {
using namespace nickel;
using namespace nickel::foundation_detail;
namespace fs = std::filesystem;
constexpr std::uint64_t overhead = 64 * 1024 * 1024;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::uint64_t number(std::string_view value) {
    require(!value.empty() && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }), "Unsigned decimal argument required");
    std::size_t consumed = 0;
    const auto parsed = std::stoull(std::string(value), &consumed);
    require(consumed == value.size(), "Invalid byte count");
    return parsed;
}
struct Bound {
    Bound() {
        require(tracked_count == 0 && live_bytes == 0, "Prior measured allocations remain live");
        largest = 0; requests = 0; peak_live_bytes = 0; measuring = true;
    }
    ~Bound() { measuring = false; }
};
struct File {
#ifdef _WIN32
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit File(const fs::path& path) : value(CreateFileW(native_filesystem_path(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {
        require(value != INVALID_HANDLE_VALUE, "Cannot retain experiment file");
    }
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
#else
    int value = -1;
    explicit File(const fs::path& path) : value(::open(native_filesystem_path(path).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)) {
        require(value >= 0, "Cannot retain experiment file");
    }
    ~File() { if (value >= 0) ::close(value); }
#endif
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
Json inspect(const File& file) {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_STANDARD_INFO standard{};
    require(GetFileInformationByHandle(file.value, &info) && GetFileInformationByHandleEx(file.value, FileStandardInfo, &standard, sizeof(standard)), "Cannot inspect actual file allocation");
    constexpr DWORD refused = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED;
    require(!(info.dwFileAttributes & refused) && info.nNumberOfLinks == 1 && !standard.Directory && !standard.DeletePending, "Experiment requires ordinary uncompressed, nonsparse, single-link files");
    require(standard.EndOfFile.QuadPart >= 0 && standard.AllocationSize.QuadPart >= standard.EndOfFile.QuadPart, "Experiment file is not allocated to its full logical length");
    return Json{{"bytes", std::uint64_t(standard.EndOfFile.QuadPart)}, {"allocatedBytes", std::uint64_t(standard.AllocationSize.QuadPart)},
        {"attributes", info.dwFileAttributes}, {"links", info.nNumberOfLinks},
        {"object", Json::array({info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow})},
        {"writeTime", Json::array({info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime})}};
#else
    struct stat info{};
    require(::fstat(file.value, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_size >= 0 && info.st_blocks >= 0, "Experiment requires ordinary single-link files");
    const auto allocated = std::uint64_t(info.st_blocks) * 512;
    require(allocated >= std::uint64_t(info.st_size), "Sparse/compressed allocation does not qualify as a physical full-size copy");
#ifdef __APPLE__
    const auto stamp = info.st_mtimespec;
#else
    const auto stamp = info.st_mtim;
#endif
    return Json{{"bytes", std::uint64_t(info.st_size)}, {"allocatedBytes", allocated}, {"links", std::uint64_t(info.st_nlink)},
        {"object", Json::array({std::uint64_t(info.st_dev), std::uint64_t(info.st_ino)})},
        {"writeTime", Json::array({std::int64_t(stamp.tv_sec), std::int64_t(stamp.tv_nsec)})}};
#endif
}
std::uint64_t available(const fs::path& path) {
    DirectoryGuard guard(path);
#ifdef _WIN32
    ULARGE_INTEGER bytes{};
    require(GetDiskFreeSpaceExW(native_filesystem_path(path).c_str(), &bytes, nullptr, nullptr), "Cannot inspect actual output volume reserve");
    const auto result = bytes.QuadPart;
#else
    struct statvfs info{};
    require(::fstatvfs(static_cast<int>(guard.native_handle()), &info) == 0, "Cannot inspect actual output volume reserve");
    require(info.f_frsize != 0 && std::uint64_t(info.f_bavail) <= std::numeric_limits<std::uint64_t>::max() / std::uint64_t(info.f_frsize), "Volume capacity cannot be represented");
    const auto result = std::uint64_t(info.f_bavail) * std::uint64_t(info.f_frsize);
#endif
    guard.verify();
    return result;
}
std::uint64_t peak_resident_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    require(GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)), "Cannot inspect process memory evidence");
    return counters.PeakWorkingSetSize;
#else
    struct rusage usage{};
    require(::getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0, "Cannot inspect process memory evidence");
#ifdef __APPLE__
    return std::uint64_t(usage.ru_maxrss);
#else
    return std::uint64_t(usage.ru_maxrss) * 1024;
#endif
#endif
}
int run(const std::vector<std::string>& arguments) {
    require(!arguments.empty(), "Probe mode required");
    const auto& mode = arguments[0];
    if (mode == "allocation-check") {
        require(arguments.size() == 1, "allocation-check accepts no path");
        bool refused = false;
        {
            Bound measured;
            try { auto* memory = ::operator new(allocation_ceiling + 1); ::operator delete(memory); }
            catch (const std::bad_alloc&) { refused = true; }
            require(::operator new[](allocation_ceiling + 1, std::nothrow) == nullptr &&
                    ::operator new(allocation_ceiling + 1, std::align_val_t(64), std::nothrow) == nullptr,
                    "Aligned/nothrow allocation bypassed the ceiling");
        }
        require(refused && largest == allocation_ceiling + 1, "Allocation ceiling is not active in the test probe");
        bool live_refused = false;
        std::array<void*, 5> held{};
        {
            Bound measured;
            try { for (auto& memory : held) memory = ::operator new(allocation_ceiling); }
            catch (const std::bad_alloc&) { live_refused = true; }
            for (auto* memory : held) ::operator delete(memory);
        }
        require(live_refused && peak_live_bytes == live_allocation_ceiling && live_bytes == 0 && tracked_count == 0,
                "Cumulative live allocation ceiling is not active in the test probe");
        std::cout << Json{{"allocationCeilingVerified", true}, {"liveAllocationCeilingVerified", true},
            {"refusedRequestBytes", allocation_ceiling + 1}, {"liveAllocationCeilingBytes", live_allocation_ceiling}}.dump() << '\n';
        return 0;
    }
    if (mode == "budget") {
        require(arguments.size() == 4, "budget requires existing scratch parent, size and reserve");
        const auto scratch = checked_absolute_path(path_from_utf8(arguments[1]));
        const auto size = number(arguments[2]);
        const auto reserve = number(arguments[3]);
        require(reserve <= std::numeric_limits<std::uint64_t>::max() - overhead && size <= (std::numeric_limits<std::uint64_t>::max() - reserve - overhead) / 2, "Fixture budget overflows uint64");
        const auto required = size * 2 + reserve + overhead;
        const auto free = available(scratch);
        const bool allowed = free >= required;
        std::cout << Json{{"allowed", allowed}, {"availableBytes", free}, {"sourceBytes", size}, {"destinationBytes", size},
            {"reserveBytes", reserve}, {"overheadBytes", overhead}, {"requiredBytes", required}, {"fixtureWrites", 0}}.dump() << '\n';
        return allowed ? 0 : 77;
    }
    if (mode == "prepare") {
        require(arguments.size() == 3, "prepare requires unique root and destination parent");
        const auto root = checked_absolute_path(path_from_utf8(arguments[1]));
        const auto target = checked_absolute_path(path_from_utf8(arguments[2]));
        { DirectoryGuard guard(root, true); guard.verify(); }
        { DirectoryGuard guard(root / "source", true); guard.verify(); }
        { DirectoryGuard guard(target, true); guard.verify(); }
        std::cout << Json{{"prepared", true}, {"root", path_to_utf8(root)}, {"destinationParent", path_to_utf8(target)}}.dump() << '\n';
        return 0;
    }
    require(mode == "copy" && arguments.size() == 6, "copy requires source, destination, size, SHA256 and reserve");
    const auto source = checked_absolute_path(path_from_utf8(arguments[1]));
    const auto destination = checked_absolute_path(path_from_utf8(arguments[2]));
    const auto size = number(arguments[3]);
    const auto hash = arguments[4];
    const auto reserve = number(arguments[5]);
    File input(source);
    const auto before = inspect(input);
    require(before["bytes"].get<std::uint64_t>() == size, "Generated source length differs from explicit bound");
    const auto free_before = available(destination.parent_path());
    require(free_before >= reserve && free_before - reserve >= size && free_before - reserve - size >= overhead, "Insufficient output capacity; copy refused before stage creation");
    const auto start = std::chrono::steady_clock::now();
    bool copied = false;
    { Bound measured; copied = atomic_copy_no_replace(source, destination, size, nullptr, hash); }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    require(copied && largest <= allocation_ceiling && peak_live_bytes <= live_allocation_ceiling && live_bytes == 0 && tracked_count == 0,
            "Actual copy failed, leaked measured allocations or used an unbounded C++ buffer");
    const auto after = inspect(input);
    File output(destination);
    const auto target = inspect(output);
    require(before == after && target["bytes"].get<std::uint64_t>() == size && before["object"] != target["object"], "Copy did not preserve a distinct unchanged source object");
    require(!atomic_copy_no_replace(source, destination, size, nullptr, hash), "Repeat copy consumed an existing destination");
    const auto resident = peak_resident_bytes();
    require(resident <= resident_ceiling, "Process resident peak exceeded the constant memory ceiling");
    std::cout << Json{{"copied", copied}, {"source", path_to_utf8(source)}, {"destination", path_to_utf8(destination)},
        {"bytes", size}, {"sourceBefore", before}, {"sourceAfter", after}, {"destinationFile", target},
        {"expectedSha256", hash}, {"copySeconds", elapsed}, {"largestCppAllocationBytes", largest.load()},
        {"cppAllocationRequests", requests.load()}, {"cppAllocationCeilingBytes", allocation_ceiling},
        {"peakLiveCppAllocationBytes", peak_live_bytes.load()}, {"liveCppAllocationBytesAfterCopy", live_bytes.load()},
        {"liveCppAllocationCeilingBytes", live_allocation_ceiling}, {"peakResidentBytes", resident},
        {"residentCeilingBytes", resident_ceiling}, {"destinationNativeUnits", destination.native().size()},
        {"freeBeforeCopy", free_before}, {"freeAfterCopy", available(destination.parent_path())},
        {"reserveBytes", reserve}, {"existingDestinationPreserved", true}}.dump() << '\n';
    return 0;
}
int entry(const std::vector<std::string>& arguments) {
    try { return run(arguments); }
    catch (const ErrorException& error) {
        std::cerr << Json{{"error", error.error().code}, {"message", error.what()}, {"details", error.error().details}}.dump() << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) arguments.push_back(nickel::path_to_utf8(std::filesystem::path(argv[i])));
    return entry(arguments);
}
#else
int main(int argc, char** argv) {
    return entry(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
