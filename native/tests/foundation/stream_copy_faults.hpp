#pragma once

#include "core/common/cancellation.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nickel {
bool foundation_test_atomic_copy_no_replace(const std::filesystem::path& source,
    const std::filesystem::path& destination, std::uint64_t max_bytes,
    const Cancellation* cancellation, std::string_view expected_sha256);
namespace foundation_test {
enum class CopyFault {
    None, DiskFull, ReadError, FlushError, ZeroWrite, OversizedWrite, ShortIo,
    EarlyEof, ExtraByte, RevisionEnd, CancelRead, CancelWrite, CancelFlush,
    Contender, Mutator
};
struct CopyFaultState {
    CopyFault mode = CopyFault::None;
    std::filesystem::path source;
    std::filesystem::path destination;
    std::filesystem::path temporary;
    Cancellation* cancellation = nullptr;
    std::intptr_t source_handle = -1;
    std::intptr_t temporary_handle = -1;
    std::uint64_t source_size = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t written_bytes = 0;
    std::size_t read_calls = 0;
    std::size_t write_calls = 0;
    std::size_t largest_read = 0;
    std::size_t largest_write = 0;
    bool action_ran = false;
    bool source_read_only = false;
    bool temporary_private_exclusive = false;
    bool mutators_blocked = false;
};
extern CopyFaultState copy_fault;
}
}

#ifdef _WIN32
HANDLE WINAPI foundation_test_CreateFileW(LPCWSTR name, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file);
BOOL WINAPI foundation_test_ReadFile(HANDLE file, LPVOID bytes, DWORD size, LPDWORD read, LPOVERLAPPED overlapped);
BOOL WINAPI foundation_test_WriteFile(HANDLE file, LPCVOID bytes, DWORD size, LPDWORD written, LPOVERLAPPED overlapped);
BOOL WINAPI foundation_test_FlushFileBuffers(HANDLE file);
BOOL WINAPI foundation_test_GetFileInformationByHandle(HANDLE file, LPBY_HANDLE_FILE_INFORMATION info);
#else
int foundation_test_openat(int directory, const char* name, int flags, ...);
ssize_t foundation_test_read(int file, void* bytes, std::size_t size);
ssize_t foundation_test_write(int file, const void* bytes, std::size_t size);
int foundation_test_fstat(int file, struct stat* info);
int foundation_test_fsync(int file);
#endif
