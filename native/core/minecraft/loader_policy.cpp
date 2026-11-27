#include "loader_policy.hpp"
#include "maven.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <set>
#include <stdexcept>

namespace nickel::minecraft {
namespace {

std::string lower(std::string_view input) {
    std::string value(input);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::optional<std::array<unsigned, 2>> legacy_parts(std::string_view mc) {
    if (!mc.starts_with("1.")) return {};
    mc.remove_prefix(2);
    std::array<unsigned, 2> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto delimiter = mc.find('.');
        const auto part = mc.substr(0, delimiter);
        auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), result[index]);
        if (error != std::errc{} || end != part.data() + part.size()) return {};
        if (delimiter == std::string_view::npos) return result;
        mc.remove_prefix(delimiter + 1);
    }
    return {};
}

bool at_least(std::string_view mc, std::array<unsigned, 2> minimum) {
    const auto value = legacy_parts(mc);
    return value && *value >= minimum;
}

bool in_range(std::string_view mc, std::array<unsigned, 2> minimum, std::array<unsigned, 2> maximum) {
    const auto value = legacy_parts(mc);
    return value && *value >= minimum && *value <= maximum;
}

}

std::string_view loader_name(LoaderKind kind) noexcept {
    switch (kind) {
    case LoaderKind::Vanilla: return "vanilla";
    case LoaderKind::Fabric: return "fabric";
    case LoaderKind::LegacyFabric: return "legacyfabric";
    case LoaderKind::Quilt: return "quilt";
    case LoaderKind::Forge: return "forge";
    case LoaderKind::NeoForge: return "neoforge";
    case LoaderKind::OptiFine: return "optifine";
    case LoaderKind::LiteLoader: return "liteloader";
    }
    return "vanilla";
}

LoaderKind parse_loader_kind(std::string_view name) {
    const auto normalized = lower(name);
    for (auto kind : {LoaderKind::Vanilla, LoaderKind::Fabric, LoaderKind::LegacyFabric, LoaderKind::Quilt, LoaderKind::Forge, LoaderKind::NeoForge, LoaderKind::OptiFine, LoaderKind::LiteLoader}) {
        if (normalized == loader_name(kind) || (normalized.empty() && kind == LoaderKind::Vanilla)) return kind;
    }
    throw std::invalid_argument("Unknown Minecraft loader");
}

void validate_loader_combination(std::span<const LoaderSelection> loaders, std::string_view mc) {
    std::set<LoaderKind> unique;
    unsigned primary = 0;
    bool lite = false, optifine = false, forge = false;
    for (const auto& loader : loaders) {
        if (loader.kind == LoaderKind::Vanilla) {
            if (loaders.size() > 1) throw std::invalid_argument("Vanilla cannot be combined with loaders");
            continue;
        }
        if (!unique.insert(loader.kind).second) throw std::invalid_argument("Duplicate Minecraft loader");
        if (loader.kind == LoaderKind::LiteLoader) lite = true;
        else if (loader.kind == LoaderKind::OptiFine) optifine = true;
        else ++primary;
        if (loader.kind == LoaderKind::Forge) forge = true;
        if (loader.version.empty()) throw std::invalid_argument("Loader version selector is empty");
    }
    if (primary > 1) throw std::invalid_argument("Multiple primary loaders are incompatible");
    if (lite && primary && !forge) throw std::invalid_argument("LiteLoader can only combine with Forge or OptiFine");
    if (lite && !in_range(mc, {7, 0}, {12, 999})) throw std::invalid_argument("LiteLoader supports Minecraft 1.7 through 1.12 only");
    if (optifine && (unique.contains(LoaderKind::NeoForge) || unique.contains(LoaderKind::Quilt) || unique.contains(LoaderKind::LegacyFabric))) throw std::invalid_argument("Selected primary loader is incompatible with OptiFine");
}

bool forge_build_matches(std::string_view mc, std::string_view build, const std::optional<std::string>& requirement) {
    if (!requirement) return false;
    std::string requested(*requirement);
    requested.erase(0, requested.find_first_not_of(" \t"));
    if (requested.empty()) return true;
    if (requested.starts_with("Forge ")) requested.erase(0, 6);
    const auto trailing = requested.find_last_not_of(" \t");
    if (trailing != std::string::npos) requested.resize(trailing + 1);
    const auto prefix = std::string(mc) + '-';
    if (build.starts_with(prefix)) build.remove_prefix(prefix.size());
    const auto suffix = '-' + std::string(mc);
    if (build.ends_with(suffix)) build.remove_suffix(suffix.size());
    if (requested.find('.') != std::string::npos) return build == requested;
    return build.substr(build.rfind('.') == std::string_view::npos ? 0 : build.rfind('.') + 1) == requested;
}

