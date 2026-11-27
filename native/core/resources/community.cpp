#include "community.hpp"
#include "mods.hpp"
#include "utility.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <limits>
#include <set>
#include <thread>

namespace nickel::resources {
namespace {

const Json& member(const Json& object, const char* name) {
    static const Json absent;
    const auto found = object.find(name);
    return found == object.end() ? absent : *found;
}

std::string text(const Json& object, const char* key, std::size_t maximum = 16384) {
    const auto& value = member(object, key);
    if (!value.is_string()) return {};
    const auto& result = value.get_ref<const std::string&>();
    if (result.size() > maximum || !validUtf8(result)) fail("invalid_response", "Provider text exceeds its field budget or is invalid UTF-8");
    return result;
}

std::uint64_t count(const Json& value, std::uint64_t fallback = 0) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer()) { const auto number = value.get<std::int64_t>(); return number >= 0 ? std::uint64_t(number) : fallback; }
    return fallback;
}

std::string idValue(const Json& value) {
    if (value.is_string()) return trim(value.get_ref<const std::string&>());
    const auto id = count(value);
    return id && id <= 9007199254740991ULL ? std::to_string(id) : std::string{};
}

Strings stringList(const Json& object, const char* key, bool normalize = false) {
    Strings result;
    const auto& array = member(object, key);
    if (!array.is_array()) return result;
    if (array.size() > 1024) fail("too_large", "Provider string list exceeds budget");
    for (const auto& item : array) {
        if (!item.is_string()) continue;
        auto value = trim(item.get_ref<const std::string&>());
        if (normalize) value = lower(value);
        if (!value.empty() && value.size() <= 256 && validUtf8(value) && !contains(result, value)) result.push_back(std::move(value));
    }
    return result;
}

int classId(ResourceKind kind) {
    switch (kind) {
    case ResourceKind::Mod: return 6;
    case ResourceKind::Modpack: return 4471;
    case ResourceKind::Datapack: return 6945;
    case ResourceKind::Resourcepack: return 12;
    case ResourceKind::Shader: return 6552;
    }
    return 0;
}

ResourceKind classKind(std::uint64_t id) {
    for (const auto kind : {ResourceKind::Mod, ResourceKind::Modpack, ResourceKind::Datapack, ResourceKind::Resourcepack, ResourceKind::Shader}) if (std::uint64_t(classId(kind)) == id) return kind;
    fail("invalid_response", "Unknown provider project class");
}

int loaderId(const std::string& raw) {
    const auto loader = lower(trim(raw));
    if (loader == "forge") return 1;
    if (loader == "fabric") return 4;
    if (loader == "quilt") return 5;
    if (loader == "neoforge") return 6;
    return 0;
}

std::string category(ResourceKind kind) {
    switch (kind) {
    case ResourceKind::Mod: return "mc-mods";
    case ResourceKind::Modpack: return "modpacks";
    case ResourceKind::Datapack: return "data-packs";
    case ResourceKind::Resourcepack: return "texture-packs";
    case ResourceKind::Shader: return "shaders";
    }
    return {};
}

std::string pageUrl(Source source, ResourceKind kind, const std::string& slug) {
    if (!validIdentifier(Source::Modrinth, slug)) return {};
    return source == Source::Modrinth ? "https://modrinth.com/project/" + slug : "https://www.curseforge.com/minecraft/" + category(kind) + '/' + slug;
}

std::string hashValue(const Json& value, std::size_t length) {
    return value.is_string() ? checkedHash(value.get_ref<const std::string&>(), length) : std::string{};
}

std::optional<std::vector<Dependency>> dependencies(Source source, const Json& object) {
    const auto& array = member(object, "dependencies");
    if (!array.is_array()) return {};
    if (array.size() > 1024) fail("too_large", "Dependency list exceeds budget");
    std::vector<Dependency> result;
    for (const auto& item : array) {
        if (!item.is_object()) continue;
        if (source == Source::Modrinth) {
            if (text(item, "dependency_type") != "required") continue;
            result.push_back({trim(text(item, "project_id", 128)), trim(text(item, "version_id", 128)), trim(text(item, "file_name", 256))});
        } else if (count(member(item, "relationType")) == 3) result.push_back({idValue(member(item, "modId")), {}, {}});
    }
    return result;
}

