#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"
#include "core/config/conditional_file.hpp"
#include "core/common/error.hpp"
#include <exception>
#include <optional>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <aclapi.h>
#include <sddl.h>
#include "core/config/windows_security.hpp"
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <stdio.h>
#endif
#endif

namespace nickel {
namespace {
using namespace foundation_detail;

#ifdef _WIN32
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};


using FileIdentity = BY_HANDLE_FILE_INFORMATION;
FileIdentity file_identity(HANDLE handle, const std::filesystem::path& path, bool allow_directory = false) {
    FileIdentity info{};
    if (!GetFileInformationByHandle(handle, &info)) io_fail("inspect file", path, static_cast<int>(GetLastError()));
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) fail("UnsafePath", "Path contains a reparse point");
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (!allow_directory) fail("InvalidArgument", "Expected a regular file");
    } else if (info.nNumberOfLinks != 1) {
        fail("UnsafePath", "File has multiple hard links");
    }
    if (GetFileType(handle) != FILE_TYPE_DISK) fail("InvalidArgument", "Expected a disk file");
    return info;
}
bool same_identity(const FileIdentity& a, const FileIdentity& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}
bool same_revision(const FileIdentity& a, const FileIdentity& b) {
    return same_identity(a, b) && a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
        a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
        a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime;
}
void check_regular(HANDLE handle, const std::filesystem::path& path) { (void)file_identity(handle, path); }
void write_chunks(HANDLE handle, const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        const auto length = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data(), length, &written, nullptr)) io_fail("write file", path, static_cast<int>(GetLastError()));
        if (!written || written > length) fail("IoError", "File write did not make valid progress");
        bytes = bytes.subspan(written);
    }
}
void flush_file(HANDLE handle, const std::filesystem::path& path) {
    if (!FlushFileBuffers(handle)) io_fail("flush file", path, static_cast<int>(GetLastError()));
}
void write_contents(HANDLE handle, const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    write_chunks(handle, path, bytes);
    flush_file(handle, path);
}

