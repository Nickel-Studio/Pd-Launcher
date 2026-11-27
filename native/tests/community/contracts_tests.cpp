#include "fixtures.hpp"
#include "core/resources/utility.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <thread>

namespace nickel::tests {
namespace {
using namespace documents;
using namespace std::chrono_literals;

template<class Predicate>
void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, "Contract fixture exceeded deadline");
        std::this_thread::sleep_for(1ms);
    }
}

HttpRequest apiRequest(std::string suffix) {
    HttpRequest request;
    request.url = "https://api.modrinth.com/v2/" + suffix;
    request.rules = providerApiRules();
    return request;
}

FullDescription description(std::string value) {
    FullDescription result;
    result.projectId = "p1";
    result.format = BodyFormat::Html;
    result.text = std::move(value);
    result.canonicalBase = "https://cdn.modrinth.com/data/p1/";
    return result;
}

HttpResponse png(const HttpRequest& request, std::string type = "image/png") {
    return {Bytes{0x89, 'P', 'N', 'G', 13, 10, 26, 10}, 200, {{"content-type", std::move(type)}}, request.url};
}

void boundedJson() {
    require(detail::parseBoundedJson(bytes("{\"a\":[1,2]}"), 64)["a"].size() == 2, "Valid bounded JSON differs");
    expectError("too_large", [&] { (void)detail::parseBoundedJson(bytes("[1,2]"), 2); });
    expectError("invalid_response", [&] { (void)detail::parseBoundedJson(bytes("{"), 64); });
    expectError("invalid_response", [&] { (void)detail::parseBoundedJson(Bytes{0xff}, 64); });
    const auto deep = std::string(70, '[') + "0" + std::string(70, ']');
    expectError("too_large", [&] { (void)detail::parseBoundedJson(bytes(deep), 256); });
}

void dependencyAndHistory() {
    Strings calls;
    CachedHttp http([&](const HttpRequest& request, const Cancellation&) {
        calls.push_back(request.url);
        if (request.url.ends_with("version/v2")) return response(request, mrVersion("v2", "p2"));
        if (request.url.ends_with("project/p2")) return response(request, mrProject("p2", "dependency"));
        if (request.url.find("index=0") != std::string::npos)
            return response(request, {{"data", Json::array({cfVersion(101), cfVersion(102)})}, {"pagination", {{"totalCount", 4}}}});
        return response(request, {{"data", Json::array({cfVersion(102), cfVersion(103)})}, {"pagination", {{"totalCount", 4}}}});
    }, 32768, 1);
    CommunityService service(http);
    Dependency dependency;
    dependency.versionId = "v2";
    const auto resolved = service.resolveDependency(Source::Modrinth, dependency);
    require(resolved.project.id == "p2" && resolved.exactVersion && resolved.exactVersion->id == "v2", "Version-only dependency lost its pinned project");
    require(resolved.project.loaders == resolved.exactVersion->loaders, "Pinned dependency compatibility is not exact");
    dependency.projectId = "p1";
    expectError("invalid_response", [&] { (void)service.resolveDependency(Source::Modrinth, dependency); });
    Dependency filenameOnly;
    expectError("invalid_input", [&] { (void)service.resolveDependency(Source::Modrinth, filenameOnly); });
    const auto history = service.allVersions(Source::CurseForge, ResourceKind::Mod, "42");
    require(history.size() == 3 && history[2].id == "103", "Provider history did not deduplicate overlapping pages");
    require(std::any_of(calls.begin(), calls.end(), [](const auto& url) { return url.find("index=2") != std::string::npos; }), "History did not advance by raw provider count");
}