Project readProjectObject(Source source, const Json& object, std::optional<ResourceKind> requestedKind = {}) {
    if (!object.is_object()) fail("invalid_response", "Provider project is not an object");
    Project result;
    result.source = source;
    result.id = source == Source::Modrinth ? text(object, "project_id", 128) : idValue(member(object, "id"));
    if (source == Source::Modrinth && result.id.empty()) result.id = text(object, "id", 128);
    result.slug = trim(text(object, "slug", 128));
    if (!validIdentifier(source, result.id) || !validIdentifier(Source::Modrinth, result.slug)) fail("invalid_response", "Provider project identity is invalid");
    if (source == Source::Modrinth) {
        result.kind = parseKind(text(object, "project_type", 32));
        result.title = text(object, "title", 512);
        result.summary = text(object, "description");
        result.iconUrl = text(object, "icon_url", 4096);
        result.downloads = count(member(object, "downloads"));
        result.author = text(object, "author", 256);
        result.gameVersions = stringList(object, "versions");
        result.loaders = stringList(object, "loaders", true);
        result.modifiedAt = text(object, "date_modified", 64);
        if (result.modifiedAt.empty()) result.modifiedAt = text(object, "updated", 64);
    } else {
        result.kind = classKind(count(member(object, "classId")));
        result.title = text(object, "name", 512);
        result.summary = text(object, "summary");
        const auto& logo = member(object, "logo");
        result.iconUrl = text(logo, "thumbnailUrl", 4096);
        if (result.iconUrl.empty()) result.iconUrl = text(logo, "url", 4096);
        result.downloads = count(member(object, "downloadCount"));
        const auto& authors = member(object, "authors");
        if (authors.is_array() && !authors.empty()) result.author = text(authors.front(), "name", 256);
        result.loaders = stringList(object, "modLoaderTypes", true);
        const auto& loaderTypes = member(object, "modLoaderTypes");
        if (loaderTypes.is_array()) for (const auto& loader : loaderTypes) {
            switch (count(loader)) {
            case 1: if (!contains(result.loaders, "forge")) result.loaders.push_back("forge"); break;
            case 4: if (!contains(result.loaders, "fabric")) result.loaders.push_back("fabric"); break;
            case 5: if (!contains(result.loaders, "quilt")) result.loaders.push_back("quilt"); break;
            case 6: if (!contains(result.loaders, "neoforge")) result.loaders.push_back("neoforge"); break;
            default: break;
            }
        }
        result.modifiedAt = text(object, "dateModified", 64);
    }
    if (requestedKind && result.kind != *requestedKind) fail("invalid_response", "Provider returned the wrong resource kind");
    if (result.title.empty()) result.title = result.slug;
    if (!result.iconUrl.empty()) try { result.iconUrl = checkedUrl(result.iconUrl, descriptionImageRules()); } catch (const ErrorException&) { result.iconUrl.clear(); }
    result.pageUrl = pageUrl(source, result.kind, result.slug);
    result.revision = result.modifiedAt;
    return result;
}

Json parseJson(const HttpResponse& response) {
    return detail::parseBoundedJson(response.bytes, 8 * 1024 * 1024);
}

std::string querySuffix(const std::string& path, const std::map<std::string, std::string>& query) {
    auto result = path;
    bool first = true;
    for (const auto& [key, value] : query) { result += first ? '?' : '&'; first = false; result += encodeQuery(key) + '=' + encodeQuery(value); }
    return result;
}

std::map<std::string, std::string> searchParameters(Source source, ResourceKind kind, const std::string& query, const Filter& filter, std::uint32_t offset, std::uint32_t limit) {
    std::map<std::string, std::string> params;
    const auto minecraft = trim(filter.minecraftVersion);
    const auto loader = defaultSearchLoader(filter.loader);
    if (source == Source::Modrinth) {
        Json facets = Json::array({Json::array({"project_type:" + kindName(kind)})});
        if (!minecraft.empty()) facets.push_back(Json::array({"versions:" + minecraft}));
        if ((kind == ResourceKind::Mod || kind == ResourceKind::Modpack) && !loader.empty()) facets.push_back(Json::array({"categories:" + loader}));
        params = {{"facets", facets.dump()}, {"offset", std::to_string(offset)}, {"limit", std::to_string(limit)}, {"index", "relevance"}};
        if (!trim(query).empty()) params["query"] = trim(query);
    } else {
        params = {{"gameId", "432"}, {"classId", std::to_string(classId(kind))}, {"index", std::to_string(offset)}, {"pageSize", std::to_string(limit)}, {"sortField", "2"}, {"sortOrder", "desc"}};
        if (!trim(query).empty()) params["searchFilter"] = trim(query);
        if (!minecraft.empty()) params["gameVersion"] = minecraft;
        if ((kind == ResourceKind::Mod || kind == ResourceKind::Modpack) && !minecraft.empty() && loaderId(loader)) params["modLoaderType"] = std::to_string(loaderId(loader));
    }
    return params;
}

