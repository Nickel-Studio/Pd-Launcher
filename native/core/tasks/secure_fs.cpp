#include "core/tasks/secure_fs.hpp"
#include "core/network/control.hpp"
#include "core/network/errors.hpp"
#include "core/network/hash.hpp"
#include "core/tasks/test_hooks.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winternl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

namespace nickel::tasks::detail {
namespace {
[[noreturn]] void os_error(const char* operation) {
#ifdef _WIN32
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(), operation);
#else
    throw std::system_error(errno, std::generic_category(), operation);
#endif
}

#ifdef _WIN32
using NativeHandle = HANDLE;
const NativeHandle invalid_handle = INVALID_HANDLE_VALUE;
void close_handle(NativeHandle value) noexcept {
    if (value != invalid_handle) CloseHandle(value);
}
BY_HANDLE_FILE_INFORMATION info(NativeHandle handle) {
    BY_HANDLE_FILE_INFORMATION out{};
    if (!GetFileInformationByHandle(handle, &out)) os_error("file information");
    if (out.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw std::runtime_error("reparse points are not permitted");
    return out;
}
FileIdentity identity_of(NativeHandle handle) {
    const auto value = info(handle);
    return {value.dwVolumeSerialNumber,
            (static_cast<std::uint64_t>(value.nFileIndexHigh) << 32) |
                value.nFileIndexLow};
}
void rename_native(NativeHandle handle, const ParentGuard& source_parent,
                   const ParentGuard& destination_parent,
                   const std::filesystem::path& leaf, bool replace) {
    if (source_parent.mode() != GuardMode::NamespaceMutation ||
        destination_parent.mode() != GuardMode::NamespaceMutation)
        throw std::runtime_error("namespace rename requires canonical mutation guards");
    source_parent.verify(); destination_parent.verify();
    const auto name = leaf.native();
    if (name.empty() || leaf != leaf.filename() || name == L"." || name == L"..")
        throw std::runtime_error("namespace rename requires one destination leaf");
    const auto bytes = name.size() * sizeof(wchar_t);
    if (bytes > std::numeric_limits<ULONG>::max() - sizeof(FILE_RENAME_INFO))
        throw std::runtime_error("rename destination name too long");
    std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + bytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = replace ? TRUE : FALSE;
    const auto from = reinterpret_cast<NativeHandle>(source_parent.native_handle());
    const auto to = reinterpret_cast<NativeHandle>(destination_parent.native_handle());
    // Win32 rewrites this form; native rename keeps the fixed parent and leaf.
    rename->RootDirectory = identity_of(from) == identity_of(to) ? nullptr : to;
    rename->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(rename->FileName, name.data(), bytes);
    using SetInformation = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    using StatusToError = ULONG (WINAPI*)(NTSTATUS);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto set_information = reinterpret_cast<SetInformation>(GetProcAddress(module, "NtSetInformationFile"));
    const auto status_to_error = reinterpret_cast<StatusToError>(GetProcAddress(module, "RtlNtStatusToDosError"));
    if (!set_information || !status_to_error)
        transport::detail::fail("unsupported_operation", "Native handle rename is unavailable");
    IO_STATUS_BLOCK result{};
    const auto status = set_information(handle, &result, rename, static_cast<ULONG>(storage.size()),
                                        static_cast<FILE_INFORMATION_CLASS>(10));
    if (status < 0)
        throw std::system_error(static_cast<int>(status_to_error(status)), std::system_category(),
                                "rename retained handle");
}
std::wstring native_path(const std::filesystem::path& path) {
    // Public paths remain ordinary drive paths; only the Win32 syscall receives the long-path prefix.
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    auto text = absolute.native();
    if (text.size() < 3 || text[1] != L':' || text[2] != L'\\' || text.size() > 32760)
        throw std::runtime_error("invalid native drive path");
    return L"\\\\?\\" + text;
}
#else
using NativeHandle = int;
constexpr NativeHandle invalid_handle = -1;
void close_handle(NativeHandle value) noexcept {
    if (value != invalid_handle) ::close(value);
}
struct stat info(NativeHandle handle) {
    struct stat out{};
    if (::fstat(handle, &out)) os_error("file information");
    return out;
}
FileIdentity identity_of(NativeHandle handle) {
    const auto value = info(handle);
    return {static_cast<std::uint64_t>(value.st_dev),
            static_cast<std::uint64_t>(value.st_ino)};
}
NativeHandle open_directory(const std::filesystem::path& path) {
    auto handle = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (handle == invalid_handle) os_error("open directory");
    return handle;
}
#endif
FileRevision revision_of(NativeHandle handle) {
#ifdef _WIN32
    FILE_BASIC_INFO value{};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &value, sizeof(value)))
        os_error("file revision");
    return {static_cast<std::uint64_t>(value.LastWriteTime.QuadPart),
            static_cast<std::uint64_t>(value.ChangeTime.QuadPart)};
#else
    const auto value = info(handle);
#ifdef __APPLE__
    const auto modified = value.st_mtimespec, changed = value.st_ctimespec;
#else
    const auto modified = value.st_mtim, changed = value.st_ctim;
#endif
    return {static_cast<std::uint64_t>(modified.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(modified.tv_nsec),
            static_cast<std::uint64_t>(changed.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(changed.tv_nsec)};
#endif
}
void require_regular(NativeHandle handle) {
    const auto value = info(handle);
#ifdef _WIN32
    if (GetFileType(handle) != FILE_TYPE_DISK || (value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        value.nNumberOfLinks != 1)
#else
    if (!S_ISREG(value.st_mode) || value.st_nlink != 1)
#endif
        transport::detail::fail("revision_conflict", "An exclusively linked regular file is required");
}
bool beneath(const std::filesystem::path& root, const std::filesystem::path& file) {
    auto parent = root.begin(), child = file.begin();
    for (; parent != root.end(); ++parent, ++child) {
        if (child == file.end()) return false;
        // On byte-exact volumes case_key is the identity (byte-exact comparison); on
        // case-folding volumes it folds, so containment follows the host name matching.
        // Inode identity checks remain the authorization authority.
        if (case_key(path_utf8(*parent)) != case_key(path_utf8(*child))) return false;
    }
    return child != file.end();
}
std::string regular_digest(SecureFile& file, std::uint64_t expected_size,
                           const std::function<bool()>& cancelled) {
    const auto before = file.revision();
    if (file.size() != expected_size) transport::detail::fail("revision_conflict", "File size changed");
    transport::detail::IncrementalHash hash(transport::HashAlgorithm::Sha256);
    std::array<std::byte, 64 * 1024> bytes;
    std::uint64_t total{};
    file.seek(0);
    for (;;) {
        transport::detail::check_cancel(cancelled);
        const auto count = file.read(bytes);
        if (!count) break;
        if (count > expected_size - total) transport::detail::fail("revision_conflict", "File changed while hashing");
        total += count; hash.update(std::span(bytes).first(count));
        NICKEL_TRANSPORT_CHECKPOINT("regular.hash", file.path());
    }
    transport::detail::check_cancel(cancelled);
    if (total != expected_size || file.size() != expected_size || file.revision() != before)
        transport::detail::fail("revision_conflict", "File changed while hashing");
    file.seek(0);
    return hash.finish();
}
#ifndef _WIN32
void rename_at_no_replace(int from_parent, const std::filesystem::path& from,
                          int to_parent, const std::filesystem::path& to) {
#ifdef __linux__
    if (::syscall(SYS_renameat2, from_parent, from.c_str(), to_parent, to.c_str(), 1))
        os_error("relative no-replace rename");
#elif defined(__APPLE__)
    if (::renameatx_np(from_parent, from.c_str(), to_parent, to.c_str(), RENAME_EXCL))
        os_error("relative no-replace rename");
#else
    transport::detail::fail("unsupported", "Atomic no-replace quarantine is unavailable");
#endif
}
struct Quarantine {
    int parent;
    std::filesystem::path name;
    NativeHandle handle{invalid_handle};
    FileIdentity identity;
    bool occupied{};
    explicit Quarantine(int parent_handle) : parent(parent_handle), name(utf8_path(".nickel-delete-" + unique_id())) {
        if (::mkdirat(parent, name.c_str(), 0700)) os_error("create private deletion quarantine");
        handle = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (handle == invalid_handle) os_error("pin deletion quarantine");
        try {
            const auto value = info(handle);
            if (!S_ISDIR(value.st_mode) || value.st_uid != ::geteuid() || (value.st_mode & 077))
                transport::detail::fail("revision_conflict", "Deletion quarantine is not private");
            identity = identity_of(handle);
        } catch (...) { close_handle(handle); handle = invalid_handle; throw; }
    }
    ~Quarantine() {
        if (!occupied && handle != invalid_handle) {
            struct stat value{};
            if (!::fstatat(parent, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) && S_ISDIR(value.st_mode) &&
                FileIdentity{static_cast<std::uint64_t>(value.st_dev), static_cast<std::uint64_t>(value.st_ino)} == identity)
                (void)::unlinkat(parent, name.c_str(), AT_REMOVEDIR);
        }
        close_handle(handle);
    }
    Quarantine(const Quarantine&) = delete;
    Quarantine& operator=(const Quarantine&) = delete;
};
#endif

void check_utf8(std::string_view value) {
    for (std::size_t at = 0; at < value.size();) {
        const auto first = static_cast<unsigned char>(value[at++]);
        if (first < 0x80) continue;
        unsigned count{};
        std::uint32_t code{};
        if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 0x07; }
        else throw std::runtime_error("invalid UTF-8 path");
        if (value.size() - at < count) throw std::runtime_error("truncated UTF-8 path");
        for (unsigned index = 0; index < count; ++index) {
            const auto next = static_cast<unsigned char>(value[at++]);
            if ((next & 0xc0) != 0x80) throw std::runtime_error("invalid UTF-8 path");
            code = (code << 6) | (next & 0x3f);
        }
        if ((count == 1 && code < 0x80) || (count == 2 && code < 0x800) ||
            (count == 3 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff) || code == 0x2028 ||
            code == 0x2029 || (code >= 0x202a && code <= 0x202e) ||
            (code >= 0x2066 && code <= 0x2069))
            throw std::runtime_error("non-canonical UTF-8 path");
    }
}

std::vector<std::filesystem::path> ancestors(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> result;
    for (auto current = path; !current.empty();) {
        result.push_back(current);
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

[[maybe_unused]] std::string fold_ascii(std::string_view text) {
    std::string out(text);
    for (auto& byte : out)
        if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte - 'A' + 'a');
    return out;
}

// Name matching is a property of the volume holding the paths, not of the OS.
// Windows volumes fold case (per-directory NTFS case sensitivity stays out of
// scope); POSIX volumes are probed at runtime: two exclusive probe names that
// differ only in one byte's case can coexist on a case-sensitive volume (Linux
// ext4, case-sensitive APFS) and collide on a case-folding volume (macOS APFS
// default). Verdicts are cached per volume identity; probe failures keep the
// previous byte-exact POSIX behavior.
std::atomic<int>& case_volume_override() {
    static std::atomic<int> state{0};  // 0 = probe the host, 1 = byte-exact, 2 = folding
    return state;
}
#ifndef _WIN32
bool probe_volume_by_files(const std::filesystem::path& directory) {
    const auto tag = unique_id();
    const auto lower = directory / utf8_path("nickel-case-probe-" + tag + "-a");
    const auto upper = directory / utf8_path("nickel-case-probe-" + tag + "-A");
    const int first = ::open(lower.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (first < 0) os_error("case sensitivity probe");
    const int second = ::open(upper.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    const int collision = errno;
    if (second >= 0) close_handle(second);
    close_handle(first);
    (void)::unlink(lower.c_str());
    (void)::unlink(upper.c_str());
    if (second >= 0) return true;
    if (collision == EEXIST) return false;
    os_error("case sensitivity probe");
}

std::optional<bool> probe_volume_by_pathconf(const std::filesystem::path& directory) {
#if defined(_PC_CASE_SENSITIVE)
    const auto value = ::pathconf(directory.c_str(), _PC_CASE_SENSITIVE);
    if (value < 0) return std::nullopt;
    return value > 0;
#else
    (void)directory;
    return std::nullopt;
#endif
}

bool decide_volume_case(const std::filesystem::path& directory) {
    try {
        return probe_volume_by_files(directory);
    } catch (...) {
        if (const auto verdict = probe_volume_by_pathconf(directory)) return *verdict;
        return true;  // probe unavailable: keep the previous byte-exact POSIX behavior
    }
}
#endif
void remove_tree_checked(const std::filesystem::path& path,
                         const FileIdentity* authorized = nullptr) {
    reject_links(path);
#ifdef _WIN32
    ParentGuard parent(path.parent_path(), false, GuardMode::NamespaceMutation);
    const auto open_leaf = [&](std::uint32_t access) {
        const auto handle = reinterpret_cast<NativeHandle>(foundation_detail::windows_open_relative(
            parent, path.filename(), access, FILE_SHARE_READ, 1, 0));
        if (handle == invalid_handle) os_error("open owned removal entry");
        return handle;
    };
    const auto inspected = open_leaf(FILE_READ_ATTRIBUTES);
    FileIdentity expected;
    bool directory{};
    try {
        const auto status = info(inspected); expected = identity_of(inspected);
        if (authorized && expected != *authorized)
            throw std::runtime_error("owned removal entry changed identity");
        directory = (status.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!directory) require_regular(inspected);
    } catch (...) { close_handle(inspected); throw; }
    close_handle(inspected);
    if (directory) {
        std::vector<std::pair<std::filesystem::path, FileIdentity>> children;
        {
            ParentGuard locked(path);
            const auto handle = reinterpret_cast<NativeHandle>(locked.native_handle());
            if (identity_of(handle) != expected) throw std::runtime_error("owned directory identity changed");
            alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 64 * 1024> buffer{};
            for (;;) {
                if (!GetFileInformationByHandleEx(handle, FileIdBothDirectoryInfo, buffer.data(),
                                                static_cast<DWORD>(buffer.size()))) {
                    if (GetLastError() == ERROR_NO_MORE_FILES) break;
                    os_error("enumerate owned directory");
                }
                std::size_t offset{};
                for (;;) {
                    if (offset > buffer.size() - offsetof(FILE_ID_BOTH_DIR_INFO, FileName))
                        throw std::runtime_error("invalid directory information offset");
                    const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
                    const auto available = buffer.size() - offset - offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
                    if (entry->FileNameLength > available || entry->FileNameLength % sizeof(wchar_t))
                        throw std::runtime_error("invalid directory information name");
                    const std::wstring name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
                    if (name != L"." && name != L"..") {
                        if (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                            throw std::runtime_error("owned directory contains a redirect");
                        const auto child = reinterpret_cast<NativeHandle>(foundation_detail::windows_open_relative(
                            locked, std::filesystem::path(name), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, 1, 0));
                        if (child == invalid_handle) os_error("inspect owned directory child");
                        try {
                            if (!(info(child).dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) require_regular(child);
                            children.emplace_back(path / name, identity_of(child));
                        } catch (...) { close_handle(child); throw; }
                        close_handle(child);
                        if (children.size() > 200000) throw std::runtime_error("owned directory entry limit exceeded");
                    }
                    if (!entry->NextEntryOffset) break;
                    if (entry->NextEntryOffset < offsetof(FILE_ID_BOTH_DIR_INFO, FileName) ||
                        entry->NextEntryOffset > buffer.size() - offset)
                        throw std::runtime_error("invalid directory information stride");
                    offset += entry->NextEntryOffset;
                }
            }
            locked.verify();
        }
        // The canonical mutation anchor must not enter the enumerated child set.
        {
            ParentGuard mutating(path, false, GuardMode::NamespaceMutation);
            if (identity_of(reinterpret_cast<NativeHandle>(mutating.native_handle())) != expected)
                throw std::runtime_error("owned directory identity changed before removal");
            for (const auto& [child, identity] : children) remove_tree_checked(child, &identity);
            mutating.verify();
        }
    }
    const auto deleting = open_leaf(FILE_READ_ATTRIBUTES | DELETE);
    try {
        if (identity_of(deleting) != expected) throw std::runtime_error("owned removal entry changed identity");
        if (!directory) require_regular(deleting);
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(deleting, FileDispositionInfo, &disposition, sizeof(disposition)))
            os_error("delete owned entry");
    } catch (...) { close_handle(deleting); throw; }
    close_handle(deleting); parent.verify();
#else
    if (authorized && directory_identity(path) != *authorized)
        throw std::runtime_error("owned directory identity changed");
    const auto status = std::filesystem::symlink_status(io_path(path));
    if (std::filesystem::is_directory(status)) {
        ParentGuard locked(path);
        std::vector<std::filesystem::path> children;
        for (const auto& entry : std::filesystem::directory_iterator(io_path(path)))
            children.push_back(logical_path(entry.path()));
        for (const auto& child : children) remove_tree_checked(child);
        locked.verify();
    } else if (!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("refusing to remove a link or special file");
    }
    if (!std::filesystem::remove(io_path(path))) throw std::runtime_error("owned removal failed");
#endif
}
} // namespace

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path utf8_path(std::string_view text) {
    check_utf8(text);
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string unique_id() {
    std::array<unsigned char, 16> random{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("cryptographic random generator failed");
#else
    auto file = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (file < 0) os_error("open random source");
    std::size_t at{};
    while (at != random.size()) {
        const auto count = ::read(file, random.data() + at, random.size() - at);
        if (count <= 0) { close_handle(file); os_error("read random source"); }
        at += static_cast<std::size_t>(count);
    }
    close_handle(file);
#endif
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : random) { out += hex[byte >> 4]; out += hex[byte & 15]; }
    return out;
}

std::string case_key(std::string_view utf8) {
    check_utf8(utf8);
    if (utf8.empty()) return {};
    if (default_volume_case_sensitive())
        return std::string(utf8);  // byte-exact volume: names collide only when identical
#ifdef _WIN32
    const auto wide = utf8_path(utf8).native();
    const auto count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                                    wide.data(), static_cast<int>(wide.size()),
                                    nullptr, 0, nullptr, nullptr, 0);
    if (!count) os_error("path case folding");
    std::wstring folded(static_cast<std::size_t>(count), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
                       static_cast<int>(wide.size()), folded.data(), count,
                       nullptr, nullptr, 0)) os_error("path case folding");
    return path_utf8(std::filesystem::path(folded));
#else
    // Case-insensitive POSIX volume (macOS APFS default): fold within the ASCII
    // domain, which matches the Windows invariant fold for these bytes. Non-ASCII
    // case pairs deliberately stay byte-distinct (documented regional limitation).
    return fold_ascii(utf8);
#endif
}

namespace {
// Reserved DOS device names must stay rejected on every host. The check is independent of
// the filesystem collision rule, so POSIX performs a local ASCII fold while Windows keeps
// its full invariant Unicode folding.
std::string reserved_stem_key(std::string_view text) {
#ifdef _WIN32
    return case_key(text);
#else
    // DOS device names are rejected on every host regardless of the volume collision
    // rule, so this key stays ASCII-folded even on byte-exact POSIX volumes.
    return fold_ascii(text);
#endif
}
} // namespace

bool volume_case_sensitive(const std::filesystem::path& directory) {
    if (const auto forced = case_volume_override().load(std::memory_order_relaxed); forced != 0)
        return forced == 1;
#ifdef _WIN32
    (void)directory;
    return false;
#else
    static std::mutex cache_mutex;
    static std::unordered_map<std::uint64_t, bool> verdicts;
    struct stat root_stat{};
    if (::stat(directory.c_str(), &root_stat) || !S_ISDIR(root_stat.st_mode))
        return true;  // unknown volume: keep byte-exact matching
    {
        const std::lock_guard guard(cache_mutex);
        if (const auto found = verdicts.find(root_stat.st_dev); found != verdicts.end())
            return found->second;
    }
    const bool sensitive = decide_volume_case(directory);
    const std::lock_guard guard(cache_mutex);
    verdicts.emplace(root_stat.st_dev, sensitive);
    return sensitive;
#endif
}

#ifndef _WIN32
const std::filesystem::path& default_probe_anchor() {
    static const std::filesystem::path anchor = [] {
        std::error_code code;
        const auto temp = std::filesystem::temp_directory_path(code);
        return code ? std::filesystem::path(".") : temp;
    }();
    return anchor;
}
#endif

bool default_volume_case_sensitive() {
    if (const auto forced = case_volume_override().load(std::memory_order_relaxed); forced != 0)
        return forced == 1;
#ifdef _WIN32
    return false;  // Windows name matching folds case on every volume
#else
    static const bool cached = volume_case_sensitive(default_probe_anchor());
    return cached;
#endif
}

void override_volume_case_sensitive(std::optional<bool> decision) {
    case_volume_override().store(decision ? (*decision ? 1 : 2) : 0, std::memory_order_relaxed);
}

std::string validate_relative(std::string_view input, std::size_t max_depth,
                              std::size_t max_bytes, RelativePathPolicy policy) {
    if (input.empty() || input.size() > max_bytes || input.front() == '/' ||
        input.front() == '\\') throw std::runtime_error("unsafe archive path");
    check_utf8(input);
    std::string text(input);
    if (text.back() == '/') text.pop_back();
    if (text.empty()) throw std::runtime_error("empty archive path");
    for (const auto byte : text) {
        const auto code = static_cast<unsigned char>(byte);
        if (code < 0x20 || code == 0x7f || byte == '\\' || byte == ':' ||
            byte == '<' || byte == '>' || byte == '"' || byte == '|' ||
            byte == '?' || byte == '*') throw std::runtime_error("unsafe archive path character");
    }
    std::size_t depth{}, start{};
    while (start < text.size()) {
        auto end = text.find('/', start);
        if (end == std::string::npos) end = text.size();
        const auto part = std::string_view(text).substr(start, end - start);
        if (++depth > max_depth || part.empty() || part == "." || part == ".." ||
            part.size() > 255 || part.back() == '.' || part.back() == ' ')
            throw std::runtime_error("unsafe archive path component");
        auto stem = reserved_stem_key(part.substr(0, part.find('.')));
        if (policy == RelativePathPolicy::DiskOutput &&
            (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
             stem == "conin$" || stem == "conout$" ||
             (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
              stem[3] >= '1' && stem[3] <= '9') ||
             ((stem.starts_with("com") || stem.starts_with("lpt")) &&
              (stem.ends_with("\xc2\xb9") || stem.ends_with("\xc2\xb2") ||
               stem.ends_with("\xc2\xb3")))))
            throw std::runtime_error("reserved device path");
        start = end + 1;
    }
    return text;
}

void reject_links(const std::filesystem::path& path) {
    for (const auto& ancestor : ancestors(path)) {
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(native_path(ancestor).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
            os_error("path attributes");
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("path contains a reparse point");
#else
        struct stat status{};
        if (::lstat(ancestor.c_str(), &status)) {
            if (errno == ENOENT) continue;
            os_error("path attributes");
        }
        if (S_ISLNK(status.st_mode)) throw std::runtime_error("path contains a symlink");
#endif
    }
}

std::filesystem::path io_path(const std::filesystem::path& path) {
#ifdef _WIN32
    return std::filesystem::path(native_path(path));
#else
    return path;
#endif
}
std::filesystem::path logical_path(const std::filesystem::path& path) {
#ifdef _WIN32
    auto text = path.native();
    if (text.starts_with(L"\\\\?\\")) {
        text.erase(0, 4);
        if (text.size() < 3 || text[1] != L':' || text[2] != L'\\')
            throw std::runtime_error("unexpected internal namespace path");
        return std::filesystem::path(std::move(text));
    }
#endif
    return path;
}
std::filesystem::path checked_absolute(const std::filesystem::path& input) {
    if (input.empty()) throw std::runtime_error("empty target path");
    const auto text = path_utf8(input);
    if (text.starts_with("//") || text.starts_with("\\\\") ||
        (input.has_root_name() && !input.has_root_directory()))
        throw std::runtime_error("UNC, device and drive-relative paths are not permitted");
    auto result = std::filesystem::absolute(input).lexically_normal();
    if (result == result.root_path()) throw std::runtime_error("root directory is not a target");
#ifdef _WIN32
    (void)validate_relative(path_utf8(result.lexically_relative(result.root_path())));
#else
    check_utf8(text);
    for (const auto& component : input.relative_path())
        if (component == ".." || path_utf8(component).find('\0') != std::string::npos)
            throw std::runtime_error("unsafe filesystem path component");
#endif
    reject_links(std::filesystem::absolute(input));
    reject_links(result);
    result = logical_path(std::filesystem::weakly_canonical(io_path(result)));
    reject_links(result);
    return result;
}

bool paths_overlap(const std::filesystem::path& left,
                   const std::filesystem::path& right) {
    const auto a = case_key(path_utf8(left.lexically_normal()));
    const auto b = case_key(path_utf8(right.lexically_normal()));
    const auto inside = [](const std::string& parent, const std::string& child) {
        return child == parent || (child.size() > parent.size() &&
               child.starts_with(parent) && child[parent.size()] == '/');
    };
    return inside(a, b) || inside(b, a);
}

#ifndef _WIN32
struct ParentGuard::Impl {
    std::filesystem::path path;
    std::vector<NativeHandle> handles;
    std::vector<FileIdentity> identities;
    ~Impl() { for (auto value : handles) close_handle(value); }
};
ParentGuard::ParentGuard(const std::filesystem::path& directory, bool create_missing, GuardMode mode)
    : impl_(std::make_unique<Impl>()) {
    (void)mode;
    if (directory.empty()) throw std::runtime_error("empty guard directory");
    impl_->path = std::filesystem::absolute(directory).lexically_normal();
    const auto text = path_utf8(impl_->path);
    if (text.starts_with("//") || text.starts_with("\\\\")) throw std::runtime_error("UNC directory is not permitted");
    const auto chain = ancestors(impl_->path);
    impl_->handles.reserve(chain.size()); impl_->identities.reserve(chain.size());
    for (const auto& path : chain) {
        const auto parent = impl_->handles.empty() ? AT_FDCWD : impl_->handles.back();
        const auto name = impl_->handles.empty() ? path : path.filename();
        if (create_missing && !impl_->handles.empty() && ::mkdirat(parent, name.c_str(), 0700) && errno != EEXIST)
            os_error("create parent directory");
        const auto handle = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (handle == invalid_handle) os_error("open relative parent directory");
        impl_->handles.push_back(handle); impl_->identities.push_back(identity_of(handle));
    }
}
ParentGuard::~ParentGuard() = default;
ParentGuard::ParentGuard(ParentGuard&&) noexcept = default;
ParentGuard& ParentGuard::operator=(ParentGuard&&) noexcept = default;
const std::filesystem::path& ParentGuard::path() const noexcept { return impl_->path; }
std::intptr_t ParentGuard::native_handle() const noexcept { return impl_->handles.back(); }
void ParentGuard::verify() const {
    const auto chain = ancestors(impl_->path);
    if (chain.size() != impl_->identities.size()) throw std::runtime_error("guard chain changed");
    for (std::size_t at = 0; at < chain.size(); ++at) {
        if (identity_of(impl_->handles[at]) != impl_->identities[at]) throw std::runtime_error("guard directory identity changed");
        struct stat status{};
        if (::lstat(chain[at].c_str(), &status) || !S_ISDIR(status.st_mode) ||
            FileIdentity{static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino)} != impl_->identities[at])
            throw std::runtime_error("guard directory pathname changed");
    }
}
#endif

struct SecureFile::Impl {
    std::filesystem::path path;
    NativeHandle handle{invalid_handle};
    std::unique_ptr<ParentGuard> parents;
    bool publication{};
    ~Impl() { close_handle(handle); }
};
SecureFile::SecureFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SecureFile::~SecureFile() = default;
SecureFile::SecureFile(SecureFile&&) noexcept = default;
SecureFile& SecureFile::operator=(SecureFile&&) noexcept = default;

SecureFile SecureFile::create_new(const std::filesystem::path& input) {
    auto impl = std::make_unique<Impl>();
    impl->path = checked_absolute(input);
    impl->parents = std::make_unique<ParentGuard>(impl->path.parent_path(), false, GuardMode::NamespaceMutation);
#ifdef _WIN32
    impl->handle = reinterpret_cast<NativeHandle>(foundation_detail::windows_open_relative(
        *impl->parents, impl->path.filename(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ, 2, 0x40));
#else
    impl->handle = ::openat(static_cast<int>(impl->parents->native_handle()), impl->path.filename().c_str(),
                            O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#endif
    if (impl->handle == invalid_handle) os_error("create exclusive file");
    require_regular(impl->handle); impl->publication = true;
    return SecureFile(std::move(impl));
}

SecureFile SecureFile::open_read(const std::filesystem::path& input) {
    return open_read(input, GuardMode::ReadOnly);
}
SecureFile SecureFile::open_read(const std::filesystem::path& input, GuardMode mode) {
    auto impl = std::make_unique<Impl>();
    impl->path = checked_absolute(input);
    impl->parents = std::make_unique<ParentGuard>(impl->path.parent_path(), false, mode);
#ifdef _WIN32
    impl->handle = reinterpret_cast<NativeHandle>(foundation_detail::windows_open_relative(
        *impl->parents, impl->path.filename(), GENERIC_READ, FILE_SHARE_READ, 1, 0x40));
#else
    impl->handle = ::openat(static_cast<int>(impl->parents->native_handle()), impl->path.filename().c_str(),
                            O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
#endif
    if (impl->handle == invalid_handle) os_error("open regular file");
    require_regular(impl->handle);
    return SecureFile(std::move(impl));
}

SecureFile SecureFile::open_owned(const std::filesystem::path& input,
                                  const FileIdentity& expected) {
    auto impl = std::make_unique<Impl>();
    impl->path = checked_absolute(input);
    impl->parents = std::make_unique<ParentGuard>(impl->path.parent_path(), false, GuardMode::NamespaceMutation);
#ifdef _WIN32
    impl->handle = reinterpret_cast<NativeHandle>(foundation_detail::windows_open_relative(
        *impl->parents, impl->path.filename(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, 1, 0x40));
#else
    impl->handle = ::openat(static_cast<int>(impl->parents->native_handle()), impl->path.filename().c_str(),
                            O_RDWR | O_NOFOLLOW | O_CLOEXEC);
#endif
    if (impl->handle == invalid_handle) os_error("open owned file");
    require_regular(impl->handle);
    if (identity_of(impl->handle) != expected)
        throw std::runtime_error("owned file identity changed");
    return SecureFile(std::move(impl));
}
SecureFile SecureFile::open_publication(const std::filesystem::path& input,
                                       const FileIdentity& expected, GuardMode mode) {
    auto impl = std::make_unique<Impl>();
    impl->path = checked_absolute(input);
    impl->parents = std::make_unique<ParentGuard>(impl->path.parent_path(), false, mode);
#ifdef _WIN32
    impl->handle = reinterpret_cast<NativeHandle>(foundation_detail::windows_open_relative(
        *impl->parents, impl->path.filename(), GENERIC_READ | DELETE, FILE_SHARE_READ, 1, 0x40));
#else
    impl->handle = ::openat(static_cast<int>(impl->parents->native_handle()), impl->path.filename().c_str(),
                            O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
#endif
    if (impl->handle == invalid_handle) os_error("lock publication source");
    const auto status = info(impl->handle);
#ifdef _WIN32
    if ((status.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || status.nNumberOfLinks != 1)
#else
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1)
#endif
        throw std::runtime_error("publication requires an unlinked regular file");
    if (identity_of(impl->handle) != expected) throw std::runtime_error("publication revision identity changed");
    impl->publication = true;
    impl->parents->verify();
    return SecureFile(std::move(impl));
}
SecureFile SecureFile::lock_workspace(const std::filesystem::path& input, bool create) {
    auto impl = std::make_unique<Impl>();
    impl->path = checked_absolute(input);
    impl->parents = std::make_unique<ParentGuard>(impl->path.parent_path(), false, GuardMode::NamespaceMutation);
#ifdef _WIN32
    impl->handle = reinterpret_cast<NativeHandle>(foundation_detail::windows_open_relative(
        *impl->parents, impl->path.filename(), GENERIC_READ | GENERIC_WRITE,
        0, create ? 2 : 1, 0x40));
#else
    impl->handle = ::openat(static_cast<int>(impl->parents->native_handle()), impl->path.filename().c_str(),
                            O_RDWR | O_NOFOLLOW | O_CLOEXEC | (create ? O_CREAT | O_EXCL : 0), 0600);
    if (impl->handle != invalid_handle && ::flock(impl->handle, LOCK_EX | LOCK_NB))
        os_error("lock transaction workspace");
#endif
    if (impl->handle == invalid_handle) os_error("lock transaction workspace");
    const auto status = info(impl->handle);
#ifdef _WIN32
    if ((status.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || status.nNumberOfLinks != 1)
#else
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1)
#endif
        throw std::runtime_error("workspace lock is not a regular owned file");
    return SecureFile(std::move(impl));
}
void SecureFile::publish_no_replace(const std::filesystem::path& destination) {
    publish_file(destination, false);
}
void SecureFile::publish_file(const std::filesystem::path& destination, bool replace) {
    if (!impl_ || !impl_->publication || impl_->handle == invalid_handle)
        throw std::runtime_error("publication requires a retained source handle");
    auto to = checked_absolute(destination);
    auto destination_parents = std::make_unique<ParentGuard>(to.parent_path(), false, GuardMode::NamespaceMutation);
    impl_->parents->verify(); destination_parents->verify(); require_regular(impl_->handle);
#ifdef _WIN32
    rename_native(impl_->handle, *impl_->parents, *destination_parents, to.filename(), replace);
#else
    const auto parent = static_cast<int>(impl_->parents->native_handle());
    const auto destination_parent = static_cast<int>(destination_parents->native_handle());
    struct stat status{};
    if (::fstatat(parent, impl_->path.filename().c_str(), &status, AT_SYMLINK_NOFOLLOW))
        os_error("inspect publication source");
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        FileIdentity{static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino)} != identity())
        throw std::runtime_error("publication source identity changed");
    if (replace) {
        if (::renameat(parent, impl_->path.filename().c_str(), destination_parent, to.filename().c_str()))
            os_error("replace journal from retained handle");
    } else rename_at_no_replace(parent, impl_->path.filename(), destination_parent, to.filename());
#endif
    impl_->path.swap(to);
    impl_->parents = std::move(destination_parents);
    impl_->parents->verify();
    sync_directory(impl_->path.parent_path());
}

std::size_t SecureFile::read(std::span<std::byte> buffer) {
    if (!impl_ || impl_->handle == invalid_handle) throw std::runtime_error("closed file");
#ifdef _WIN32
    DWORD count{};
    const auto size = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), MAXDWORD));
    if (!ReadFile(impl_->handle, buffer.data(), size, &count, nullptr)) os_error("read file");
#else
    const auto count = ::read(impl_->handle, buffer.data(), buffer.size());
    if (count < 0) os_error("read file");
#endif
    return static_cast<std::size_t>(count);
}

void SecureFile::write(std::span<const std::byte> buffer) {
    if (!impl_ || impl_->handle == invalid_handle) throw std::runtime_error("closed file");
    while (!buffer.empty()) {
#ifdef _WIN32
        DWORD count{};
        const auto size = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), MAXDWORD));
        if (!WriteFile(impl_->handle, buffer.data(), size, &count, nullptr)) os_error("write file");
#else
        const auto count = ::write(impl_->handle, buffer.data(), buffer.size());
        if (count < 0) os_error("write file");
#endif
        if (!count) throw std::runtime_error("zero length file write");
        buffer = buffer.subspan(static_cast<std::size_t>(count));
    }
}

void SecureFile::seek(std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("file offset overflow");
#ifdef _WIN32
    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(impl_->handle, distance, nullptr, FILE_BEGIN)) os_error("seek file");
#else
    if (::lseek(impl_->handle, static_cast<off_t>(offset), SEEK_SET) < 0) os_error("seek file");
#endif
}
void SecureFile::truncate(std::uint64_t bytes) {
    seek(bytes);
#ifdef _WIN32
    if (!SetEndOfFile(impl_->handle)) os_error("truncate file");
#else
    if (::ftruncate(impl_->handle, static_cast<off_t>(bytes))) os_error("truncate file");
#endif
}
void SecureFile::flush() {
#ifdef _WIN32
    if (!FlushFileBuffers(impl_->handle)) os_error("flush file");
#else
    if (::fsync(impl_->handle)) os_error("flush file");
#endif
}
std::uint64_t SecureFile::size() const {
#ifdef _WIN32
    LARGE_INTEGER result{};
    if (!GetFileSizeEx(impl_->handle, &result)) os_error("file size");
    return static_cast<std::uint64_t>(result.QuadPart);
#else
    return static_cast<std::uint64_t>(info(impl_->handle).st_size);
#endif
}
FileIdentity SecureFile::identity() const { return identity_of(impl_->handle); }
FileRevision SecureFile::revision() const { return revision_of(impl_->handle); }
void SecureFile::remove_verified(const RegularFileToken& expected, const std::function<bool()>& cancelled) {
    transport::detail::boundary("remove_failed", "Cannot remove authorized regular file", [&] {
        bool committed{};
        // Prepare the removal facts before the irreversible native operation.
        Error pending{"remove_persistence_pending", "Regular file removal committed; durability is pending",
            Json{{"committed", true}, {"removed", true}, {"removedBytes", expected.size}, {"durabilityPending", true}}};
        try {
            if (!impl_ || !impl_->publication || impl_->handle == invalid_handle)
                transport::detail::fail("revision_conflict", "Deletion requires a fixed publication handle");
#ifdef _WIN32
            if (impl_->parents->mode() != GuardMode::ReadOnly)
                transport::detail::fail("revision_conflict", "Authorized deletion requires a strict parent guard");
#endif
            const auto root = checked_absolute(expected.root), leaf = checked_absolute(expected.path);
            if (root != expected.root || leaf != expected.path || impl_->path != leaf || !beneath(root, leaf) ||
                expected.sha256.size() != 64 || !std::all_of(expected.sha256.begin(), expected.sha256.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                })) transport::detail::fail("revision_conflict", "Invalid regular file authorization");
            ParentGuard root_pin(root);
            const auto verify_parents = [&] {
                root_pin.verify(); impl_->parents->verify();
#ifdef _WIN32
                const auto root_handle = reinterpret_cast<NativeHandle>(root_pin.native_handle());
                const auto parent_handle = reinterpret_cast<NativeHandle>(impl_->parents->native_handle());
#else
                const auto root_handle = static_cast<NativeHandle>(root_pin.native_handle());
                const auto parent_handle = static_cast<NativeHandle>(impl_->parents->native_handle());
#endif
                if (identity_of(root_handle) != expected.root_identity || identity_of(parent_handle) != expected.parent_identity)
                    transport::detail::fail("revision_conflict", "Authorized parent identity changed");
            };
            const auto verify_file = [&](const FileRevision& revision) {
                require_regular(impl_->handle);
                if (identity() != expected.file_identity || size() != expected.size || this->revision() != revision)
                    transport::detail::fail("revision_conflict", "Authorized file revision changed");
            };
            transport::detail::check_cancel(cancelled); verify_parents(); verify_file(expected.revision);
            if (!transport::detail::digest_equal(regular_digest(*this, expected.size, cancelled), expected.sha256))
                transport::detail::fail("revision_conflict", "Authorized file content changed");
            NICKEL_TRANSPORT_CHECKPOINT("regular.validated", leaf);
            transport::detail::check_cancel(cancelled); verify_parents(); verify_file(expected.revision);
#ifdef _WIN32
            FILE_DISPOSITION_INFO disposition{}; disposition.DeleteFile = TRUE;
            if (!SetFileInformationByHandle(impl_->handle, FileDispositionInfo, &disposition, sizeof(disposition)))
                os_error("delete locked regular file");
            close_handle(impl_->handle); impl_->handle = invalid_handle;
            committed = true;
            NICKEL_TRANSPORT_CHECKPOINT("regular.committed", leaf);
#else
            const auto parent = static_cast<int>(impl_->parents->native_handle());
            Quarantine quarantine(parent);
            NICKEL_TRANSPORT_CHECKPOINT("regular.quarantining", leaf);
            transport::detail::check_cancel(cancelled); verify_parents(); verify_file(expected.revision);
            rename_at_no_replace(parent, leaf.filename(), quarantine.handle, std::filesystem::path("leaf"));
            quarantine.occupied = true;
            try {
                // Rename advances ctime. Validate the preview first, then hash the isolated inode again.
                const auto isolated_revision = revision();
                verify_parents(); require_regular(impl_->handle);
                struct stat isolated{};
                if (::fstatat(quarantine.handle, "leaf", &isolated, AT_SYMLINK_NOFOLLOW) || !S_ISREG(isolated.st_mode) ||
                    isolated.st_nlink != 1 ||
                    FileIdentity{static_cast<std::uint64_t>(isolated.st_dev), static_cast<std::uint64_t>(isolated.st_ino)} != expected.file_identity ||
                    isolated_revision.modified != expected.revision.modified)
                    transport::detail::fail("revision_conflict", "Quarantined file identity changed");
                if (!transport::detail::digest_equal(regular_digest(*this, expected.size, cancelled), expected.sha256))
                    transport::detail::fail("revision_conflict", "Quarantined file content changed");
                NICKEL_TRANSPORT_CHECKPOINT("regular.quarantined", leaf);
                transport::detail::check_cancel(cancelled); verify_parents(); verify_file(isolated_revision);
                // Catch same-tick changes at regular.quarantined; regular.hash remains observable on every chunk.
                if (!transport::detail::digest_equal(regular_digest(*this, expected.size, cancelled), expected.sha256))
                    transport::detail::fail("revision_conflict", "Quarantined file content changed before deletion");
                transport::detail::check_cancel(cancelled); verify_parents(); verify_file(isolated_revision);
                if (::fstatat(quarantine.handle, "leaf", &isolated, AT_SYMLINK_NOFOLLOW) || !S_ISREG(isolated.st_mode) ||
                    isolated.st_nlink != 1 ||
                    FileIdentity{static_cast<std::uint64_t>(isolated.st_dev), static_cast<std::uint64_t>(isolated.st_ino)} != expected.file_identity)
                    transport::detail::fail("revision_conflict", "Quarantine entry changed before deletion");
                // Final content observation through unlink is not atomic against a pre-opened hostile same-UID writer.
                if (::unlinkat(quarantine.handle, "leaf", 0)) os_error("delete isolated regular file");
                committed = true;
                quarantine.occupied = false;
                close_handle(impl_->handle); impl_->handle = invalid_handle;
                NICKEL_TRANSPORT_CHECKPOINT("regular.committed", leaf);
                if (::fsync(quarantine.handle) || ::fsync(parent)) os_error("sync regular file deletion");
            } catch (...) {
                if (quarantine.occupied) {
                    try {
                        verify_parents();
                        rename_at_no_replace(quarantine.handle, std::filesystem::path("leaf"), parent, leaf.filename());
                        quarantine.occupied = false;
                        if (::fsync(parent)) os_error("sync restored regular file");
                    } catch (...) {
                        throw ErrorException(Error{"revision_conflict", "Deletion conflict retained without overwriting another file"});
                    }
                }
                throw;
            }
#endif
            sync_directory(leaf.parent_path());
        } catch (const std::system_error& error) {
            if (!committed) throw;
            pending.details["nativeError"] = error.code().value();
            throw ErrorException(std::move(pending));
        } catch (const ErrorException& error) {
            if (!committed) throw;
            const auto native = error.error().details.find("nativeError");
            if (native != error.error().details.end() && native->is_number_integer())
                pending.details["nativeError"] = *native;
            throw ErrorException(std::move(pending));
        } catch (...) {
            if (!committed) throw;
            throw ErrorException(std::move(pending));
        }
    });
}
RegularFileToken capture_regular_file(const std::filesystem::path& root_input,
                                      const std::filesystem::path& path_input,
                                      const std::function<bool()>& cancelled) {
    return capture_regular_file(root_input, path_input, std::numeric_limits<std::uint64_t>::max(), cancelled);
}
RegularFileToken capture_regular_file(const std::filesystem::path& root_input,
                                      const std::filesystem::path& path_input, std::uint64_t max_bytes,
                                      const std::function<bool()>& cancelled) {
    return transport::detail::boundary("capture_failed", "Cannot capture authorized regular file", [&] {
        transport::detail::check_cancel(cancelled);
        const auto root = transport::detail::boundary("unsafe_path", "Unsafe capture root", [&] { return checked_absolute(root_input); });
        const auto leaf = transport::detail::boundary("unsafe_path", "Unsafe capture path", [&] { return checked_absolute(path_input); });
        if (!beneath(root, leaf)) transport::detail::fail("outside_root", "Regular file is outside the authorized root");
        ParentGuard root_pin(root), parent_pin(leaf.parent_path());
        auto file = transport::detail::boundary("revision_conflict", "Regular file is no longer capturable", [&] { return SecureFile::open_read(leaf); });
#ifdef _WIN32
        const auto root_handle = reinterpret_cast<NativeHandle>(root_pin.native_handle());
        const auto parent_handle = reinterpret_cast<NativeHandle>(parent_pin.native_handle());
#else
        const auto root_handle = static_cast<NativeHandle>(root_pin.native_handle());
        const auto parent_handle = static_cast<NativeHandle>(parent_pin.native_handle());
#endif
        RegularFileToken token{root, file.path(), identity_of(root_handle), identity_of(parent_handle),
                               file.identity(), file.size(), file.revision(), {}};
        if (token.size > max_bytes) transport::detail::fail("file_limits", "Regular file exceeds the capture byte budget");
        token.sha256 = regular_digest(file, token.size, cancelled);
        root_pin.verify(); parent_pin.verify();
        if (file.identity() != token.file_identity || file.size() != token.size || file.revision() != token.revision)
            transport::detail::fail("revision_conflict", "Regular file changed during capture");
        return token;
    });
}
void remove_regular_file(const RegularFileToken& expected, const std::function<bool()>& cancelled) {
    transport::detail::boundary("remove_failed", "Cannot remove authorized regular file", [&] {
        transport::detail::check_cancel(cancelled);
        const auto root = transport::detail::boundary("unsafe_path", "Unsafe authorized root", [&] { return checked_absolute(expected.root); });
        const auto leaf = transport::detail::boundary("unsafe_path", "Unsafe authorized path", [&] { return checked_absolute(expected.path); });
        if (!beneath(root, leaf)) transport::detail::fail("outside_root", "Regular file is outside the authorized root");
        if (root != expected.root || leaf != expected.path)
            transport::detail::fail("revision_conflict", "Regular file authorization is not canonical");
        ParentGuard root_pin(root);
        auto file = transport::detail::boundary("revision_conflict", "Authorized regular file was replaced or is in use", [&] {
            return SecureFile::open_publication(leaf, expected.file_identity);
        });
        file.remove_verified(expected, cancelled);
    });
}
const std::filesystem::path& SecureFile::path() const noexcept { return impl_->path; }
void SecureFile::close() noexcept {
    if (impl_) { close_handle(impl_->handle); impl_->handle = invalid_handle; impl_->parents.reset(); }
}