void searchProviderLimits() {
    Strings requests;
    CachedHttp http([&](const HttpRequest& request, const Cancellation&) {
        requests.push_back(request.url);
        if (request.url.find("api.modrinth.com") != std::string::npos)
            return response(request, {{"hits", Json::array()}, {"total_hits", 100}});
        Json projects = Json::array();
        const auto offset = request.url.find("index=50&") != std::string::npos ? 50U : 0U;
        for (unsigned index = 0; index < 50; ++index) {
            auto project = cfProject();
            project["id"] = offset + index + 1;
            project["slug"] = "fixture-" + std::to_string(offset + index + 1);
            projects.push_back(std::move(project));
        }
        return response(request, {{"data", projects}, {"pagination", {{"totalCount", 120}}}});
    }, 65536, 1);
    CommunityService service(http);
    SearchQuery query;
    query.sources = SourceSelection::CurseForge;
    query.pageSize = 100;
    const auto first = service.search(query);
    query.page = 2;
    const auto second = service.search(query);
    require(first.projects.size() == 50 && second.projects.size() == 50, "Capped provider page size differs");
    require(first.sourceCursors.at(Source::CurseForge).nextOffset == 50 && second.sourceCursors.at(Source::CurseForge).offset == 50 &&
        second.sourceCursors.at(Source::CurseForge).nextOffset == 100, "CurseForge page cap skips results between pages");
    require(first.projects.front().id != second.projects.front().id && second.hasMore && second.total == 120, "Capped search repeats results or loses provider totals");
    require(requests.size() == 2 && requests[1].find("index=50&") != std::string::npos && requests[1].find("pageSize=50&") != std::string::npos,
        "CurseForge offset is not based on its effective page size");
    query.page = 202;
    expectError("invalid_input", [&] { (void)service.search(query); });
    require(requests.size() == 2, "Out-of-range search reaches a provider");
    query.sources = SourceSelection::Modrinth;
    query.page = 2;
    (void)service.search(query);
    require(requests.back().find("offset=100") != std::string::npos && requests.back().find("limit=100&") != std::string::npos,
        "CurseForge page cap changes the Modrinth page size");
}

void cachePolicyAndScopes() {
    unsigned calls = 0;
    CachedHttp http([&](const HttpRequest& request, const Cancellation&) {
        ++calls;
        return response(request, {{"ok", true}});
    }, 32768, 1);
    auto request = apiRequest("scopes");
    request.validationScope = "a";
    request.validate = [](const auto&) {};
    (void)http.fetch(request);
    request.validationScope = "b";
    request.validate = [](const auto&) { fail("integrity", "Different validator must run"); };
    expectError("integrity", [&] { (void)http.fetch(request); });
    require(calls == 2, "Cache reused another response validation scope");
    request.validationScope = "a";
    request.validate = [](const auto&) {};
    request.headers["authorization"] = "different-credential";
    (void)http.fetch(request);
    require(calls == 3, "Credential scopes share a cache entry");
    CachePolicy unreasonable;
    unreasonable.staleFor = std::chrono::milliseconds::max();
    expectError("invalid_input", [&] { (void)http.fetch(request, unreasonable); });
    unsigned offlineCalls = 0;
    CachedHttp offline([&](const HttpRequest& raw, const Cancellation&) {
        if (++offlineCalls > 1) fail("network", "Controlled offline response");
        return response(raw, {{"ok", true}}, 200, {{"cache-control", "max-age=0, must-revalidate"}});
    }, 8192, 1);
    CachePolicy stale;
    stale.staleFor = 1s;
    (void)offline.fetch(apiRequest("mandatory"), stale);
    expectError("network", [&] { (void)offline.fetch(apiRequest("mandatory"), stale); });
}

void cacheQueueAndClear() {
    std::atomic<bool> started{false}, physicalCancelled{false};
    std::atomic<unsigned> physical{0};
    CachedHttp http([&](const HttpRequest& request, const Cancellation& cancel) {
        ++physical;
        started = true;
        while (!cancel.cancelled()) std::this_thread::sleep_for(1ms);
        physicalCancelled = true;
        cancel.check();
        return response(request, {});
    }, 8192, 1, 2);
    Cancellation firstCancel, queuedCancel;
    auto first = std::async(std::launch::async, [&] { expectError("cancelled", [&] { (void)http.fetch(apiRequest("running"), {}, firstCancel); }); });
    await([&] { return started.load(); });
    auto queued = std::async(std::launch::async, [&] { expectError("cancelled", [&] { (void)http.fetch(apiRequest("queued"), {}, queuedCancel); }); });
    await([&] { return http.stats().inFlight == 2; });
    expectError("busy", [&] { (void)http.fetch(apiRequest("overflow")); });
    queuedCancel.cancel();
    queued.get();
    require(http.stats().inFlight == 1 && physical == 1, "Queued cancellation starts an unwanted transfer");
    http.clear();
    first.get();
    await([&] { return physicalCancelled.load(); });
    require(http.stats().inFlight == 0 && http.stats().bytes == 0, "Cache clear leaves an old-generation flight");
}