std::optional<std::uint64_t> paginationTotal(const Json& document, Source source) {
    const auto& pagination = member(document, "pagination");
    const auto& value = source == Source::Modrinth
        ? (member(document, "total_hits").is_null() ? member(document, "total") : member(document, "total_hits"))
        : (member(pagination, "totalCount").is_null() ? member(pagination, "total") : member(pagination, "totalCount"));
    return value.is_number_integer() ? std::optional{count(value)} : std::nullopt;
}

std::string revision(const CachedResponse& response) {
    auto value = responseHeader(*response.response, "etag");
    if (value.empty()) value = responseHeader(*response.response, "last-modified");
    return value;
}

bool httpNotFound(const Error& error) {
    return error.code == "not_found" || (error.details.is_object() && member(error.details, "httpStatus") == 404);
}

} // namespace

ProjectVersion readVersionResponse(Source source, const Json& object, const std::string& expectedProject) {
    if (!object.is_object()) fail("invalid_response", "Provider version is not an object");
    ProjectVersion version;
    version.source = source;
    version.id = source == Source::Modrinth ? text(object, "id", 128) : idValue(member(object, "id"));
    version.projectId = source == Source::Modrinth ? text(object, "project_id", 128) : idValue(member(object, "modId"));
    if (!validIdentifier(source, version.id) || !validIdentifier(source, version.projectId)) fail("invalid_response", "Provider version identity is invalid");
    if (!expectedProject.empty() && version.projectId != expectedProject) fail("invalid_response", "Version belongs to another project");
    version.requiredDependencies = dependencies(source, object);
    if (source == Source::Modrinth) {
        version.name = text(object, "name", 512);
        version.versionNumber = text(object, "version_number", 512);
        version.loaders = stringList(object, "loaders", true);
        version.gameVersions = stringList(object, "game_versions");
        version.publishedAt = text(object, "date_published", 64);
        const auto release = text(object, "version_type", 32);
        version.releaseType = release == "release" ? ReleaseType::Release : release == "beta" ? ReleaseType::Beta : release == "alpha" ? ReleaseType::Alpha : ReleaseType::Unknown;
        const auto& files = member(object, "files");
        if (!files.is_array() || files.size() > 64) fail("invalid_response", "Version file manifest is invalid or too large");
        for (const auto& item : files) {
            if (!item.is_object()) continue;
            VersionFile file;
            file.url = text(item, "url", 4096);
            file.fileName = text(item, "filename", 256);
            file.sizeBytes = count(member(item, "size"));
            file.primary = member(item, "primary").is_boolean() && member(item, "primary").get<bool>();
            const auto& hashes = member(item, "hashes");
            file.sha1 = hashValue(member(hashes, "sha1"), 20);
            file.sha512 = hashValue(member(hashes, "sha512"), 64);
            if (!file.url.empty()) version.files.push_back(std::move(file));
        }
    } else {
        version.name = text(object, "displayName", 512);
        version.versionNumber = text(object, "fileName", 256);
        if (version.name.empty()) version.name = version.versionNumber;
        version.loaders = stringList(object, "modLoaderTypes", true);
        for (const auto& tag : stringList(object, "gameVersions")) {
            const auto loader = lower(tag);
            if (knownLoader(loader)) { if (!contains(version.loaders, loader)) version.loaders.push_back(loader); }
            else if (!tag.empty() && ((tag.front() >= '0' && tag.front() <= '9') || tag.starts_with("b1.") || tag.starts_with("a1.") || tag.starts_with("c0.") || tag.starts_with("rd-") || tag.starts_with("inf-"))) version.gameVersions.push_back(tag);
        }
        version.publishedAt = text(object, "fileDate", 64);
        const auto release = count(member(object, "releaseType"));
        version.releaseType = release == 1 ? ReleaseType::Release : release == 2 ? ReleaseType::Beta : release == 3 ? ReleaseType::Alpha : ReleaseType::Unknown;
        VersionFile file;
        file.url = text(object, "downloadUrl", 4096);
        file.fileName = version.versionNumber;
        file.sizeBytes = count(member(object, "fileLength"));
        file.primary = true;
        const auto& hashes = member(object, "hashes");
        if (hashes.is_array()) for (const auto& item : hashes) if (count(member(item, "algo")) == 1) file.sha1 = hashValue(member(item, "value"), 20);
        if (!file.url.empty()) version.files.push_back(std::move(file));
    }
    version.revision = version.publishedAt;
    return version;
}

