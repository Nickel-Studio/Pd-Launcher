#pragma once
#include "core/config/conditional_file.hpp"
#include "core/common/cancellation.hpp"
#include <filesystem>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif

namespace nickel {
ConditionalReplaceResult foundation_test_replace_file_conditionally(const ConditionalFileSnapshot&,
    std::span<const std::uint8_t>, std::uint64_t, const AtomicWriteOptions&);
ConditionalReplaceResult foundation_test_replace_text_conditionally(const ConditionalFileSnapshot&,
    std::string_view, std::uint64_t, const AtomicWriteOptions&);
namespace foundation_test {
enum class ConditionalFault { None, StageWrite, StageFlush, CancelPrepared, BackupConflict,
    CancelQuarantined, GapContender, RestoreConflict, CancelPublished, FailPublished, RestoreFailure };
struct ConditionalFaultState {
    ConditionalFault mode = ConditionalFault::None;
    Cancellation* cancellation = nullptr;
    std::filesystem::path target, stage, backup, receipt;
    std::intptr_t stage_handle = -1;
    bool writing_stage = false;
    bool fired = false;
};
extern ConditionalFaultState conditional_fault;
void conditional_hook(std::string_view point, const std::filesystem::path& target,
    const std::filesystem::path& stage, const std::filesystem::path& backup,
    const std::filesystem::path& receipt);
} // namespace foundation_test
} // namespace nickel
#ifdef _WIN32
BOOL WINAPI conditional_test_WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL WINAPI conditional_test_FlushFileBuffers(HANDLE);
#else
#include <unistd.h>
ssize_t conditional_test_write(int, const void*, std::size_t);
int conditional_test_fsync(int);
#endif