OptiFineCompatibility optifine_compatibility(std::string_view rawMc,
                                             const std::optional<std::string>& forgeRequirement,
                                             std::span<const LoaderSelection> companions,
                                             bool optifabric, bool fabricApi) {
    const auto mc = canonical_minecraft_id(rawMc);
    for (const auto& loader : companions) {
        if (loader.kind == LoaderKind::OptiFine || loader.kind == LoaderKind::Vanilla) continue;
        if (loader.kind == LoaderKind::NeoForge || loader.kind == LoaderKind::Quilt || loader.kind == LoaderKind::LegacyFabric) return {OptiFineRoute::Incompatible, {}, {}, "Primary loader is incompatible with OptiFine"};
        if (loader.kind == LoaderKind::Forge) {
            if (in_range(mc, {13, 0}, {14, 3})) return {OptiFineRoute::Incompatible, {}, {}, "Forge 1.13 through 1.14.3 is incompatible with OptiFine"};
            if (!forge_build_matches(mc, loader.version, forgeRequirement)) return {OptiFineRoute::Incompatible, {}, {}, "OptiFine requires a different Forge build"};
            return {OptiFineRoute::Mods, "mods", {}, {}};
        }
        if (loader.kind == LoaderKind::Fabric) {
            if (at_least(mc, {20, 5})) return {OptiFineRoute::Incompatible, {}, {}, "Fabric with OptiFine is unsupported on Minecraft 1.20.5 or newer"};
            if (in_range(mc, {14, 0}, {15, 999})) return {OptiFineRoute::Manual, {}, {"Fabric API", "OptiFabric Origins"}, "OptiFabric Origins must be installed explicitly"};
            if (!optifabric || !fabricApi) return {OptiFineRoute::Manual, {}, {"Fabric API", "OptiFabric"}, "Matching OptiFabric and Fabric API artifacts are required"};
            return {OptiFineRoute::Mods, "mods", {"Fabric API", "OptiFabric"}, {}};
        }
    }
    if (std::any_of(companions.begin(), companions.end(), [](const auto& loader) { return loader.kind == LoaderKind::LiteLoader; })) return {OptiFineRoute::Mods, "mods/" + mc, {}, {}};
    const auto parts = legacy_parts(mc);
    const auto modern = !mc.starts_with("1.") && !mc.empty() && std::isdigit(static_cast<unsigned char>(mc.front()));
    if (mc.find('w') != std::string::npos || (parts && (*parts)[0] >= 14) || modern) return {OptiFineRoute::Installer, {}, {}, {}};
    return {OptiFineRoute::LaunchWrapper, {}, {}, {}};
}

bool neoforge_matches_minecraft(std::string_view mc, std::string_view build) {
    if (mc == "1.20.1") return build.starts_with("1.20.1-");
    const auto parts = legacy_parts(mc);
    if (!parts || (*parts)[0] < 20) return false;
    const auto prefix = std::to_string((*parts)[0]) + '.' + std::to_string((*parts)[1]) + '.';
    return build.starts_with(prefix);
}

std::string fabric_minecraft_id(std::string_view mc) {
    if (mc == "Combat Test 7c") return "1.16_combat-3";
    std::string result(mc);
    constexpr std::string_view infinity = "\xE2\x88\x9E";
    for (auto at = result.find(infinity); at != std::string::npos; at = result.find(infinity, at + 8)) result.replace(at, infinity.size(), "infinite");
    return result;
}

std::string canonical_minecraft_id(std::string_view mc) {
    if (mc == "1.8.0") return "1.8";
    if (mc == "1.9.0") return "1.9";
    return std::string(mc);
}

std::string optifine_library_version(std::string_view filename) {
    if (filename.ends_with(".jar")) filename.remove_suffix(4);
    if (filename.starts_with("preview_OptiFine_")) return "preview_" + std::string(filename.substr(17));
    if (filename.starts_with("OptiFine_")) filename.remove_prefix(9);
    if (filename.empty()) throw std::invalid_argument("OptiFine filename is empty");
    validate_version_id(filename);
    return std::string(filename);
}

std::vector<std::string> parse_maven_versions(std::string_view xml) {
    if (xml.find("<!DOCTYPE") != std::string_view::npos || xml.find("<!ENTITY") != std::string_view::npos) throw std::invalid_argument("Maven metadata entities are forbidden");
    const auto begin = xml.find("<versions>"), end = xml.find("</versions>");
    if (begin == std::string_view::npos || end == std::string_view::npos || end <= begin) throw std::invalid_argument("Maven metadata has no versions section");
    xml = xml.substr(begin + 10, end - begin - 10);
    std::vector<std::string> result;
    while (true) {
        const auto start = xml.find("<version>");
        if (start == std::string_view::npos) break;
        xml.remove_prefix(start + 9);
        const auto finish = xml.find("</version>");
        if (finish == std::string_view::npos) throw std::invalid_argument("Unterminated Maven metadata version");
        auto value = xml.substr(0, finish);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
        if (!value.empty() && value.find("..") == std::string_view::npos && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == '+'; })) result.emplace_back(value);
        xml.remove_prefix(finish + 10);
        if (result.size() > 100000) throw std::invalid_argument("Maven metadata version limit exceeded");
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return compare_versions(a, b) < 0; });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

JavaRequirement loader_java_requirement(std::string_view mc, std::span<const LoaderSelection> loaders,
                                         std::string_view release, std::optional<unsigned> metadataMajor) {
    std::vector<JavaRequirement> requirements;
    for (const auto& loader : loaders) {
        if (loader.kind != LoaderKind::Vanilla) requirements.emplace_back(java_requirement(mc, loader_name(loader.kind), loader.version, release, metadataMajor));
    }
    if (requirements.empty()) return java_requirement(mc, {}, {}, release, metadataMajor);
    return intersect_java_requirements(requirements);
}

}
