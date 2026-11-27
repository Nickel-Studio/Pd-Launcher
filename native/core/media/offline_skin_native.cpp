#include "offline_skin_native.hpp"
#include "../accounts/detail/util.hpp"

#include "core/archive/archive.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/conditional_file.hpp"
#include "core/config/path_guard.hpp"
#include "core/tasks/secure_fs.hpp"

#include <functional>
#include <algorithm>
#include <limits>
#include <system_error>

namespace nickel::media {
namespace {
namespace fs = std::filesystem;
namespace sd = nickel::tasks::detail;

[[noreturn]] void failure(std::string_view code, std::string_view message) {
    throw nickel::ErrorException({"offline_skin_" + std::string(code), std::string(message), nickel::Json::object()});
}
template<class Operation>
decltype(auto) filesystem_boundary(Operation&& operation) {
    try { return std::forward<Operation>(operation)(); }
    catch (const nickel::ErrorException& error) {
        if (error.error().code == "Cancelled" || error.error().code == "cancelled")
            throw accounts::IdentityError(accounts::IdentityErrorCode::cancelled);
        throw;
    }
    catch (...) { failure("filesystem", "Offline skin file operation failed. Check paths, disk space and permissions, close the game, then retry."); }
}
fs::path checked(const fs::path& path) {
    if (!path.is_absolute()) failure("invalid_path", "Use the canonical absolute instance, client JAR or selected PNG path.");
    return sd::checked_absolute(path);
}
bool exists_as(const fs::path& path, bool directory) {
    sd::reject_links(path);
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) return false;
    if (error) failure("filesystem", "Cannot inspect the offline skin path. Check file permissions and retry.");
    if (directory ? !fs::is_directory(status) : !fs::is_regular_file(status))
        failure("unsafe_path", "Offline skin path is not a real ordinary file or directory. Move the conflicting entry or link aside and retry.");
    return true;
}
class DirectoryPin final : public SkinFileLock {
public:
    explicit DirectoryPin(const fs::path& path)
        : guard_(path, false, nickel::foundation_detail::DirectoryGuardMode::NamespaceMutation) {}
private:
    nickel::foundation_detail::DirectoryGuard guard_;
};
class RuntimeLock final : public SkinFileLock {
public:
    RuntimeLock(const fs::path& path, std::stop_token stop)
        : stopped_(stop, [this] { cancellation_.cancel(); }),
          lock_(path, &cancellation_, std::chrono::seconds(30),
                nickel::foundation_detail::DirectoryGuardMode::NamespaceMutation) {}
private:
    nickel::Cancellation cancellation_;
    std::stop_callback<std::function<void()>> stopped_;
    nickel::FileLock lock_;
};
class NativeOptionsSnapshot final : public SkinOptionsSnapshot {
public:
    explicit NativeOptionsSnapshot(nickel::ConditionalFileSnapshot snapshot)
        : snapshot_(std::move(snapshot)) {}
    bool exists() const noexcept override { return snapshot_.exists(); }
    std::span<const std::uint8_t> bytes() const noexcept override { return snapshot_.bytes(); }
    void replace(std::span<const std::uint8_t> bytes, std::stop_token stop) override {
        filesystem_boundary([&] {
            accounts::detail::check_stop(stop);
            NativeSkinFileStore{}.check_write_capacity(snapshot_.path(), bytes.size());
            nickel::Cancellation cancellation;
            std::stop_callback stopped(stop, [&] { cancellation.cancel(); });
            nickel::AtomicWriteOptions options;
            options.create_parents = false;
            options.cancellation = &cancellation;
            try {
                const auto result = nickel::replace_file_conditionally(
                    snapshot_, bytes, offline_skin_options_max_bytes, options);
                if (!result.committed) failure("filesystem", "Game settings publication did not commit. Close the game and retry.");
                // A committed publication cannot be cancelled. Cleanup must still
                // finish before launch; canonical recovery preserves competitors.
                if (!result.recovery_receipt.empty())
                    (void)nickel::recover_conditional_replacements(snapshot_.path().parent_path());
            } catch (const nickel::ErrorException& error) {
                if (error.error().code == "Conflict")
                    failure("changing_options", "Game settings changed during skin preparation. Close the game and retry; user settings were not overwritten.");
                throw;
            }
        });
    }
private:
    nickel::ConditionalFileSnapshot snapshot_;
};
nickel::archive::Limits archive_limits() {
    nickel::archive::Limits limits;
    limits.max_entries = 100000;
    limits.max_entry_bytes = 64ULL * 1024 * 1024;
    limits.max_archive_bytes = 512ULL * 1024 * 1024;
    limits.max_total_bytes = 1024ULL * 1024 * 1024;
    limits.max_duration = std::chrono::seconds(30);
    return limits;
}
} // namespace

