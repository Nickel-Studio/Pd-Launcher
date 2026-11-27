#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"
#include "core/config/conditional_file.hpp"
#include "core/config/path_guard.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace nickel;
using namespace nickel::foundation_detail;
namespace fs = std::filesystem;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string failure(const auto& work) {
    try { work(); } catch (const ErrorException& error) { return error.error().code; }
    throw std::runtime_error("Conditional operation unexpectedly succeeded");
}
struct Fixture {
    fs::path root = fs::current_path() / ("foundation-conditional-" + unique_token());
    Fixture() { fs::create_directories(root); }
    ~Fixture() { std::error_code error; fs::remove_all(native_filesystem_path(root), error); }
};
void clean_receipts(const fs::path& parent) {
    for (const auto& entry : fs::directory_iterator(native_filesystem_path(parent))) {
        const auto name = path_to_utf8(entry.path().filename());
        require(!name.starts_with(".nickel-conditional-") && !name.starts_with(".nickel-dir-anchor-"), "Successful replacement left a receipt, backup, stage or anchor");
    }
}
void basic(const fs::path& root) {
    const auto target = root / "options.txt";
    const auto missing = read_conditional_file(target, 64);
    require(!missing.exists() && missing.bytes().empty() && missing.sha256().empty(), "Missing snapshot is not explicit");
    const auto first = replace_text_conditionally(missing, "", 64);
    require(first.committed && first.recovery_receipt.empty(), "Missing expectation did not publish exclusively");
    const auto empty = read_conditional_file(target, 64);
    require(empty.exists() && empty.bytes().empty() && empty.sha256() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "Existing empty file became missing");
    require(failure([&] { (void)replace_text_conditionally(missing, "stale", 64); }) == "Conflict", "Missing expectation replaced an existing empty file");
    const auto result = replace_text_conditionally(empty, "offline-options", 64, {.private_file = false, .create_parents = false});
    require(result.committed && result.recovery_receipt.empty() && read_text_bounded(target, 64) == "offline-options", "Existing-file two-phase replacement failed");
    require(failure([&] { (void)replace_text_conditionally(empty, "stale", 64); }) == "Conflict", "Stale existing expectation overwrote committed file");
    const auto fresh = read_conditional_file(target, 64);
    require(failure([&] { (void)replace_text_conditionally(fresh, std::string(65, 'x'), 64); }) == "LimitExceeded", "Replacement size limit ignored");
    require(failure([&] { (void)read_conditional_file(target, 2); }) == "LimitExceeded", "Observation size limit ignored");
    require(failure([&] { (void)read_conditional_file(target, 8 * 1024 * 1024 + 1); }) == "LimitExceeded", "Global conditional bound ignored");
    Cancellation cancellation;
    cancellation.cancel();
    require(failure([&] { (void)replace_text_conditionally(fresh, "cancelled", 64, {.cancellation = &cancellation}); }) == "Cancelled", "Precommit cancellation ignored");
    require(read_text_bounded(target, 64) == "offline-options", "Cancellation changed original");
    clean_receipts(root);
    std::cout << "PASS conditional: explicit missing, existing-empty, ordinary existing replacement, no-replace conflict, bounds, cancellation\n";
}
void changed_expectations(const fs::path& root) {
    const auto target = root / "changed.txt";
    atomic_write_text(target, "same-object");
    const auto revision = read_conditional_file(target, 128);
    fs::last_write_time(native_filesystem_path(target), fs::last_write_time(native_filesystem_path(target)) + std::chrono::seconds(2));
    require(failure([&] { (void)replace_text_conditionally(revision, "revision-lost", 128); }) == "Conflict", "Changed revision with unchanged hash was accepted");
    const auto identity = read_conditional_file(target, 128);
    fs::rename(native_filesystem_path(target), native_filesystem_path(root / "retained-original"));
    atomic_write_text(target, "same-object");
    require(failure([&] { (void)replace_text_conditionally(identity, "identity-lost", 128); }) == "Conflict", "Different identity with same bytes was accepted");
    const auto digest = read_conditional_file(target, 128);
    const auto stamp = fs::last_write_time(native_filesystem_path(target));
#ifdef _WIN32
    const auto handle = CreateFileW(native_filesystem_path(target).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Cannot open actual content mutator");
    DWORD written = 0;
    const bool ok = WriteFile(handle, "hash-change", 11, &written, nullptr) && FlushFileBuffers(handle);
    CloseHandle(handle);
    require(ok && written == 11, "Cannot mutate actual original content");
#else
    const auto fd = ::open(native_filesystem_path(target).c_str(), O_WRONLY | O_NOFOLLOW);
    require(fd >= 0, "Cannot open actual content mutator");
    require(::write(fd, "hash-change", 11) == 11, "Cannot mutate actual original content");
    ::close(fd);
#endif
    fs::last_write_time(native_filesystem_path(target), stamp);
    require(failure([&] { (void)replace_text_conditionally(digest, "digest-lost", 128); }) == "Conflict", "Changed digest with restored write time was accepted");
#ifdef _WIN32
    const auto current = read_conditional_file(target, 128);
    const auto writer = CreateFileW(native_filesystem_path(target).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(writer != INVALID_HANDLE_VALUE, "Cannot retain preexisting writer");
    const auto code = failure([&] { (void)replace_text_conditionally(current, "writer-lost", 128); });
    CloseHandle(writer);
    require(code == "IoError", "Preexisting Windows writer did not fail closed");
#endif
    clean_receipts(root);
    std::cout << "PASS conditional: identity, revision and content mismatch; retained preexisting Windows writer fails closed\n";
}
#ifdef _WIN32
DWORD set_tag(HANDLE handle, const fs::path& target, bool symlink) {
    const auto substitute = L"\\??\\" + target.native();
    const auto printed = target.native();
    const auto offset = symlink ? 20u : 16u;
    std::vector<std::uint8_t> data(offset + (substitute.size() + printed.size() + 2) * sizeof(wchar_t));
    const DWORD tag = symlink ? 0xa000000c : 0xa0000003;
    const std::array<USHORT, 6> fields{static_cast<USHORT>(data.size() - 8), 0, 0,
        static_cast<USHORT>(substitute.size() * sizeof(wchar_t)), static_cast<USHORT>((substitute.size() + 1) * sizeof(wchar_t)),
        static_cast<USHORT>(printed.size() * sizeof(wchar_t))};
    std::memcpy(data.data(), &tag, sizeof(tag));
    std::memcpy(data.data() + 4, fields.data(), sizeof(fields));
    std::memcpy(data.data() + offset, substitute.data(), substitute.size() * sizeof(wchar_t));
    std::memcpy(data.data() + offset + (substitute.size() + 1) * sizeof(wchar_t), printed.data(), printed.size() * sizeof(wchar_t));
    DWORD returned = 0;
    const auto ok = DeviceIoControl(handle, 0x000900a4, data.data(), static_cast<DWORD>(data.size()), nullptr, 0, &returned, nullptr);
    return ok ? 0 : GetLastError();
}
void delete_tag(HANDLE handle, bool symlink) {
    const std::array<DWORD, 2> value{symlink ? 0xa000000cu : 0xa0000003u, 0};
    DWORD returned = 0;
    require(DeviceIoControl(handle, 0x000900ac, const_cast<DWORD*>(value.data()), sizeof(value), nullptr, 0, &returned, nullptr) != FALSE, "Cannot remove fixture reparse tag");
}
void mutation_mode(const fs::path& root) {
    const auto final = root / "mutation" / "empty-final";
    const auto outside = root / "outside";
    fs::create_directories(final);
    fs::create_directories(outside);
    {
        DirectoryGuard parent(final, false, DirectoryGuardMode::NamespaceMutation);
        require(parent.mode() == DirectoryGuardMode::NamespaceMutation, "Mutation mode not retained");
        std::size_t anchors = 0;
        for (const auto& entry : fs::directory_iterator(native_filesystem_path(final))) {
            if (path_to_utf8(entry.path().filename()).starts_with(".nickel-dir-anchor-")) {
                ++anchors;
                const auto attacker = CreateFileW(entry.path().c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                require(attacker == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "Mutation anchor could be changed or deleted externally");
            }
        }
        require(anchors == 1, "Empty final directory is not privately anchored");
        for (const auto& path : {final, final.parent_path()}) {
            const auto writer = CreateFileW(native_filesystem_path(path).c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            require(writer != INVALID_HANDLE_VALUE, "Mutation directory writer did not open for actual FSCTL probe");
            for (const bool symlink : {false, true}) {
                const auto error = set_tag(writer, outside, symlink);
                if (!error) delete_tag(writer, symlink);
                std::cout << "RAW mutation tag=" << (symlink ? "symlink" : "mount") << " ancestor=" << (path != final) << " nativeError=" << error << '\n';
                require(error == ERROR_DIR_NOT_EMPTY, "Anchored directory accepted an FSCTL redirect or failed for an unestablished reason");
            }
            CloseHandle(writer);
        }
        require(!MoveFileExW(native_filesystem_path(final.parent_path()).c_str(), native_filesystem_path(root / "moved").c_str(), 0) &&
            GetLastError() == ERROR_SHARING_VIOLATION, "Mutation ancestor could be substituted");
        const auto file = reinterpret_cast<HANDLE>(windows_open_relative(parent, "stage", GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, 2, 0x40));
        require(file != INVALID_HANDLE_VALUE, "Bound relative stage creation failed");
        struct Rename { BOOLEAN replace; HANDLE root; ULONG length; WCHAR name[1]; };
        const std::wstring leaf = L"published";
        std::vector<std::uint64_t> storage((sizeof(Rename) + leaf.size() * sizeof(wchar_t) + 7) / 8);
        auto* rename = reinterpret_cast<Rename*>(storage.data());
        rename->replace = FALSE; rename->root = nullptr; rename->length = static_cast<ULONG>(leaf.size() * sizeof(wchar_t));
        std::memcpy(rename->name, leaf.data(), rename->length);
        using Set = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
        const auto set = reinterpret_cast<Set>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
        IO_STATUS_BLOCK io{};
        require(set && set(file, &io, rename, static_cast<ULONG>(storage.size() * 8), static_cast<FILE_INFORMATION_CLASS>(10)) >= 0,
            "Legitimate inner retained-handle rename failed in mutation mode");
        CloseHandle(file);
        parent.verify();
        require(failure([&] { (void)windows_open_relative(parent, "../escape", GENERIC_READ, FILE_SHARE_READ, 1, 0x40); }) == "UnsafePath", "Relative I/O accepted traversal");
        require(fs::is_empty(outside), "Mutation I/O wrote through an outside redirect");
    }
    for (const auto& entry : fs::directory_iterator(native_filesystem_path(final))) {
        require(!path_to_utf8(entry.path().filename()).starts_with(".nickel-dir-anchor-"), "DELETE_ON_CLOSE anchor survived guard lifetime");
    }
    const auto tagged = root / "preexisting-junction";
    fs::create_directories(tagged);
    const auto raw = CreateFileW(native_filesystem_path(tagged).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(raw != INVALID_HANDLE_VALUE && set_tag(raw, outside, false) == 0, "Actual empty-junction control did not succeed");
    const auto code = failure([&] { DirectoryGuard guard(tagged, false, DirectoryGuardMode::NamespaceMutation); });
    delete_tag(raw, false);
    CloseHandle(raw);
    require(code == "UnsafePath", "Mutation mode followed a preexisting junction");
    std::cout << "PASS mutation guard: retained exclusive anchor, actual final/ancestor mount+symlink145, ancestor32, legal inner rename, relative traversal refusal, preexisting junction rejection\n";
}
#endif
void long_path(const fs::path& root) {
    auto deep = root / "deep";
    while (deep.native().size() < 549) deep /= path_from_utf8("conditional-space-\xe6\xb7\xb1-012345678901234567890123456789");
    { DirectoryGuard create(deep, true); create.verify(); }
    const auto target = deep / "options.txt";
    atomic_write_text(target, "deep-original");
    const auto snapshot = read_conditional_file(target, 256);
    const auto result = replace_text_conditionally(snapshot, "deep-replacement", 256);
    require(result.committed && result.recovery_receipt.empty() && read_text_bounded(target, 256) == "deep-replacement", "Deep-path two-phase replacement failed");
    clean_receipts(deep);
    std::cout << "PASS conditional deep path: " << deep.native().size() << " native units, Unicode+spaces, real observation/receipt/backup/stage publication\n";
}
} // namespace
int main() {
    try {
        Fixture fixture;
        basic(fixture.root);
        changed_expectations(fixture.root);
#ifdef _WIN32
        mutation_mode(fixture.root);
#endif
        long_path(fixture.root);
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL " << error.error().code << ": " << error.what() << " details=" << error.error().details.dump() << '\n';
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    return 1;
}