SourceSearchPage readSearchResponse(Source source, ResourceKind kind, const Json& document, std::uint32_t offset, std::uint32_t limit) {
    if (!document.is_object()) fail("invalid_response", "Provider search is not an object");
    const auto& raw = member(document, source == Source::Modrinth ? "hits" : "data");
    if (!raw.is_array() || raw.size() > 1000) fail("invalid_response", "Provider search page is invalid");
    SourceSearchPage page;
    page.source = source;
    for (const auto& item : raw) {
        if (!item.is_object()) continue;
        try { page.projects.push_back(readProjectObject(source, item, kind)); }
        catch (const ErrorException& failure) { if (failure.error().code == "too_large") throw; }
    }
    page.cursor.total = paginationTotal(document, source);
    page.cursor.offset = offset;
    if (std::uint64_t(offset) + raw.size() > std::numeric_limits<std::uint32_t>::max()) fail("invalid_response", "Search cursor overflow");
    page.cursor.nextOffset = offset + std::uint32_t(raw.size());
    page.cursor.hasMore = !raw.empty() && (page.cursor.total ? page.cursor.nextOffset < *page.cursor.total : raw.size() >= limit);
    return page;
}

std::vector<ProjectVersion> readVersionsResponse(Source source, const Json& document, const std::string& expectedProject) {
    const auto& raw = source == Source::Modrinth ? document : member(document, "data");
    if (!raw.is_array()) fail("invalid_response", "Provider version history is not an array");
    if (raw.size() > 20000) fail("too_large", "Version history exceeds budget");
    std::set<std::string> ids;
    std::vector<ProjectVersion> result;
    for (const auto& item : raw) {
        if (!item.is_object()) continue;
        auto version = readVersionResponse(source, item, expectedProject);
        if (ids.insert(version.id).second) result.push_back(std::move(version));
    }
    return result;
}

Project readProjectResponse(Source source, const Json& document, const std::string& expectedIdentity) {
    if (!document.is_object()) fail("invalid_response", "Provider project response is not an object");
    auto result = readProjectObject(source, source == Source::Modrinth ? document : member(document, "data"));
    if (!expectedIdentity.empty() && result.id != expectedIdentity && !(source == Source::Modrinth && result.slug == expectedIdentity)) fail("invalid_response", "Provider returned a different project");
    return result;
}

CommunityService::CommunityService(CachedHttp& http, ProviderSettings settings, ResourceServices services)
    : http_(http), settings_(std::move(settings)), services_(std::move(services)) {}
void CommunityService::setSettings(ProviderSettings settings) { std::lock_guard lock(settingsMutex_); settings_ = std::move(settings); }
ProviderSettings CommunityService::settings() const { std::lock_guard lock(settingsMutex_); return settings_; }

InstanceContext CommunityService::instanceContext(const InstanceIdentity& instance) const {
    if (!services_.context) fail("unsupported", "Shared instance context is unavailable");
    auto context = services_.context(instance);
    if (context.identity != instance) fail("policy_denied", "Instance context has a different identity");
    return context;
}

CachedResponse CommunityService::json(Source source, const std::string& suffix, const std::string& scope, ResponseValidator validator,
                                      const Cancellation& cancel, bool refresh, const Bytes& body) {
    const auto snapshot = settings();
    auto candidates = apiCandidates(source, suffix, snapshot);
    for (auto& candidate : candidates) {
        candidate.request.validationScope = scope + ':' + snapshot.credentialRevision;
        if (!body.empty()) { candidate.request.method = "POST"; candidate.request.body = body; candidate.request.headers["content-type"] = "application/json"; }
    }
    CachePolicy policy;
    policy.forceRefresh = refresh;
    policy.cache = body.empty();
    return fetchCandidates(http_, std::move(candidates), snapshot.apiPolicy, policy, std::move(validator), cancel);
}