bool NativeSkinFileStore::directory(const fs::path& path) {
    return filesystem_boundary([&] { return exists_as(checked(path), true); });
}
fs::path NativeSkinFileStore::canonical_directory(const fs::path& path) {
    return filesystem_boundary([&] {
        const auto result = checked(path);
        if (!exists_as(result, true)) failure("invalid_path", "Resolved game or configuration directory does not exist. Select the directory again.");
        return result;
    });
}
std::string NativeSkinFileStore::directory_identity(const fs::path& path) {
    return filesystem_boundary([&] {
        const auto id = sd::directory_identity(checked(path));
        return std::to_string(id.volume) + ":" + std::to_string(id.index);
    });
}
std::unique_ptr<SkinFileLock> NativeSkinFileStore::pin_directory(const fs::path& path) {
    return filesystem_boundary([&]() -> std::unique_ptr<SkinFileLock> {
        return std::make_unique<DirectoryPin>(checked(path));
    });
}
std::optional<SkinBytes> NativeSkinFileStore::read(const fs::path& path, std::size_t max_bytes) {
    return filesystem_boundary([&]() -> std::optional<SkinBytes> {
        const auto source = checked(path);
        if (!exists_as(source, false)) return std::nullopt;
        return nickel::read_file_bounded(source, max_bytes);
    });
}
std::unique_ptr<SkinOptionsSnapshot> NativeSkinFileStore::observe_options(
    const fs::path& path, std::stop_token stop) {
    return filesystem_boundary([&]() -> std::unique_ptr<SkinOptionsSnapshot> {
        accounts::detail::check_stop(stop);
        const auto destination = checked(path);
        if (destination.filename() != "options.txt")
            failure("invalid_path", "Offline skin observations require the resolved runtime options.txt path.");
        nickel::Cancellation cancellation;
        std::stop_callback stopped(stop, [&] { cancellation.cancel(); });
        return std::make_unique<NativeOptionsSnapshot>(nickel::read_conditional_file(
            destination, offline_skin_options_max_bytes, &cancellation));
    });
}
void NativeSkinFileStore::check_write_capacity(const fs::path& path, std::uint64_t bytes) {
    filesystem_boundary([&] {
        auto parent = checked(path);
        while (!fs::exists(parent)) {
            const auto next = parent.parent_path();
            if (next.empty() || next == parent) failure("disk_full", "Cannot determine free space for offline skin files. No pack or options were written.");
            parent = next;
        }
        sd::reject_links(parent);
        if (!fs::is_directory(parent)) parent = parent.parent_path();
        std::error_code error;
        const auto capacity = fs::space(parent, error);
        constexpr std::uint64_t reserve = 8ULL * 1024 * 1024;
        if (error || capacity.available == std::numeric_limits<std::uintmax_t>::max() ||
            bytes > capacity.available || capacity.available - bytes < reserve)
            failure("disk_full", "Not enough free space for offline skin publication. Free space on the instance/configuration drive and retry; existing files were not truncated.");
    });
}
void NativeSkinFileStore::ensure_directory(const fs::path& path) {
    filesystem_boundary([&] { sd::ParentGuard guard(checked(path), true); });
}
void NativeSkinFileStore::create_directory(const fs::path& path) {
    filesystem_boundary([&] { sd::create_directory_new(checked(path)); });
}
void NativeSkinFileStore::create_file(const fs::path& path, std::span<const std::uint8_t> bytes) {
    filesystem_boundary([&] {
        const auto destination = checked(path);
        check_write_capacity(destination, bytes.size());
        nickel::create_file_exclusive(destination, bytes);
    });
}
void NativeSkinFileStore::replace_file(const fs::path& path, std::span<const std::uint8_t> bytes) {
    filesystem_boundary([&] {
        const auto destination = checked(path);
        check_write_capacity(destination, bytes.size());
        if (fs::exists(destination)) (void)exists_as(destination, false);
        nickel::AtomicWriteOptions options;
        options.create_parents = false;
        nickel::atomic_write_file(destination, bytes, options);
    });
}
std::unique_ptr<SkinFileLock> NativeSkinFileStore::lock(const fs::path& runtime, std::stop_token stop) {
    return filesystem_boundary([&]() -> std::unique_ptr<SkinFileLock> {
        accounts::detail::check_stop(stop);
        return std::make_unique<RuntimeLock>(checked(runtime) / ".nickel-offline-skin.lock", stop);
    });
}
std::vector<SkinArchiveEntry> NativeClientSkinArchive::entries(const fs::path& jar, std::stop_token stop) {
    return filesystem_boundary([&] {
        accounts::detail::check_stop(stop);
        nickel::Cancellation cancellation;
        std::stop_callback stopped(stop, [&] { cancellation.cancel(); });
        const auto entries = nickel::archive::Archive::inspect_members(checked(jar), archive_limits(), cancellation);
        std::vector<SkinArchiveEntry> result;
        result.reserve(entries.size());
        for (const auto& entry : entries) result.push_back({entry.name, entry.size, entry.directory});
        accounts::detail::check_stop(stop);
        return result;
    });
}
SkinBytes NativeClientSkinArchive::read(const fs::path& jar, std::string_view entry,
                                       std::size_t max_bytes, std::stop_token stop) {
    return filesystem_boundary([&] {
        accounts::detail::check_stop(stop);
        if (max_bytes > skin_png_max_bytes) failure("archive", "Offline skin archive reads must stay within the one-MiB skin bound.");
        nickel::Cancellation cancellation;
        std::stop_callback stopped(stop, [&] { cancellation.cancel(); });
        const auto data = nickel::archive::Archive::read_member_exact(checked(jar), entry, max_bytes, cancellation, archive_limits());
        SkinBytes result(data.size());
        std::transform(data.begin(), data.end(), result.begin(), [](std::byte byte) { return std::to_integer<std::uint8_t>(byte); });
        accounts::detail::check_stop(stop);
        return result;
    });
}

} // namespace nickel::media