bool rename_handle(HANDLE handle, const DirectoryGuard& source_parent,
                   const DirectoryGuard& destination_parent, const std::filesystem::path& destination, bool replace) {
    source_parent.verify();
    destination_parent.verify();
    const auto source_identity = file_identity(reinterpret_cast<HANDLE>(source_parent.native_handle()), source_parent.path(), true);
    const auto destination_identity = file_identity(reinterpret_cast<HANDLE>(destination_parent.native_handle()), destination_parent.path(), true);
    if (!same_identity(source_identity, destination_identity)) {
        fail("Unsupported", "Cross-directory rename cannot preserve the Windows directory guard");
    }
    const auto name = destination.filename().native();
    const auto bytes = name.size() * sizeof(wchar_t);
    if (bytes > std::numeric_limits<ULONG>::max() - sizeof(FILE_RENAME_INFO)) {
        fail("LimitExceeded", "Rename destination is too long");
    }
    std::vector<std::uint8_t> storage(sizeof(FILE_RENAME_INFO) + bytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists = replace ? TRUE : FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(info->FileName, name.data(), bytes);
    using SetInformation = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    using StatusToError = ULONG (WINAPI*)(NTSTATUS);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto set_information = reinterpret_cast<SetInformation>(GetProcAddress(module, "NtSetInformationFile"));
    const auto status_to_error = reinterpret_cast<StatusToError>(GetProcAddress(module, "RtlNtStatusToDosError"));
    if (!set_information || !status_to_error) fail("Unsupported", "Native handle rename is not available");
    // A simple native name keeps the existing parent. Win32 and RootDirectory
    // forms reopen the destination for write, conflicting with the guard.
    IO_STATUS_BLOCK io{};
    const auto status = set_information(handle, &io, info, static_cast<ULONG>(storage.size()), static_cast<FILE_INFORMATION_CLASS>(10));
    if (status >= 0) return true;
    const auto error = status_to_error(status);
    if (!replace && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)) return false;
    io_fail("rename pinned file", destination, static_cast<int>(error));
}
void delete_handle(HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{TRUE};
    (void)SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
}
#else
struct Handle {
    int value = -1;
    explicit Handle(int h = -1) : value(h) {}
    ~Handle() { if (value >= 0) ::close(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
using FileIdentity = struct stat;
FileIdentity file_identity(int handle, const std::filesystem::path& path, bool allow_directory = false) {
    FileIdentity info{};
    if (::fstat(handle, &info) != 0) io_fail("inspect file", path, errno);
    if (!S_ISREG(info.st_mode) && !(allow_directory && S_ISDIR(info.st_mode))) fail("InvalidArgument", "Expected a regular file");
    if (S_ISREG(info.st_mode) && info.st_nlink != 1) fail("UnsafePath", "File has multiple hard links");
    return info;
}
bool same_identity(const FileIdentity& a, const FileIdentity& b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }
bool same_revision(const FileIdentity& a, const FileIdentity& b) {
#if defined(__APPLE__)
    const bool times_equal = a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
        a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
    const bool times_equal = a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
    return same_identity(a, b) && a.st_size == b.st_size && times_equal;
}
void check_regular(int handle, const std::filesystem::path& path) { (void)file_identity(handle, path); }
void write_chunks(int handle, const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        const auto request = std::min<std::size_t>(bytes.size(), 1024 * 1024);
        const auto count = ::write(handle, bytes.data(), request);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) io_fail("write file", path, errno);
        if (count == 0 || static_cast<std::size_t>(count) > request) fail("IoError", "File write did not make valid progress");
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
}
void flush_file(int handle, const std::filesystem::path& path) {
    if (::fsync(handle) != 0) io_fail("flush file", path, errno);
}
void write_contents(int handle, const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    write_chunks(handle, path, bytes);
    flush_file(handle, path);
}

bool reserve_relative(const DirectoryGuard& source_parent, const std::filesystem::path& source,
                      const DirectoryGuard& destination_parent, const std::filesystem::path& destination,
                      bool allow_link_fallback) {
    const auto source_fd = static_cast<int>(source_parent.native_handle());
    const auto destination_fd = static_cast<int>(destination_parent.native_handle());
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, source_fd, source.filename().c_str(), destination_fd, destination.filename().c_str(), 1) == 0) return true;
    if (errno == EEXIST) return false;
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) io_fail("reserve target", destination, errno);
#elif defined(__APPLE__)
    if (::renameatx_np(source_fd, source.filename().c_str(), destination_fd, destination.filename().c_str(), RENAME_EXCL) == 0) return true;
    if (errno == EEXIST) return false;
    if (errno != ENOTSUP && errno != EINVAL) io_fail("reserve target", destination, errno);
#endif
    if (!allow_link_fallback) fail("Unsupported", "Atomic no-replace rename is unavailable on this filesystem");
    struct stat info{};
    if (::fstatat(source_fd, source.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) io_fail("inspect reservation source", source, errno);
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1) fail("Unsupported", "Atomic directory reservation is unavailable");
    if (::linkat(source_fd, source.filename().c_str(), destination_fd, destination.filename().c_str(), 0) != 0) {
        if (errno == EEXIST) return false;
        io_fail("reserve target", destination, errno);
    }
    if (::unlinkat(source_fd, source.filename().c_str(), 0) != 0) io_fail("remove reserved source", source, errno);
    return true;
}
#endif

void sync_pinned_directory(const DirectoryGuard& directory) {
    directory.verify();
#ifndef _WIN32
    if (::fsync(static_cast<int>(directory.native_handle())) != 0 && errno != EINVAL && errno != ENOTSUP) {
        io_fail("flush directory", directory.path(), errno);
    }
#endif
}