void nativeHandleFencing() {
    TemporaryRoot root;
    const auto file = root.path / "source.jar";
    write(file, "original");
    write(root.path / "occupied.jar", "keep");
    expectError("conflict", [&] { detail::renameExclusive(root.path, "source.jar", "occupied.jar"); });
    expectError("conflict", [&] { detail::copyExclusive(file, root.path, "occupied.jar", 64, {}); });
    require(hashResourceFile(root.path / "occupied.jar").sha1 != hashResourceFile(file).sha1, "No-replace operation overwrote its target");
    Cancellation cancel;
    cancel.cancel();
    expectError("cancelled", [&] { detail::copyExclusive(file, root.path, "cancelled.jar", 64, cancel); });
    require(!std::filesystem::exists(root.path / "cancelled.jar"), "Cancelled copy publishes a destination");
    {
        detail::FileReader reader(file, 64);
        std::error_code error;
        std::filesystem::rename(file, root.path / "swapped.jar", error);
#ifdef _WIN32
        require(bool(error), "Pinned Windows reader permits delete/rename sharing");
        std::ofstream writer(file, std::ios::binary | std::ios::trunc);
        require(!writer, "Pinned Windows reader permits write sharing");
#else
        require(!error, "Cannot create POSIX swap fixture");
        write(file, "changed");
        expectError("conflict", [&] { reader.verifyUnchanged(); });
#endif
    }
    detail::renameExclusive(root.path, "source.jar", "renamed.jar");
    require(!std::filesystem::exists(file) && std::filesystem::exists(root.path / "renamed.jar"), "Exclusive rename did not publish");
    detail::removeRegular(root.path, "renamed.jar");
    require(!std::filesystem::exists(root.path / "renamed.jar"), "Regular removal failed");
}

void hostileBodyBudgets() {
    BodyReader reader;
    Cancellation cancelled;
    cancelled.cancel();
    expectError("cancelled", [&] { (void)reader.convert(description("<p>cancelled</p>"), cancelled); });
    expectError("too_large", [&] { (void)reader.convert(description(std::string(512 * 1024 + 1, 'x'))); });
    expectError("invalid_response", [&] { (void)reader.convert(description(std::string("\xff", 1))); });
    const auto unknown = reader.convert(description("<custom-box><p>safe &amp; visible</p></custom-box><iframe>hidden</iframe>"));
    require(unknown.plainText().find("safe & visible") != std::string::npos && unknown.html().find("hidden") == std::string::npos, "Unknown and active tag handling differs");
    BodyLimits allocatorLimit;
    allocatorLimit.maximumOutputBytes = 256;
    const auto arenaReader = BodyReader(allocatorLimit);
    for (unsigned run = 0; run < 32; ++run)
        expectError("too_large", [&] { (void)arenaReader.convert(description("<p>" + std::string(250, 'x') + "</p>")); });
    require(reader.convert(description("<p>after allocator unwinding</p>")).valid(), "Allocator interruption poisons subsequent parsing");
    BodyLimits tableLimit;
    tableLimit.maximumTableCells = 1;
    expectError("too_large", [&] { (void)BodyReader(tableLimit).convert(description("<table><tr><td>a</td><td>b</td></tr></table>")); });
    BodyLimits resourceLimit;
    resourceLimit.maximumImages = 1;
    resourceLimit.maximumLinks = 1;
    const auto limited = BodyReader(resourceLimit).convert(description("<img src='https://cdn.modrinth.com/a.png'><img src='https://cdn.modrinth.com/b.png' alt='bounded'><a href='https://example.org/a'>a</a><a href='https://example.org/b'>b</a>"));
    require(limited.images().size() == 1 && limited.links().size() == 1 && limited.plainText().find("bounded") != std::string::npos, "Description resource budget loses safe fallback text");
}

