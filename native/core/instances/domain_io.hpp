#pragma once

#include "instances.hpp"
#include <functional>

namespace nickel { class FileLock; }
namespace nickel::instances::detail {
std::string ascii_lower(std::string_view value);
std::string trim(std::string_view value);
std::string random_id();
std::uint64_t current_pid();
bool process_alive(std::uint64_t pid);
// Convert only at native filesystem boundaries; never store the extended Windows prefix in public DTOs.
fs::path native_path(const fs::path& path);
fs::path logical_path(const fs::path& path);
// Resolve the existing pinned ancestor, preserving a missing suffix without following links.
fs::path canonical_path(const fs::path& path);
bool path_exists(const fs::path& path);
void create_directories(const fs::path& path);
// Reserve one directory atomically under its pinned existing parent; never reuse an existing entry.
bool create_directory_no_replace(const fs::path& path);
struct DirectoryEntry {
    fs::path file;
    fs::file_type type = fs::file_type::none;
    std::uintmax_t bytes = 0;
    const fs::path& path() const noexcept { return file; }
    bool is_directory() const noexcept { return type == fs::file_type::directory; }
    bool is_regular_file() const noexcept { return type == fs::file_type::regular; }
    std::uintmax_t file_size() const noexcept { return bytes; }
};
// Logical paths only; native enumeration is pinned and never follows links.
std::vector<DirectoryEntry> directory_entries(const fs::path& directory, bool recursive = false,
                                               std::stop_token cancellation = {});
void verify_directory_tree(const fs::path& directory);
std::string read_bounded(const fs::path& path, std::size_t limit);
std::optional<Json> read_optional_json(const fs::path& path, std::size_t limit,
                                      bool lenient = false);
Json parse_strict_json(std::string_view text, std::size_t limit);
void write_exclusive(const fs::path& path, std::string_view content);
void atomic_write(const fs::path& path, std::string_view content, bool replace = true);
void rename_no_replace(const fs::path& source, const fs::path& destination);
// Streaming publication retains the source; its owning workspace remains responsible for cleanup.
void copy_no_replace(const fs::path& source, const fs::path& destination, std::uint64_t max_bytes,
                      std::stop_token cancellation = {}, std::string_view expected_sha256 = {});
void remove_owned_tree(const fs::path& path);
void check_cancelled(std::stop_token token);
void strict_keys(const Json& value, std::span<const std::string_view> allowed,
                 std::span<const std::string_view> required = {});
std::optional<std::uint64_t> unsigned_value(const Json& value);
std::optional<std::string> string_at(const Json& value, std::string_view key);
class DirectoryLock {
public:
    explicit DirectoryLock(const fs::path& directory);
    ~DirectoryLock();
    DirectoryLock(const DirectoryLock&) = delete;
    DirectoryLock& operator=(const DirectoryLock&) = delete;
private:
    std::unique_ptr<nickel::FileLock> lock_;
};
} // namespace nickel::instances::detail
