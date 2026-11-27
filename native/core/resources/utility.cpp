#include "utility.hpp"
#include "mods.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <random>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <stdio.h>
#endif
#endif

namespace nickel::resources::detail {
namespace {

[[noreturn]] void pathDenied() { fail("policy_denied", "Only non-linked regular files in the resolved resource root are permitted"); }
[[noreturn]] void ioFailed(const char* message) { fail("io", message); }

// Walks every ancestor of an absolute path and rejects links/reparse points. The depth
// cap bounds hostile deep paths; allowMissing applies to the leaf only.
void rejectLinks(const Path& raw, bool allowMissing = false) {
    if (raw.empty() || !raw.is_absolute()) pathDenied();
    const auto encoded = utf8Path(raw);
    if (!validUtf8(encoded) || encoded.starts_with("//") || encoded.starts_with("\\\\")) pathDenied();
    for (const auto& part : raw) if (part == "..") pathDenied();
    const auto path = raw.lexically_normal();
    auto current = path;
    bool leaf = true;
    for (unsigned depth = 0; depth < 128; ++depth) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error && error != std::errc::no_such_file_or_directory) ioFailed("Cannot inspect resource path");
        if (std::filesystem::is_symlink(status)) pathDenied();
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) pathDenied();
        if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) ioFailed("Cannot inspect resource path attributes");
#endif
        if (!std::filesystem::exists(status) && !(leaf && allowMissing)) ioFailed("Resource path does not exist");
        const auto parent = current.parent_path();
        if (parent == current || parent.empty()) return;
        current = parent;
        leaf = false;
    }
    pathDenied();
}

bool pathExists(const Path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) ioFailed("Cannot inspect resource destination");
    return std::filesystem::exists(status);
}

