#include "conditional_write_faults.hpp"
#include "core/common/error.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/path_guard.hpp"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <fcntl.h>
#endif

namespace nickel::foundation_test {
ConditionalFaultState conditional_fault;
void raw_exclusive(const std::filesystem::path& file, std::string_view content) {
#ifdef _WIN32
    const auto handle = CreateFileW(foundation_detail::native_filesystem_path(file).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create actual competing name");
    DWORD written = 0;
    const bool ok = WriteFile(handle, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok || written != content.size()) throw std::runtime_error("Cannot write actual competing name");
#else
    const auto fd = ::open(foundation_detail::native_filesystem_path(file).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("Cannot create actual competing name");
    const bool ok = ::write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) throw std::runtime_error("Cannot write actual competing name");
#endif
}
void conditional_hook(std::string_view point, const std::filesystem::path& target,
    const std::filesystem::path& stage, const std::filesystem::path& backup,
    const std::filesystem::path& receipt) {
    auto& state = conditional_fault;
    state.target = target; state.stage = stage; state.backup = backup; state.receipt = receipt;
    state.writing_stage = point == "stage-write" || point == "stage-flush";
    if (point == "receipt-flushed" && state.mode == ConditionalFault::CancelPrepared) {
        state.fired = true; state.cancellation->cancel();
    }
    if (point == "before-quarantine" && state.mode == ConditionalFault::BackupConflict) {
        state.fired = true; raw_exclusive(backup, "external-backup");
    }
    if (point == "after-quarantine" && (state.mode == ConditionalFault::CancelQuarantined ||
        state.mode == ConditionalFault::RestoreConflict || state.mode == ConditionalFault::RestoreFailure)) {
        state.fired = true; state.cancellation->cancel();
    }
    if (point == "before-publication" && state.mode == ConditionalFault::GapContender) {
        state.fired = true; raw_exclusive(target, "external-contender");
    }
    if (point == "before-restoration" && state.mode == ConditionalFault::RestoreConflict) {
        state.fired = true; raw_exclusive(target, "external-restoration");
    }
    if (point == "before-restoration" && state.mode == ConditionalFault::RestoreFailure) {
        state.fired = true;
        throw ErrorException(Error{"IoError", "Actual restoration boundary failure", Json::object()});
    }
    if (point == "after-publication" && state.mode == ConditionalFault::CancelPublished) {
        state.fired = true; state.cancellation->cancel();
    }
    if (point == "after-publication" && state.mode == ConditionalFault::FailPublished) {
        state.fired = true;
        throw ErrorException(Error{"IoError", "Actual postpublication boundary failure", Json::object()});
    }
}
} // namespace nickel::foundation_test
#ifdef _WIN32
BOOL WINAPI conditional_test_WriteFile(HANDLE file, LPCVOID bytes, DWORD size, LPDWORD written, LPOVERLAPPED overlapped) {
    auto& state = nickel::foundation_test::conditional_fault;
    if (state.writing_stage && state.mode == nickel::foundation_test::ConditionalFault::StageWrite) {
        state.fired = true;
        *written = 0;
        SetLastError(ERROR_DISK_FULL);
        return FALSE;
    }
    return WriteFile(file, bytes, size, written, overlapped);
}
BOOL WINAPI conditional_test_FlushFileBuffers(HANDLE file) {
    auto& state = nickel::foundation_test::conditional_fault;
    if (state.writing_stage && state.mode == nickel::foundation_test::ConditionalFault::StageFlush) {
        state.fired = true;
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return FlushFileBuffers(file);
}
#else
ssize_t conditional_test_write(int file, const void* bytes, std::size_t size) {
    auto& state = nickel::foundation_test::conditional_fault;
    if (state.writing_stage && state.mode == nickel::foundation_test::ConditionalFault::StageWrite) {
        state.fired = true; errno = ENOSPC; return -1;
    }
    return ::write(file, bytes, size);
}
int conditional_test_fsync(int file) {
    auto& state = nickel::foundation_test::conditional_fault;
    if (state.writing_stage && state.mode == nickel::foundation_test::ConditionalFault::StageFlush) {
        state.fired = true; errno = EIO; return -1;
    }
    return ::fsync(file);
}
#endif

namespace {
using namespace nickel;
using namespace nickel::foundation_test;
namespace fs = std::filesystem;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    fs::path root = fs::current_path() / ("foundation-conditional-fault-" + unique_token());
    Fixture() { fs::create_directories(root); }
    ~Fixture() { std::error_code error; fs::remove_all(foundation_detail::native_filesystem_path(root), error); }
};
Error error_from(const auto& work) {
    try { work(); } catch (const ErrorException& error) { return error.error(); }
    throw std::runtime_error("Fault window did not fail");
}
void assert_no_owned_artifacts(const fs::path& root, const fs::path& allowed = {}) {
    for (const auto& entry : fs::directory_iterator(foundation_detail::native_filesystem_path(root))) {
        const auto name = path_to_utf8(entry.path().filename());
        if (name.starts_with(".nickel-conditional-") || name.starts_with(".nickel-dir-anchor-")) {
            require(!allowed.empty() && entry.path().filename() == allowed.filename(), "Abort/cleanup left unexpected owned artifacts");
        }
    }
}
void run_window(ConditionalFault mode, const char* label, bool missing = false) {
    Fixture fixture;
    const auto target = fixture.root / "options.txt";
    if (!missing) atomic_write_text(target, "original");
    const auto snapshot = read_conditional_file(target, 128);
    Cancellation cancellation;
    conditional_fault = {};
    conditional_fault.mode = mode;
    conditional_fault.cancellation = &cancellation;
    if (missing) {
        const auto error = error_from([&] { (void)foundation_test_replace_text_conditionally(snapshot, "replacement", 128, {.cancellation = &cancellation}); });
        require(conditional_fault.fired && error.code == "Conflict", "Independent missing publication did not report contention");
        require(read_text_bounded(target, 128) == "external-contender", "Missing publication consumed external contender");
        require(!fs::exists(conditional_fault.backup), "Missing publication invented an original backup");
        assert_no_owned_artifacts(fixture.root);
    } else if (mode == ConditionalFault::CancelPublished || mode == ConditionalFault::FailPublished) {
        const auto result = foundation_test_replace_text_conditionally(snapshot, "replacement", 128, {.cancellation = &cancellation});
        require(conditional_fault.fired && result.committed && read_text_bounded(target, 128) == "replacement", "Postpublication cancellation/failure erased committed fact");
        if (mode == ConditionalFault::FailPublished) {
            require(!result.recovery_receipt.empty() && fs::exists(result.recovery_receipt), "Committed cleanup failure lost recovery receipt");
            // Same-byte identity substitution is not a valid publication proof.
            const auto held = fixture.root / "held-committed";
            fs::rename(target, held);
            raw_exclusive(target, "replacement");
            require(error_from([&] { (void)recover_conditional_replacements(fixture.root); }).code == "RecoveryRequired", "Recovery accepted different target identity with same bytes");
            require(read_text_bounded(target, 128) == "replacement" && fs::exists(result.recovery_receipt), "Recovery consumed external same-byte occupant");
            fs::rename(target, fixture.root / "external-retained");
            fs::rename(held, target);
            const auto recovered = recover_conditional_replacements(fixture.root);
            require(recovered.committed == 1 && recovered.restored == 0, "Published-stage recovery did not finish committed cleanup");
            require(read_text_bounded(target, 128) == "replacement", "Published-stage recovery changed committed content");
        } else {
            require(result.recovery_receipt.empty(), "Late cancellation prevented committed cleanup");
        }
        assert_no_owned_artifacts(fixture.root);
    } else {
        const auto error = error_from([&] { (void)foundation_test_replace_text_conditionally(snapshot, "replacement", 128, {.cancellation = &cancellation}); });
        require(conditional_fault.fired, "Requested fault window was not reached");
        if (mode == ConditionalFault::GapContender || mode == ConditionalFault::RestoreConflict || mode == ConditionalFault::RestoreFailure) {
            require(error.code == "RecoveryRequired" && error.details["receipt"].get<std::string>() == path_to_utf8(conditional_fault.receipt), "Ambiguous rollback did not expose exact recovery receipt");
            require(fs::exists(conditional_fault.stage) && fs::exists(conditional_fault.receipt), "RecoveryRequired lost stage or receipt");
            require(read_text_bounded(conditional_fault.backup, 128) == "original", "RecoveryRequired lost retained original backup");
            if (mode != ConditionalFault::RestoreFailure) {
                const auto external = read_text_bounded(target, 128);
                require(external == (mode == ConditionalFault::GapContender ? "external-contender" : "external-restoration"), "Failed restore replaced external occupant");
                const auto again = error_from([&] { (void)recover_conditional_replacements(fixture.root); });
                require(again.code == "RecoveryRequired" && read_text_bounded(target, 128) == external, "Recovery overwrote external occupant");
                require(error_from([&] { (void)read_conditional_file(target, 128); }).code == "RecoveryRequired", "Observation bypassed mandatory pending recovery");
                fs::rename(target, fixture.root / "external-preserved");
            } else require(!fs::exists(target), "Failed restore fabricated an original target");
            const auto report = recover_conditional_replacements(fixture.root);
            require(report.restored == 1 && report.committed == 0, "No-replace recovery did not restore the precommit fact");
            require(read_text_bounded(target, 128) == "original", "Recovery did not restore original content");
            assert_no_owned_artifacts(fixture.root);
        } else {
            const auto expected_code = mode == ConditionalFault::BackupConflict ? "Conflict" :
                mode == ConditionalFault::StageWrite || mode == ConditionalFault::StageFlush ? "IoError" : "Cancelled";
            require(error.code == expected_code, "Prepublication failure returned the wrong observable outcome");
            require(read_text_bounded(target, 128) == "original", "Prepublication failure changed original");
            if (mode == ConditionalFault::BackupConflict) {
                require(read_text_bounded(conditional_fault.backup, 128) == "external-backup", "Backup conflict consumed contender");
                assert_no_owned_artifacts(fixture.root, conditional_fault.backup);
            } else assert_no_owned_artifacts(fixture.root);
        }
    }
    std::cout << "PASS conditional fault: " << label << (missing ? " missing-expectation" : "") << '\n';
    conditional_fault = {};
}
void ambiguous_missing_publication() {
    Fixture fixture;
    const auto target = fixture.root / "missing.txt";
    const auto snapshot = read_conditional_file(target, 128);
    Cancellation cancellation;
    conditional_fault = {};
    conditional_fault.mode = ConditionalFault::FailPublished;
    conditional_fault.cancellation = &cancellation;
    const auto result = foundation_test_replace_text_conditionally(snapshot, "replacement", 128, {});
    require(conditional_fault.fired && result.committed && !result.recovery_receipt.empty(), "Missing publication did not retain its committed receipt");
    const auto held = fixture.root / "held-publication";
    fs::rename(target, held);
    const auto error = error_from([&] { (void)recover_conditional_replacements(fixture.root); });
    require(error.code == "RecoveryRequired" && fs::exists(result.recovery_receipt), "Receipt-only missing recovery invented an aborted outcome");
    require(!fs::exists(target) && read_text_bounded(held, 128) == "replacement", "Ambiguous missing recovery consumed held publication");
    fs::rename(held, target);
    const auto report = recover_conditional_replacements(fixture.root);
    require(report.committed == 1 && report.restored == 0 && read_text_bounded(target, 128) == "replacement", "Restored publication identity did not recover the committed fact");
    assert_no_owned_artifacts(fixture.root);
    conditional_fault = {};
    std::cout << "PASS conditional fault: receipt-only missing publication remains ambiguous until identity restored\n";
}
} // namespace
int main() {
    try {
        run_window(ConditionalFault::StageWrite, "stage write disk-full");
        run_window(ConditionalFault::StageFlush, "stage flush error");
        run_window(ConditionalFault::CancelPrepared, "cancel after durable receipt before quarantine");
        run_window(ConditionalFault::BackupConflict, "unique backup no-replace conflict");
        run_window(ConditionalFault::CancelQuarantined, "cancel after quarantine restores no-replace");
        run_window(ConditionalFault::GapContender, "external occupant during visible name gap");
        run_window(ConditionalFault::RestoreConflict, "external occupant immediately before restoration");
        run_window(ConditionalFault::CancelPublished, "cancel after publication remains committed");
        run_window(ConditionalFault::FailPublished, "postpublication failure preserves receipt and committed fact");
        run_window(ConditionalFault::RestoreFailure, "restoration I/O failure retains backup/receipt");
        run_window(ConditionalFault::GapContender, "independent missing no-replace contention", true);
        ambiguous_missing_publication();
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL " << error.error().code << ": " << error.what() << " details=" << error.error().details.dump() << '\n';
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    return 1;
}
