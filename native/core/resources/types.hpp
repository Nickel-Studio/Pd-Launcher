#pragma once

#include "core/common/cancellation.hpp"
#include "core/common/json.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::resources {

using Bytes = std::vector<std::uint8_t>;
using Strings = std::vector<std::string>;
using Path = std::filesystem::path;

enum class Source { Modrinth, CurseForge };
enum class SourceSelection { All, Modrinth, CurseForge };
enum class ResourceKind { Mod, Modpack, Datapack, Resourcepack, Shader };
enum class ReleaseType { Release, Beta, Alpha, Unknown };
enum class Freshness { Fresh, Revalidated, Stale };
enum class SourcePolicy { OfficialOnly, MirrorOnly, OfficialFirst, MirrorFirst, Fastest };

struct InstanceIdentity {
    std::string rootId;
    std::string instanceId;
    bool operator==(const InstanceIdentity&) const = default;
};

struct InstanceContext {
    InstanceIdentity identity;
    std::string label;
    std::string minecraftVersion;
    std::string loader;
    Path resourceRoot;
    Path modsRoot;
    Path resourcepacksRoot;
    Path shaderpacksRoot;
    Path datapacksRoot;
    std::string revision;
    bool updatesDisabled = false;
    bool running = false;
};

struct Filter {
    std::string minecraftVersion;
    std::string loader;
};

struct SearchQuery {
    SourceSelection sources = SourceSelection::All;
    ResourceKind kind = ResourceKind::Mod;
    std::string text;
    Filter filter;
    std::uint32_t page = 1;
    std::uint32_t pageSize = 20;
    bool forceRefresh = false;
    std::string revision;
};

struct Project {
    Source source = Source::Modrinth;
    ResourceKind kind = ResourceKind::Mod;
    std::string id;
    std::string slug;
    std::string title;
    std::string summary;
    std::string iconUrl;
    std::uint64_t downloads = 0;
    std::string author;
    Strings gameVersions;
    Strings loaders;
    std::string modifiedAt;
    std::string pageUrl;
    std::string revision;
};

struct Dependency {
    std::string projectId;
    std::string versionId;
    std::string fileName;
    bool operator==(const Dependency&) const = default;
};

struct VersionFile {
    std::string url;
    std::string fileName;
    std::uint64_t sizeBytes = 0;
    bool primary = false;
    std::string sha1;
    std::string sha512;
};

struct ProjectVersion {
    Source source = Source::Modrinth;
    std::string id;
    std::string projectId;
    std::string name;
    std::string versionNumber;
    Strings loaders;
    Strings gameVersions;
    std::string publishedAt;
    ReleaseType releaseType = ReleaseType::Unknown;
    std::vector<VersionFile> files;
    // Absent dependencies are not an authoritative empty dependency list.
    std::optional<std::vector<Dependency>> requiredDependencies;
    std::string revision;
};

struct PageCursor {
    std::uint32_t offset = 0;
    std::uint32_t nextOffset = 0;
    bool hasMore = false;
    std::optional<std::uint64_t> total;
};

struct SourceSearchPage {
    Source source = Source::Modrinth;
    std::vector<Project> projects;
    PageCursor cursor;
    Freshness freshness = Freshness::Fresh;
    std::string revision;
};

struct SearchPage {
    std::vector<Project> projects;
    std::optional<std::uint64_t> total;
    bool hasMore = false;
    std::map<Source, PageCursor> sourceCursors;
    std::map<Source, Error> sourceErrors;
    Freshness freshness = Freshness::Fresh;
    std::string revision;
};

struct VersionPage {
    std::vector<ProjectVersion> versions;
    PageCursor cursor;
    Freshness freshness = Freshness::Fresh;
    std::string revision;
};

struct ResolvedDependency {
    Project project;
    std::optional<ProjectVersion> exactVersion;
};

struct VersionGroup {
    std::string key;
    std::string version;
    std::string loader;
    std::vector<ProjectVersion> versions;
    bool selected = false;
    bool other = false;
};

struct VersionFilters {
    Strings groups;
    bool groupByMinor = false;
    bool foldOld = false;
};

std::string sourceName(Source source);
std::string kindName(ResourceKind kind);
ResourceKind parseKind(std::string_view kind);
std::string projectIdentity(const Project& project);
bool validIdentifier(Source source, std::string_view identifier);
bool knownLoader(std::string_view loader);
std::string defaultSearchLoader(std::string_view instanceLoader);
std::vector<Project> mergeProjects(const std::vector<Project>& primary, const std::vector<Project>& secondary);
std::vector<Dependency> deduplicateDependencies(const Project& project, const ProjectVersion& version);
VersionFilters deriveVersionFilters(const std::vector<ProjectVersion>& versions);
std::vector<VersionGroup> groupVersions(const std::vector<ProjectVersion>& versions, const Project& project,
                                      const Filter& filter, const InstanceContext& instance,
                                      const std::string& exactVersionId = {});

std::string trim(std::string_view value);
std::string lower(std::string_view value);
bool contains(const Strings& values, std::string_view value, bool ignoreCase = false);
bool validUtf8(std::string_view text);
std::string utf8Path(const Path& path);
Path pathFromUtf8(std::string_view text);
std::string checkedHash(std::string_view hash, std::size_t bytes);
[[noreturn]] void fail(std::string code, std::string message, Json details = Json::object());

} // namespace nickel::resources
