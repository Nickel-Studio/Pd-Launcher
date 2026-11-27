#include "fixtures.hpp"
#include "core/resources/utility.hpp"
#ifndef NICKEL_CORE_ONLY
#include "core/resources/adapters.hpp"
#include "core/resources/export.hpp"
#include "core/archive/archive.hpp"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

using namespace nickel;
using namespace nickel::resources;
using namespace nickel::documents;
using namespace nickel::tests;
using namespace std::chrono_literals;

namespace {

template<class Predicate>
void waitUntil(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < end, "Controlled operation exceeded test deadline");
        std::this_thread::sleep_for(1ms);
    }
}

void identities() {
    const std::array<std::pair<std::string_view, std::uint32_t>, 5> vectors{{
        {"", 0x5bd15e36U}, {"a", 0x2550b18cU}, {"abc", 0x60a4fcc1U},
        {"test", 0x9ef9d837U}, {"The quick brown fox jumps over the lazy dog", 0xdf9f94f7U}}};
    TemporaryRoot root;
    const auto file = root.path / "fixture.jar";
    for (const auto& [text, expected] : vectors) {
        require(curseForgeFingerprint(bytes(text)) == expected, "Official Murmur2 vector differs");
        write(file, text);
        const auto identity = hashResourceFile(file);
        require(identity.curseForgeFingerprint == expected && identity.sizeBytes == text.size(), "Streaming fingerprint differs");
    }
    require(curseForgeFingerprint(bytes("a \t\r\nb")) == curseForgeFingerprint(bytes("ab")), "Fingerprint whitespace filter differs");
    require(curseForgeFingerprint(bytes("a\vb")) != curseForgeFingerprint(bytes("ab")), "Fingerprint filters extra whitespace");
    write(file, "abc");
    const auto hash = hashResourceFile(file);
    require(hash.sha1 == "a9993e364706816aba3e25717850c26c9cd0d89d", "Shared SHA1 differs");
    require(hash.sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "Shared SHA256 differs");
    require(hash.sha512 == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", "Shared SHA512 differs");
    expectError("too_large", [&] { (void)hashResourceFile(file, {}, 2); });
    Cancellation cancelled;
    cancelled.cancel();
    expectError("cancelled", [&] { (void)hashResourceFile(file, cancelled); });
    std::string large(131079, 'a');
    for (std::size_t index = 65530; index < 65542; ++index) large[index] = ' ';
    write(file, large);
    require(hashResourceFile(file).curseForgeFingerprint == curseForgeFingerprint(bytes(large)), "Fingerprint chunk boundary differs");
    for (const auto name : {"../bad.jar", "CON.jar", "AUX.jar", "com1.zip", "nul.txt", "trailing.jar.", "a:b.jar", "a\\b.jar", "a/b.jar", "conin$.jar", "LPT\xc2\xb2.jar"})
        require(!validResourceFileName(name), "Unsafe native filename accepted");
    require(validResourceFileName("a-1.0.jar") && splitModFileName("a.jar.disabled")->second == ModState::Disabled, "Valid state filename rejected");
    require(!validArchiveRelativePath("a/../b") && !validArchiveRelativePath("C:/x") && !validArchiveRelativePath("a//b"), "Archive traversal accepted");
    const auto linked = root.path / "linked.jar";
    std::error_code error;
    std::filesystem::create_hard_link(file, linked, error);
    require(!error, "Cannot create controlled hardlink fixture");
    expectError("policy_denied", [&] { (void)hashResourceFile(file); });
}

void urlPolicy() {
    const auto rules = providerFileRules();
    require(checkedUrl("https://cdn.modrinth.com/a", rules) == "https://cdn.modrinth.com/a", "Public approved URL rejected");
    for (const auto url : {"file:///C:/x", "qrc:/x", "data:image/png,x", "https://localhost/a", "https://127.0.0.1/a", "https://192.168.1.1/a", "https://[::1]/a", "https://user:pass@cdn.modrinth.com/a", "https://cdn.modrinth.com:8443/a", "https://cdn.modrinth.com.evil.com/a", "//cdn.modrinth.com/a", "https://cdn.modrinth.com/%5cfoo", "https://cdn.modrinth.com/%2500", "https://cdn.modrinth.com/a#fragment", "http://cdn.modrinth.com/a"})
        expectError("policy_denied", [&] { (void)checkedUrl(url, rules); });
    UrlRules controlled;
    controlled.testLoopbackOrigins = {"http://127.0.0.1:43001"};
    require(checkedUrl("http://127.0.0.1:43001/a", controlled).ends_with("/a"), "Explicit loopback fixture rejected");
    expectError("policy_denied", [&] { (void)checkedUrl("http://127.0.0.1:43002/a", controlled); });
    require(checkedRawUrl("../icon.png", descriptionImageRules(), "https://cdn.modrinth.com/a/b/") == "https://cdn.modrinth.com/a/icon.png", "Remote relative URL was not fenced");
    ProviderSettings settings;
    settings.curseforgeKey = "private-test-key";
    settings.curseforgeMirror = "https://mirror.example.org/v1";
    settings.modrinthMirror = "https://mirror.example.org/v2";
    const auto candidates = apiCandidates(Source::CurseForge, "mods/42", settings);
    require(candidates.size() == 2 && candidates.front().request.headers.contains("x-api-key") && !candidates.back().request.headers.contains("x-api-key"), "Provider credential leaked to mirror");
    for (const auto& candidate : candidates)
        require(!candidate.request.headers.contains("user-agent"), "CurseForge candidate overrides operation identity");
    for (const auto& candidate : apiCandidates(Source::Modrinth, "project/p1", settings))
        require(!candidate.request.headers.contains("user-agent"), "Modrinth candidate overrides operation identity");
    settings.fileMirror = "https://mirror.example.org/files";
    for (const auto& candidate : fileCandidates("https://cdn.modrinth.com/data/p1/fixture.jar", settings))
        require(!candidate.request.headers.contains("user-agent"), "File candidate overrides operation identity");
}

void providerSearchAndPages() {
    std::mutex mutex;
    Strings requests;
    CachedHttp http([&](const HttpRequest& request, const Cancellation&) {
        { std::lock_guard lock(mutex); requests.push_back(request.url); }
        if (request.url.find("api.modrinth.com") != std::string::npos)
            return response(request, {{"hits", Json::array({mrProject()})}, {"offset", 20}, {"limit", 20}, {"total_hits", 60}});
        return response(request, {{"data", Json::array({cfProject()})}, {"pagination", {{"index", 20}, {"pageSize", 20}, {"resultCount", 1}, {"totalCount", 60}}}});
    });
    CommunityService community(http);
    SearchQuery query;
    query.text = "fixture words";
    query.filter = {"1.20.1", "neoforge"};
    query.page = 2;
    query.pageSize = 40;
    query.revision = "search-revision";
    const auto page = community.search(query);
    require(page.sourceCursors.size() == 2 && page.sourceErrors.empty() && page.hasMore && page.revision == query.revision && page.total == 120, "Dual search cursor, provider total or revision differs");
    require(page.sourceCursors.at(Source::Modrinth).offset == 20 && page.sourceCursors.at(Source::CurseForge).offset == 20, "Merged page size is not split between providers");
    require(requests.size() == 2, "Dual search does not issue one request per provider");
    require(std::any_of(requests.begin(), requests.end(), [](const auto& url) { return url.find("offset=20") != std::string::npos && url.find("categories%3Aneoforge") != std::string::npos; }), "Modrinth facet/page request differs");
    require(std::any_of(requests.begin(), requests.end(), [](const auto& url) { return url.find("index=20") != std::string::npos && url.find("modLoaderType=6") != std::string::npos; }), "CurseForge filter/page request differs");
    CachedHttp versionsHttp([](const HttpRequest& request, const Cancellation&) {
        if (request.url.find("api.modrinth.com") != std::string::npos) {
            auto other = mrVersion("v2");
            other["loaders"] = Json::array({"forge"});
            return response(request, Json::array({mrVersion(), other}));
        }
        auto other = cfVersion(102);
        other["gameVersions"] = Json::array({"1.19.4", "Forge"});
        return response(request, {{"data", Json::array({cfVersion(), other})}, {"pagination", {{"totalCount", 10}}}});
    });
    CommunityService versionService(versionsHttp);
    const auto mr = versionService.versions(Source::Modrinth, ResourceKind::Mod, "p1", {"1.20.1", "fabric"}, 0, 1);
    require(mr.versions.size() == 1 && mr.cursor.total == 1 && !mr.cursor.hasMore, "Modrinth exact filter pagination differs");
    const auto cf = versionService.versions(Source::CurseForge, ResourceKind::Mod, "42", {"1.20.1", "fabric"}, 0, 50);
    require(cf.versions.size() == 1 && cf.cursor.nextOffset == 2 && cf.cursor.hasMore, "CurseForge cursor advances by filtered count rather than raw count");
    auto dependency = mrVersion();
    dependency["dependencies"] = Json::array({Json{{"project_id", "p2"}, {"version_id", "v2"}, {"dependency_type", "required"}}, Json{{"project_id", "p3"}, {"dependency_type", "optional"}}});
    const auto parsed = readVersionResponse(Source::Modrinth, dependency);
    require(parsed.requiredDependencies && parsed.requiredDependencies->size() == 1 && parsed.requiredDependencies->front().versionId == "v2", "Pinned required dependency differs");
    dependency.erase("dependencies");
    require(!readVersionResponse(Source::Modrinth, dependency).requiredDependencies, "Absent dependencies became authoritative empty dependencies");
}

void descriptionsAndExactExport() {
    TemporaryRoot root;
    const auto file = root.path / "fixture.jar";
    write(file, "abc");
    const auto hashes = hashResourceFile(file);
    std::atomic<unsigned> fileDownloads{0};
    const std::string markdown = "# Full title\n\nBody **not a summary**\n";
    const std::string html = "<h2>Full dedicated description</h2><p>Content</p>";
    CachedHttp http([&](const HttpRequest& request, const Cancellation&) {
        if (parseUrl(request.url).host == "cdn.modrinth.com") { ++fileDownloads; fail("network", "Descriptor resolver must not download"); }
        if (request.url.find("version_file/") != std::string::npos) {
            auto version = mrVersion();
            version["files"] = Json::array({mrFile("primary.jar", true, std::string(40, '3'), std::string(128, '4'), 9), mrFile("matched.jar", false, hashes.sha1, hashes.sha512, hashes.sizeBytes)});
            return response(request, version);
        }
        if (request.url.ends_with("/description")) return response(request, {{"data", html}});
        auto project = mrProject();
        project["body"] = markdown;
        return response(request, project, 200, {{"etag", "revision-a"}});
    });
    CommunityService service(http);
    const auto mrBody = service.fullDescription(Source::Modrinth, "p1");
    require(mrBody.text == markdown && mrBody.format == BodyFormat::Markdown && mrBody.revision == "revision-a", "Full Modrinth Markdown body was truncated or replaced");
    require(service.fullDescription(Source::CurseForge, "42").text == html, "Dedicated CurseForge HTML body was not preserved");
    const auto resolved = service.resolveFileHashes(hashes.sha1, hashes.sha512, true);
    require(resolved && resolved->file.fileName == "matched.jar" && !resolved->file.primary, "Exact export resolver chose version primary file");
#ifndef NICKEL_CORE_ONLY
    const auto exported = resolveLocalResourceBlocking(service, file, "mods/fixture.jar", true);
    require(exported && exported->path == "mods/fixture.jar" && exported->sha1 == hashes.sha1 && exported->sha512 == hashes.sha512 && exported->size == 3 && exported->downloads.front().ends_with("matched.jar"), "Exact local export descriptor differs");
    expectError("policy_denied", [&] { (void)resolveLocalResourceBlocking(service, file, "mods/../outside.jar", true); });
#endif
    require(fileDownloads == 0, "Descriptor resolution performed a content download");
    std::atomic<unsigned> fingerprintCalls{0};
    CachedHttp cfHttp([&](const HttpRequest& request, const Cancellation&) {
        if (request.url.find("version_file/") != std::string::npos) return response(request, Json::object(), 404);
        ++fingerprintCalls;
        const auto body = Json::parse(request.body);
        require(body["fingerprints"][0] == 0, "Fingerprint zero is not a valid descriptor probe");
        return response(request, {{"data", {{"exactMatches", Json::array({Json{{"file", cfVersion(101, hashes.sha1)}}})}}}});
    });
    CommunityService cfService(cfHttp);
    const auto cf = cfService.resolveFileHashes(hashes.sha1, hashes.sha512, false, 0);
    require(cf && cf->source == Source::CurseForge && cf->file.sha512 == hashes.sha512 && fingerprintCalls == 1, "Zero CurseForge fingerprint fallback differs");
}

void cacheSemantics() {
    unsigned calls = 0;
    CachedHttp http([&](const HttpRequest& request, const Cancellation&) {
        ++calls;
        if (calls == 1) return response(request, {{"value", 1}}, 200, {{"etag", "a"}, {"cache-control", "no-cache"}});
        require(request.headers.contains("if-none-match"), "Stale cache did not send conditional validation");
        return HttpResponse{{}, 304, {{"cache-control", "max-age=60"}}, request.url};
    });
    HttpRequest request;
    request.url = "https://api.modrinth.com/v2/fixture";
    request.rules = providerApiRules();
    const auto first = http.fetch(request);
    const auto second = http.fetch(request);
    const auto third = http.fetch(request);
    require(calls == 2 && first.response->bytes == second.response->bytes && second.freshness == Freshness::Revalidated && third.freshness == Freshness::Fresh, "Conditional cache freshness differs");
    unsigned noStoreCalls = 0;
    CachedHttp noStore([&](const HttpRequest& raw, const Cancellation&) { ++noStoreCalls; return response(raw, {}, 200, {{"cache-control", "no-store"}}); });
    (void)noStore.fetch(request);
    (void)noStore.fetch(request);
    require(noStoreCalls == 2 && noStore.stats().entries == 0, "no-store response remained cached");
    unsigned evictCalls = 0;
    CachedHttp eviction([&](const HttpRequest& raw, const Cancellation&) { ++evictCalls; return HttpResponse{Bytes(900, 'x'), 200, {}, raw.url}; }, 3072, 1);
    (void)eviction.fetch(request);
    request.url += "-other";
    (void)eviction.fetch(request);
    require(eviction.stats().bytes <= 3072 && eviction.stats().evictions > 0 && evictCalls == 2, "Byte-accounted LRU did not evict");
    unsigned staleCalls = 0;
    CachedHttp stale([&](const HttpRequest& raw, const Cancellation&) {
        if (++staleCalls > 1) fail("network", "Controlled offline fixture");
        return response(raw, {{"value", 1}}, 200, {{"cache-control", "no-cache"}});
    });
    CachePolicy policy;
    policy.staleFor = 1s;
    (void)stale.fetch(request, policy);
    require(stale.fetch(request, policy).freshness == Freshness::Stale, "Bounded offline stale fallback failed");
}

void coalescingCancellation() {
    std::atomic<bool> entered{false}, finish{false}, physicallyCancelled{false};
    std::atomic<unsigned> physical{0};
    CachedHttp http([&](const HttpRequest& raw, const Cancellation& cancel) {
        ++physical;
        entered = true;
        while (!finish && !cancel.cancelled()) std::this_thread::sleep_for(1ms);
        if (cancel.cancelled()) physicallyCancelled = true;
        cancel.check();
        return response(raw, {{"ok", true}});
    }, 8192, 1);
    HttpRequest request;
    request.url = "https://api.modrinth.com/v2/coalescing";
    request.rules = providerApiRules();
    Cancellation firstCancel;
    auto first = std::async(std::launch::async, [&] {
        expectError("cancelled", [&] { (void)http.fetch(request, {}, firstCancel); });
    });
    waitUntil([&] { return entered.load(); });
    auto second = std::async(std::launch::async, [&] { return http.fetch(request); });
    waitUntil([&] { return http.stats().joins == 1; });
    firstCancel.cancel();
    first.get();
    require(!physicallyCancelled && physical == 1, "One subscriber cancelled a surviving coalesced request");
    finish = true;
    require(second.get().response->status == 200, "Surviving coalesced subscriber failed");
    entered = false;
    finish = false;
    request.url += "-cancel-last";
    Cancellation lastCancel;
    auto last = std::async(std::launch::async, [&] { expectError("cancelled", [&] { (void)http.fetch(request, {}, lastCancel); }); });
    waitUntil([&] { return entered.load(); });
    lastCancel.cancel();
    last.get();
    waitUntil([&] { return physicallyCancelled.load(); });
    require(http.stats().inFlight == 0, "Cancelled last subscriber retained an in-flight entry");
}

void metadataAndState() {
    const auto fabric = bytes(R"({"id":"fixture","name":"Fixture","version":"1.0","description":"Description","icon":{"64":"icons/small.png","256":"icons/medium.png"},"custom":{"modmenu":{"badges":["library"]}}})");
    const auto parsed = readModMetadata({{"fabric.mod.json", fabric}}, "fixture.jar", "fabric");
    require(parsed.ids == Strings{"fixture"} && parsed.version == "1.0" && parsed.iconEntry == "icons/medium.png", "Fabric metadata or icon differs");
    const auto quilt = readModMetadata({{"quilt.mod.json", bytes(R"({"quilt_loader":{"id":"quilt-fixture","version":"2.0","metadata":{"name":"Quilt","icon":"quilt.png"}}})")}}, "fixture.jar", "quilt");
    require(quilt.ids == Strings{"quilt-fixture"} && quilt.version == "2.0", "Quilt metadata differs");
    const auto forge = readModMetadata({{"META-INF/mods.toml", bytes("modLoader = \"javafml\"\n[[mods]]\nmodId = \"fixture\"\ndisplayName = \"Forge\"\nversion = \"${file.jarVersion}\"\ndescription = '''Line one\nLine two'''\nlogoFile = \"logo.png\"\n")}, {"META-INF/MANIFEST.MF", bytes("Manifest-Version: 1.0\r\nImplementation-Version: 3.0\r\n")}}, "forge.jar", "forge");
    require(forge.version == "3.0" && forge.description.find("Line two") != std::string::npos, "Forge TOML or manifest fallback differs");
    const auto unsafe = readModMetadata({{"fabric.mod.json", bytes(R"({"id":"fixture","icon":"../../secret.png"})")}}, "fixture.jar", "fabric");
    require(unsafe.iconEntry.empty(), "Archive icon traversal was approved");
    TemporaryRoot root;
    const auto selected = context(root.path);
    std::filesystem::create_directory(selected.modsRoot);
    ModServices services;
    services.resources = authority(selected);
    services.hashes = hashResourceFile;
    ModsService mods(services);
    write(selected.modsRoot / "fixture.jar", "same");
    write(selected.modsRoot / "fixture.jar.disabled", "same");
    require(mods.list(selected.identity).size() == 1, "Physical Mod states are not logically grouped");
    (void)mods.setEnabled(selected.identity, {"fixture.jar"}, true);
    require(std::filesystem::exists(selected.modsRoot / "fixture.jar") && !std::filesystem::exists(selected.modsRoot / "fixture.jar.disabled"), "Identical Mod copy was not safely removed");
    write(selected.modsRoot / "fixture.jar.disabled", "older version");
    expectError("conflict", [&] { (void)mods.setEnabled(selected.identity, {"fixture.jar.disabled"}, true); });
    require(std::filesystem::exists(selected.modsRoot / "fixture.jar") && std::filesystem::exists(selected.modsRoot / "fixture.jar.disabled"), "Different Mod versions were lost during conflict");
    (void)mods.setEnabled(selected.identity, {"fixture.jar"}, false);
    require(!std::filesystem::exists(selected.modsRoot / "fixture.jar") && std::filesystem::exists(selected.modsRoot / "fixture.jar.old") && std::filesystem::exists(selected.modsRoot / "fixture.jar.disabled"), "Disabled differing Mod versions were not retained");
#ifndef NICKEL_CORE_ONLY
    const auto source = root.path / "source.json";
    write(source, std::string(reinterpret_cast<const char*>(fabric.data()), fabric.size()));
    const auto jar = root.path / "import.jar";
    archive::Archive::write_zip(jar, {{"fabric.mod.json", source}}, {}, {});
    services = bindModServices(services);
    ModsService archiveMods(services);
    const auto installed = archiveMods.installLocal(selected.identity, {jar});
    require(installed == Strings{"import.jar"} && archiveMods.metadata(selected.identity, "import.jar")->ids == Strings{"fixture"}, "Shared archive local Mod installation failed");
    expectError("conflict", [&] { (void)archiveMods.installLocal(selected.identity, {jar}); });
#endif
}

std::map<std::string, std::string> localFileState(const Path& directory) {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        result.emplace(utf8Path(entry.path().filename()), hashResourceFile(entry.path()).sha256);
    return result;
}

void localModMutationPreflight() {
    TemporaryRoot root;
    auto selected = context(root.path);
    std::filesystem::create_directory(selected.modsRoot);
    const auto exportRoot = root.path / "export";
    std::filesystem::create_directory(exportRoot);
    write(selected.modsRoot / "alpha.jar", "active alpha");
    write(selected.modsRoot / "alpha.jar.disabled", "disabled alpha");
    write(selected.modsRoot / "alpha.jar.old", "retained alpha");
    write(selected.modsRoot / "beta.jar", "beta");
    write(selected.modsRoot / "gamma.jar.disabled", "gamma");
    const auto initial = localFileState(selected.modsRoot);
    unsigned contextCalls = 0, acquired = 0, released = 0, live = 0;
    bool changedRevision = false, deniedLease = false;
    ModServices services;
    services.resources.context = [&](const InstanceIdentity& identity) {
        require(identity == selected.identity, "Local Mod authority received a foreign identity");
        auto current = selected;
        if (++contextCalls > 1 && changedRevision) {
            require(live == 1, "Local Mod revision check lost its mutation lease");
            current.revision += "-changed";
        }
        return current;
    };
    services.resources.acquireMutation = [&](const InstanceIdentity& identity, const std::string& revision) -> std::shared_ptr<void> {
        require(identity == selected.identity && revision == selected.revision, "Local mutation lease uses a different authority snapshot");
        require(live == 0, "Local Mod service acquires a nested mutation lease");
        if (deniedLease) return {};
        ++acquired;
        ++live;
        return std::shared_ptr<void>(new int(1), [&](void* value) {
            delete static_cast<int*>(value);
            --live;
            ++released;
        });
    };
    services.hashes = hashResourceFile;
    ModsService mods(services);
    expectError("conflict", [&] { (void)mods.setEnabled(selected.identity, {"alpha.jar"}, true); });
    expectError("conflict", [&] { (void)mods.setEnabled(selected.identity, {"alpha.jar"}, false); });
    expectError("invalid_input", [&] { (void)mods.setEnabled(selected.identity, {}, false); });
    expectError("invalid_input", [&] { (void)mods.remove(selected.identity, {"beta.jar", "beta.jar"}); });
    require(localFileState(selected.modsRoot) == initial, "Local Mod preflight failures change files or leave temporary files");
    Cancellation cancelled;
    cancelled.cancel();
    expectError("cancelled", [&] { (void)mods.setEnabled(selected.identity, {"beta.jar"}, false, cancelled); });
    expectError("cancelled", [&] { (void)mods.remove(selected.identity, {"alpha.jar"}, cancelled); });
    expectError("cancelled", [&] { (void)mods.copyTo(selected.identity, {"beta.jar"}, exportRoot, cancelled); });
    require(localFileState(selected.modsRoot) == initial && localFileState(exportRoot).empty(), "Cancelled local Mod operations publish files");
    contextCalls = 0;
    changedRevision = true;
    expectError("conflict", [&] { (void)mods.setEnabled(selected.identity, {"beta.jar"}, false); });
    contextCalls = 0;
    expectError("conflict", [&] { (void)mods.remove(selected.identity, {"alpha.jar"}); });
    changedRevision = false;
    require(localFileState(selected.modsRoot) == initial && live == 0, "Stale local Mod revision mutates files or leaks authority");
    selected.running = true;
    const auto beforeRunning = acquired;
    expectError("busy", [&] { (void)mods.remove(selected.identity, {"beta.jar"}); });
    require(acquired == beforeRunning, "Running instance reaches mutation acquisition");
    selected.running = false;
    deniedLease = true;
    expectError("busy", [&] { (void)mods.setEnabled(selected.identity, {"beta.jar"}, false); });
    deniedLease = false;
    write(exportRoot / "gamma.jar.disabled", "occupied export");
    const auto occupied = localFileState(exportRoot);
    expectError("conflict", [&] { (void)mods.copyTo(selected.identity, {"beta.jar", "gamma.jar.disabled"}, exportRoot); });
    require(localFileState(exportRoot) == occupied && localFileState(selected.modsRoot) == initial, "Batch export preflight partially publishes or overwrites");
    const auto cleanExport = root.path / "clean-export";
    std::filesystem::create_directory(cleanExport);
    const Strings exportNames{"beta.jar", "gamma.jar.disabled"};
    require(mods.copyTo(selected.identity, exportNames, cleanExport) == exportNames, "Local Mod export loses selected physical names");
    for (const auto& name : exportNames)
        require(hashResourceFile(cleanExport / pathFromUtf8(name)).sha256 == initial.at(name), "Local Mod export changes content or state suffix");
    const auto exported = localFileState(cleanExport);
    expectError("conflict", [&] { (void)mods.copyTo(selected.identity, {"beta.jar"}, cleanExport); });
    require(localFileState(cleanExport) == exported, "Existing exported Mod was overwritten");
    require(mods.setEnabled(selected.identity, {"beta.jar"}, false) == Strings{"beta.jar.disabled"}, "Local disable result loses state suffix");
    require(hashResourceFile(selected.modsRoot / "beta.jar.disabled").sha256 == initial.at("beta.jar"), "Disabling changes Mod bytes");
    require(mods.setEnabled(selected.identity, {"beta.jar.disabled"}, true) == Strings{"beta.jar"}, "Local re-enable result differs");
    require(mods.remove(selected.identity, {"alpha.jar", "alpha.jar.disabled"}) == 3, "Logical Mod removal does not remove all three distinct states exactly once");
    auto retained = initial;
    retained.erase("alpha.jar");
    retained.erase("alpha.jar.disabled");
    retained.erase("alpha.jar.old");
    require(localFileState(selected.modsRoot) == retained, "Logical Mod removal affects another Mod or leaves temporary files");
    require(live == 0 && acquired == released, "Local Mod mutation leases outlive completed or failed operations");
}

#ifndef NICKEL_CORE_ONLY
void localModArchiveTransactions() {
    TemporaryRoot root;
    const auto selected = context(root.path);
    std::filesystem::create_directory(selected.modsRoot);
    const auto input = root.path / "input";
    std::filesystem::create_directory(input);
    const auto manifest = input / "fabric.json";
    const auto icon = input / "logo.png";
    write(icon, "opaque icon fixture");
    write(manifest, R"({"id":"shared-fixture","name":"Fixture","version":"1.0","icon":"icons/logo.png"})");
    const auto first = input / "first.jar";
    archive::Archive::write_zip(first, {{"fabric.mod.json", manifest}, {"icons/logo.png", icon}}, {}, {});
    write(manifest, R"({"id":"shared-fixture","name":"Fixture","version":"2.0","icon":"icons/logo.png"})");
    const auto second = input / "second.jar";
    archive::Archive::write_zip(second, {{"fabric.mod.json", manifest}, {"icons/logo.png", icon}}, {}, {});
    ModServices services;
    services.resources = authority(selected);
    services = bindModServices(std::move(services));
    const auto inspect = services.archiveEntries;
    unsigned contextCalls = 0;
    bool changedRevision = false, cancelStaged = false;
    Cancellation stagedCancellation;
    services.resources.context = [&](const InstanceIdentity& identity) {
        require(identity == selected.identity, "Archive mutation resolves a foreign instance");
        auto current = selected;
        if (++contextCalls > 1 && changedRevision) current.revision += "-changed";
        return current;
    };
    services.archiveEntries = [&](const Path& path, const Cancellation& cancel) {
        auto entries = inspect(path, cancel);
        if (cancelStaged && path.parent_path() == selected.modsRoot) stagedCancellation.cancel();
        return entries;
    };
    ModsService mods(services);
    write(selected.modsRoot / "second.jar", "occupied Mod");
    const auto occupied = localFileState(selected.modsRoot);
    expectError("conflict", [&] { (void)mods.installLocal(selected.identity, {first, second}); });
    require(localFileState(selected.modsRoot) == occupied, "Batch local install publishes before checking every destination");
    std::filesystem::remove(selected.modsRoot / "second.jar");
    Cancellation cancelled;
    cancelled.cancel();
    expectError("cancelled", [&] { (void)mods.installLocal(selected.identity, {first, second}, cancelled); });
    require(localFileState(selected.modsRoot).empty(), "Cancelled archive preflight leaves Mod files");
    require(mods.installLocal(selected.identity, {first, second}) == Strings{"first.jar", "second.jar"}, "Batch archive installation result differs");
    require(hashResourceFile(selected.modsRoot / "first.jar").sha256 == hashResourceFile(first).sha256 &&
        hashResourceFile(selected.modsRoot / "second.jar").sha256 == hashResourceFile(second).sha256, "Installed archives differ from their pinned sources");
    const auto scanned = mods.scan(selected.identity);
    require(scanned.size() == 2 && scanned[0].metadataLoaded && scanned[1].metadataLoaded &&
        scanned[0].version == "1.0" && scanned[1].version == "2.0" &&
        !scanned[0].duplicateGroup.empty() && scanned[0].duplicateGroup == scanned[1].duplicateGroup,
        "Real archive scan loses metadata versions or duplicate identity grouping");
    require(mods.icon(selected.identity, "first.jar", "icons/logo.png") == bytes("opaque icon fixture"), "Approved archive icon bytes differ");
    expectError("policy_denied", [&] { (void)mods.icon(selected.identity, "first.jar", "fabric.mod.json"); });
    const auto before = localFileState(selected.modsRoot);
    contextCalls = 0;
    changedRevision = true;
    expectError("conflict", [&] { (void)mods.scan(selected.identity); });
    const auto pending = input / "pending.jar";
    archive::Archive::write_zip(pending, {{"fabric.mod.json", manifest}}, {}, {});
    contextCalls = 0;
    expectError("conflict", [&] { (void)mods.installLocal(selected.identity, {pending}); });
    changedRevision = false;
    require(localFileState(selected.modsRoot) == before, "Stale archive authority publishes or leaves staging files");
    cancelStaged = true;
    expectError("cancelled", [&] { (void)mods.installLocal(selected.identity, {pending}, stagedCancellation); });
    cancelStaged = false;
    require(localFileState(selected.modsRoot) == before, "Cancellation after archive staging changes installed Mods or leaks staging");
    write(input / "readme.txt", "not a Mod filename");
    expectError("invalid_input", [&] { (void)mods.installLocal(selected.identity, {input / "readme.txt"}); });
    require(localFileState(selected.modsRoot) == before, "Rejected local filename mutates Mods");
}
#endif


void updateConfirmationRace() {
    TemporaryRoot root;
    const auto selected = context(root.path);
    std::filesystem::create_directory(selected.modsRoot);
    const auto file = selected.modsRoot / "fixture.jar";
    write(file, "before");
    const auto original = hashResourceFile(file);
    CachedHttp http([](const HttpRequest&, const Cancellation&) -> HttpResponse { fail("network", "No provider should be queried after a local mutation"); });
    CommunityService community(http);
    ModServices services;
    services.resources = authority(selected);
    services.hashes = hashResourceFile;
    unsigned replaced = 0;
    services.replace = [&](const InstanceContext&, const std::string&, const std::string&, const DownloadPlan&, std::shared_ptr<void>, const Cancellation&, const ProgressCallback&) {
        ++replaced;
        return InstallReceipt{};
    };
    ModsService mods(services, &community);
    ModUpdate update;
    update.fileName = "fixture.jar";
    update.originalSha256 = original.sha256;
    update.instance = selected.identity;
    update.instanceRevision = selected.revision;
    update.identified = true;
    update.available = DownloadPlan{};
    write(file, "changed after confirmation");
    expectError("conflict", [&] { (void)mods.updateConfirmed(selected.identity, {update}); });
    require(replaced == 0, "Changed original Mod reached replacement callback");
}

FullDescription bodyDescription(std::string text) {
    FullDescription result;
    result.projectId = "p1";
    result.format = BodyFormat::Html;
    result.text = std::move(text);
    result.canonicalBase = "https://cdn.modrinth.com/data/p1/";
    return result;
}

void safeBodyPipeline() {
    BodyReader reader;
    const auto body = reader.convert(bodyDescription("<h1 style='background:url(file:///secret)'>Title</h1><script>evil()</script><svg><a href='file:///secret'>active</a></svg><table><tr><td>cell</td></tr></table><pre><code>&lt;safe&gt;</code></pre><a href='https://example.org/page'>approved link</a><a href='file:///secret'>denied</a><img src='https://cdn.modrinth.com/icon.png' alt='icon' width='99999' onerror='evil()'><img src='data:image/svg+xml,x' alt='blocked'>"));
    require(body.valid() && body.html().find("<h1>Title</h1>") != std::string::npos && body.html().find("<table>") != std::string::npos && body.html().find("<code>&lt;safe&gt;</code>") != std::string::npos, "Structural body formatting was not preserved");
    for (const auto active : {"evil", "<script", "<svg", "style=", "onerror", "file:///", "https://cdn.modrinth.com", "https://example.org", "data:image"}) require(body.html().find(active) == std::string::npos, "Active or original resource URL leaked into safe HTML");
    require(body.images().size() == 1 && body.images().front().key.starts_with("nickel-image://") && body.images().front().width <= 1024, "Body image opaque approval or dimensions differ");
    require(body.links().size() == 1 && body.links().front().key.starts_with("nickel-link://") && body.externalLink(body.links().front().key) == "https://example.org/page", "Body external link opaque approval differs");
    expectError("policy_denied", [&] { (void)body.externalLink("https://example.org/page"); });
    std::string deep;
    for (unsigned i = 0; i < 80; ++i) deep += "<div>";
    deep += "text";
    for (unsigned i = 0; i < 80; ++i) deep += "</div>";
    expectError("too_large", [&] { (void)reader.convert(bodyDescription(deep)); });
    BodyLimits small;
    small.maximumNodes = 12;
    expectError("too_large", [&] { (void)BodyReader(small).convert(bodyDescription("<p>a</p><p>b</p><p>c</p><p>d</p><p>e</p><p>f</p>")); });
    auto markdown = bodyDescription("# Markdown");
    markdown.format = BodyFormat::Markdown;
    expectError("unsupported", [&] { (void)reader.convert(markdown); });
    const auto normalized = BodyReader({}, descriptionImageRules(), externalLinkRules(), [](std::string_view, const Cancellation&) { return "<h1>Markdown</h1><script>not approved</script>"; }).convert(markdown);
    require(normalized.html().find("<h1>Markdown</h1>") != std::string::npos && normalized.html().find("script") == std::string::npos, "Markdown bypassed the sole sanitizer");
    std::atomic<unsigned> requests{0};
    CachedHttp http([&](const HttpRequest& request, const Cancellation&) {
        ++requests;
        return HttpResponse{Bytes{0x89, 'P', 'N', 'G', 13, 10, 26, 10}, 200, {{"content-type", "image/png"}}, request.url};
    });
    ImageLimits imageLimits;
    imageLimits.maximumConcurrent = 1;
    BodyResources images(http, imageLimits, [](std::span<const std::uint8_t>, const ImageLimits&, const Cancellation&) { return RasterImage{}; });
    images.setBody(body);
    const auto key = body.images().front().key;
    const auto pending = images.lookup(key);
    require(pending.state == ResourceState::Pending && pending.image && pending.image->rgba.size() == 4 && requests == 0, "Non-IO image lookup performed a download or returned invalid placeholder");
    const auto denied = images.lookup("https://cdn.modrinth.com/icon.png");
    require(denied.state == ResourceState::Denied && denied.image && requests == 0, "Raw image URL triggered fallback");
    const auto ready = images.requestVisible(key);
    require(ready.state == ResourceState::Ready && ready.image && requests == 1 && images.lookup(key).state == ResourceState::Ready && images.decodedCacheBytes() <= imageLimits.maximumCacheBytes, "Visible image pipeline or cache budget differs");
    BodyResources failed(http, imageLimits, [](std::span<const std::uint8_t>, const ImageLimits&, const Cancellation&) -> RasterImage { fail("invalid_response", "Controlled decode failure"); });
    failed.setBody(body);
    const auto failure = failed.requestVisible(key);
    require(failure.state == ResourceState::Failed && failure.image && failure.image->width == 1 && failed.lookup(key).state == ResourceState::Failed, "Failed image fell back or lost its valid placeholder");
    images.setBody(reader.convert(bodyDescription("<p>New body</p>")));
    require(images.lookup(key).state == ResourceState::Denied && images.decodedCacheBytes() == 0, "Document replacement retained old approval or cache");
}

} // namespace