struct CreatedFile {
    const DirectoryGuard& parent;
    std::filesystem::path path;
    Handle handle;
    FileIdentity identity{};
    bool owned = false;
    CreatedFile(const DirectoryGuard& directory, std::filesystem::path file, bool private_file)
        : parent(directory), path(std::move(file)), handle([&] {
#ifdef _WIN32
            PrivateSecurity security(private_file);
            if (parent.mode() == DirectoryGuardMode::NamespaceMutation) {
                if (checked_absolute_path(path).parent_path() != parent.path()) fail("UnsafePath", "Stage is not in its pinned parent");
                return reinterpret_cast<HANDLE>(windows_open_relative(parent, path.filename(),
                    GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, 2, 0x40, security.descriptor));
            }
            const auto native = native_filesystem_path(path);
            return CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                               security.get(), CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
#else
            return ::openat(static_cast<int>(parent.native_handle()), path.filename().c_str(),
                O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, private_file ? 0600 : 0666);
#endif
        }()) {
#ifdef _WIN32
        if (handle.value == INVALID_HANDLE_VALUE) io_fail("create file", path, static_cast<int>(GetLastError()));
#else
        if (handle.value < 0) io_fail("create file", path, errno);
#endif
        try { identity = file_identity(handle.value, path); owned = true; }
        catch (...) {
#ifdef _WIN32
            delete_handle(handle.value);
#endif
            throw;
        }
    }
    void verify_name() const {
        parent.verify();
#ifdef _WIN32
        if (!same_identity(identity, file_identity(handle.value, path))) fail("Conflict", "Created file changed identity");
#else
        struct stat current{};
        if (::fstatat(static_cast<int>(parent.native_handle()), path.filename().c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
            io_fail("verify created file", path, errno);
        }
        if (!S_ISREG(current.st_mode) || current.st_nlink != 1 || !same_identity(identity, current)) {
            fail("Conflict", "Created file was replaced externally");
        }
#endif
    }
    ~CreatedFile() {
        if (!owned) return;
#ifdef _WIN32
        delete_handle(handle.value);
#else
        struct stat current{};
        if (::fstatat(static_cast<int>(parent.native_handle()), path.filename().c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(current.st_mode) && same_identity(identity, current)) {
            (void)::unlinkat(static_cast<int>(parent.native_handle()), path.filename().c_str(), 0);
        }
#endif
    }
};

std::vector<std::uint8_t> read_contents(const Handle& handle, const std::filesystem::path& path, std::uint64_t limit) {
    if (limit > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() - 1)) {
        fail("LimitExceeded", "File size limit cannot be represented");
    }
    const auto before = file_identity(handle.value, path);
#ifdef _WIN32
    const auto expected = (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
#else
    if (before.st_size < 0) fail("CorruptData", "File length is invalid");
    const auto expected = static_cast<std::uint64_t>(before.st_size);
#endif
    if (expected > limit) fail("LimitExceeded", "File exceeds its size limit");
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(expected));
    std::array<std::uint8_t, 64 * 1024> buffer{};
    for (;;) {
        const auto request = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), limit + 1 - bytes.size()));
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(handle.value, buffer.data(), static_cast<DWORD>(request), &count, nullptr)) {
            io_fail("read file", path, static_cast<int>(GetLastError()));
        }
#else
        const auto count = ::read(handle.value, buffer.data(), request);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) io_fail("read file", path, errno);
#endif
        if (count == 0) break;
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
        if (bytes.size() > limit) fail("LimitExceeded", "File grew beyond its size limit");
    }
    if (bytes.size() != expected || !same_revision(before, file_identity(handle.value, path))) {
        fail("Conflict", "File changed while it was being read");
    }
    return bytes;
}

bool publish_created_file(CreatedFile& temp, const std::filesystem::path& file, bool replace,
                          const Cancellation* cancellation = nullptr) {
    temp.verify_name();
    reject_path_links(file, false);
    reject_file_hardlinks(file);
    check_cancel(cancellation);
    const auto& parent = temp.parent;
#ifdef _WIN32
    if (!rename_handle(temp.handle.value, parent, parent, file, replace)) return false;
    temp.owned = false;
    flush_file(temp.handle.value, file);
#else
    if (replace) {
        if (::renameat(static_cast<int>(parent.native_handle()), temp.path.filename().c_str(),
                       static_cast<int>(parent.native_handle()), file.filename().c_str()) != 0) io_fail("replace file", file, errno);
    } else if (!reserve_relative(parent, temp.path, parent, file, false)) {
        return false;
    }
    temp.owned = false;
#endif
    sync_pinned_directory(parent);
    return true;
}
bool write_atomically(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                      const AtomicWriteOptions& options, bool replace) {
    check_cancel(options.cancellation);
    const auto file = checked_absolute_path(path);
    DirectoryGuard parent(file.parent_path(), options.create_parents);
    reject_path_links(file, false);
    reject_file_hardlinks(file);
    CreatedFile temp(parent, parent.path() / path_from_utf8("." + path_to_utf8(file.filename()) + ".tmp-" + unique_token()), options.private_file);
    write_contents(temp.handle.value, temp.path, bytes);
    return publish_created_file(temp, file, replace, options.cancellation);
}

} // namespace

