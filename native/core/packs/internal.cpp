#include "internal.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nickel::packs::detail {
namespace {
#ifdef _WIN32
HANDLE native(std::intptr_t handle) { return reinterpret_cast<HANDLE>(handle); }
void inspect_handle(HANDLE handle, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)
        || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        || static_cast<bool>(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory
        || (!directory && (GetFileType(handle) != FILE_TYPE_DISK || info.nNumberOfLinks != 1)))
        throw Error("unsafe_path", "File identity is not a plain owned file");
}
std::uint64_t ticks(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
#endif
}
NativeFile::NativeFile(const fs::path& path, Mode mode) : mode_(mode) {
    if (!path.is_absolute()) throw Error("unsafe_path", "File must use an absolute path");
    for (const auto& component : path.parent_path().relative_path())
        if (component == "." || component == "..") throw Error("unsafe_path", "Noncanonical file path");
    path_ = io::logical_path(path);
    reject_links(path_);
    try {
#ifdef _WIN32
        auto part = path_.parent_path().root_path();
        const auto pin = [&](const fs::path& parent) {
            const auto native_parent = io::native_path(parent);
            const auto h = CreateFileW(native_parent.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h == INVALID_HANDLE_VALUE) throw Error("io", "Cannot pin file parent");
            parents_.push_back(reinterpret_cast<std::intptr_t>(h));
            inspect_handle(h, true);
        };
        pin(part);
        for (const auto& component : path_.parent_path().relative_path()) {
            part /= component;
            pin(part);
        }
        const auto native_file = io::native_path(path_);
        const auto h = CreateFileW(native_file.c_str(), mode == Mode::read ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
                                  mode == Mode::read ? FILE_SHARE_READ : 0, nullptr,
                                  mode == Mode::read ? OPEN_EXISTING : CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) throw Error(mode == Mode::read ? "io" : "conflict", "Cannot open exclusive plain file");
        handle_ = reinterpret_cast<std::intptr_t>(h);
        inspect_handle(h, false);
#else
        int parent = open(path_.root_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (parent < 0) throw Error("io", "Cannot open file root");
        parents_.push_back(parent);
        for (const auto& component : path_.parent_path().relative_path()) {
            parent = openat(parent, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (parent < 0) throw Error("unsafe_path", "Cannot open plain file parent");
            parents_.push_back(parent);
        }
        const auto flags = O_CLOEXEC | O_NOFOLLOW | (mode == Mode::read ? O_RDONLY : O_RDWR | O_CREAT | O_EXCL);
        const auto h = openat(parent, path_.filename().c_str(), flags, 0600);
        if (h < 0) throw Error(mode == Mode::read ? "io" : "conflict", "Cannot open exclusive plain file");
        handle_ = h;
        struct stat info{};
        if (fstat(h, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1)
            throw Error("unsafe_path", "File identity is not a plain owned file");
#endif
        original_size_ = size();
        original_identity_ = identity();
    } catch (...) { close(); throw; }
}
NativeFile::~NativeFile() { close(); }
void NativeFile::close() noexcept {
#ifdef _WIN32
    if (handle_ != -1) CloseHandle(native(handle_));
    for (auto parent : parents_) CloseHandle(native(parent));
#else
    if (handle_ != -1) ::close(static_cast<int>(handle_));
    for (auto parent : parents_) ::close(static_cast<int>(parent));
#endif
    handle_ = -1;
    parents_.clear();
}
std::size_t NativeFile::read_at(std::uint64_t offset, void* buffer, std::size_t count) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) throw Error("limit", "File offset exceeds limit");
    std::size_t done = 0;
    while (done < count) {
        const auto chunk = std::min(count - done, copy_buffer_bytes);
#ifdef _WIN32
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset + done);
        DWORD read = 0;
        if (!SetFilePointerEx(native(handle_), position, nullptr, FILE_BEGIN)
            || !ReadFile(native(handle_), static_cast<char*>(buffer) + done, static_cast<DWORD>(chunk), &read, nullptr))
            throw Error("io", "Cannot read plain file");
#else
        const auto read = pread(static_cast<int>(handle_), static_cast<char*>(buffer) + done, chunk, static_cast<off_t>(offset + done));
        if (read < 0 && errno == EINTR) continue;
        if (read < 0) throw Error("io", "Cannot read plain file");
#endif
        done += static_cast<std::size_t>(read);
        if (static_cast<std::size_t>(read) < chunk) break;
    }
    return done;
}
void NativeFile::write_at(std::uint64_t offset, const void* buffer, std::size_t count) {
    if (mode_ != Mode::exclusive || offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw Error("io", "Invalid exclusive write");
    std::size_t done = 0;
    while (done < count) {
        const auto chunk = std::min(count - done, copy_buffer_bytes);
#ifdef _WIN32
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset + done);
        DWORD written = 0;
        if (!SetFilePointerEx(native(handle_), position, nullptr, FILE_BEGIN)
            || !WriteFile(native(handle_), static_cast<const char*>(buffer) + done, static_cast<DWORD>(chunk), &written, nullptr))
            throw Error("io", "Cannot write exclusive file");
#else
        const auto written = pwrite(static_cast<int>(handle_), static_cast<const char*>(buffer) + done, chunk, static_cast<off_t>(offset + done));
        if (written < 0 && errno == EINTR) continue;
        if (written < 0) throw Error("io", "Cannot write exclusive file");
#endif
        if (!written) throw Error("io", "Short exclusive write");
        done += static_cast<std::size_t>(written);
    }
}
std::uint64_t NativeFile::size() const {
#ifdef _WIN32
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(native(handle_), &size) || size.QuadPart < 0) throw Error("io", "Cannot inspect file size");
    return static_cast<std::uint64_t>(size.QuadPart);