void create_directory_new(const std::filesystem::path& input) {
    const auto path = checked_absolute(input);
    ParentGuard parent(path.parent_path(), false, GuardMode::NamespaceMutation);
#ifdef _WIN32
    const auto handle = reinterpret_cast<NativeHandle>(foundation_detail::windows_open_relative(
        parent, path.filename(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 2, 0x1));
    if (handle == invalid_handle) os_error("create exclusive directory");
    try {
        if (!(info(handle).dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("created entry is not a directory");
    } catch (...) { close_handle(handle); throw; }
    close_handle(handle);
#else
    if (::mkdirat(static_cast<int>(parent.native_handle()), path.filename().c_str(), 0700))
        os_error("create exclusive directory");
#endif
    parent.verify();
}

namespace {
void rename_impl(const std::filesystem::path& source,
                 const std::filesystem::path& destination, bool replace,
                 const FileIdentity* expected = nullptr) {
    const auto from = checked_absolute(source), to = checked_absolute(destination);
    ParentGuard from_parent(from.parent_path(), false, GuardMode::NamespaceMutation);
    ParentGuard to_parent(to.parent_path(), false, GuardMode::NamespaceMutation);
#ifdef _WIN32
    const auto handle = reinterpret_cast<NativeHandle>(foundation_detail::windows_open_relative(
        from_parent, from.filename(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, 1, 0));
    if (handle == invalid_handle) os_error("open rename source");
    try {
        const auto status = info(handle);
        if (!(status.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && status.nNumberOfLinks != 1)
            throw std::runtime_error("rename refuses hardlinked file");
        if (expected && identity_of(handle) != *expected)
            throw std::runtime_error("rename source identity changed");
        rename_native(handle, from_parent, to_parent, to.filename(), replace);
    } catch (...) { close_handle(handle); throw; }
    close_handle(handle);
#else
    const auto from_handle = static_cast<int>(from_parent.native_handle());
    const auto to_handle = static_cast<int>(to_parent.native_handle());
    struct stat status{};
    if (::fstatat(from_handle, from.filename().c_str(), &status, AT_SYMLINK_NOFOLLOW))
        os_error("rename source information");
    if ((!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode)) ||
        (S_ISREG(status.st_mode) && status.st_nlink != 1))
        throw std::runtime_error("rename refuses links and special files");
    const FileIdentity actual{static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino)};
    if (expected && actual != *expected) throw std::runtime_error("rename source identity changed");
    from_parent.verify(); to_parent.verify();
    if (replace) {
        if (::renameat(from_handle, from.filename().c_str(), to_handle, to.filename().c_str()))
            os_error("atomic rename");
    } else {
        rename_at_no_replace(from_handle, from.filename(), to_handle, to.filename());
    }
#endif
    from_parent.verify(); to_parent.verify();
    sync_directory(to.parent_path());
}
} // namespace

void rename_no_replace(const std::filesystem::path& source,
                       const std::filesystem::path& destination) {
    rename_impl(source, destination, false);
}
void rename_owned_no_replace(const std::filesystem::path& source,
                             const std::filesystem::path& destination,
                             const FileIdentity& expected) {
    rename_impl(source, destination, false, &expected);
}

void write_atomic(const std::filesystem::path& input, std::string_view bytes) {
    const auto path = checked_absolute(input);
    const auto temporary = path.parent_path() / utf8_path(".nickel-journal-" + unique_id());
    auto file = SecureFile::create_new(temporary);
    try {
        file.write(std::as_bytes(std::span(bytes.data(), bytes.size())));
        file.flush();
        file.publish_file(path, true);
        file.flush(); file.close();
    } catch (...) {
        if (file.impl_ && file.impl_->handle != invalid_handle && file.impl_->path == temporary) {
#ifdef _WIN32
            FILE_DISPOSITION_INFO disposition{TRUE};
            (void)SetFileInformationByHandle(file.impl_->handle, FileDispositionInfo, &disposition, sizeof(disposition));
#else
            const auto parent = static_cast<int>(file.impl_->parents->native_handle());
            struct stat status{};
            if (!::fstatat(parent, temporary.filename().c_str(), &status, AT_SYMLINK_NOFOLLOW) &&
                S_ISREG(status.st_mode) && status.st_nlink == 1 &&
                FileIdentity{static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino)} == file.identity())
                (void)::unlinkat(parent, temporary.filename().c_str(), 0);
#endif
        }
        file.close();
        throw;
    }
}
std::vector<std::byte> read_bounded(const std::filesystem::path& path,
                                  std::uint64_t limit) {
    auto file = SecureFile::open_read(path);
    if (file.size() > limit || file.size() > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("file exceeds read limit");
    std::vector<std::byte> result;
    std::array<std::byte, 64 * 1024> buffer;
    for (;;) {
        const auto count = file.read(buffer);
        if (!count) break;
        if (count > limit - result.size()) throw std::runtime_error("file exceeds read limit");
        result.insert(result.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
    }
    return result;
}
FileIdentity directory_identity(const std::filesystem::path& path) {
    ParentGuard pin(path);
#ifdef _WIN32
    return identity_of(reinterpret_cast<NativeHandle>(pin.native_handle()));
#else
    return identity_of(static_cast<NativeHandle>(pin.native_handle()));
#endif
}
void remove_owned_tree(const std::filesystem::path& path, const FileIdentity& identity) {
    ParentGuard parent(path.parent_path(), false, GuardMode::NamespaceMutation);
    if (directory_identity(path) != identity)
        throw std::runtime_error("refusing to remove foreign directory");
    remove_tree_checked(path, &identity);
    parent.verify();
    sync_directory(path.parent_path());
}
void sync_directory(const std::filesystem::path& path) {
#ifndef _WIN32
    const auto handle = open_directory(path);
    if (::fsync(handle)) { close_handle(handle); os_error("sync directory"); }
    close_handle(handle);
#else
    (void)path;
    // Windows namespace publication is atomic; every journal/data handle is flushed.
#endif
}
} // namespace nickel::tasks::detail