bool valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        std::uint32_t code = 0;
        std::uint32_t minimum = 0;
        unsigned remaining = 0;
        if (first >= 0xC2 && first <= 0xDF) { code = first & 0x1F; remaining = 1; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { code = first & 0x0F; remaining = 2; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { code = first & 0x07; remaining = 3; minimum = 0x10000; }
        else return false;
        if (text.size() - i < remaining) return false;
        for (unsigned n = 0; n < remaining; ++n) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    }
    return true;
}

std::filesystem::path path_from_utf8(std::string_view text) {
    if (!valid_utf8(text) || text.find('\0') != std::string_view::npos) {
        fail("InvalidArgument", "Path is not valid UTF-8");
    }
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

void reject_path_links(const std::filesystem::path& path, bool ancestors) {
    auto candidate = checked_absolute_path(path);
    for (;;) {
#ifdef _WIN32
        const auto native = native_filesystem_path(candidate);
        const auto attributes = GetFileAttributesW(native.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) io_fail("inspect path", candidate, static_cast<int>(code));
        } else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            fail("UnsafePath", "Path contains a reparse point; select a real directory");
        }
#else
        struct stat info{};
        if (::lstat(candidate.c_str(), &info) != 0) {
            if (errno != ENOENT && errno != ENOTDIR) io_fail("inspect path", candidate, errno);
        } else if (S_ISLNK(info.st_mode)) {
            fail("UnsafePath", "Path contains a symbolic link; select a real directory");
        }
#endif
        if (!ancestors || !candidate.has_relative_path()) break;
#ifdef _WIN32
        if (candidate.root_name().native().starts_with(L"\\\\")) {
            const auto relative = candidate.relative_path();
            if (std::distance(relative.begin(), relative.end()) == 1) break;
        }
#endif
        const auto parent = candidate.parent_path();
        if (parent.empty() || parent == candidate) break;
        candidate = parent;
    }
}

