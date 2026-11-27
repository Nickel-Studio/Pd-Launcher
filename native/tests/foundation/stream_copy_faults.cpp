#include "stream_copy_faults.hpp"
#include "core/common/error.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <tuple>

namespace nickel::foundation_test { CopyFaultState copy_fault; }
namespace {
using namespace nickel;
using namespace nickel::foundation_test;
namespace fs = std::filesystem;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
fs::path raw_native(fs::path path) {
#ifdef _WIN32
    path.make_preferred();
    const auto text = path.native();
    if (text.starts_with(L"\\\\?\\")) return path;
    return fs::path(text.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + text.substr(2) : L"\\\\?\\" + text);
#else
    return path;
#endif
}
void raw_write(const fs::path& path, std::string_view text) {
    fs::create_directories(raw_native(path.parent_path()));
    std::ofstream output(raw_native(path), std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    require(static_cast<bool>(output), "Fault fixture write failed");
}
std::string raw_read(const fs::path& path) {
    std::ifstream input(raw_native(path), std::ios::binary);
    require(static_cast<bool>(input), "Fault fixture read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void no_temporaries(const fs::path& directory) {
    for (const auto& item : fs::directory_iterator(raw_native(directory))) {
        require(!path_to_utf8(item.path().filename()).starts_with(".nickel-copy-"), "Failed copy leaked an owned temporary");
    }
}
std::string failure(const auto& action) {
    try { action(); } catch (const ErrorException& error) { return error.error().code; }
    throw std::runtime_error("Faulted copy unexpectedly succeeded");
}
void on_write() {
    if (copy_fault.action_ran) return;
    if (copy_fault.mode == CopyFault::Contender) {
        raw_write(copy_fault.destination, "external-contender");
        copy_fault.action_ran = true;
    } else if (copy_fault.mode == CopyFault::Mutator) {
#ifdef _WIN32
        bool blocked = true;
        for (const auto& [path, directory] : std::array<std::pair<fs::path, bool>, 4>{{
            {copy_fault.source, false}, {copy_fault.temporary, false},
            {copy_fault.source.parent_path(), true}, {copy_fault.destination.parent_path(), true}
        }}) {
            const auto handle = CreateFileW(raw_native(path).c_str(), GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
            const auto error = GetLastError();
            if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); blocked = false; }
            else if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) blocked = false;
        }
        copy_fault.mutators_blocked = blocked;
#else
        const auto handle = ::open(copy_fault.source.c_str(), O_WRONLY | O_CLOEXEC);
        require(handle >= 0, "POSIX external writer fixture unavailable");
        const char changed = 'z';
        const auto written = ::pwrite(handle, &changed, 1, 0);
        const auto flushed = ::fsync(handle);
        ::close(handle);
        require(written == 1 && flushed == 0, "POSIX same-size source mutation failed");
#endif
        copy_fault.action_ran = true;
    }
}
struct Root {
    fs::path top = fs::current_path() / ("foundation-copyfault-" + unique_token());
    fs::path deep = top;
    Root() {
        require(fs::space(raw_native(fs::current_path())).available >= 64 * 1024 * 1024,
            "Insufficient fixture space: refused before writing");
        while (deep.native().size() < 520) deep /= path_from_utf8("fault-\xE6\xB5\x8B\xE8\xAF\x95 space-" + std::string(40, 'd'));
        fs::create_directories(raw_native(deep));
    }
    ~Root() { std::error_code error; fs::remove_all(raw_native(top), error); }
};
}

