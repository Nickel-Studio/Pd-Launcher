#include "domain_io.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <limits>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace nickel::instances {
namespace {
template<class Fn> decltype(auto) foundation_call(Fn&& operation, std::string_view fallback, std::string_view message) {
    try { return operation(); }
    catch (const nickel::ErrorException& error) {
        const auto& code = error.error().code;
        const auto domain_code = code == "UnsafePath" ? "unsafe_path"
            : code == "LimitExceeded" ? "limit" : code == "Cancelled" ? "cancelled"
            : code == "AlreadyExists" || code == "Conflict" || code == "Busy" ? "conflict"
            : code == "Unsupported" ? "unsupported_platform" : fallback;
        auto safe_message = std::string(message) + " (" + code;
        const auto& details = error.error().details;
        if (details.is_object() && details.contains("nativeError") && details["nativeError"].is_number_integer()) safe_message += ":" + details["nativeError"].dump();
        throw Error(std::string(domain_code), safe_message + ')');
    }
    catch (const std::exception&) { throw Error(std::string(fallback), std::string(message)); }
}
}
Error::Error(std::string code, std::string message)
    : nickel::ErrorException(nickel::Error{std::move(code), std::move(message)}) {}
const std::string& Error::code() const noexcept { return error().code; }
fs::path path_from_utf8(std::string_view value) {
    return foundation_call([&] { return nickel::path_from_utf8(value); }, "unsafe_path", "Path is not valid UTF-8");
}
std::string path_to_utf8(const fs::path& path) { return nickel::path_to_utf8(path); }
void reject_links(const fs::path& path, bool ancestors) {
    if (path.empty()) throw Error("unsafe_path", "Empty path");
    foundation_call([&] { nickel::reject_path_links(path, ancestors); }, "io", "Cannot inspect path safely");
}
fs::path checked_instance_directory(const fs::path& root, std::string_view id, bool must_exist) {
    validate_name(id);
    if (!root.is_absolute()) throw Error("unsafe_path", "Game root must be absolute");
    const auto directory = root / "versions" / path_from_utf8(id);
    reject_links(directory);
    if (must_exist && !fs::is_directory(detail::native_path(directory))) throw Error("missing_instance", "Instance directory does not exist");
    const auto relative = detail::logical_path(directory).lexically_relative(detail::logical_path(root));
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        throw Error("unsafe_path", "Instance escapes game root");
    return directory;
}
Json parse_legacy_json(std::string_view text, std::size_t limit) {
    if (text.size() > limit) throw Error("limit", "JSON exceeds size limit");
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    std::string cleaned;
    cleaned.reserve(text.size());
    bool quote = false;
    bool escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (!quote && c == '/' && i + 1 < text.size()) {
            if (text[i + 1] == '/') {
                i += 2;
                while (i < text.size() && text[i] != '\n' && text[i] != '\r') ++i;
                cleaned += ' ';
                if (i < text.size()) cleaned += text[i];
                continue;
            }
            if (text[i + 1] == '*') {
                i += 2;
                bool closed = false;
                for (; i + 1 < text.size(); ++i) {
                    if (text[i] == '*' && text[i + 1] == '/') { ++i; closed = true; break; }
                    if (text[i] == '\n' || text[i] == '\r') cleaned += text[i];
                }
                if (!closed) throw Error("invalid_json", "Unterminated JSON comment");
                cleaned += ' ';
                continue;
            }
        }
        cleaned += c;
        if (c == '"' && !escaped) quote = !quote;
        escaped = quote && c == '\\' && !escaped;
    }
    std::string result;
    quote = false;
    escaped = false;
    for (std::size_t i = 0; i < cleaned.size(); ++i) {
        const char c = cleaned[i];
        if (!quote && c == ',') {
            auto next = i + 1;
            while (next < cleaned.size() && std::isspace(static_cast<unsigned char>(cleaned[next]))) ++next;
            if (next < cleaned.size() && (cleaned[next] == '}' || cleaned[next] == ']')) continue;
        }
        result += c;
        if (c == '"' && !escaped) quote = !quote;
        escaped = quote && c == '\\' && !escaped;
    }
    return detail::parse_strict_json(result, limit);
}
} // namespace nickel::instances

