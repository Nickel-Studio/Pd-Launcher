#include "core/config/path_guard.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"

#include <algorithm>
#include <cerrno>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include "core/config/windows_security.hpp"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nickel::foundation_detail {
namespace {
#ifdef _WIN32
bool valid_windows_component(const std::filesystem::path& path) {
    const auto text = path.native();
    if (text.empty() || text.back() == L'.' || text.back() == L' ' || text == L"..") return false;
    for (const auto c : text) {
        if (c < 32 || c == 127 || (c >= 0x80 && c <= 0x9F) || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) return false;
    }
    auto stem = text.substr(0, text.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    for (auto& c : stem) if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        stem == L"CLOCK$" || stem == L"CONIN$" || stem == L"CONOUT$") return false;
    if (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT"))) {
        const auto c = stem[3];
        if ((c >= L'1' && c <= L'9') || c == 0xB9 || c == 0xB2 || c == 0xB3) return false;
    }
    return true;
}
std::filesystem::path logical_windows_path(std::filesystem::path path) {
    path.make_preferred();
    const auto& text = path.native();
    if (text.starts_with(L"\\\\?\\")) {
        const auto tail = std::wstring_view(text).substr(4);
        if (tail.size() >= 4 && CompareStringOrdinal(tail.data(), 4, L"UNC\\", 4, TRUE) == CSTR_EQUAL) {
            return std::filesystem::path(L"\\\\" + std::wstring(tail.substr(4)));
        }
        if (tail.size() >= 3 && ((tail[0] >= L'A' && tail[0] <= L'Z') || (tail[0] >= L'a' && tail[0] <= L'z')) &&
            tail[1] == L':' && tail[2] == L'\\') return std::filesystem::path(tail);
        fail("UnsafePath", "Extended path is not a DOS drive or UNC share");
    }
    if (text.starts_with(L"\\\\.\\") || text.starts_with(L"\\??\\") || text.starts_with(L"\\\\??\\") ||
        text.starts_with(L"\\Device\\")) fail("UnsafePath", "Device namespace paths are not allowed");
    return path;
}
void validate_windows_root(const std::filesystem::path& path) {
    const auto root = path.root_name().native();
    if (root.empty()) return;
    if (root.size() == 2 && ((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')) &&
        root[1] == L':') return;
    if (root.starts_with(L"\\\\") && valid_windows_component(std::filesystem::path(root.substr(2)))) {
        const auto relative = path.relative_path();
        const auto first = relative.begin();
        if (first != relative.end() && valid_windows_component(*first)) return;
    }
    fail("UnsafePath", "Path root is not a DOS drive or UNC share");
}
struct Directory {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path path;
    BY_HANDLE_FILE_INFORMATION identity{};
    Directory(HANDLE h, std::filesystem::path p, const BY_HANDLE_FILE_INFORMATION& i)
        : handle(h), path(std::move(p)), identity(i) {}
    ~Directory() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    Directory(Directory&& other) noexcept : handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)),
        path(std::move(other.path)), identity(other.identity) {}
    Directory(const Directory&) = delete;
};
struct DirectoryAnchor {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~DirectoryAnchor() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
DWORD directory_sharing(DirectoryGuardMode mode) {
    return FILE_SHARE_READ | (mode == DirectoryGuardMode::NamespaceMutation ? FILE_SHARE_WRITE : 0);
}
BY_HANDLE_FILE_INFORMATION inspect_directory(HANDLE handle, const std::filesystem::path& path) {
    BY_HANDLE_FILE_INFORMATION identity{};
    if (!GetFileInformationByHandle(handle, &identity)) io_fail("inspect path ancestor", path, static_cast<int>(GetLastError()));
    if (!(identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        fail("UnsafePath", "Path ancestor is not a real directory");
    }
    return identity;
}
HANDLE open_relative_native(HANDLE parent, const std::filesystem::path& leaf, ULONG access,
    ULONG sharing, ULONG disposition, ULONG options, void* security = nullptr) {
    if (leaf.has_parent_path() || !valid_windows_component(leaf) || leaf == ".") {
        fail("UnsafePath", "Relative native I/O requires one safe leaf");
    }
    const auto& text = leaf.native();
    if (text.size() > 32766) fail("LimitExceeded", "Relative native name is too long");
    UNICODE_STRING name{};
    name.Length = static_cast<USHORT>(text.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    name.Buffer = const_cast<PWSTR>(text.c_str());
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &name;
    attributes.Attributes = 0x40 | 0x1000; // CASE_INSENSITIVE | DONT_REPARSE
    attributes.SecurityDescriptor = security;
    using Create = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
        PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using StatusToError = ULONG (WINAPI*)(NTSTATUS);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto create = reinterpret_cast<Create>(GetProcAddress(module, "NtCreateFile"));
    const auto status_to_error = reinterpret_cast<StatusToError>(GetProcAddress(module, "RtlNtStatusToDosError"));
    if (!create || !status_to_error) fail("Unsupported", "Native relative file I/O is not available");
    HANDLE handle = INVALID_HANDLE_VALUE;
    IO_STATUS_BLOCK io{};
    const auto status = create(&handle, access | SYNCHRONIZE, &attributes, &io, nullptr,
        FILE_ATTRIBUTE_NORMAL, sharing, disposition, options | 0x00200020, nullptr, 0);
    if (status < 0) {
        const auto error = status_to_error(status);
        if (error == ERROR_CANT_ACCESS_FILE || error == ERROR_STOPPED_ON_SYMLINK ||
            error == ERROR_REPARSE_TAG_INVALID || error == ERROR_REPARSE_TAG_MISMATCH) {
            fail("UnsafePath", "Relative native I/O encountered a redirect");
        }
        SetLastError(error);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}
Directory accept_directory(HANDLE handle, const std::filesystem::path& path) {
    if (handle == INVALID_HANDLE_VALUE) io_fail("pin path ancestor", path, static_cast<int>(GetLastError()));
    try { return Directory(handle, path, inspect_directory(handle, path)); }
    catch (...) { CloseHandle(handle); throw; }
}
Directory open_directory(const std::filesystem::path& path, DirectoryGuardMode mode = DirectoryGuardMode::ReadOnly) {
    const auto native = native_filesystem_path(path);
    // Attribute-only opens do not participate in Windows share-access checks.
    return accept_directory(CreateFileW(native.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        directory_sharing(mode), nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr), path);
}
Directory open_directory_relative(HANDLE parent, const std::filesystem::path& leaf,
    const std::filesystem::path& path, DirectoryGuardMode mode, bool create) {
    return accept_directory(open_relative_native(parent, leaf,
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        directory_sharing(mode), create ? 3 : 1, 0x1), path);
}
#else
struct Directory {
    int handle = -1;
    std::filesystem::path path;
    struct stat identity{};
    Directory(int h, std::filesystem::path p, const struct stat& i) : handle(h), path(std::move(p)), identity(i) {}
    ~Directory() { if (handle >= 0) ::close(handle); }
    Directory(Directory&& other) noexcept : handle(std::exchange(other.handle, -1)),
        path(std::move(other.path)), identity(other.identity) {}
    Directory(const Directory&) = delete;
};
Directory open_directory(int parent, const std::filesystem::path& name, const std::filesystem::path& path) {
    const auto handle = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (handle < 0) {
        const auto error = errno;
        // Linux reports ENOTDIR rather than ELOOP for O_DIRECTORY symlinks.
        if (error == ELOOP || error == ENOTDIR) fail("UnsafePath", "Path ancestor is not a real directory");
        io_fail("pin path ancestor", path, error);
    }
    struct stat identity{};
    if (::fstat(handle, &identity) != 0) { const auto error = errno; ::close(handle); io_fail("inspect path ancestor", path, error); }
    if (!S_ISDIR(identity.st_mode)) { ::close(handle); fail("UnsafePath", "Path ancestor is not a real directory"); }
    return Directory(handle, path, identity);
}
#endif
} // namespace

std::filesystem::path checked_absolute_path(const std::filesystem::path& path) {
    if (path.empty()) fail("InvalidArgument", "Path must not be empty");
    (void)path_from_utf8(path_to_utf8(path));
#ifdef _WIN32
    auto logical = logical_windows_path(path);
    validate_windows_root(logical);
#else
    const auto& logical = path;
#endif
    const auto validate_components = [](const std::filesystem::path& candidate) {
        for (const auto& item : candidate.relative_path()) {
            if (item == "." || item.empty()) continue;
            if (item == "..") fail("UnsafePath", "Path traversal components are not allowed");
#ifdef _WIN32
            if (!valid_windows_component(item)) fail("UnsafePath", "Windows path component is unsafe or reserved");
#endif
        }
    };
    // Validate before absolute() or extended-prefix conversion can normalize it.
    validate_components(logical);
    std::error_code error;
    auto absolute = std::filesystem::absolute(logical, error);
    if (error) io_fail("resolve path", path, error.value());
#ifdef _WIN32
    validate_windows_root(absolute);
#endif
    validate_components(absolute);
    return absolute.lexically_normal();
}
std::filesystem::path native_filesystem_path(const std::filesystem::path& path) {
    auto absolute = checked_absolute_path(path);
#ifdef _WIN32
    absolute.make_preferred();
    const auto& text = absolute.native();
    auto native = text.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + text.substr(2) : L"\\\\?\\" + text;
    if (native.size() >= 32767) fail("LimitExceeded", "Native filesystem path exceeds the Windows limit");
    return std::filesystem::path(native);
#else
    return absolute;
#endif
}

struct DirectoryGuard::Impl {
    std::filesystem::path directory;
    DirectoryGuardMode mode;
    std::vector<Directory> ancestors;
#ifdef _WIN32
    DirectoryAnchor anchor;
#endif
    Impl(const std::filesystem::path& path, bool create, DirectoryGuardMode requested_mode)
        : directory(checked_absolute_path(path)), mode(requested_mode) {
        auto current = directory.root_path();
        const auto relative = directory.relative_path();
        auto component = relative.begin();
#ifdef _WIN32
        // A UNC server is not an openable directory; the share is the first root.
        if (directory.root_name().native().starts_with(L"\\\\")) {
            current /= *component;
            ++component;
        }
        ancestors.push_back(open_directory(current, mode));
#else
        ancestors.push_back(open_directory(AT_FDCWD, current, current));
#endif
        for (; component != relative.end(); ++component) {
            const auto& item = *component;
            if (item.empty() || item == ".") continue;
            current /= item;
#ifdef _WIN32
            if (mode == DirectoryGuardMode::NamespaceMutation) {
                (void)inspect_directory(ancestors.back().handle, ancestors.back().path);
                ancestors.push_back(open_directory_relative(ancestors.back().handle, item, current, mode, create));
            } else {
                if (create) {
                    const auto native = native_filesystem_path(current);
                    if (!CreateDirectoryW(native.c_str(), nullptr)) {
                        const auto error = GetLastError();
                        if (error != ERROR_ALREADY_EXISTS) io_fail("create pinned directory", current, static_cast<int>(error));
                    }
                }
                ancestors.push_back(open_directory(current));
            }
#else
            if (create && ::mkdirat(ancestors.back().handle, item.c_str(), 0700) != 0 && errno != EEXIST) {
                io_fail("create pinned directory", current, errno);
            }
            ancestors.push_back(open_directory(ancestors.back().handle, item, current));
#endif
        }
#ifdef _WIN32
        if (mode == DirectoryGuardMode::NamespaceMutation) {
            PrivateSecurity security(true);
            anchor.handle = open_relative_native(ancestors.back().handle,
                path_from_utf8(".nickel-dir-anchor-" + unique_token()), GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_SHARE_READ, 2, 0x1040, security.descriptor);
            if (anchor.handle == INVALID_HANDLE_VALUE) io_fail("create directory mutation anchor", directory, static_cast<int>(GetLastError()));
            (void)inspect_directory(ancestors.back().handle, directory);
        }
#endif
    }
};

DirectoryGuard::DirectoryGuard(const std::filesystem::path& directory, bool create, DirectoryGuardMode mode)
    : impl_(std::make_unique<Impl>(directory, create, mode)) {}
DirectoryGuard::~DirectoryGuard() = default;
DirectoryGuard::DirectoryGuard(DirectoryGuard&&) noexcept = default;
DirectoryGuard& DirectoryGuard::operator=(DirectoryGuard&&) noexcept = default;
const std::filesystem::path& DirectoryGuard::path() const noexcept { return impl_->directory; }
std::intptr_t DirectoryGuard::native_handle() const noexcept {
#ifdef _WIN32
    return reinterpret_cast<std::intptr_t>(impl_->ancestors.back().handle);
#else
    return impl_->ancestors.back().handle;
#endif
}
DirectoryGuardMode DirectoryGuard::mode() const noexcept { return impl_->mode; }
void DirectoryGuard::verify() const {
    for (std::size_t index = 0; index < impl_->ancestors.size(); ++index) {
        const auto& directory = impl_->ancestors[index];
#ifdef _WIN32
        (void)inspect_directory(directory.handle, directory.path);
        const auto current = impl_->mode == DirectoryGuardMode::NamespaceMutation && index != 0
            ? open_directory_relative(impl_->ancestors[index - 1].handle, directory.path.filename(), directory.path, impl_->mode, false)
            : open_directory(directory.path, impl_->mode);
        const auto& a = current.identity;
        const auto& b = directory.identity;
        if (a.dwVolumeSerialNumber != b.dwVolumeSerialNumber || a.nFileIndexHigh != b.nFileIndexHigh || a.nFileIndexLow != b.nFileIndexLow) {
            fail("Conflict", "Pinned path ancestor changed identity");
        }
#else
        struct stat current{};
        if (::lstat(directory.path.c_str(), &current) != 0) io_fail("verify pinned path ancestor", directory.path, errno);
        if (!S_ISDIR(current.st_mode) || current.st_dev != directory.identity.st_dev || current.st_ino != directory.identity.st_ino) {
            fail("Conflict", "Pinned path ancestor changed identity");
        }
#endif
    }
}
#ifdef _WIN32
std::intptr_t windows_open_relative(const DirectoryGuard& parent, const std::filesystem::path& leaf,
    std::uint32_t access, std::uint32_t sharing, std::uint32_t disposition,
    std::uint32_t create_options, void* security_descriptor) {
    parent.verify();
    const auto handle = open_relative_native(reinterpret_cast<HANDLE>(parent.native_handle()), leaf,
        access, sharing, disposition, create_options, security_descriptor);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    try { parent.verify(); }
    catch (...) {
        if (disposition == 2) {
            FILE_DISPOSITION_INFO remove{TRUE};
            (void)SetFileInformationByHandle(handle, FileDispositionInfo, &remove, sizeof(remove));
        }
        CloseHandle(handle);
        throw;
    }
    return reinterpret_cast<std::intptr_t>(handle);
}
#endif

void reject_file_hardlinks(const std::filesystem::path& path) {
    const auto file = checked_absolute_path(path);
    if (path_missing(file)) return;
    DirectoryGuard parent(file.parent_path());
#ifdef _WIN32
    const auto native = native_filesystem_path(file);
    const auto handle = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) io_fail("inspect file identity", file, static_cast<int>(GetLastError()));
    BY_HANDLE_FILE_INFORMATION info{};
    const auto inspected = GetFileInformationByHandle(handle, &info);
    const auto error = GetLastError();
    CloseHandle(handle);
    if (!inspected) io_fail("inspect file identity", file, static_cast<int>(error));
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && info.nNumberOfLinks != 1)) {
        fail("UnsafePath", "File is a symbolic or hard link");
    }
#else
    struct stat info{};
    if (::fstatat(static_cast<int>(parent.native_handle()), file.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) io_fail("inspect file identity", file, errno);
    if (S_ISLNK(info.st_mode) || (S_ISREG(info.st_mode) && info.st_nlink != 1)) fail("UnsafePath", "File is a symbolic or hard link");
#endif
    parent.verify();
}

} // namespace nickel::foundation_detail
