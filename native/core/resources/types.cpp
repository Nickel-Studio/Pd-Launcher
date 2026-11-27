#include "types.hpp"

#include <algorithm>
#include <charconv>
#include <set>
#include <tuple>

namespace nickel::resources {
namespace {

enum class VersionClass { Release, Snapshot, Legacy, Other };

std::optional<std::vector<std::uint64_t>> numericVersion(std::string_view raw) {
    std::vector<std::uint64_t> parts;
    while (!raw.empty()) {
        const auto dot = raw.find('.');
        const auto piece = raw.substr(0, dot);
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(piece.data(), piece.data() + piece.size(), value);
        if (piece.empty() || parsed.ec != std::errc{} || parsed.ptr != piece.data() + piece.size()) return {};
        parts.push_back(value);
        if (dot == std::string_view::npos) break;
        raw.remove_prefix(dot + 1);
        if (raw.empty()) return {};
    }
    return parts.size() == 2 || parts.size() == 3 ? std::optional{parts} : std::nullopt;
}

VersionClass versionClass(std::string_view raw) {
    if (numericVersion(raw)) return VersionClass::Release;
    for (const auto prefix : {"b1.", "a1.", "c0.", "rd-", "inf-"}) if (raw.starts_with(prefix)) return VersionClass::Legacy;
    if (!raw.empty() && raw.front() >= '0' && raw.front() <= '9') return VersionClass::Snapshot;
    return VersionClass::Other;
}

std::string versionGroup(std::string_view raw, bool minor, bool foldOld) {
    const auto value = trim(raw);
    switch (versionClass(value)) {
    case VersionClass::Other: return "other";
    case VersionClass::Snapshot: return "snapshot";
    case VersionClass::Legacy: return "legacy";
    case VersionClass::Release: {
        const auto parts = *numericVersion(value);
        if (foldOld && parts[0] == 1 && parts[1] < 20) return "legacy";
        if (minor) return std::to_string(parts[0]) + '.' + std::to_string(parts[1]);
        return value;
    }
    }
    return "other";
}

int compareVersions(const std::string& left, const std::string& right) {
    const auto first = numericVersion(left);
    const auto second = numericVersion(right);
    if (bool(first) != bool(second)) return first ? -1 : 1;
    if (first && second) {
        for (std::size_t i = 0; i < std::max(first->size(), second->size()); ++i) {
            const auto a = i < first->size() ? (*first)[i] : 0;
            const auto b = i < second->size() ? (*second)[i] : 0;
            if (a != b) return a > b ? -1 : 1;
        }
    }
    return left.compare(right);
}

int loaderRank(std::string_view raw) {
    const Strings order{"forge", "neoforge", "fabric", "quilt", "liteloader", "optifine"};
    const auto loader = lower(trim(raw));
    const auto found = std::find(order.begin(), order.end(), loader);
    return found == order.end() ? 99 : int(found - order.begin());
}

Strings uniqueStrings(const Strings& values) {
    Strings result;
    for (const auto& raw : values) {
        const auto value = trim(raw);
        if (!value.empty() && !contains(result, value)) result.push_back(value);
    }
    return result;
}

} // namespace

[[noreturn]] void fail(std::string code, std::string message, Json details) {
    throw ErrorException(Error{std::move(code), std::move(message), std::move(details)});
}

std::string trim(std::string_view value) {
    const auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!value.empty() && whitespace(value.front())) value.remove_prefix(1);
    while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
    return std::string(value);
}

std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return result;
}

bool contains(const Strings& values, std::string_view value, bool ignoreCase) {
    return std::any_of(values.begin(), values.end(), [&](const auto& item) { return ignoreCase ? lower(item) == lower(value) : item == value; });
}

