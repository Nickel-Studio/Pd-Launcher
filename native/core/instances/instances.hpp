#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include "core/common/json.hpp"
#include "core/common/error.hpp"
#include "core/common/cancellation.hpp"

namespace nickel::instances {
using Json = nickel::Json;
namespace fs = std::filesystem;
inline constexpr std::string_view profile_file = "nickel.profile.json";
inline constexpr std::string_view settings_file = "nickel.settings.json";
inline constexpr std::string_view pending_file = ".nickel-pack.pending";
inline constexpr std::size_t settings_max_bytes = 256 * 1024;
inline constexpr std::size_t descriptor_max_bytes = 8 * 1024 * 1024;

class Error : public nickel::ErrorException {
public:
    Error(std::string code, std::string message);
    const std::string& code() const noexcept;
};

fs::path path_from_utf8(std::string_view value);
std::string path_to_utf8(const fs::path& path);
std::string root_identity(std::string_view value);
void validate_name(std::string_view name);
bool valid_minecraft_version(std::string_view value) noexcept;
void reject_links(const fs::path& path, bool ancestors = true);
fs::path checked_instance_directory(const fs::path& root, std::string_view id,
                                    bool must_exist = true);
Json parse_legacy_json(std::string_view text, std::size_t max_bytes = descriptor_max_bytes);

struct Registry {
    std::vector<std::string> roots;
    std::optional<std::string> active;
    Json labels = Json::object();
    Json unknown = Json::object();
};
Registry registry_from_text(std::string_view text) noexcept;
Json registry_to_json(const Registry& registry);
std::vector<std::string> ordered_roots(const std::vector<std::string>& incoming,
                                      const std::vector<std::string>& previous,
                                      std::optional<std::string_view> default_root = {});

struct NamedProfile {
    std::uint32_t schema_version = 1;
    std::string name;
    std::string minecraft_version;
    std::optional<std::string> loader;
    std::string loader_version;
    Json loader_artifact = nullptr;
    bool ready = false;
    Json unknown = Json::object();
};
Json profile_to_json(const NamedProfile& profile);
NamedProfile profile_from_json(const Json& value, std::string_view expected_name);
std::optional<NamedProfile> load_profile(const fs::path& root, std::string_view id);
void save_profile(const fs::path& directory, const NamedProfile& profile);
bool is_pending(const fs::path& directory);

struct InstanceEntry {
    std::string id;
    std::string display_name;
    bool favorite = false;
    std::optional<std::uint64_t> favorite_at;
    std::string category;
    std::string version_type = "unknown";
    std::string loader = "unknown";
    std::optional<std::string> loader_version;
    std::string icon = "redstone_block";
    fs::path directory;
    std::optional<std::string> release_time;
    bool has_json = false;
    std::optional<std::string> minecraft_version;
    bool named_profile = false;
    bool ready = false;
};
Json entry_to_json(const InstanceEntry& entry);
std::optional<fs::path> pick_version_json(const fs::path& directory, std::string_view id);
std::vector<InstanceEntry> scan_instances(const fs::path& root);
InstanceEntry instance_detail(const fs::path& root, std::string_view id);
// Icon-only changes patch local settings; other changes patch nickelCustom without replacing omitted fields.
void patch_instance_metadata(const fs::path& root, std::string_view id, const Json& changed,
                             std::stop_token cancellation = {});
void rename_display(const fs::path& root, std::string_view id, std::string_view display_name,
                    std::string_view description, std::string_view category, bool favorite,
                    std::uint64_t timestamp_ms);
void delete_instance(const fs::path& root, std::string_view id);
fs::path mods_directory(const fs::path& resource_root, std::string_view loader,
                        std::string_view minecraft_version);

class InstanceRegistry {
public:
    InstanceRegistry(fs::path config_directory, fs::path default_game_root);
    Registry get() const;
    void set(const Registry& incoming);
    fs::path ensure_default();
    fs::path create_root(const fs::path& parent);
    void rename_root(const fs::path& root, std::string_view label);
    void remove_root(const fs::path& root, bool delete_files,
                     std::span<const fs::path> protected_directories);
    const fs::path& config_directory() const noexcept;
private:
    fs::path config_;
    fs::path default_;
};

struct InstallIdentity {
    std::string minecraft_version;
    std::optional<std::string> loader;
    std::string loader_version;
    Json descriptor;
    Json loader_artifact = nullptr;
};

// Installs use the final directory; unpublished targets remain invisible to all readers.
class InstanceTransaction {
public:
    static InstanceTransaction reserve(const fs::path& root, std::string_view name,
                                       std::string_view minecraft_version,
                                       std::optional<std::string> loader = {},
                                       std::string_view loader_version = {}, bool choose_suffix = false);
    InstanceTransaction(InstanceTransaction&&) noexcept;
    InstanceTransaction& operator=(InstanceTransaction&&) noexcept;
    ~InstanceTransaction();
    InstanceTransaction(const InstanceTransaction&) = delete;
    InstanceTransaction& operator=(const InstanceTransaction&) = delete;
    const fs::path& directory() const;
    const fs::path& root() const;
    const NamedProfile& profile() const;
    void check_owned() const;
    void complete(const InstallIdentity& installed);
    void commit(std::string_view display_name, const Json& source,
                std::stop_token cancellation = {});
    void cleanup();
    // Used when the caller intentionally leaves recoverable work after interruption.
    void leave_for_recovery(bool writers_drained = true);
private:
    struct State;
    explicit InstanceTransaction(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

struct RecoveryResult {
    std::vector<std::string> removed;
    std::vector<std::string> published;
    std::vector<std::string> skipped;
    std::vector<std::string> errors;
};
RecoveryResult recover_interrupted_instances(const fs::path& root);
} // namespace nickel::instances