#ifdef _WIN32
HANDLE WINAPI foundation_test_CreateFileW(LPCWSTR name, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file) {
    const auto handle = CreateFileW(name, access, sharing, security, creation, flags, template_file);
    if (handle == INVALID_HANDLE_VALUE) return handle;
    const fs::path path(name);
    if (creation == OPEN_EXISTING && path == raw_native(copy_fault.source)) {
        copy_fault.source_handle = reinterpret_cast<std::intptr_t>(handle);
        copy_fault.source_read_only = access == GENERIC_READ && sharing == FILE_SHARE_READ &&
            (flags & FILE_FLAG_OPEN_REPARSE_POINT) != 0;
    } else if (creation == CREATE_NEW && path.filename().native().starts_with(L".nickel-copy-")) {
        copy_fault.temporary_handle = reinterpret_cast<std::intptr_t>(handle);
        copy_fault.temporary = path;
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        copy_fault.temporary_private_exclusive = access == (GENERIC_READ | GENERIC_WRITE | DELETE) &&
            sharing == FILE_SHARE_READ && security && security->lpSecurityDescriptor &&
            GetSecurityDescriptorControl(security->lpSecurityDescriptor, &control, &revision) &&
            (control & SE_DACL_PROTECTED) != 0;
    }
    return handle;
}
BOOL WINAPI foundation_test_ReadFile(HANDLE file, LPVOID bytes, DWORD size, LPDWORD read, LPOVERLAPPED overlapped) {
    if (reinterpret_cast<std::intptr_t>(file) != copy_fault.source_handle) return ReadFile(file, bytes, size, read, overlapped);
    ++copy_fault.read_calls;
    copy_fault.largest_read = std::max<std::size_t>(copy_fault.largest_read, size);
    if (copy_fault.mode == CopyFault::ReadError && copy_fault.read_bytes) {
        copy_fault.action_ran = true; SetLastError(ERROR_READ_FAULT); return FALSE;
    }
    if (copy_fault.mode == CopyFault::EarlyEof && copy_fault.read_bytes) {
        copy_fault.action_ran = true; *read = 0; return TRUE;
    }
    if (copy_fault.mode == CopyFault::ExtraByte && copy_fault.read_bytes == copy_fault.source_size) {
        copy_fault.action_ran = true; *static_cast<std::uint8_t*>(bytes) = 0xEF; *read = 1; return TRUE;
    }
    const auto requested = copy_fault.mode == CopyFault::ShortIo ? std::min<DWORD>(size, 4093) : size;
    const auto result = ReadFile(file, bytes, requested, read, overlapped);
    if (result) copy_fault.read_bytes += *read;
    if (result && *read && copy_fault.mode == CopyFault::CancelRead) {
        copy_fault.action_ran = true; copy_fault.cancellation->cancel();
    }
    return result;
}
BOOL WINAPI foundation_test_WriteFile(HANDLE file, LPCVOID bytes, DWORD size, LPDWORD written, LPOVERLAPPED overlapped) {
    if (reinterpret_cast<std::intptr_t>(file) != copy_fault.temporary_handle) return WriteFile(file, bytes, size, written, overlapped);
    ++copy_fault.write_calls;
    copy_fault.largest_write = std::max<std::size_t>(copy_fault.largest_write, size);
    on_write();
    if (copy_fault.written_bytes && copy_fault.mode == CopyFault::DiskFull) {
        copy_fault.action_ran = true; SetLastError(ERROR_DISK_FULL); return FALSE;
    }
    if (copy_fault.written_bytes && copy_fault.mode == CopyFault::ZeroWrite) {
        copy_fault.action_ran = true; *written = 0; return TRUE;
    }
    if (copy_fault.written_bytes && copy_fault.mode == CopyFault::OversizedWrite) {
        copy_fault.action_ran = true; *written = size + 1; return TRUE;
    }
    const auto requested = copy_fault.mode == CopyFault::ShortIo ? std::min<DWORD>(size, 4091) : size;
    const auto result = WriteFile(file, bytes, requested, written, overlapped);
    if (result) copy_fault.written_bytes += *written;
    if (result && copy_fault.mode == CopyFault::CancelWrite) {
        copy_fault.action_ran = true; copy_fault.cancellation->cancel();
    }
    return result;
}
BOOL WINAPI foundation_test_FlushFileBuffers(HANDLE file) {
    if (reinterpret_cast<std::intptr_t>(file) != copy_fault.temporary_handle) return FlushFileBuffers(file);
    if (copy_fault.mode == CopyFault::FlushError) {
        copy_fault.action_ran = true; SetLastError(ERROR_WRITE_FAULT); return FALSE;
    }
    const auto result = FlushFileBuffers(file);
    if (result && copy_fault.mode == CopyFault::CancelFlush) {
        copy_fault.action_ran = true; copy_fault.cancellation->cancel();
    }
    return result;
}
BOOL WINAPI foundation_test_GetFileInformationByHandle(HANDLE file, LPBY_HANDLE_FILE_INFORMATION info) {
    const auto result = GetFileInformationByHandle(file, info);
    if (result && reinterpret_cast<std::intptr_t>(file) == copy_fault.source_handle &&
        copy_fault.mode == CopyFault::RevisionEnd && copy_fault.read_bytes == copy_fault.source_size) {
        copy_fault.action_ran = true;
        info->ftLastWriteTime.dwLowDateTime ^= 1;
    }
    return result;
}
#else
int foundation_test_openat(int directory, const char* name, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags); mode = static_cast<mode_t>(va_arg(args, int)); va_end(args);
    }
    const auto handle = ::openat(directory, name, flags, mode);
    if (handle < 0) return handle;
    if ((flags & O_ACCMODE) == O_RDONLY && fs::path(name) == copy_fault.source.filename()) {
        copy_fault.source_handle = handle;
        copy_fault.source_read_only = (flags & O_NOFOLLOW) && (flags & O_CLOEXEC);
    } else if ((flags & O_CREAT) && fs::path(name).native().starts_with(".nickel-copy-")) {
        copy_fault.temporary_handle = handle;
        copy_fault.temporary = copy_fault.destination.parent_path() / name;
        struct stat info{};
        copy_fault.temporary_private_exclusive = (flags & O_EXCL) && (flags & O_NOFOLLOW) &&
            ::fstat(handle, &info) == 0 && (info.st_mode & 0777) == 0600;
    }
    return handle;
}
ssize_t foundation_test_read(int file, void* bytes, std::size_t size) {
    if (file != copy_fault.source_handle) return ::read(file, bytes, size);
    ++copy_fault.read_calls;
    copy_fault.largest_read = std::max(copy_fault.largest_read, size);
    if (copy_fault.mode == CopyFault::ReadError && copy_fault.read_bytes) { copy_fault.action_ran = true; errno = EIO; return -1; }
    if (copy_fault.mode == CopyFault::EarlyEof && copy_fault.read_bytes) { copy_fault.action_ran = true; return 0; }
    if (copy_fault.mode == CopyFault::ExtraByte && copy_fault.read_bytes == copy_fault.source_size) {
        copy_fault.action_ran = true; *static_cast<std::uint8_t*>(bytes) = 0xEF; return 1;
    }
    const auto result = ::read(file, bytes, copy_fault.mode == CopyFault::ShortIo ? std::min<std::size_t>(size, 4093) : size);
    if (result > 0) copy_fault.read_bytes += static_cast<std::uint64_t>(result);
    if (result > 0 && copy_fault.mode == CopyFault::CancelRead) { copy_fault.action_ran = true; copy_fault.cancellation->cancel(); }
    return result;
}
ssize_t foundation_test_write(int file, const void* bytes, std::size_t size) {
    if (file != copy_fault.temporary_handle) return ::write(file, bytes, size);
    ++copy_fault.write_calls;
    copy_fault.largest_write = std::max(copy_fault.largest_write, size);
    on_write();
    if (copy_fault.written_bytes && copy_fault.mode == CopyFault::DiskFull) { copy_fault.action_ran = true; errno = ENOSPC; return -1; }
    if (copy_fault.written_bytes && copy_fault.mode == CopyFault::ZeroWrite) { copy_fault.action_ran = true; return 0; }
    if (copy_fault.written_bytes && copy_fault.mode == CopyFault::OversizedWrite) { copy_fault.action_ran = true; return static_cast<ssize_t>(size + 1); }
    const auto result = ::write(file, bytes, copy_fault.mode == CopyFault::ShortIo ? std::min<std::size_t>(size, 4091) : size);
    if (result > 0) copy_fault.written_bytes += static_cast<std::uint64_t>(result);
    if (result > 0 && copy_fault.mode == CopyFault::CancelWrite) { copy_fault.action_ran = true; copy_fault.cancellation->cancel(); }
    return result;
}
int foundation_test_fstat(int file, struct stat* info) {
    const auto result = ::fstat(file, info);
    if (result == 0 && file == copy_fault.source_handle && copy_fault.mode == CopyFault::RevisionEnd &&
        copy_fault.read_bytes == copy_fault.source_size) {
        copy_fault.action_ran = true;
#if defined(__APPLE__)
        info->st_mtimespec.tv_nsec ^= 1;
#else
        info->st_mtim.tv_nsec ^= 1;
#endif
    }
    return result;
}
int foundation_test_fsync(int file) {
    if (file != copy_fault.temporary_handle) return ::fsync(file);
    if (copy_fault.mode == CopyFault::FlushError) { copy_fault.action_ran = true; errno = EIO; return -1; }
    const auto result = ::fsync(file);
    if (result == 0 && copy_fault.mode == CopyFault::CancelFlush) { copy_fault.action_ran = true; copy_fault.cancellation->cancel(); }
    return result;
}
#endif