#ifdef _WIN32
class DirectoryPin final {
public:
    explicit DirectoryPin(const Path& path) {
        auto current = path;
        try {
            for (;;) {
                const auto handle = CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (handle == INVALID_HANDLE_VALUE) ioFailed("Cannot pin resource directory");
                handles_.push_back(handle);
                BY_HANDLE_FILE_INFORMATION information{};
                if (!GetFileInformationByHandle(handle, &information)) ioFailed("Cannot inspect pinned resource directory");
                if (!(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) pathDenied();
                const auto parent = current.parent_path();
                if (parent == current || parent.empty()) break;
                current = parent;
            }
        } catch (...) { close(); throw; }
    }
    ~DirectoryPin() { close(); }
private:
    std::vector<HANDLE> handles_;
    void close() { for (const auto handle : handles_) CloseHandle(handle); handles_.clear(); }
};

bool sameInformation(const BY_HANDLE_FILE_INFORMATION& first, const BY_HANDLE_FILE_INFORMATION& second) {
    return first.dwVolumeSerialNumber == second.dwVolumeSerialNumber && first.nFileIndexHigh == second.nFileIndexHigh &&
        first.nFileIndexLow == second.nFileIndexLow && first.nFileSizeHigh == second.nFileSizeHigh && first.nFileSizeLow == second.nFileSizeLow &&
        first.ftLastWriteTime.dwHighDateTime == second.ftLastWriteTime.dwHighDateTime && first.ftLastWriteTime.dwLowDateTime == second.ftLastWriteTime.dwLowDateTime &&
        second.nNumberOfLinks == 1 && !(second.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
}
#else
class DirectoryPin final {
public:
    explicit DirectoryPin(const Path& path) {
        handle_ = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (handle_ < 0) ioFailed("Cannot open resource path root");
        try {
            for (const auto& component : path.relative_path()) {
                if (component == "." || component.empty()) continue;
                if (component == "..") pathDenied();
                const auto next = ::openat(handle_, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (next < 0) ioFailed("Cannot pin resource directory");
                ::close(handle_);
                handle_ = next;
            }
        } catch (...) { ::close(handle_); handle_ = -1; throw; }
    }
    ~DirectoryPin() { if (handle_ >= 0) ::close(handle_); }
    int handle() const { return handle_; }
private:
    int handle_ = -1;
};

bool sameInformation(const struct stat& first, const struct stat& second) {
    if (first.st_dev != second.st_dev || first.st_ino != second.st_ino || first.st_size != second.st_size || second.st_nlink != 1 || !S_ISREG(second.st_mode)) return false;
#ifdef __APPLE__
    return first.st_mtimespec.tv_sec == second.st_mtimespec.tv_sec && first.st_mtimespec.tv_nsec == second.st_mtimespec.tv_nsec &&
        first.st_ctimespec.tv_sec == second.st_ctimespec.tv_sec && first.st_ctimespec.tv_nsec == second.st_ctimespec.tv_nsec;
#else
    return first.st_mtim.tv_sec == second.st_mtim.tv_sec && first.st_mtim.tv_nsec == second.st_mtim.tv_nsec &&
        first.st_ctim.tv_sec == second.st_ctim.tv_sec && first.st_ctim.tv_nsec == second.st_ctim.tv_nsec;
#endif
}
#endif

} // namespace

Path checkedDirectory(const Path& raw, bool create) {
    rejectLinks(raw, create);
    if (create && !pathExists(raw)) {
        const auto parent = checkedDirectory(raw.parent_path());
        DirectoryPin pin(parent);
        std::error_code error;
        std::filesystem::create_directory(raw, error);
        if (error) ioFailed("Cannot create resource directory");
    }
    rejectLinks(raw);
    std::error_code error;
    if (!std::filesystem::is_directory(raw, error) || error) pathDenied();
    const auto result = std::filesystem::canonical(raw, error);
    if (error) ioFailed("Cannot resolve resource directory");
    return result;
}

// A single hard link is part of the contract: extra links would let another name swap
// content behind the canonical path after validation.
Path checkedRegularFile(const Path& raw) {
    rejectLinks(raw);
    std::error_code error;
    if (!std::filesystem::is_regular_file(raw, error) || error) pathDenied();
    const auto links = std::filesystem::hard_link_count(raw, error);
    if (error || links != 1) pathDenied();
    const auto path = std::filesystem::canonical(raw, error);
    if (error) ioFailed("Cannot resolve resource file");
    return path;
}

Path checkedLeaf(const Path& rawRoot, std::string_view name, bool requireMod) {
    if (!validResourceFileName(name) || (requireMod && !splitModFileName(name))) pathDenied();
    const auto root = checkedDirectory(rawRoot);
    const auto path = checkedRegularFile(root / pathFromUtf8(name));
    if (path.parent_path() != root) pathDenied();
    return path;
}

void checkedMissingLeaf(const Path& rawRoot, std::string_view name) {
    if (!validResourceFileName(name)) pathDenied();
    const auto root = checkedDirectory(rawRoot);
    const auto target = root / pathFromUtf8(name);
    rejectLinks(target, true);
    if (pathExists(target)) fail("conflict", "Resource destination already exists");
}

std::string fileRevision(const Path& path) {
    const auto regular = checkedRegularFile(path);
    std::error_code error;
    const auto size = std::filesystem::file_size(regular, error);
    if (error) ioFailed("Cannot inspect resource size");
    const auto time = std::filesystem::last_write_time(regular, error);
    if (error) ioFailed("Cannot inspect resource modification time");
    return std::to_string(size) + ':' + std::to_string(time.time_since_epoch().count());
}

std::string temporaryName(std::string_view operation) {
    static std::atomic<std::uint64_t> sequence{0};
    static const auto nonce = [] { std::random_device random; return (std::uint64_t(random()) << 32) | random(); }();
    return ".nickel-" + std::string(operation) + '-' + std::to_string(nonce) + '-' + std::to_string(sequence.fetch_add(1));
}

struct FileReader::State {
    Path path;
    DirectoryPin parent;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION before{};
#else
    int handle = -1;
    struct stat before{};
#endif
    std::uint64_t length = 0;
    std::uint64_t offset = 0;
    explicit State(Path file) : path(std::move(file)), parent(path.parent_path()) {}
    ~State() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        if (handle >= 0) ::close(handle);
#endif
    }
};

FileReader::FileReader(const Path& raw, std::uint64_t maximumBytes)
    : state_(std::make_unique<State>(checkedRegularFile(raw))) {
    auto& state = *state_;
#ifdef _WIN32
    state.handle = CreateFileW(state.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (state.handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(state.handle, &state.before)) ioFailed("Cannot open resource file");
    if (state.before.nNumberOfLinks != 1 || (state.before.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) pathDenied();
    state.length = (std::uint64_t(state.before.nFileSizeHigh) << 32) | state.before.nFileSizeLow;
#else
    state.handle = ::openat(state.parent.handle(), state.path.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (state.handle < 0 || ::fstat(state.handle, &state.before)) ioFailed("Cannot open resource file");
    if (!S_ISREG(state.before.st_mode) || state.before.st_nlink != 1 || state.before.st_size < 0) pathDenied();
    state.length = std::uint64_t(state.before.st_size);
#endif
    if (state.length > maximumBytes) fail("too_large", "Resource file exceeds read budget");
}
FileReader::~FileReader() = default;
std::uint64_t FileReader::size() const { return state_->length; }

std::size_t FileReader::read(std::span<std::uint8_t> bytes, const Cancellation& cancel) {
    cancel.check();
    if (bytes.empty()) return 0;
    std::size_t count = 0;
#ifdef _WIN32
    DWORD received = 0;
    if (!ReadFile(state_->handle, bytes.data(), DWORD(std::min<std::size_t>(bytes.size(), 65536)), &received, nullptr)) ioFailed("Cannot read resource file");
    count = received;
#else
    ssize_t received = 0;
    do { received = ::read(state_->handle, bytes.data(), std::min<std::size_t>(bytes.size(), 65536)); } while (received < 0 && errno == EINTR && !cancel.cancelled());
    cancel.check();
    if (received < 0) ioFailed("Cannot read resource file");
    count = std::size_t(received);
#endif
    if (count > state_->length - state_->offset) fail("conflict", "Resource grew while reading");
    state_->offset += count;
    if (!count && state_->offset != state_->length) fail("conflict", "Resource shrank while reading");
    return count;
}

void FileReader::rewind() {
    verifyUnchanged();
#ifdef _WIN32
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(state_->handle, zero, nullptr, FILE_BEGIN)) ioFailed("Cannot rewind resource file");
#else
    if (::lseek(state_->handle, 0, SEEK_SET) < 0) ioFailed("Cannot rewind resource file");
#endif
    state_->offset = 0;
}

void FileReader::verifyUnchanged() const {
    checkedRegularFile(state_->path);
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION after{};
    if (!GetFileInformationByHandle(state_->handle, &after)) ioFailed("Cannot verify resource file");
    const auto pathHandle = CreateFileW(state_->path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (pathHandle == INVALID_HANDLE_VALUE) fail("conflict", "Resource path changed while reading");
    BY_HANDLE_FILE_INFORMATION pathInformation{};
    const bool valid = GetFileInformationByHandle(pathHandle, &pathInformation) && sameInformation(state_->before, after) && sameInformation(state_->before, pathInformation);
    CloseHandle(pathHandle);
#else
    struct stat after{}, pathInformation{};
    const bool valid = !::fstat(state_->handle, &after) && !::fstatat(state_->parent.handle(), state_->path.filename().c_str(), &pathInformation, AT_SYMLINK_NOFOLLOW) &&
        sameInformation(state_->before, after) && sameInformation(state_->before, pathInformation);
#endif
    if (!valid) fail("conflict", "Resource changed while reading");
}

// No-replace is the contract: failure is reported instead of overwriting a competing
// file, and a platform without the primitive fails to compile (see the #error below).
void renameExclusive(const Path& rawRoot, std::string_view source, std::string_view target) {
    const auto root = checkedDirectory(rawRoot);
    DirectoryPin pin(root);
    const auto from = checkedLeaf(root, source, false);
    checkedMissingLeaf(root, target);
    const auto to = root / pathFromUtf8(target);
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) fail("conflict", "Resource rename failed without replacing another file");
#elif defined(__linux__)
    if (::syscall(SYS_renameat2, pin.handle(), from.filename().c_str(), pin.handle(), to.filename().c_str(), 1U)) fail("conflict", "Resource rename failed without replacing another file");
#elif defined(__APPLE__)
    if (::renameatx_np(pin.handle(), from.filename().c_str(), pin.handle(), to.filename().c_str(), RENAME_EXCL)) fail("conflict", "Resource rename failed without replacing another file");
#else
#error "A no-replace resource rename adapter is required on this platform"
#endif
    checkedLeaf(root, target, false);
}

void removeRegular(const Path& rawRoot, std::string_view name) {
    const auto root = checkedDirectory(rawRoot);
    DirectoryPin pin(root);
    const auto path = checkedLeaf(root, name, false);
#ifdef _WIN32
    if (!DeleteFileW(path.c_str())) ioFailed("Cannot remove resource recovery file");
#else
    if (::unlinkat(pin.handle(), path.filename().c_str(), 0)) ioFailed("Cannot remove resource recovery file");
#endif
}

// The source is re-verified after the copy: growth, shrink, or replacement while
// reading is a conflict, and the partial target is removed on any failure.
void copyExclusive(const Path& source, const Path& rawRoot, std::string_view target,
                   std::uint64_t maximumBytes, const Cancellation& cancel) {
    cancel.check();
    FileReader input(source, maximumBytes);
    const auto root = checkedDirectory(rawRoot);
    DirectoryPin pin(root);
    checkedMissingLeaf(root, target);
    const auto destination = root / pathFromUtf8(target);
#ifdef _WIN32
    const auto output = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (output == INVALID_HANDLE_VALUE) fail("conflict", "Cannot create new resource file");
#else
    const auto output = ::openat(pin.handle(), destination.filename().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0) fail("conflict", "Cannot create new resource file");
#endif
    try {
        std::array<std::uint8_t, 65536> bytes{};
        for (auto count = input.read(bytes, cancel); count; count = input.read(bytes, cancel)) {
            std::size_t offset = 0;
            while (offset < count) {
                cancel.check();
#ifdef _WIN32
                DWORD written = 0;
                if (!WriteFile(output, bytes.data() + offset, DWORD(count - offset), &written, nullptr) || !written) ioFailed("Cannot copy resource file");
                offset += written;
#else
                const auto written = ::write(output, bytes.data() + offset, count - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) ioFailed("Cannot copy resource file");
                offset += std::size_t(written);
#endif
            }
        }
        input.verifyUnchanged();
        cancel.check();
#ifdef _WIN32
        if (!FlushFileBuffers(output)) ioFailed("Cannot flush resource copy");
#else
        if (::fsync(output)) ioFailed("Cannot flush resource copy");
#endif
    } catch (...) {
#ifdef _WIN32
        CloseHandle(output);
        DeleteFileW(destination.c_str());
#else
        ::close(output);
        ::unlinkat(pin.handle(), destination.filename().c_str(), 0);
#endif
        throw;
    }
#ifdef _WIN32
    CloseHandle(output);
#else
    ::close(output);
#endif
    checkedLeaf(root, target, false);
}

// Depth is enforced in the SAX callback before the DOM exists; nlohmann has no default
// nesting limit, and this input is remote.
Json parseBoundedJson(std::span<const std::uint8_t> bytes, std::size_t maximumBytes) {
    if (bytes.size() > maximumBytes) fail("too_large", "JSON exceeds its byte budget");
    if (bytes.empty()) fail("invalid_response", "JSON is empty");
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!validUtf8(text)) fail("invalid_response", "JSON is not valid UTF-8");
    try {
        const auto callback = [](int depth, Json::parse_event_t, Json&) {
            if (depth > 64) fail("too_large", "JSON nesting exceeds budget");
            return true;
        };
        return Json::parse(bytes.begin(), bytes.end(), callback);
    } catch (const Json::exception&) { fail("invalid_response", "JSON syntax is invalid"); }
}

} // namespace nickel::resources::detail
