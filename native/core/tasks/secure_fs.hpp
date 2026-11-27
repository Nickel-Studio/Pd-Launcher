#pragma once

#ifdef _WIN32
#include "core/config/path_guard.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::tasks::detail {

// Internal OS layer: errors throw; service boundaries translate into common Error.
std::string path_utf8(const std::filesystem::path& path);
std::filesystem::path utf8_path(std::string_view text);
std::string unique_id();
std::string case_key(std::string_view utf8);
// Volume name-matching semantics: true when two names on the volume of `directory`
// collide only when byte-identical (case-sensitive volumes), false when the volume
// folds case (Windows NTFS, macOS APFS default). Probed once per volume and cached;
// probe failures fall back to byte-exact matching.
bool volume_case_sensitive(const std::filesystem::path& directory);
// The same decision for the default anchor volume that case_key() consults on POSIX.
bool default_volume_case_sensitive();
// Test-only: force the volume decision for every probe and case_key() fold until
// reset with nullopt. Production code must not call this.
void override_volume_case_sensitive(std::optional<bool> decision);
enum class RelativePathPolicy { DiskOutput, ArchiveMember };
std::string validate_relative(std::string_view text, std::size_t max_depth = 64,
                              std::size_t max_bytes = 4096,
                              RelativePathPolicy policy = RelativePathPolicy::DiskOutput);
std::filesystem::path checked_absolute(const std::filesystem::path& path);
// Extended Windows paths are internal I/O values, never DTO or journal paths.
std::filesystem::path io_path(const std::filesystem::path& path);
std::filesystem::path logical_path(const std::filesystem::path& path);
bool paths_overlap(const std::filesystem::path& left,
                   const std::filesystem::path& right);
void reject_links(const std::filesystem::path& path);

#ifdef _WIN32
using GuardMode = foundation_detail::DirectoryGuardMode;
using ParentGuard = foundation_detail::DirectoryGuard;
#else
enum class GuardMode { ReadOnly, NamespaceMutation };
class ParentGuard {
public:
    explicit ParentGuard(const std::filesystem::path& directory,
                         bool create_missing = false, GuardMode mode = GuardMode::ReadOnly);
    ~ParentGuard();
    ParentGuard(ParentGuard&&) noexcept;
    ParentGuard& operator=(ParentGuard&&) noexcept;
    ParentGuard(const ParentGuard&) = delete;
    ParentGuard& operator=(const ParentGuard&) = delete;
    const std::filesystem::path& path() const noexcept;
    std::intptr_t native_handle() const noexcept;
    void verify() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

struct FileIdentity {
    std::uint64_t volume{};
    std::uint64_t index{};
    bool operator==(const FileIdentity&) const = default;
};
struct FileRevision {
    std::uint64_t modified{};
    std::uint64_t changed{};
    bool operator==(const FileRevision&) const = default;
};
struct RegularFileToken {
    std::filesystem::path root;
    std::filesystem::path path;
    FileIdentity root_identity;
    FileIdentity parent_identity;
    FileIdentity file_identity;
    std::uint64_t size{};
    FileRevision revision;
    std::string sha256;
};
RegularFileToken capture_regular_file(const std::filesystem::path& root,
                                      const std::filesystem::path& path,
                                      const std::function<bool()>& cancelled = {});
RegularFileToken capture_regular_file(const std::filesystem::path& root,
                                      const std::filesystem::path& path, std::uint64_t max_bytes,
                                      const std::function<bool()>& cancelled = {});
// remove_persistence_pending reports committed deletion with Error.details committed/removed=true,
// removedBytes (authorized logical size) and durabilityPending=true. Do not retry or roll it back.
void remove_regular_file(const RegularFileToken& expected,
                         const std::function<bool()>& cancelled = {});

class SecureFile {
public:
    static SecureFile create_new(const std::filesystem::path& path);
    static SecureFile open_read(const std::filesystem::path& path);
    static SecureFile open_read(const std::filesystem::path& path, GuardMode mode);
    static SecureFile open_owned(const std::filesystem::path& path,
                                 const FileIdentity& expected);
    static SecureFile open_publication(const std::filesystem::path& path,
                                      const FileIdentity& expected,
                                      GuardMode mode = GuardMode::ReadOnly);
    static SecureFile lock_workspace(const std::filesystem::path& path, bool create);
    void publish_no_replace(const std::filesystem::path& destination);
    ~SecureFile();
    SecureFile(SecureFile&&) noexcept;
    SecureFile& operator=(SecureFile&&) noexcept;
    SecureFile(const SecureFile&) = delete;
    SecureFile& operator=(const SecureFile&) = delete;

    std::size_t read(std::span<std::byte> buffer);
    void write(std::span<const std::byte> buffer);
    void seek(std::uint64_t offset);
    void truncate(std::uint64_t bytes);
    void flush();
    std::uint64_t size() const;
    FileIdentity identity() const;
    FileRevision revision() const;
    void remove_verified(const RegularFileToken& expected,
                         const std::function<bool()>& cancelled = {});
    const std::filesystem::path& path() const noexcept;
    void close() noexcept;

private:
    struct Impl;
    explicit SecureFile(std::unique_ptr<Impl> impl);
    void publish_file(const std::filesystem::path& destination, bool replace);
    friend void write_atomic(const std::filesystem::path& path, std::string_view bytes);
    std::unique_ptr<Impl> impl_;
};

void create_directory_new(const std::filesystem::path& path);
void rename_no_replace(const std::filesystem::path& source,
                       const std::filesystem::path& destination);
void rename_owned_no_replace(const std::filesystem::path& source,
                             const std::filesystem::path& destination,
                             const FileIdentity& expected);
void write_atomic(const std::filesystem::path& path, std::string_view bytes);
std::vector<std::byte> read_bounded(const std::filesystem::path& path,
                                  std::uint64_t limit);
void remove_owned_tree(const std::filesystem::path& root,
                       const FileIdentity& identity);
FileIdentity directory_identity(const std::filesystem::path& path);
void sync_directory(const std::filesystem::path& path);

} // namespace nickel::tasks::detail