SearchPage CommunityService::search(const SearchQuery& query, const Cancellation& cancel) {
    cancel.check();
    if (!query.page || !query.pageSize || query.pageSize > 100 || query.text.size() > 1024 || !validUtf8(query.text) ||
        query.filter.minecraftVersion.size() > 256 || query.filter.loader.size() > 64 || kindName(query.kind).empty()) fail("invalid_input", "Invalid community search parameters");
    std::vector<Source> sources;
    if (query.sources != SourceSelection::CurseForge) sources.push_back(Source::Modrinth);
    if (query.sources != SourceSelection::Modrinth) sources.push_back(Source::CurseForge);
    const auto perSource = query.sources == SourceSelection::All ? std::max(1U, (query.pageSize + 1) / 2) : query.pageSize;
    for (const auto source : sources) {
        const auto limit = std::min(perSource, source == Source::Modrinth ? 100U : 50U);
        if (std::uint64_t(query.page - 1) * limit > 10000) fail("invalid_input", "Search page exceeds provider limit");
    }
    std::map<Source, SourceSearchPage> pages;
    std::map<Source, Error> errors;
    std::mutex mutex;
    std::vector<std::jthread> workers;
    for (const auto source : sources) workers.emplace_back([&, source] {
        try {
            const auto limit = std::min(perSource, source == Source::Modrinth ? 100U : 50U);
            const auto offset = std::uint32_t(std::uint64_t(query.page - 1) * limit);
            const auto suffix = querySuffix(source == Source::Modrinth ? "search" : "mods/search", searchParameters(source, query.kind, query.text, query.filter, offset, limit));
            const auto response = json(source, suffix, "search:" + kindName(query.kind), [=](const auto& raw) { (void)readSearchResponse(source, query.kind, parseJson(raw), offset, limit); }, cancel, query.forceRefresh);
            auto page = readSearchResponse(source, query.kind, parseJson(*response.response), offset, limit);
            page.freshness = response.freshness;
            page.revision = revision(response);
            std::lock_guard lock(mutex);
            pages.emplace(source, std::move(page));
        } catch (const ErrorException& error) { std::lock_guard lock(mutex); errors.emplace(source, error.error()); }
          catch (...) { std::lock_guard lock(mutex); errors.emplace(source, Error{"network", "Search provider failed"}); }
    });
    for (auto& worker : workers) worker.join();
    cancel.check();
    if (pages.empty()) throw ErrorException(errors.begin()->second);
    SearchPage result;
    result.sourceErrors = std::move(errors);
    bool totalsKnown = result.sourceErrors.empty();
    std::uint64_t total = 0;
    std::vector<Project> first, second;
    for (const auto& [source, page] : pages) {
        (source == Source::Modrinth ? first : second) = page.projects;
        result.sourceCursors.emplace(source, page.cursor);
        result.hasMore |= page.cursor.hasMore;
        if (page.cursor.total) {
            if (*page.cursor.total > std::numeric_limits<std::uint64_t>::max() - total) fail("invalid_response", "Combined search total overflow");
            total += *page.cursor.total;
        } else totalsKnown = false;
        if (page.freshness == Freshness::Stale) result.freshness = Freshness::Stale;
        else if (page.freshness == Freshness::Revalidated && result.freshness != Freshness::Stale) result.freshness = Freshness::Revalidated;
    }
    result.projects = mergeProjects(first, second);
    if (totalsKnown) result.total = total;
    result.revision = query.revision;
    return result;
}

Project CommunityService::project(Source source, const std::string& idOrSlug, const Cancellation& cancel) {
    if (!validIdentifier(source, idOrSlug)) fail("invalid_input", "Invalid project identifier");
    const auto response = json(source, (source == Source::Modrinth ? "project/" : "mods/") + idOrSlug, "project:" + idOrSlug,
                               [=](const auto& raw) { (void)readProjectResponse(source, parseJson(raw), idOrSlug); }, cancel);
    auto value = readProjectResponse(source, parseJson(*response.response), idOrSlug);
    const auto etag = revision(response);
    if (!etag.empty()) value.revision = etag;
    return value;
}