namespace {
void reset(CopyFault mode, const fs::path& source, const fs::path& destination, Cancellation& cancellation) {
    copy_fault = {};
    copy_fault.mode = mode;
    copy_fault.source = source;
    copy_fault.destination = destination;
    copy_fault.cancellation = &cancellation;
    copy_fault.source_size = fs::file_size(raw_native(source));
}
void test_faults(const Root& root) {
    const auto source = root.top / "source" / "pack.data";
    const std::string original(2 * 64 * 1024 + 37, 'q');
    raw_write(source, original);
    for (const auto& [mode, name, expected] : std::array<std::tuple<CopyFault, const char*, const char*>, 11>{{
        {CopyFault::DiskFull, "disk-full", "IoError"}, {CopyFault::ReadError, "read-error", "IoError"},
        {CopyFault::FlushError, "flush-error", "IoError"}, {CopyFault::ZeroWrite, "zero-write", "IoError"},
        {CopyFault::OversizedWrite, "oversized-write", "IoError"}, {CopyFault::EarlyEof, "early-eof", "Conflict"},
        {CopyFault::ExtraByte, "extra-byte", "Conflict"}, {CopyFault::RevisionEnd, "revision-at-end", "Conflict"},
        {CopyFault::CancelRead, "cancel-read", "Cancelled"}, {CopyFault::CancelWrite, "cancel-write", "Cancelled"},
        {CopyFault::CancelFlush, "cancel-flush", "Cancelled"}
    }}) {
        const auto destination = root.deep / name / "published";
        Cancellation cancel;
        reset(mode, source, destination, cancel);
        const auto code = failure([&] { (void)foundation_test_atomic_copy_no_replace(source, destination, original.size(), &cancel, {}); });
        if (code != expected) throw std::runtime_error(std::string(name) + ": expected=" + expected + " actual=" + code);
        require(copy_fault.action_ran, "Fault did not reach its injection boundary");
        require(copy_fault.source_read_only && copy_fault.temporary_private_exclusive, "Copy weakened source or private temporary contract");
        require(copy_fault.largest_read <= 64 * 1024 && copy_fault.largest_write <= 64 * 1024, "Copy used unbounded I/O chunks");
        require(!fs::exists(raw_native(destination)) && raw_read(source) == original, "Failed copy published or changed source");
        no_temporaries(destination.parent_path());
        std::cout << "PASS injected " << name << ": " << code << "; source intact; handle-owned cleanup\n";
    }
    for (const auto mode : {CopyFault::None, CopyFault::ShortIo, CopyFault::Contender, CopyFault::Mutator}) {
        const auto destination = root.deep / ("success-" + std::to_string(static_cast<int>(mode))) / "published";
        Cancellation cancel;
        reset(mode, source, destination, cancel);
#ifndef _WIN32
        if (mode == CopyFault::Mutator) {
            require(failure([&] { (void)foundation_test_atomic_copy_no_replace(source, destination, original.size(), &cancel, {}); }) == "Conflict",
                "POSIX same-size external mutation was not detected");
            require(raw_read(source) == "z" + original.substr(1) && !fs::exists(destination), "POSIX mutation case changed unrelated objects");
            no_temporaries(destination.parent_path());
            raw_write(source, original);
            std::cout << "PASS actual POSIX source mutation: Conflict and owned cleanup\n";
            continue;
        }
#endif
        const auto copied = foundation_test_atomic_copy_no_replace(source, destination, original.size(), &cancel, {});
        require(copied == (mode != CopyFault::Contender), "Copy contention return contract changed");
        require(raw_read(source) == original && raw_read(destination) == (mode == CopyFault::Contender ? "external-contender" : original),
            "Successful copy overwrote contender or consumed source");
        require(copy_fault.source_read_only && copy_fault.temporary_private_exclusive && copy_fault.read_bytes == original.size() &&
            copy_fault.written_bytes == original.size() && copy_fault.largest_read <= 64 * 1024 && copy_fault.largest_write <= 64 * 1024,
            "Copy did not use one retained source/private temporary with bounded exact I/O");
        if (mode == CopyFault::Mutator) require(copy_fault.action_ran && copy_fault.mutators_blocked, "Windows source/temp/ancestor write-delete mutator was admitted");
        if (mode == CopyFault::Contender) require(copy_fault.action_ran, "External contender fixture was not installed");
        no_temporaries(destination.parent_path());
        std::cout << "PASS copy mode " << static_cast<int>(mode) << ": partial I/O/contender/mutator; source retained\n";
    }
#ifdef _WIN32
    const auto destination = root.deep / "preexisting-writer" / "published";
    const auto writer = CreateFileW(raw_native(source).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(writer != INVALID_HANDLE_VALUE, "Existing source writer fixture unavailable");
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{writer};
    Cancellation cancel;
    reset(CopyFault::None, source, destination, cancel);
    (void)failure([&] { (void)foundation_test_atomic_copy_no_replace(source, destination, original.size(), &cancel, {}); });
    require(copy_fault.temporary_handle == -1 && !fs::exists(raw_native(destination)) && raw_read(source) == original,
        "Copy admitted a pre-existing writable source handle");
    std::cout << "PASS pre-existing Windows writer: fail-closed before destination creation\n";
#endif
}
}
int main() {
    try {
        std::cout << std::unitbuf;
        Root root;
        test_faults(root);
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL " << error.error().code << ": " << error.what() << " details=" << error.error().details.dump() << '\n';
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    return 1;
}