#else
    struct stat info{};
    if (fstat(static_cast<int>(handle_), &info) || info.st_size < 0) throw Error("io", "Cannot inspect file size");
    return static_cast<std::uint64_t>(info.st_size);
#endif
}
std::array<std::uint64_t, 4> NativeFile::identity() const {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(native(handle_), &info)) throw Error("io", "Cannot inspect file identity");
    return {info.dwVolumeSerialNumber, (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
            ticks(info.ftCreationTime), ticks(info.ftLastWriteTime)};
#else
    struct stat info{};
    if (fstat(static_cast<int>(handle_), &info)) throw Error("io", "Cannot inspect file identity");
#if defined(__APPLE__)
    const auto modified = info.st_mtimespec;
    const auto changed = info.st_ctimespec;
#else
    const auto modified = info.st_mtim;
    const auto changed = info.st_ctim;
#endif
    return {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino),
            static_cast<std::uint64_t>(modified.tv_sec) * 1000000000ULL + modified.tv_nsec,
            static_cast<std::uint64_t>(changed.tv_sec) * 1000000000ULL + changed.tv_nsec};
#endif
}
void NativeFile::unchanged() const {
    reject_links(path_);
#ifdef _WIN32
    inspect_handle(native(handle_), false);
#endif
    if (mode_ == Mode::read && (size() != original_size_ || identity() != original_identity_))
        throw Error("source_changed", "File changed during bounded operation");
#ifndef _WIN32
    NativeFile current(path_, Mode::read);
    if (current.identity()[0] != original_identity_[0] || current.identity()[1] != original_identity_[1])
        throw Error("source_changed", "File path identity changed");
#endif
}
void NativeFile::flush() {
#ifdef _WIN32
    if (!FlushFileBuffers(native(handle_))) throw Error("io", "Cannot flush exclusive file");
#else
    if (fsync(static_cast<int>(handle_))) throw Error("io", "Cannot flush exclusive file");
#endif
}
Workspace::Workspace(const fs::path& parent) : owner_(io::random_id()) {
    if (!parent.is_absolute()) throw Error("unsafe_path", "Workspace parent must be absolute");
    const auto logical_parent = io::logical_path(parent);
    reject_links(logical_parent);
    io::create_directories(logical_parent);
    directory_ = logical_parent / (".nickel-export-" + owner_);
    if (!io::create_directory_no_replace(directory_)) throw Error("conflict", "Cannot reserve private workspace");
    try { io::write_exclusive(directory_ / ".nickel-export.owner", owner_); }
    catch (...) { std::error_code ec; fs::remove(io::native_path(directory_), ec); throw; }
}
Workspace::~Workspace() { try { cleanup(); } catch (...) {} }
const fs::path& Workspace::directory() const noexcept { return directory_; }
void Workspace::cleanup() {
    if (directory_.empty()) return;
    reject_links(directory_);
    if (!io::path_exists(directory_)) return;
    if (io::read_bounded(directory_ / ".nickel-export.owner", 64) != owner_)
        throw Error("ownership_changed", "Workspace ownership changed");
    io::remove_owned_tree(directory_);
    directory_.clear();
}
std::uint64_t copy_file(const fs::path& source, const fs::path& target,
                        std::uint64_t maximum, std::stop_token cancel) {
    io::check_cancelled(cancel);
    NativeFile input(source, NativeFile::Mode::read);
    const auto size = input.size();
    if (size > maximum) throw Error("limit", "Snapshot file exceeds quota");
    reject_links(target);
    io::create_directories(target.parent_path());
    NativeFile output(target, NativeFile::Mode::exclusive);
    std::array<char, copy_buffer_bytes> buffer{};
    std::uint64_t offset = 0;
    while (offset < size) {
        io::check_cancelled(cancel);
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
        if (input.read_at(offset, buffer.data(), chunk) != chunk) throw Error("source_changed", "Snapshot source changed");
        output.write_at(offset, buffer.data(), chunk);
        offset += chunk;
    }
    input.unchanged();
    io::check_cancelled(cancel);
    output.flush();
    return size;
}
bool within(std::string_view path, std::string_view directory) {
    return path == directory || (path.starts_with(directory) && path.size() > directory.size() && path[directory.size()] == '/');
}
std::string required_text(const Json& value, std::string_view key, std::size_t maximum) {
    const auto text = io::string_at(value, key);
    if (!text || text->empty() || text->size() > maximum || io::trim(*text).empty()
        || !valid_utf8(*text) || std::any_of(text->begin(), text->end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw Error("invalid_schema", "Invalid bounded text field: " + std::string(key));
    return *text;
}
std::uint64_t required_unsigned(const Json& value, std::string_view key, std::uint64_t maximum) {
    const auto it = value.find(std::string(key));
    const auto number = it == value.end() ? std::nullopt : io::unsigned_value(*it);
    if (!number || *number > maximum) throw Error("invalid_schema", "Invalid unsigned field: " + std::string(key));
    return *number;
}
std::optional<std::string> optional_hash(const Json& hashes, std::string_view key, std::size_t length) {
    if (!hashes.contains(std::string(key))) return {};
    const auto value = io::string_at(hashes, key);
    if (!value || value->size() != length || !std::all_of(value->begin(), value->end(), [](unsigned char c) { return std::isxdigit(c); }))
        throw Error("invalid_schema", "Invalid digest field");
    return io::ascii_lower(*value);
}
Json file_to_json(const PackFile& file) {
    Json hashes = Json::object();
    if (file.sha1) hashes["sha1"] = *file.sha1;
    if (file.sha512) hashes["sha512"] = *file.sha512;
    return {{"path", file.path}, {"fileSize", file.size}, {"hashes", hashes}, {"downloads", file.downloads}};
}
void verify_pack_file(const PackFile& file, const fs::path& path, std::stop_token cancel) {
    io::check_cancelled(cancel);
    NativeFile pin(path, NativeFile::Mode::read);
    if (pin.size() != file.size) throw Error("integrity", "Pack file size mismatch");
    if (!file.sha1 && !file.sha512) throw Error("invalid_schema", "Pack file requires a digest");
    if (file.sha1 && hash_file(path, "sha1", cancel) != *file.sha1) throw Error("integrity", "Pack SHA-1 mismatch");
    if (file.sha512 && hash_file(path, "sha512", cancel) != *file.sha512) throw Error("integrity", "Pack SHA-512 mismatch");
    pin.unchanged();
    io::check_cancelled(cancel);
}
} // namespace nickel::packs::detail