namespace nickel::instances::detail {
std::string ascii_lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}
std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}
std::string random_id() {
    return foundation_call([] { return nickel::unique_token(); }, "random", "Cannot generate transaction identity");
}
std::uint64_t current_pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}
bool process_alive(std::uint64_t pid) {
    if (pid == 0) return false;
#ifdef _WIN32
    if (pid > std::numeric_limits<DWORD>::max()) return true;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    if (pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) return true;
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}
fs::path native_path(const fs::path& path) {
    return foundation_call([&] { return nickel::foundation_detail::native_filesystem_path(path); },
        "unsafe_path", "Cannot resolve native filesystem path");
}
fs::path logical_path(const fs::path& path) {
    return foundation_call([&] { return nickel::foundation_detail::checked_absolute_path(path); },
        "unsafe_path", "Cannot resolve logical filesystem path");
}
fs::path canonical_path(const fs::path& path) {
    reject_links(path);
    return foundation_call([&] {
        const auto logical = nickel::foundation_detail::checked_absolute_path(path);
#ifdef _WIN32
        auto ancestor = logical;
        std::vector<fs::path> suffix;
        for (;;) {
            const auto native = nickel::foundation_detail::native_filesystem_path(ancestor);
            const auto attributes = GetFileAttributesW(native.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) break;
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const auto code = GetLastError();
                if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
                    nickel::foundation_detail::io_fail("inspect canonical ancestor", ancestor, static_cast<int>(code));
            }
            const auto parent = ancestor.parent_path();
            if (parent.empty() || parent == ancestor)
                nickel::foundation_detail::fail("UnsafePath", "Canonical path has no existing directory ancestor");
            suffix.push_back(ancestor.filename());
            ancestor = parent;
        }
        nickel::foundation_detail::DirectoryGuard guard(ancestor);
        const auto handle = reinterpret_cast<HANDLE>(guard.native_handle());
        std::wstring text(512, L'\0');
        for (;;) {
            const auto size = GetFinalPathNameByHandleW(handle, text.data(), static_cast<DWORD>(text.size()),
                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (size == 0)
                nickel::foundation_detail::io_fail("resolve pinned canonical path", ancestor, static_cast<int>(GetLastError()));
            if (size < text.size()) { text.resize(size); break; }
            if (size >= 32767) nickel::foundation_detail::fail("LimitExceeded", "Canonical path exceeds the native limit");
            text.resize(static_cast<std::size_t>(size) + 1);
        }
        auto result = nickel::foundation_detail::checked_absolute_path(fs::path(text));
        for (auto item = suffix.rbegin(); item != suffix.rend(); ++item) result /= *item;
        guard.verify();
        return result;
#else
        return fs::weakly_canonical(logical);
#endif
    }, "unsafe_path", "Cannot resolve pinned canonical path");
}
bool path_exists(const fs::path& path) {
    reject_links(path);
    return foundation_call([&] { return !nickel::foundation_detail::path_missing(path); },
        "io", "Cannot inspect filesystem path");
}
void create_directories(const fs::path& path) {
    foundation_call([&] {
        nickel::foundation_detail::DirectoryGuard guard(path, true);
        guard.verify();
    }, "io", "Cannot create a pinned directory");
}
bool create_directory_no_replace(const fs::path& path) {
    return foundation_call([&] {
        const auto logical = nickel::foundation_detail::checked_absolute_path(path);
        if (logical == logical.root_path())
            nickel::foundation_detail::fail("UnsafePath", "Cannot reserve a filesystem root");
        nickel::foundation_detail::DirectoryGuard parent(logical.parent_path());
#ifdef _WIN32
        const auto native = nickel::foundation_detail::native_filesystem_path(logical);
        if (!CreateDirectoryW(native.c_str(), nullptr)) {
            const auto code = GetLastError();
            if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) return false;
            nickel::foundation_detail::io_fail("reserve pinned directory", logical, static_cast<int>(code));
        }
#else
        std::error_code error;
        if (!fs::create_directory(logical, error)) {
            if (!error || error == std::errc::file_exists) return false;
            nickel::foundation_detail::io_fail("reserve pinned directory", logical, error.value());
        }
#endif
        nickel::foundation_detail::DirectoryGuard created(logical);
        parent.verify();
        created.verify();
        return true;
    }, "io", "Cannot atomically reserve a pinned directory");
}
std::vector<DirectoryEntry> directory_entries(const fs::path& directory, bool recursive,
                                               std::stop_token cancellation) {
    return foundation_call([&] {
        std::vector<DirectoryEntry> result;
        std::size_t path_bytes = 0;
        const auto append = [&](DirectoryEntry entry) {
            const auto bytes = path_to_utf8(entry.path()).size() + sizeof(DirectoryEntry);
            if (result.size() >= 100000 || bytes > 64 * 1024 * 1024 - path_bytes)
                nickel::foundation_detail::fail("LimitExceeded", "Directory enumeration exceeds its quota");
            path_bytes += bytes;
            result.push_back(std::move(entry));
        };
        const auto visit = [&](auto&& self, const fs::path& current, std::size_t depth) -> void {
            if (cancellation.stop_requested()) nickel::foundation_detail::fail("Cancelled", "Directory enumeration was cancelled");
            if (depth > 128) nickel::foundation_detail::fail("LimitExceeded", "Directory enumeration exceeds its depth quota");
            nickel::foundation_detail::DirectoryGuard guard(current);
#ifdef _WIN32
            // MinGW's extended-path directory iterator can enumerate CWD instead of this directory.
            auto pattern = nickel::foundation_detail::native_filesystem_path(current).native();
            pattern += L"\\*";
            WIN32_FIND_DATAW data{};
            struct Search {
                HANDLE handle;
                ~Search() { if (handle != INVALID_HANDLE_VALUE) FindClose(handle); }
            } search{FindFirstFileW(pattern.c_str(), &data)};
            if (search.handle == INVALID_HANDLE_VALUE) {
                const auto code = GetLastError();
                if (code != ERROR_FILE_NOT_FOUND && code != ERROR_NO_MORE_FILES)
                    nickel::foundation_detail::io_fail("enumerate pinned directory", current, static_cast<int>(code));
                guard.verify();
                return;
            }
            for (;;) {
                if (cancellation.stop_requested()) nickel::foundation_detail::fail("Cancelled", "Directory enumeration was cancelled");
                const std::wstring_view name(data.cFileName);
                if (name != L"." && name != L"..") {
                    const auto child = nickel::foundation_detail::checked_absolute_path(current / fs::path(name));
                    const auto type = data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ? fs::file_type::symlink
                        : data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? fs::file_type::directory
                        : data.dwFileAttributes & FILE_ATTRIBUTE_DEVICE ? fs::file_type::unknown : fs::file_type::regular;
                    append({child, type, (std::uintmax_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow});
                    if (recursive && type == fs::file_type::directory) self(self, child, depth + 1);
                    if (recursive && type != fs::file_type::directory && type != fs::file_type::regular)
                        nickel::foundation_detail::fail("UnsafePath", "Special file or link in directory tree");
                }
                if (!FindNextFileW(search.handle, &data)) {
                    const auto code = GetLastError();
                    if (code != ERROR_NO_MORE_FILES)
                        nickel::foundation_detail::io_fail("enumerate pinned directory", current, static_cast<int>(code));
                    break;
                }
            }
#else
            for (const auto& entry : fs::directory_iterator(current)) {
                if (cancellation.stop_requested()) nickel::foundation_detail::fail("Cancelled", "Directory enumeration was cancelled");
                const auto type = entry.symlink_status().type();
                append({entry.path(), type, type == fs::file_type::regular ? entry.file_size() : 0});
                if (recursive && type == fs::file_type::directory) self(self, entry.path(), depth + 1);
                if (recursive && type != fs::file_type::directory && type != fs::file_type::regular)
                    nickel::foundation_detail::fail("UnsafePath", "Special file or link in directory tree");
            }
#endif
            guard.verify();
        };
        visit(visit, nickel::foundation_detail::checked_absolute_path(directory), 0);
        return result;
    }, "io", "Cannot enumerate a pinned directory");
}
void verify_directory_tree(const fs::path& directory) {
    (void)directory_entries(directory, true);
}
std::string read_bounded(const fs::path& path, std::size_t limit) {
    return foundation_call([&] {
        const auto bytes = nickel::read_file_bounded(path, limit);
        return std::string(bytes.begin(), bytes.end());
    }, "io", "Cannot read bounded regular file safely");
}
Json parse_strict_json(std::string_view text, std::size_t limit) {
    if (text.size() > limit) throw Error("limit", "JSON exceeds size limit");
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 64) throw Error("invalid_json", "JSON nesting exceeds limit");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key) {
            const auto key = value.get<std::string>();
            if (keys.empty() || !keys.back().insert(key).second) throw Error("invalid_json", "Duplicate JSON key");
        }
        if (event == Json::parse_event_t::object_end && !keys.empty()) keys.pop_back();
        return true;
    };
    try { return Json::parse(text.begin(), text.end(), callback); }
    catch (const Error&) { throw; }
    catch (const Json::exception&) { throw Error("invalid_json", "Invalid JSON document"); }
}
std::optional<Json> read_optional_json(const fs::path& path, std::size_t limit, bool lenient) {
    if (!path_exists(path)) return {};
    const auto text = read_bounded(path, limit);
    return lenient ? parse_legacy_json(text, limit) : parse_strict_json(text, limit);
}
void check_cancelled(std::stop_token token) {
    if (token.stop_requested()) throw Error("cancelled", "Operation cancelled");
}
void write_exclusive(const fs::path& path, std::string_view content) {
    foundation_call([&] {
        nickel::create_file_exclusive(path, {reinterpret_cast<const std::uint8_t*>(content.data()), content.size()}, true);
    }, "io", "Cannot exclusively create durable file");
}
void rename_no_replace(const fs::path& source, const fs::path& destination) {
    const auto moved = foundation_call([&] { return nickel::rename_no_replace(source, destination); }, "io", "Cannot publish owned file safely");
    if (!moved) throw Error("conflict", "Destination already exists");
}
void copy_no_replace(const fs::path& source, const fs::path& destination, std::uint64_t max_bytes,
                      std::stop_token cancellation, std::string_view expected_sha256) {
    check_cancelled(cancellation);
    nickel::Cancellation shared;
    std::stop_callback bridge(cancellation, [&] { shared.cancel(); });
    const auto copied = foundation_call([&] {
        return nickel::atomic_copy_no_replace(source, destination, max_bytes, &shared, expected_sha256);
    }, "io", "Cannot publish a verified bounded file safely");
    if (!copied) throw Error("conflict", "Destination already exists");
}
void atomic_write(const fs::path& path, std::string_view content, bool replace) {
    if (replace) {
        foundation_call([&] { nickel::atomic_write_text(path, content, {.private_file = true}); }, "io", "Atomic replacement or durability confirmation failed");
        return;
    }
    const auto created = foundation_call([&] {
        nickel::foundation_detail::DirectoryGuard parent(path.parent_path(), true);
        return nickel::foundation_detail::atomic_create_file(path, {reinterpret_cast<const std::uint8_t*>(content.data()), content.size()}, true);
    }, "io", "Cannot atomically create owned file");
    if (!created) throw Error("conflict", "Destination already exists");
}
void remove_owned_tree(const fs::path& path) {
    reject_links(path);
    if (!path_exists(path)) return;
    const auto root = logical_path(path);
    verify_directory_tree(root);
    foundation_call([&] {
        nickel::foundation_detail::DirectoryGuard parent(root.parent_path());
#ifdef _WIN32
        struct File {
            HANDLE handle;
            ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        };
        const auto remove = [&](auto&& self, const fs::path& current, bool directory) -> void {
            BY_HANDLE_FILE_INFORMATION original{};
            if (directory) {
                nickel::foundation_detail::DirectoryGuard guard(current);
                const auto handle = reinterpret_cast<HANDLE>(guard.native_handle());
                if (!GetFileInformationByHandle(handle, &original))
                    nickel::foundation_detail::io_fail("inspect owned directory", current, static_cast<int>(GetLastError()));
                for (const auto& entry : directory_entries(current)) {
                    if (!entry.is_directory() && !entry.is_regular_file())
                        nickel::foundation_detail::fail("UnsafePath", "Special file or link in owned tree");
                    self(self, entry.path(), entry.is_directory());
                }
                guard.verify();
            }
            const auto native = nickel::foundation_detail::native_filesystem_path(current);
            File file{CreateFileW(native.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
            if (file.handle == INVALID_HANDLE_VALUE)
                nickel::foundation_detail::io_fail("open owned deletion target", current, static_cast<int>(GetLastError()));
            BY_HANDLE_FILE_INFORMATION identity{};
            if (!GetFileInformationByHandle(file.handle, &identity))
                nickel::foundation_detail::io_fail("inspect owned deletion target", current, static_cast<int>(GetLastError()));
            if ((identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || GetFileType(file.handle) != FILE_TYPE_DISK
                || bool(identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
                nickel::foundation_detail::fail("UnsafePath", "Owned deletion target changed type");
            if (directory && (original.dwVolumeSerialNumber != identity.dwVolumeSerialNumber
                || original.nFileIndexHigh != identity.nFileIndexHigh || original.nFileIndexLow != identity.nFileIndexLow))
                nickel::foundation_detail::fail("Conflict", "Owned directory changed identity before deletion");
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(file.handle, FileDispositionInfo, &disposition, sizeof(disposition)))
                nickel::foundation_detail::io_fail("delete owned target", current, static_cast<int>(GetLastError()));
        };
        remove(remove, root, true);
#else
        fs::remove_all(root);
#endif
        parent.verify();
    }, "io", "Cannot remove owned directory tree");
}
void strict_keys(const Json& value, std::span<const std::string_view> allowed,
                 std::span<const std::string_view> required) {
    if (!value.is_object()) throw Error("invalid_schema", "Expected JSON object");
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end()) throw Error("invalid_schema", "Unknown schema field: " + it.key());
    }
    for (const auto key : required) {
        if (!value.contains(std::string(key))) throw Error("invalid_schema", "Missing required field: " + std::string(key));
    }
}
std::optional<std::uint64_t> unsigned_value(const Json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) return static_cast<std::uint64_t>(value.get<std::int64_t>());
    return {};
}
std::optional<std::string> string_at(const Json& value, std::string_view key) {
    const auto it = value.find(std::string(key));
    if (it != value.end() && it->is_string()) return it->get<std::string>();
    return {};
}
DirectoryLock::DirectoryLock(const fs::path& directory)
    : lock_(foundation_call([&] { return std::make_unique<nickel::FileLock>(directory / ".nickel-instances.lock"); }, "io", "Cannot acquire domain lock")) {}
DirectoryLock::~DirectoryLock() = default;
} // namespace nickel::instances::detail