VersionPage CommunityService::versions(Source source, ResourceKind kind, const std::string& idOrSlug, Filter filter,
                                       std::uint32_t offset, std::uint32_t limit, const Cancellation& cancel, bool refresh) {
    if (!validIdentifier(source, idOrSlug) || !limit || limit > 100 || offset > 1000000 || filter.minecraftVersion.size() > 256 || filter.loader.size() > 64) fail("invalid_input", "Invalid version page parameters");
    std::map<std::string, std::string> params;
    auto loader = defaultSearchLoader(filter.loader);
    if (kind != ResourceKind::Mod && kind != ResourceKind::Modpack) loader.clear();
    std::string path;
    if (source == Source::Modrinth) {
        path = "project/" + idOrSlug + "/version";
        if (!filter.minecraftVersion.empty()) params["game_versions"] = Json::array({filter.minecraftVersion}).dump();
        if (!loader.empty()) params["loaders"] = Json::array({loader}).dump();
    } else {
        limit = std::min(limit, 50U);
        path = "mods/" + idOrSlug + "/files";
        params = {{"index", std::to_string(offset)}, {"pageSize", std::to_string(limit)}};
        if (!filter.minecraftVersion.empty()) {
            params["gameVersion"] = filter.minecraftVersion;
            if (loaderId(loader)) params["modLoaderType"] = std::to_string(loaderId(loader));
        }
    }
    const auto response = json(source, querySuffix(path, params), "versions:" + idOrSlug, [=](const auto& raw) { (void)readVersionsResponse(source, parseJson(raw)); }, cancel, refresh);
    const auto document = parseJson(*response.response);
    auto items = readVersionsResponse(source, document);
    std::erase_if(items, [&](const auto& item) { return (!filter.minecraftVersion.empty() && !contains(item.gameVersions, filter.minecraftVersion)) || (!loader.empty() && !contains(item.loaders, loader, true)); });
    VersionPage page;
    page.freshness = response.freshness;
    page.revision = revision(response);
    page.cursor.offset = offset;
    if (source == Source::Modrinth) {
        page.cursor.total = items.size();
        if (offset < items.size()) page.versions.assign(items.begin() + offset, items.begin() + std::min<std::size_t>(items.size(), std::uint64_t(offset) + limit));
        page.cursor.nextOffset = offset + std::uint32_t(page.versions.size());
        page.cursor.hasMore = page.cursor.nextOffset < items.size();
    } else {
        const auto rawCount = member(document, "data").size();
        if (std::uint64_t(offset) + rawCount > std::numeric_limits<std::uint32_t>::max()) fail("invalid_response", "Version cursor overflow");
        page.versions = std::move(items);
        page.cursor.total = paginationTotal(document, source);
        page.cursor.nextOffset = offset + std::uint32_t(rawCount);
        page.cursor.hasMore = rawCount && (page.cursor.total ? page.cursor.nextOffset < *page.cursor.total : rawCount >= limit);
    }
    return page;
}

std::vector<ProjectVersion> CommunityService::allVersions(Source source, ResourceKind kind, const std::string& idOrSlug, const Cancellation& cancel, bool refresh) {
    std::vector<ProjectVersion> result;
    std::set<std::string> ids;
    std::uint32_t offset = 0;
    for (unsigned pageNumber = 0; pageNumber < 1000; ++pageNumber) {
        cancel.check();
        const auto page = versions(source, kind, idOrSlug, {}, offset, source == Source::Modrinth ? 100 : 50, cancel, refresh);
        for (const auto& item : page.versions) if (ids.insert(item.id).second) result.push_back(item);
        if (result.size() > 20000) fail("too_large", "Version history exceeds budget");
        if (!page.cursor.hasMore) return result;
        if (page.cursor.nextOffset <= offset) fail("invalid_response", "Provider history cursor did not advance");
        offset = page.cursor.nextOffset;
    }
    fail("too_large", "Provider history exceeded page budget");
}

ProjectVersion CommunityService::exactVersion(Source source, const std::string& projectId, const std::string& versionId, const Cancellation& cancel) {
    if (!validIdentifier(source, versionId) || (source == Source::CurseForge && !validIdentifier(source, projectId)) || (!projectId.empty() && !validIdentifier(source, projectId))) fail("invalid_input", "Invalid exact version identity");
    const auto path = source == Source::Modrinth ? "version/" + versionId : "mods/" + projectId + "/files/" + versionId;
    const auto response = json(source, path, "exact:" + versionId + ':' + projectId, [=](const auto& raw) {
        const auto document = parseJson(raw);
        const auto value = readVersionResponse(source, source == Source::Modrinth ? document : member(document, "data"), projectId);
        if (value.id != versionId) fail("invalid_response", "Exact version has the wrong identity");
    }, cancel);
    const auto document = parseJson(*response.response);
    return readVersionResponse(source, source == Source::Modrinth ? document : member(document, "data"), projectId);
}

