#include "core/config/detail.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/path_guard.hpp"
#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"

#include <cerrno>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace nickel::foundation_detail {

[[noreturn]] void fail(std::string_view code, std::string_view message) {
    throw ErrorException(Error{std::string(code), std::string(message), Json::object()});
}
[[noreturn]] void io_fail(std::string_view operation, const std::filesystem::path& path, int native_error) {
    std::string code = "IoError";
#ifdef _WIN32
    if (native_error == ERROR_FILE_NOT_FOUND || native_error == ERROR_PATH_NOT_FOUND) code = "NotFound";
    if (native_error == ERROR_ACCESS_DENIED || native_error == ERROR_PRIVILEGE_NOT_HELD) code = "PermissionDenied";
    if (native_error == ERROR_ALREADY_EXISTS || native_error == ERROR_FILE_EXISTS) code = "AlreadyExists";
#else
    if (native_error == ENOENT || native_error == ENOTDIR) code = "NotFound";
    if (native_error == EACCES || native_error == EPERM) code = "PermissionDenied";
    if (native_error == EEXIST) code = "AlreadyExists";
    if (native_error == ELOOP) code = "UnsafePath";
#endif
    throw ErrorException(Error{code, "Cannot " + std::string(operation),
        Json{{"path", path_to_utf8(path)}, {"nativeError", native_error}}});
}
void check_cancel(const Cancellation* cancellation) {
    if (cancelled(cancellation)) fail("Cancelled", "Operation was cancelled");
}
bool cancelled(const Cancellation* cancellation) noexcept { return cancellation && cancellation->cancelled(); }
bool path_missing(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto native = native_filesystem_path(path);
    const auto attributes = GetFileAttributesW(native.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return false;
    const auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
    io_fail("inspect path", path, static_cast<int>(error));
#else
    struct stat info{};
    if (::lstat(path.c_str(), &info) == 0) return false;
    if (errno == ENOENT || errno == ENOTDIR) return true;
    io_fail("inspect path", path, errno);
#endif
}

} // namespace nickel::foundation_detail