bool validUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = std::uint8_t(text[i++]);
        if (first < 0x80) { if (!first) return false; continue; }
        unsigned length = 0;
        std::uint32_t point = 0;
        if (first >= 0xc2 && first <= 0xdf) { length = 1; point = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { length = 2; point = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { length = 3; point = first & 7; }
        else return false;
        if (length > text.size() - i) return false;
        for (unsigned n = 0; n < length; ++n) {
            const auto byte = std::uint8_t(text[i++]);
            if ((byte & 0xc0) != 0x80) return false;
            point = (point << 6) | (byte & 0x3f);
        }
        if ((length == 1 && point < 0x80) || (length == 2 && point < 0x800) || (length == 3 && point < 0x10000) ||
            point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
    }
    return true;
}

std::string utf8Path(const Path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

Path pathFromUtf8(std::string_view text) {
    if (!validUtf8(text)) fail("invalid_input", "Path is not valid UTF-8");
    return Path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string checkedHash(std::string_view raw, std::size_t bytes) {
    auto hash = lower(raw);
    if (hash.size() != bytes * 2) return {};
    for (const char c : hash) if (!((c >= 'a' && c <= 'f') || (c >= '0' && c <= '9'))) return {};
    return hash;
}

std::string sourceName(Source source) { return source == Source::Modrinth ? "modrinth" : "curseforge"; }

std::string kindName(ResourceKind kind) {
    switch (kind) {
    case ResourceKind::Mod: return "mod";
    case ResourceKind::Modpack: return "modpack";
    case ResourceKind::Datapack: return "datapack";
    case ResourceKind::Resourcepack: return "resourcepack";
    case ResourceKind::Shader: return "shader";
    }
    return {};
}

ResourceKind parseKind(std::string_view raw) {
    const auto value = lower(trim(raw));
    for (const auto kind : {ResourceKind::Mod, ResourceKind::Modpack, ResourceKind::Datapack, ResourceKind::Resourcepack, ResourceKind::Shader})
        if (kindName(kind) == value) return kind;
    fail("invalid_input", "Unsupported resource kind");
}

std::string projectIdentity(const Project& project) { return sourceName(project.source) + ':' + (project.id.empty() ? project.slug : project.id); }

bool validIdentifier(Source source, std::string_view value) {
    if (value.empty() || value.size() > 128 || trim(value) != value) return false;
    if (source == Source::CurseForge) {
        std::uint64_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && number > 0 && number <= 9007199254740991ULL;
    }
    for (const auto c : value) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

bool knownLoader(std::string_view raw) {
    const auto loader = lower(trim(raw));
    return loader == "forge" || loader == "neoforge" || loader == "fabric" || loader == "quilt" || loader == "liteloader" || loader == "optifine";
}

std::string defaultSearchLoader(std::string_view raw) {
    const auto loader = lower(trim(raw));
    return loader == "forge" || loader == "neoforge" || loader == "fabric" || loader == "quilt" ? loader : std::string{};
}

std::vector<Project> mergeProjects(const std::vector<Project>& primary, const std::vector<Project>& secondary) {
    std::map<std::string, Project> unique;
    for (const auto* items : {&primary, &secondary}) for (const auto& item : *items) {
        const auto key = projectIdentity(item);
        const auto found = unique.find(key);
        if (found == unique.end() || item.downloads > found->second.downloads) unique.insert_or_assign(key, item);
    }
    std::vector<Project> result;
    for (auto& [key, value] : unique) { (void)key; result.push_back(std::move(value)); }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.downloads != b.downloads ? a.downloads > b.downloads : projectIdentity(a) < projectIdentity(b);
    });
    return result;
}

std::vector<Dependency> deduplicateDependencies(const Project& project, const ProjectVersion& version) {
    if (!version.requiredDependencies) return {};
    const std::set<std::string> own{project.id, project.slug, version.projectId};
    std::vector<Dependency> required;
    std::set<std::string> pinned;
    for (const auto& dependency : *version.requiredDependencies) {
        if ((!dependency.projectId.empty() && own.contains(dependency.projectId)) || (!dependency.versionId.empty() && dependency.versionId == version.id)) continue;
        required.push_back(dependency);
        if (!dependency.projectId.empty() && !dependency.versionId.empty()) pinned.insert(dependency.projectId);
    }
    std::set<std::tuple<std::string, std::string, std::string>> seen;
    std::vector<Dependency> result;
    for (const auto& dependency : required) {
        if (dependency.versionId.empty() && pinned.contains(dependency.projectId)) continue;
        const auto key = std::tuple{dependency.projectId, dependency.versionId, dependency.projectId.empty() && dependency.versionId.empty() ? dependency.fileName : std::string{}};
        if (seen.insert(key).second) result.push_back(dependency);
    }
    return result;
}

VersionFilters deriveVersionFilters(const std::vector<ProjectVersion>& versions) {
    VersionFilters result;
    for (const auto [minor, fold] : {std::pair{false, false}, {true, false}, {false, true}, {true, true}}) {
        std::set<std::string> groups;
        for (const auto& version : versions) for (const auto& tag : version.gameVersions) {
            const auto group = versionGroup(tag, minor, fold);
            if (group != "other") groups.insert(group);
        }
        result = {Strings(groups.begin(), groups.end()), minor, fold};
        std::sort(result.groups.begin(), result.groups.end(), [](const auto& a, const auto& b) { return compareVersions(a, b) < 0; });
        if (result.groups.size() < 9) break;
    }
    return result;
}

std::vector<VersionGroup> groupVersions(const std::vector<ProjectVersion>& versions, const Project& project,
                                      const Filter& filter, const InstanceContext& instance, const std::string& exactVersionId) {
    auto sorted = versions;
    std::stable_sort(sorted.begin(), sorted.end(), [&](const auto& a, const auto& b) {
        if ((a.id == exactVersionId) != (b.id == exactVersionId)) return a.id == exactVersionId;
        return a.publishedAt != b.publishedAt ? a.publishedAt > b.publishedAt : a.id < b.id;
    });
    std::set<std::string> universe;
    for (const auto& loader : project.loaders) if (knownLoader(loader)) universe.insert(lower(loader));
    for (const auto& version : versions) for (const auto& loader : version.loaders) if (knownLoader(loader)) universe.insert(lower(loader));
    const bool split = project.kind == ResourceKind::Mod && universe.size() > 1;
    const auto pills = deriveVersionFilters(versions);
    std::map<std::string, VersionGroup> groups;
    const auto add = [&](const std::string& tag, const std::string& loader, const ProjectVersion& version, bool selected) {
        const auto key = (selected ? "selected:" : "") + loader + ':' + tag;
        auto& group = groups[key];
        group.key = key;
        group.version = tag;
        group.loader = loader;
        group.selected = selected;
        group.other = tag == "other";
        if (std::none_of(group.versions.begin(), group.versions.end(), [&](const auto& item) { return item.id == version.id && item.source == version.source; })) group.versions.push_back(version);
    };
    for (const auto& version : sorted) {
        Strings tags;
        for (const auto& tag : uniqueStrings(version.gameVersions)) {
            if (versionClass(tag) == VersionClass::Other) continue;
            if (!filter.minecraftVersion.empty() && tag != filter.minecraftVersion && versionGroup(tag, pills.groupByMinor, pills.foldOld) != filter.minecraftVersion) continue;
            tags.push_back(tag);
        }
        if (!filter.loader.empty() && !contains(version.loaders, lower(filter.loader), true)) continue;
        if (tags.empty()) { if (filter.minecraftVersion.empty()) add("other", {}, version, false); continue; }
        Strings loaders;
        for (const auto& loader : version.loaders) if (knownLoader(loader)) loaders.push_back(lower(loader));
        loaders = uniqueStrings(loaders);
        if (!split || loaders.empty()) loaders = {""};
        for (const auto& tag : tags) for (const auto& loader : loaders) add(versionGroup(tag, false, false), loader, version, false);
        const auto instanceLoader = lower(instance.loader);
        if (!instance.minecraftVersion.empty() && contains(version.gameVersions, instance.minecraftVersion) &&
            (!knownLoader(instanceLoader) || contains(version.loaders, instanceLoader, true)))
            add(instance.minecraftVersion, knownLoader(instanceLoader) ? instanceLoader : std::string{}, version, true);
    }
    std::set<std::string> selected;
    for (const auto& [key, group] : groups) { (void)key; if (group.selected) selected.insert(group.loader + ':' + group.version); }
    std::vector<VersionGroup> result;
    for (auto& [key, group] : groups) { (void)key; if (group.selected || !selected.contains(group.loader + ':' + group.version)) result.push_back(std::move(group)); }
    std::stable_sort(result.begin(), result.end(), [&](const auto& a, const auto& b) {
        const auto pinned = [&](const auto& group) { return !exactVersionId.empty() && std::any_of(group.versions.begin(), group.versions.end(), [&](const auto& version) { return version.id == exactVersionId; }); };
        if (pinned(a) != pinned(b)) return pinned(a);
        if (a.selected != b.selected) return a.selected;
        const auto order = compareVersions(a.version, b.version);
        return order ? order < 0 : loaderRank(a.loader) < loaderRank(b.loader);
    });
    return result;
}

} // namespace nickel::resources
