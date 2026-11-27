#pragma once

#include "skin_png.hpp"

#include <functional>

namespace nickel::media {

using SkinBytes = std::vector<std::uint8_t>;

inline constexpr std::string_view offline_skin_pack_folder = "Nickel-offline-skin";
inline constexpr std::string_view offline_skin_pack_id = "file/Nickel-offline-skin";
inline constexpr std::string_view offline_skin_owner_content = "Nickel offline skin pack v1\n";
inline constexpr std::size_t offline_skin_options_max_bytes = 2 * 1024 * 1024;
inline constexpr std::size_t offline_skin_record_max_bytes = 4096;

struct SkinArchiveEntry {
    std::string name;
    std::uint64_t size = 0;
    bool directory = false;
};

// These are read-only JAR operations, not whole-archive extraction validation.
class ClientSkinArchive {
public:
    virtual ~ClientSkinArchive() = default;
    virtual std::vector<SkinArchiveEntry> entries(const std::filesystem::path& jar,
                                                 std::stop_token stop) = 0;
    virtual SkinBytes read(const std::filesystem::path& jar, std::string_view entry,
                            std::size_t max_bytes, std::stop_token stop) = 0;
};

// Adapters reject links/non-regular files, bound reads and never truncate on
// failure. Runtime locks cover ownership; options use a retained expectation.
class SkinFileLock {
public:
    virtual ~SkinFileLock() = default;
};
class SkinOptionsSnapshot {
public:
    virtual ~SkinOptionsSnapshot() = default;
    virtual bool exists() const noexcept = 0;
    virtual std::span<const std::uint8_t> bytes() const noexcept = 0;
    // Recoverable conditional replacement, not a single-publication CAS. Complete
    // recovery before returning; preserve external occupants on any conflict.
    virtual void replace(std::span<const std::uint8_t> bytes, std::stop_token stop) = 0;
};
class SkinFileStore {
public:
    virtual ~SkinFileStore() = default;
    virtual bool directory(const std::filesystem::path& path) = 0;
    virtual std::filesystem::path canonical_directory(const std::filesystem::path& path) = 0;
    virtual std::string directory_identity(const std::filesystem::path& path) = 0;
    virtual std::unique_ptr<SkinFileLock> pin_directory(const std::filesystem::path& path) = 0;
    virtual std::optional<SkinBytes> read(const std::filesystem::path& path,
                                         std::size_t max_bytes) = 0;
    // Bounded observation of options.txt, with missing distinct from empty.
    // Recover pending publications before observing; do not retain a runtime lock.
    virtual std::unique_ptr<SkinOptionsSnapshot> observe_options(
        const std::filesystem::path& path, std::stop_token stop) = 0;
    virtual void check_write_capacity(const std::filesystem::path& path,
                                      std::uint64_t bytes) = 0;
    virtual void ensure_directory(const std::filesystem::path& path) = 0;
    virtual void create_directory(const std::filesystem::path& path) = 0;
    virtual void create_file(const std::filesystem::path& path,
                             std::span<const std::uint8_t> bytes) = 0;
    virtual void replace_file(const std::filesystem::path& path,
                              std::span<const std::uint8_t> bytes) = 0;
    virtual std::unique_ptr<SkinFileLock> lock(const std::filesystem::path& runtime,
                                              std::stop_token stop) = 0;
};

enum class OfflineSkinMode { random, steve, alex, official, custom };
OfflineSkinMode parse_offline_skin_mode(std::string_view mode);
std::string_view offline_skin_mode_name(OfflineSkinMode mode) noexcept;

// Construct only from canonical instance/launch resolution. No account or UUID
// is accepted: resource packs cannot change player identity or geometry.
struct OfflineSkinRequest {
    std::filesystem::path runtime_directory;
    std::filesystem::path client_jar;
    std::string minecraft_version;
    OfflineSkinMode mode = OfflineSkinMode::random;
    std::string name;
    std::optional<std::filesystem::path> path;
    bool slim = false;
};
struct OfflineSkinResult {
    bool enabled = false;
    bool options_changed = false;
    std::filesystem::path pack_directory;
    std::size_t texture_count = 0;
};

std::string patch_offline_skin_options(std::string_view text,
                                       std::optional<std::string_view> enable);

class OfflineSkinService final {
public:
    OfflineSkinService(SkinFileStore& files, ClientSkinArchive& archive,
                        accounts::IdentityHttp* http = nullptr,
                        accounts::IdentityLog log = {});

    // Native UI handles the dialog. Null selection is a zero-I/O cancellation;
    // success returns a validated, re-encoded, exclusively published copy.
    std::optional<std::filesystem::path> pick(
        const std::optional<std::filesystem::path>& selected,
        const std::filesystem::path& offline_skins_directory,
        std::stop_token stop = {});
    OfflineSkinResult prepare(const OfflineSkinRequest& request,
                               std::stop_token stop = {});
    OfflineSkinResult prepare(const OfflineSkinRequest& request,
                               const nickel::Cancellation& cancellation) {
        return prepare(request, cancellation.stop_token());
    }

private:
    SkinFileStore& files_;
    ClientSkinArchive& archive_;
    accounts::IdentityHttp* http_;
    accounts::IdentityLog log_;
};

} // namespace nickel::media