std::vector<std::uint8_t> read_file_bounded(const std::filesystem::path& path, std::uint64_t limit) {
    const auto file = checked_absolute_path(path);
    DirectoryGuard parent(file.parent_path());
#ifdef _WIN32
    const auto native = native_filesystem_path(file);
    Handle handle(CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) io_fail("read file", file, static_cast<int>(GetLastError()));
#else
    Handle handle(::openat(static_cast<int>(parent.native_handle()), file.filename().c_str(),
                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (handle.value < 0) io_fail("read file", file, errno);
#endif
    auto bytes = read_contents(handle, file, limit);
    parent.verify();
    return bytes;
}

std::string read_text_bounded(const std::filesystem::path& path, std::uint64_t limit) {
    const auto bytes = read_file_bounded(path, limit);
    std::string text(bytes.begin(), bytes.end());
    if (!valid_utf8(text)) fail("CorruptData", "File text is not valid UTF-8");
    return text;
}

std::string unique_token() {
    std::array<std::uint8_t, 16> bytes{};
    random_bytes(bytes);
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(32);
    for (auto byte : bytes) { output.push_back(hex[byte >> 4]); output.push_back(hex[byte & 15]); }
    return output;
}

void create_file_exclusive(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                           bool private_file) {
    const auto file = checked_absolute_path(path);
    DirectoryGuard parent(file.parent_path());
    CreatedFile created(parent, file, private_file);
    write_contents(created.handle.value, file, bytes);
    created.verify_name();
    sync_pinned_directory(parent);
    created.owned = false;
}

void atomic_write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                       const AtomicWriteOptions& options) {
    (void)write_atomically(path, bytes, options, true);
}

void atomic_write_text(const std::filesystem::path& path, std::string_view text,
                       const AtomicWriteOptions& options) {
    atomic_write_file(path, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, options);
}

bool atomic_copy_no_replace(const std::filesystem::path& source, const std::filesystem::path& destination,
                            std::uint64_t max_bytes, const Cancellation* cancellation,
                            std::string_view expected_sha256) {
    check_cancel(cancellation);
    if (!expected_sha256.empty() && (expected_sha256.size() != 64 ||
        !std::all_of(expected_sha256.begin(), expected_sha256.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }))) fail("InvalidArgument", "Expected SHA256 must contain 64 hexadecimal characters");
    std::string expected_hash(expected_sha256);
    for (auto& c : expected_hash) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    const auto from = checked_absolute_path(source);
    const auto to = checked_absolute_path(destination);
    DirectoryGuard source_parent(from.parent_path());
    reject_path_links(from, false);
#ifdef _WIN32
    const auto native = native_filesystem_path(from);
    Handle input(CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (input.value == INVALID_HANDLE_VALUE) io_fail("open copy source", from, static_cast<int>(GetLastError()));
#else
    Handle input(::openat(static_cast<int>(source_parent.native_handle()), from.filename().c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (input.value < 0) io_fail("open copy source", from, errno);
#endif
    const auto original = file_identity(input.value, from);
#ifdef _WIN32
    const auto expected_size = (static_cast<std::uint64_t>(original.nFileSizeHigh) << 32) | original.nFileSizeLow;
#else
    if (original.st_size < 0) fail("CorruptData", "Copy source length is invalid");
    const auto expected_size = static_cast<std::uint64_t>(original.st_size);
#endif
    if (expected_size > max_bytes) fail("LimitExceeded", "Copy source exceeds its size limit");
    const auto verify_source = [&] {
        source_parent.verify();
        if (!same_revision(original, file_identity(input.value, from))) fail("Conflict", "Copy source changed while it was being read");
#ifndef _WIN32
        struct stat named{};
        if (::fstatat(static_cast<int>(source_parent.native_handle()), from.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(named.st_mode) || named.st_nlink != 1 || !same_revision(original, named)) {
            fail("Conflict", "Copy source was replaced externally");
        }
#endif
    };
    verify_source();
    DirectoryGuard destination_parent(to.parent_path(), true);
    reject_path_links(to, false);
    reject_file_hardlinks(to);
    if (!path_missing(to)) return false;
    check_cancel(cancellation);
    CreatedFile temp(destination_parent, destination_parent.path() / (".nickel-copy-" + unique_token()), true);
    std::array<std::uint8_t, 64 * 1024> buffer{};
    struct Erase { std::span<std::uint8_t> bytes; ~Erase() { erase_bytes(bytes); } } erase{buffer};
    const auto read_chunk = [&](std::span<std::uint8_t> output) -> std::size_t {
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(input.value, output.data(), static_cast<DWORD>(output.size()), &count, nullptr)) {
            io_fail("read copy source", from, static_cast<int>(GetLastError()));
        }
        if (count > output.size()) fail("IoError", "Copy source returned an invalid read count");
        return static_cast<std::size_t>(count);
#else
        for (;;) {
            const auto count = ::read(input.value, output.data(), output.size());
            if (count < 0 && errno == EINTR) { check_cancel(cancellation); continue; }
            if (count < 0) io_fail("read copy source", from, errno);
            if (static_cast<std::size_t>(count) > output.size()) fail("IoError", "Copy source returned an invalid read count");
            return static_cast<std::size_t>(count);
        }
#endif
    };
    Sha256 hash;
    std::uint64_t copied = 0;
    while (copied < expected_size) {
        check_cancel(cancellation);
        const auto request = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), expected_size - copied));
        const auto count = read_chunk(std::span(buffer).first(request));
        if (count == 0) fail("Conflict", "Copy source ended before its original size");
        check_cancel(cancellation);
        const auto bytes = std::span<const std::uint8_t>(buffer).first(count);
        hash.update(bytes);
        write_chunks(temp.handle.value, temp.path, bytes);
        copied += count;
    }
    check_cancel(cancellation);
    if (read_chunk(std::span(buffer).first(1)) != 0) fail("Conflict", "Copy source grew while it was being read");
    verify_source();
    const auto digest = hash.finish();
    if (!expected_hash.empty() && digest != expected_hash) fail("CorruptData", "Copy source does not match its expected SHA256");
    flush_file(temp.handle.value, temp.path);
    verify_source();
    return publish_created_file(temp, to, false, cancellation);
}

