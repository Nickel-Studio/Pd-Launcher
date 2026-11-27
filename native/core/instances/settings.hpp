#pragma once

#include "instances.hpp"
#include <map>

namespace nickel::instances {
Json default_version_settings();
Json normalize_version_settings(const Json& value);
struct LocalSettings {
    Json value = nullptr;
    std::string source = "nickel";
    std::vector<std::string> unmapped_legacy_keys;
};
LocalSettings read_local_settings(const fs::path& root, std::string_view id);
// Only a live reservation may resolve settings and resources before publication.
LocalSettings read_reserved_local_settings(const InstanceTransaction& transaction,
                                           std::stop_token cancellation = {});
fs::path reserved_resource_directory(const InstanceTransaction& transaction,
                                     const Json& global_settings,
                                     std::stop_token cancellation = {});
Json get_version_settings(const fs::path& root, std::string_view id);
void set_version_settings(const fs::path& root, std::string_view id, const Json& value);
std::map<std::string, std::string> parse_legacy_setup(std::string_view text);

struct EffectiveSettings {
    bool isolation = true;
    std::string java_path;
    std::string window_title;
    std::string custom_info;
    std::string visibility = "keep";
    std::string priority = "normal";
    std::string window_mode = "default";
    std::uint32_t width = 854;
    std::uint32_t height = 480;
    std::uint64_t memory_mb = 4096;
    bool high_performance_gpu = false;
    bool disable_wrapper = false;
    bool disable_lwjgl_agent = false;
    std::string offline_skin = "random";
    std::string offline_skin_name;
    std::string offline_skin_path;
    bool offline_skin_slim = false;
    std::vector<std::string> jvm_args;
    std::vector<std::string> game_args;
    std::string pre_launch_command;
    std::string memory_management = "g1-tuned";
    bool disable_file_check = false;
    bool disable_mod_updates = false;
    std::string server_login = "any";
    std::string server_name;
    std::string auth_server;
    std::string server_address;
    std::string server_registration;
};
enum class VersionKind { regular, optifine, moddable };
struct MemoryContext {
    std::optional<std::uint64_t> total_mb;
    std::optional<std::uint64_t> available_mb;
    std::uint64_t mod_count = 0;
    VersionKind kind = VersionKind::regular;
};
std::vector<std::string> split_arguments(std::string_view raw);
bool resolve_isolation(const Json& global, const Json& local);
EffectiveSettings resolve_settings(const Json& global, const Json& local,
                                   const MemoryContext& memory = {});
std::uint64_t automatic_memory_mb(const MemoryContext& memory);
std::vector<std::string> gc_arguments(std::string_view mode, std::uint32_t java_major,
                                     std::optional<std::uint32_t> windows_build = {});
fs::path resource_directory(const fs::path& root, std::string_view id,
                            const Json& global_settings);
std::uint64_t count_mods(const fs::path& mods_root);

struct SettingsProjection {
    Json settings = Json::object();
    std::vector<std::string> included;
    std::vector<std::string> omitted;
};
bool contains_host_path(std::string_view text);
bool contains_credentials(std::string_view text);
SettingsProjection project_portable_settings(const Json& value);
Json validate_portable_settings(const Json& value);
Json merge_portable_settings(const Json& existing, const Json& incoming);
void apply_portable_settings(const fs::path& instance_directory, const Json& incoming);
} // namespace nickel::instances
