#pragma once

#include "java.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::minecraft {

enum class LoaderKind { Vanilla, Fabric, LegacyFabric, Quilt, Forge, NeoForge, OptiFine, LiteLoader };
enum class OptiFineRoute { Installer, LaunchWrapper, Mods, Manual, Incompatible };

struct LoaderSelection {
    LoaderKind kind = LoaderKind::Vanilla;
    std::string version;
};

struct OptiFineCompatibility {
    OptiFineRoute route = OptiFineRoute::Installer;
    std::string modsSubdirectory;
    std::vector<std::string> dependencies;
    std::string reason;
};

std::string_view loader_name(LoaderKind kind) noexcept;
LoaderKind parse_loader_kind(std::string_view name);
void validate_loader_combination(std::span<const LoaderSelection> loaders, std::string_view minecraftVersion);
OptiFineCompatibility optifine_compatibility(std::string_view minecraftVersion,
                                             const std::optional<std::string>& requiredForge,
                                             std::span<const LoaderSelection> companions,
                                             bool matchingOptiFabric = false,
                                             bool matchingFabricApi = false);
bool forge_build_matches(std::string_view minecraftVersion, std::string_view build,
                         const std::optional<std::string>& requirement);
bool neoforge_matches_minecraft(std::string_view minecraftVersion, std::string_view build);
std::string fabric_minecraft_id(std::string_view minecraftVersion);
std::string canonical_minecraft_id(std::string_view minecraftVersion);
std::string optifine_library_version(std::string_view filename);
std::vector<std::string> parse_maven_versions(std::string_view xml);
JavaRequirement loader_java_requirement(std::string_view minecraftVersion,
                                         std::span<const LoaderSelection> loaders,
                                         std::string_view releaseTime = {},
                                         std::optional<unsigned> metadataMajor = {});

}