void imageDecodeAndLru() {
    const ResourceReply defaultReply;
    require(defaultReply.image && defaultReply.image->rgba.size() == 4, "Default resource reply lacks a valid placeholder");
    const auto body = BodyReader().convert(description("<img src='https://cdn.modrinth.com/a.png'><img src='https://cdn.modrinth.com/b.png'>"));
    std::atomic<unsigned> decoded{0};
    CachedHttp http([](const HttpRequest& request, const Cancellation&) { return png(request); }, 8192, 1);
    ImageLimits limits;
    limits.maximumCacheBytes = 1024;
    limits.maximumConcurrent = 1;
    BodyResources images(http, limits, [&](auto, const auto&, const auto&) {
        ++decoded;
        RasterImage image;
        image.width = 10;
        image.height = 10;
        image.stride = 40;
        image.rgba.resize(400);
        return image;
    });
    images.setBody(body);
    const auto first = body.images()[0].key, second = body.images()[1].key;
    require(images.lookup(first).state == ResourceState::Pending && decoded == 0, "Image lookup decodes implicitly");
    require(images.requestVisible(first).state == ResourceState::Ready && images.requestVisible(second).state == ResourceState::Ready, "Visible image decoding failed");
    require(images.decodedCacheBytes() <= 1024 && images.lookup(first).state == ResourceState::Pending && images.lookup(second).state == ResourceState::Ready, "Decoded byte LRU retains both images");
    BodyResources invalid(http, {}, [](auto, const auto&, const auto&) { RasterImage image; image.width = 10000; return image; });
    invalid.setBody(body);
    const auto failed = invalid.requestVisible(first);
    require(failed.state == ResourceState::Failed && failed.image && failed.image->width == 1, "Oversized decoded image escapes the placeholder boundary");
    unsigned activeDecoded = 0;
    CachedHttp active([](const HttpRequest& request, const Cancellation&) { return png(request, "image/png-malicious"); }, 8192, 1);
    BodyResources badType(active, {}, [&](auto, const auto&, const auto&) { ++activeDecoded; return RasterImage{}; });
    badType.setBody(body);
    require(badType.requestVisible(first).state == ResourceState::Failed && activeDecoded == 0, "Prefix-only media type check permits an unapproved type");
}

void imageGenerationCancellation() {
    std::atomic<bool> downloading{false}, cancelledPhysical{false};
    CachedHttp http([&](const HttpRequest& request, const Cancellation& cancel) {
        downloading = true;
        while (!cancel.cancelled()) std::this_thread::sleep_for(1ms);
        cancelledPhysical = true;
        cancel.check();
        return png(request);
    }, 8192, 1);
    const auto body = BodyReader().convert(description("<img src='https://cdn.modrinth.com/a.png'>"));
    BodyResources images(http, {}, [](auto, const auto&, const auto&) { return RasterImage{}; });
    images.setBody(body);
    const auto key = body.images()[0].key;
    auto visible = std::async(std::launch::async, [&] { expectError("cancelled", [&] { (void)images.requestVisible(key); }); });
    await([&] { return downloading.load(); });
    images.setBody(BodyReader().convert(description("<p>new document</p>")));
    visible.get();
    await([&] { return cancelledPhysical.load(); });
    require(images.lookup(key).state == ResourceState::Denied && images.decodedCacheBytes() == 0, "Document navigation resurrects stale image approvals");
}
} // namespace

unsigned runAdditionalContracts() {
    const std::array<std::pair<const char*, void(*)()>, 9> cases{{
        {"bounded JSON nesting and UTF-8", boundedJson}, {"pinned dependencies and overlapping history", dependencyAndHistory},
        {"effective provider search page limits", searchProviderLimits},
        {"cache scopes and mandatory freshness", cachePolicyAndScopes}, {"queue budget and generation clear", cacheQueueAndClear},
        {"native handle and no-replace fencing", nativeHandleFencing}, {"hostile body and allocator budgets", hostileBodyBudgets},
        {"decoded image byte LRU and valid failures", imageDecodeAndLru}, {"image generation physical cancellation", imageGenerationCancellation}}};
    unsigned failures = 0;
    for (const auto& [name, test] : cases) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const ErrorException& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.error().code << ": " << error.what() << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << "Additional controlled cases: " << cases.size() << ", failures: " << failures << '\n';
    return failures;
}
} // namespace nickel::tests