ResolvedDependency CommunityService::resolveDependency(Source source, const Dependency& dependency, const Cancellation& cancel) {
    const auto requestedProject = trim(dependency.projectId);
    const auto requestedVersion = trim(dependency.versionId);
    if (requestedProject.empty() && requestedVersion.empty()) fail("invalid_input", "Filename-only dependency cannot identify a project");
    std::optional<ProjectVersion> pinned;
    if (!requestedVersion.empty()) pinned = exactVersion(source, requestedProject, requestedVersion, cancel);
    auto selected = project(source, pinned ? pinned->projectId : requestedProject, cancel);
    if (!requestedProject.empty() && requestedProject != selected.id && requestedProject != selected.slug) fail("invalid_response", "Pinned dependency belongs to another project");
    if (pinned && pinned->projectId != selected.id) fail("invalid_response", "Dependency project and exact version disagree");
    if (pinned) { selected.gameVersions = pinned->gameVersions; selected.loaders = pinned->loaders; }
    return {std::move(selected), std::move(pinned)};
}

documents::FullDescription CommunityService::fullDescription(Source source, const std::string& projectId, const Cancellation& cancel, bool refresh) {
    if (!validIdentifier(source, projectId)) fail("invalid_input", "Invalid description project identity");
    const auto suffix = source == Source::Modrinth ? "project/" + projectId : "mods/" + projectId + "/description";
    const auto response = json(source, suffix, "description:" + projectId, [=](const HttpResponse& raw) {
        const auto document = parseJson(raw);
        const auto& body = member(document, source == Source::Modrinth ? "body" : "data");
        if (!document.is_object() || !body.is_string()) fail("invalid_response", "Full project description is unavailable");
        if (body.get_ref<const std::string&>().size() > 512 * 1024) fail("too_large", "Full description exceeds budget");
        if (source == Source::Modrinth && text(document, "id", 128) != projectId && text(document, "slug", 128) != projectId) fail("invalid_response", "Description belongs to another project");
    }, cancel, refresh);
    cancel.check();
    const auto document = parseJson(*response.response);
    documents::FullDescription result;
    result.source = source;
    result.projectId = source == Source::Modrinth ? text(document, "id", 128) : projectId;
    result.format = source == Source::Modrinth ? documents::BodyFormat::Markdown : documents::BodyFormat::Html;
    result.text = text(document, source == Source::Modrinth ? "body" : "data", 512 * 1024);
    result.canonicalBase = source == Source::Modrinth ? pageUrl(source, ResourceKind::Mod, text(document, "slug", 128)) : "https://www.curseforge.com/";
    result.revision = revision(response);
    result.freshness = response.freshness;
    return result;
}

DownloadPlan CommunityService::prepareDownload(Source source, ResourceKind kind, const std::string& projectId, const std::string& versionId, const Cancellation& cancel) {
    const auto selected = project(source, projectId, cancel);
    if (selected.kind != kind) fail("invalid_response", "Selected project has a different resource kind");
    const auto version = exactVersion(source, selected.id, versionId, cancel);
    if (version.projectId != selected.id) fail("invalid_response", "Selected version belongs to another project");
    if (version.files.empty()) fail("not_found", "Author has not permitted file distribution");
    const auto primary = std::find_if(version.files.begin(), version.files.end(), [](const auto& file) { return file.primary; });
    auto file = primary == version.files.end() ? version.files.front() : *primary;
    if (!validResourceFileName(file.fileName) || (file.sha1.empty() && file.sha512.empty())) fail("integrity", "Download filename or hash is invalid");
    const auto name = lower(file.fileName);
    const bool extension = kind == ResourceKind::Mod ? name.ends_with(".jar") || name.ends_with(".litemod") || name.ends_with(".zip")
        : kind == ResourceKind::Modpack ? name.ends_with(source == Source::Modrinth ? ".mrpack" : ".zip") : name.ends_with(".zip");
    if (!extension) fail("integrity", "Download file extension does not match resource kind");
    if (!file.sizeBytes || file.sizeBytes > 512ULL * 1024 * 1024) fail("too_large", "Resource installation size is invalid or exceeds budget");
    if (kind == ResourceKind::Mod) std::replace(file.fileName.begin(), file.fileName.end(), '~', '-');
    auto candidates = fileCandidates(file.url, settings());
    return {source, kind, selected.id, versionId, std::move(file), std::move(candidates)};
}

