#pragma once

#include "core/common/json.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace nickel {

inline constexpr std::string_view application_identifier = "studio.nickel.launcher";
inline constexpr std::string_view data_directory_name = "Nickel";
inline constexpr std::string_view game_directory_name = ".minecraft";
inline constexpr std::string_view portable_manifest_name = "nickel.portable.json";

struct PathContext {
    std::filesystem::path executable;
    std::filesystem::path system_config_base;
    std::filesystem::path app_data_base;
    bool macos = false;
    static PathContext system();
};

struct PortableManifest {
    unsigned schema_version = 1;
    std::string format;
    std::string package_id;
    std::string name;
    std::string version;
    std::string instance_name;
    std::string payload;
    std::string game_directory;
    std::string data_directory;
    std::optional<Json> launcher;
    std::optional<std::string> personalization;
};

std::filesystem::path directory_for_executable(const std::filesystem::path& executable,
                                                bool macos);
bool has_configuration(const std::filesystem::path& directory);
PortableManifest parse_portable_manifest(std::string_view text);
void validate_portable_manifest(const PortableManifest& manifest);
Json portable_manifest_json(const PortableManifest& manifest);
std::optional<PortableManifest> read_portable_manifest(const std::filesystem::path& root);

class PathResolver {
public:
    explicit PathResolver(PathContext context);
    const PathContext& context() const noexcept;
    std::filesystem::path executable_directory() const;
    std::optional<PortableManifest> portable_manifest() const;
    std::filesystem::path config_root(bool create = true) const;
    std::filesystem::path default_game_directory() const;
    std::filesystem::path bundled_game_directory() const;

private:
    PathContext context_;
};

} // namespace nickel