namespace nickel::tests { unsigned runAdditionalContracts(); }

int main() {
#ifdef NICKEL_CORE_ONLY
    std::cout << "Core-only contract subset: shared HTTP/archive/install/export adapters excluded\n";
#endif
    const std::vector<std::pair<const char*, void(*)()>> cases{
        {"identity and path fencing", identities}, {"URL and mirror credential policy", urlPolicy},
        {"dual provider search and pagination", providerSearchAndPages}, {"full descriptions and exact export", descriptionsAndExactExport},
        {"cache freshness and byte eviction", cacheSemantics}, {"coalescing and subscriber cancellation", coalescingCancellation},
        {"metadata, local install and version retention", metadataAndState}, {"local Mod mutation preflight and selected-file export", localModMutationPreflight},
#ifndef NICKEL_CORE_ONLY
        {"real archive Mod scan and batch installation", localModArchiveTransactions},
#endif
        {"update confirmation mutation race", updateConfirmationRace}, {"safe bodies and no-IO image lookup", safeBodyPipeline}};
    unsigned failed = 0;
    for (const auto& [name, test] : cases) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const ErrorException& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.error().code << ": " << error.what() << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << "Baseline controlled cases: " << cases.size() << ", failures: " << failed << '\n';
    failed += runAdditionalContracts();
    return failed ? 1 : 0;
}