InstallReceipt CommunityService::install(const InstallRequest& request, const Cancellation& cancel, const ProgressCallback& progress) {
    const auto plan = prepareDownload(request.source, request.kind, request.projectId, request.versionId, cancel);
    if (request.kind == ResourceKind::Modpack) {
        if (!validResourceFileName(request.newInstanceName)) fail("invalid_input", "Invalid new instance name");
        if (!services_.installPack) fail("unsupported", "Shared modpack transaction is unavailable");
        return services_.installPack(request, plan, cancel, progress);
    }
    if (!services_.install || !services_.acquireMutation) fail("unsupported", "Shared resource transaction is unavailable");
    const auto context = instanceContext(request.instance);
    if (context.revision != request.instanceRevision || context.minecraftVersion.empty() || context.running) fail("conflict", "Instance context changed or the instance is running");
    const auto lease = services_.acquireMutation(request.instance, request.instanceRevision);
    if (!lease) fail("busy", "Instance mutation lease was not acquired");
    cancel.check();
    return services_.install(context, plan, lease, cancel, progress);
}

ProjectVersion CommunityService::lookupModrinthHash(const std::string& raw, const Cancellation& cancel) {
    const auto hash = checkedHash(raw, 20);
    if (hash.empty()) fail("invalid_input", "Invalid Modrinth SHA1");
    const auto response = json(Source::Modrinth, "version_file/" + hash + "?algorithm=sha1", "hash:" + hash, [=](const auto& rawResponse) {
        const auto value = readVersionResponse(Source::Modrinth, parseJson(rawResponse));
        if (std::none_of(value.files.begin(), value.files.end(), [&](const auto& file) { return file.sha1 == hash; })) fail("integrity", "Hash lookup returned a different file");
    }, cancel);
    return readVersionResponse(Source::Modrinth, parseJson(*response.response));
}

std::optional<ProjectVersion> CommunityService::lookupCurseForgeFingerprint(std::uint32_t fingerprint, const Cancellation& cancel) {
    const auto encoded = Json{{"fingerprints", Json::array({fingerprint})}}.dump();
    const Bytes body(encoded.begin(), encoded.end());
    const auto response = json(Source::CurseForge, "fingerprints", "fingerprint", [](const auto& raw) {
        const auto document = parseJson(raw);
        if (!member(member(document, "data"), "exactMatches").is_array()) fail("invalid_response", "Fingerprint response is invalid");
    }, cancel, false, body);
    const auto document = parseJson(*response.response);
    for (const auto& item : member(member(document, "data"), "exactMatches")) {
        const auto& file = member(item, "file");
        if (count(member(file, "fileFingerprint"), std::numeric_limits<std::uint64_t>::max()) != fingerprint) continue;
        return readVersionResponse(Source::CurseForge, file);
    }
    return {};
}

std::optional<ResolvedResourceFile> CommunityService::resolveFileHashes(const std::string& sha1, const std::string& sha512,
                                                                     bool modrinthOnly, std::optional<std::uint32_t> fingerprint,
                                                                     const Cancellation& cancel) {
    const auto first = checkedHash(sha1, 20);
    const auto second = checkedHash(sha512, 64);
    if (first.empty() || second.empty()) fail("invalid_input", "Export requires local SHA1 and SHA512");
    const auto match = [&](const ProjectVersion& version) -> std::optional<ResolvedResourceFile> {
        for (const auto& file : version.files) {
            if (file.sha1 != first) continue;
            if (!file.sha512.empty() && file.sha512 != second) fail("integrity", "Provider SHA512 differs from the local file");
            if (!validResourceFileName(file.fileName) || !file.sizeBytes) fail("invalid_response", "Export file metadata is invalid");
            auto pinned = file;
            pinned.url = checkedUrl(file.url, providerFileRules());
            pinned.sha1 = first;
            pinned.sha512 = second;
            return ResolvedResourceFile{version.source, version.projectId, version.id, std::move(pinned)};
        }
        return {};
    };
    try {
        if (const auto found = match(lookupModrinthHash(first, cancel))) return found;
    } catch (const ErrorException& error) { if (!httpNotFound(error.error())) throw; }
    if (modrinthOnly || !fingerprint) return {};
    const auto current = lookupCurseForgeFingerprint(*fingerprint, cancel);
    return current ? match(*current) : std::nullopt;
}

} // namespace nickel::resources
