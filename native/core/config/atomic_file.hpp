#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nickel {

class Cancellation;
namespace foundation_detail { enum class DirectoryGuardMode; }

std::filesystem::path path_from_utf8(std::string_view text);
std::string path_to_utf8(const std::filesystem::path& path);
bool valid_utf8(std::string_view text) noexcept;
void reject_path_links(const std::filesystem::path& path, bool ancestors = true);
std::vector<std::uint8_t> read_file_bounded(const std::filesystem::path& path,
                                          std::uint64_t limit);
std::string read_text_bounded(const std::filesystem::path& path, std::uint64_t limit);
std::string unique_token();

struct AtomicWriteOptions {
    bool private_file = false;
    bool create_parents = true;
    const Cancellation* cancellation = nullptr;
};

void atomic_write_file(const std::filesystem::path& path,
                       std::span<const std::uint8_t> bytes,
                       const AtomicWriteOptions& options = {});
void atomic_write_text(const std::filesystem::path& path, std::string_view text,
                       const AtomicWriteOptions& options = {});
void create_file_exclusive(const std::filesystem::path& path,
                           std::span<const std::uint8_t> bytes,
                           bool private_file = false);
// Returns false only when destination already exists; never replaces it.
// Windows requires the same parent directory; cross-parent moves throw Unsupported.
bool rename_no_replace(const std::filesystem::path& source,
                       const std::filesystem::path& destination);
// Streams through a 64 KiB buffer into a private temporary in destination's
// pinned parent; false only for an existing destination. Source is never removed.
// Optional SHA256 is 64 hex characters; mismatch/changed source/cancel throws.
bool atomic_copy_no_replace(const std::filesystem::path& source,
                            const std::filesystem::path& destination,
                            std::uint64_t max_bytes,
                            const Cancellation* cancellation = nullptr,
                            std::string_view expected_sha256 = {});
void sync_directory(const std::filesystem::path& directory);

class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path,
                      const Cancellation* cancellation = nullptr,
                      std::chrono::milliseconds timeout = std::chrono::seconds(30));
    FileLock(const std::filesystem::path& path, const Cancellation* cancellation,
             std::chrono::milliseconds timeout, foundation_detail::DirectoryGuardMode mode);
    ~FileLock();
    FileLock(FileLock&&) noexcept;
    FileLock& operator=(FileLock&&) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nickel