bool rename_no_replace(const std::filesystem::path& source, const std::filesystem::path& destination) {
    const auto from = checked_absolute_path(source);
    const auto to = checked_absolute_path(destination);
    DirectoryGuard source_parent(from.parent_path());
    DirectoryGuard destination_parent(to.parent_path());
    reject_path_links(from, false);
    reject_path_links(to, false);
    reject_file_hardlinks(from);
    reject_file_hardlinks(to);
#ifdef _WIN32
    const auto native = native_filesystem_path(from);
    Handle handle(CreateFileW(native.c_str(), DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) io_fail("open reservation source", from, static_cast<int>(GetLastError()));
    (void)file_identity(handle.value, from, true);
    if (!rename_handle(handle.value, source_parent, destination_parent, to, false)) return false;
#else
    if (!reserve_relative(source_parent, from, destination_parent, to, true)) return false;
#endif
    sync_pinned_directory(source_parent);
    sync_pinned_directory(destination_parent);
    return true;
}

void sync_directory(const std::filesystem::path& directory) {
    DirectoryGuard pinned(directory.empty() ? std::filesystem::path(".") : directory);
    sync_pinned_directory(pinned);
}

struct FileLock::Impl {
    DirectoryGuard parent;
    std::filesystem::path path;
    Handle handle;
#ifdef _WIN32
    OVERLAPPED overlapped{};
#endif
    bool locked = false;
    Impl(const std::filesystem::path& file, const Cancellation* cancellation, std::chrono::milliseconds timeout,
         DirectoryGuardMode mode = DirectoryGuardMode::ReadOnly)
        : parent(checked_absolute_path(file).parent_path(), true, mode), path(parent.path() / file.filename()), handle([&] {
#ifdef _WIN32
            PrivateSecurity security(true);
            if (parent.mode() == DirectoryGuardMode::NamespaceMutation) {
                return reinterpret_cast<HANDLE>(windows_open_relative(parent, path.filename(),
                    GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 3, 0x40, security.descriptor));
            }
            const auto native = native_filesystem_path(path);
            return CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                security.get(), OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
#else
            return ::openat(static_cast<int>(parent.native_handle()), path.filename().c_str(),
                            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
#endif
        }()) {
#ifdef _WIN32
        if (handle.value == INVALID_HANDLE_VALUE) io_fail("open transaction lock", path, static_cast<int>(GetLastError()));
#else
        if (handle.value < 0) io_fail("open transaction lock", path, errno);
#endif
        const auto original = file_identity(handle.value, path);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            check_cancel(cancellation);
#ifdef _WIN32
            if (LockFileEx(handle.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) { locked = true; break; }
            const auto code = GetLastError();
            if (code != ERROR_LOCK_VIOLATION && code != ERROR_IO_PENDING) io_fail("acquire transaction lock", path, static_cast<int>(code));
#else
            if (::flock(handle.value, LOCK_EX | LOCK_NB) == 0) { locked = true; break; }
            if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) io_fail("acquire transaction lock", path, errno);
#endif
            if (std::chrono::steady_clock::now() >= deadline) fail("Busy", "Configuration is locked by another operation");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        try {
            parent.verify();
            if (!same_identity(original, file_identity(handle.value, path))) fail("Conflict", "Transaction lock changed identity");
#ifndef _WIN32
            struct stat named{};
            if (::fstatat(static_cast<int>(parent.native_handle()), path.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(named.st_mode) || !same_identity(original, named)) fail("Conflict", "Transaction lock was replaced externally");
#endif
        } catch (...) {
            unlock();
            throw;
        }
    }
    void unlock() noexcept {
        if (!locked) return;
#ifdef _WIN32
        (void)UnlockFileEx(handle.value, 0, 1, 0, &overlapped);
#else
        (void)::flock(handle.value, LOCK_UN);
#endif
        locked = false;
    }
    ~Impl() { unlock(); }
};

FileLock::FileLock(const std::filesystem::path& path, const Cancellation* cancellation,
                    std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) fail("InvalidArgument", "Lock timeout must not be negative");
    check_cancel(cancellation);
    (void)checked_absolute_path(path);
    impl_ = std::make_unique<Impl>(path, cancellation, timeout);
}
FileLock::FileLock(const std::filesystem::path& path, const Cancellation* cancellation,
                    std::chrono::milliseconds timeout, DirectoryGuardMode mode) {
    if (timeout.count() < 0) fail("InvalidArgument", "Lock timeout must not be negative");
    check_cancel(cancellation);
    (void)checked_absolute_path(path);
    impl_ = std::make_unique<Impl>(path, cancellation, timeout, mode);
}
FileLock::~FileLock() = default;
FileLock::FileLock(FileLock&&) noexcept = default;
FileLock& FileLock::operator=(FileLock&&) noexcept = default;

namespace foundation_detail {
bool atomic_create_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, bool private_file) {
    return write_atomically(path, bytes, {.private_file = private_file, .create_parents = false}, false);
}

bool move_file_if_matches(const std::filesystem::path& source, const std::filesystem::path& destination,
                          std::string_view expected_sha256) {
    const auto from = checked_absolute_path(source);
    const auto to = checked_absolute_path(destination);
    DirectoryGuard source_parent(from.parent_path());
    DirectoryGuard destination_parent(to.parent_path());
#ifdef _WIN32
    const auto native = native_filesystem_path(from);
    Handle handle(CreateFileW(native.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return false;
        io_fail("open conditional move source", from, static_cast<int>(error));
    }
#else
    Handle handle(::openat(static_cast<int>(source_parent.native_handle()), from.filename().c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (handle.value < 0) {
        if (errno == ENOENT) return false;
        io_fail("open conditional move source", from, errno);
    }
#endif
    const auto identity = file_identity(handle.value, from);
    auto bytes = read_contents(handle, from, 8 * 1024 * 1024);
    struct Erase { std::vector<std::uint8_t>& bytes; ~Erase() { erase_bytes(bytes); } } erase{bytes};
    if (sha256_text(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())) != expected_sha256) return false;
    source_parent.verify();
    destination_parent.verify();
    reject_path_links(to, false);
#ifdef _WIN32
    if (!same_identity(identity, file_identity(handle.value, from))) return false;
    if (!rename_handle(handle.value, source_parent, destination_parent, to, false)) return false;
#else
    struct stat named{};
    if (::fstatat(static_cast<int>(source_parent.native_handle()), from.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(named.st_mode) || !same_identity(identity, named)) return false;
    if (!reserve_relative(source_parent, from, destination_parent, to, true)) return false;
    if (::fstatat(static_cast<int>(destination_parent.native_handle()), to.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_identity(identity, named)) fail("Conflict", "Conditional move source was replaced; destination was preserved");
#endif
    sync_pinned_directory(source_parent);
    sync_pinned_directory(destination_parent);
    return true;
}

bool remove_file_if_matches(const std::filesystem::path& path, std::string_view expected_sha256) {
    const auto file = checked_absolute_path(path);
    DirectoryGuard parent(file.parent_path());
#ifdef _WIN32
    const auto native = native_filesystem_path(file);
    Handle handle(CreateFileW(native.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return true;
        io_fail("open owned file for removal", file, static_cast<int>(error));
    }
#else
    Handle handle(::openat(static_cast<int>(parent.native_handle()), file.filename().c_str(),
                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (handle.value < 0) {
        if (errno == ENOENT) return true;
        io_fail("open owned file for removal", file, errno);
    }
#endif
    const auto identity = file_identity(handle.value, file);
    auto bytes = read_contents(handle, file, 8 * 1024 * 1024);
    struct Erase { std::vector<std::uint8_t>& bytes; ~Erase() { erase_bytes(bytes); } } erase{bytes};
    const auto view = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (sha256_text(view) != expected_sha256) return false;
    parent.verify();
#ifdef _WIN32
    if (!same_identity(identity, file_identity(handle.value, file))) return false;
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(handle.value, FileDispositionInfo, &disposition, sizeof(disposition))) {
        io_fail("remove owned file", file, static_cast<int>(GetLastError()));
    }
#else
    struct stat named{};
    if (::fstatat(static_cast<int>(parent.native_handle()), file.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return true;
        io_fail("verify owned file for removal", file, errno);
    }
    if (!S_ISREG(named.st_mode) || !same_identity(identity, named)) return false;
    if (::unlinkat(static_cast<int>(parent.native_handle()), file.filename().c_str(), 0) != 0) io_fail("remove owned file", file, errno);
#endif
    return true;
}
} // namespace foundation_detail

} // namespace nickel

// Shared translation unit keeps conditional recovery on the same pinned helpers.
#ifndef NICKEL_SKIP_CONDITIONAL_IMPLEMENTATION
#include "core/config/conditional_file_impl.inc"
#endif
