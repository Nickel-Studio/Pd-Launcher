#include "fixtures.hpp"
#include "core/resources/adapters.hpp"
#include "core/resources/export.hpp"
#include "core/resources/utility.hpp"
#include "core/packs/native_services.hpp"
#include "core/network/control.hpp"
#include "core/tasks/test_hooks.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace {
using namespace nickel;
using namespace nickel::tests;
using namespace nickel::resources;
using namespace std::chrono_literals;

Json config;
std::shared_ptr<transport::HttpClient> client;
HttpFetch fetch;

HttpRequest request(std::string path, std::string origin = {}) {
    HttpRequest result;
    result.url = (origin.empty() ? config.at("http").get<std::string>() : origin) + path;
    result.rules.testLoopbackOrigins = {config.at("http").get<std::string>(), config.at("http2").get<std::string>()};
    result.maximumBytes = 32768;
    result.timeout = 5s;
    return result;
}

Json stats() { return detail::parseBoundedJson(fetch(request("/stats"), {}).bytes, 32768); }

std::uint64_t count(const Json& value, const char* group, const std::string& key) {
    return value.at(group).value(key, std::uint64_t{});
}

template<class Predicate>
void await(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < end, "Real loopback fixture exceeded deadline");
        std::this_thread::sleep_for(5ms);
    }
}

void realHttpPolicy() {
    const auto response = fetch(request("/json"), {});
    require(response.status == 200 && detail::parseBoundedJson(response.bytes, 128).at("ok") == true, "Shared HTTP adapter loses body or status");
    auto redirect = request("/redirect");
    require(fetch(redirect, {}).status == 302, "Adapter follows redirects without an explicit request");
    redirect.followRedirects = true;
    const auto followed = fetch(redirect, {});
    require(followed.status == 200 && followed.effectiveUrl.ends_with("/json"), "Explicit approved redirect is not followed");
    redirect.url = request("/redirect-private").url;
    expectError("policy_denied", [&] { (void)fetch(redirect, {}); });
    redirect.url = request("/redirect-loop").url;
    expectError("policy_denied", [&] { (void)fetch(redirect, {}); });
    redirect.url = request("/redirect-cross").url;
    redirect.headers = {{"authorization", "synthetic-bearer"}, {"cookie", "synthetic-cookie"}, {"x-api-key", "synthetic-key"}};
    require(fetch(redirect, {}).status == 200, "Approved cross-origin redirect fails");
    const auto observed = stats().at("headers").at("/echo");
    require(!observed.contains("authorization") && !observed.contains("cookie") && !observed.contains("x-api-key"), "Redirect leaks credentials across approved origins");
    const auto echoCount = count(stats(), "counts", "/echo");
    redirect.url = request("/redirect-post-cross").url;
    redirect.method = "POST";
    redirect.body = bytes("synthetic payload");
    expectError("policy_denied", [&] { (void)fetch(redirect, {}); });
    require(count(stats(), "counts", "/echo") == echoCount, "Cross-origin POST reaches its destination");
    auto denied = request("/json");
    denied.rules.testLoopbackOrigins.clear();
    expectError("policy_denied", [&] { (void)fetch(denied, {}); });
    auto limited = request("/big");
    limited.maximumBytes = 32;
    expectError("http_failed", [&] { (void)fetch(limited, {}); });
    limited.url = request("/chunked-big").url;
    expectError("http_failed", [&] { (void)fetch(limited, {}); });
    expectError("http_failed", [&] { (void)fetch(request("/truncated"), {}); });
}

void realOperationSnapshotIdentity() {
    const auto original = client->snapshot();
    auto started = original;
    started.user_agent = "Palladium/fixture-started (studio.nickel.launcher)";
    auto future = started;
    future.user_agent = "Palladium/fixture-future (studio.nickel.launcher)";
    client->set_snapshot(started);
    try {
        auto captured = client->snapshot();
        const auto operationFetch = makeHttpFetch(client, captured);
        captured.user_agent = "must not replace captured operation";
        client->set_snapshot(future);
        const auto origin = config.at("http").get<std::string>();
        ProviderSettings settings;
        settings.modrinthMirror = origin + "/mr/v2";
        settings.testLoopbackOrigins.insert(origin);
        auto provider = apiCandidates(Source::Modrinth, "project/p1", settings);
        require(!provider.back().request.headers.contains("user-agent"), "Provider overrides shared identity");
        require(operationFetch(provider.back().request, {}).status == 200, "Pinned provider request failed");
        auto file = fileCandidates(origin + "/files/snapshot.jar", settings);
        require(!file.front().request.headers.contains("user-agent"), "File overrides shared identity");
        file.front().request.maximumBytes = config.at("size").get<std::size_t>();
        const auto downloaded = operationFetch(file.front().request, {});
        require(downloaded.status == 200 && downloaded.bytes.size() == file.front().request.maximumBytes,
            "Pinned bounded file request failed");
        documents::FullDescription description;
        description.projectId = "p1";
        description.format = documents::BodyFormat::Html;
        description.canonicalBase = origin + '/';
        description.text = "<img src='" + origin + "/images/snapshot.png' alt='snapshot'>";
        auto rules = descriptionImageRules();
        rules.testLoopbackOrigins.insert(origin);
        const auto body = documents::BodyReader({}, rules).convert(description);
        require(body.images().size() == 1, "Snapshot image approval missing");
        CachedHttp imageHttp(operationFetch);
        documents::BodyResources images(imageHttp, {}, [](std::span<const std::uint8_t> encoded,
                                                        const documents::ImageLimits&, const Cancellation&) {
            require(encoded.size() > 8, "Snapshot raster response truncated");
            return documents::RasterImage{};
        });
        images.setBody(body);
        require(images.lookup(body.images().front().key).state == documents::ResourceState::Pending &&
            count(stats(), "counts", "/images/snapshot.png") == 0,
            "Snapshot image lookup performs IO");
        require(images.requestVisible(body.images().front().key).state == documents::ResourceState::Ready,
            "Pinned body request failed");
        const auto observed = stats().at("headers");
        for (const auto* path : {"/mr/v2/project/p1", "/files/snapshot.jar", "/images/snapshot.png"})
            require(observed.at(path).at("user-agent") == started.user_agent,
                "Provider/file/body do not share the copied operation snapshot");
        require(fetch(request("/echo"), {}).status == 200 &&
            stats().at("headers").at("/echo").at("user-agent") == future.user_agent,
            "Future request ignores changed default identity");
        client->set_snapshot(started);
        auto redirect = request("/redirect-snapshot");
        redirect.followRedirects = true;
        auto transfer = std::async(std::launch::async, [&] { return fetch(redirect, {}); });
        try {
            await([] { return count(stats(), "counts", "/redirect-snapshot") == 1; });
            client->set_snapshot(future);
        } catch (...) {
            auto release = request("/release-snapshot");
            release.method = "POST";
            (void)fetch(release, {});
            transfer.wait();
            throw;
        }
        auto release = request("/release-snapshot");
        release.method = "POST";
        require(fetch(release, {}).status == 200, "Cannot release controlled redirect");
        require(transfer.get().status == 200, "Started redirect operation failed");
        const auto redirected = stats().at("headers");
        require(redirected.at("/redirect-snapshot").at("user-agent") == started.user_agent &&
            redirected.at("/echo-snapshot").at("user-agent") == started.user_agent,
            "Snapshot setter rewrites a started redirect operation");
    } catch (...) {
        client->set_snapshot(original);
        throw;
    }
    client->set_snapshot(original);
}


void realPhysicalCancellation() {
    Cancellation cancel;
    auto operation = std::async(std::launch::async, [&] { expectError("cancelled", [&] { (void)fetch(request("/silent-direct"), cancel); }); });
    await([] { return count(stats(), "counts", "/silent-direct") == 1; });
    const auto started = std::chrono::steady_clock::now();
    cancel.cancel();
    require(operation.wait_for(1s) == std::future_status::ready, "Physical shared HTTP cancellation exceeds one second");
    operation.get();
    await([] { return count(stats(), "closed", "/silent-direct") == 1; });
    require(std::chrono::steady_clock::now() - started < 2s, "Socket remains open after cancellation");
}

void realCacheSubscribers() {
    CachedHttp cache(fetch, 65536, 2);
    Cancellation firstCancel;
    auto first = std::async(std::launch::async, [&] { expectError("cancelled", [&] { (void)cache.fetch(request("/slow-cache"), {}, firstCancel); }); });
    await([] { return count(stats(), "counts", "/slow-cache") == 1; });
    auto second = std::async(std::launch::async, [&] { return cache.fetch(request("/slow-cache")); });
    await([&] { return cache.stats().joins == 1; });
    firstCancel.cancel();
    first.get();
    require(second.get().response->status == 200 && count(stats(), "counts", "/slow-cache") == 1, "Cancelled subscriber aborts a live peer or duplicates physical traffic");
    (void)cache.fetch(request("/slow-cache"));
    require(count(stats(), "counts", "/slow-cache") == 1, "Fresh cache performs unnecessary IO");
    const auto before = cache.stats().joins;
    Cancellation leftCancel, rightCancel;
    auto left = std::async(std::launch::async, [&] { expectError("cancelled", [&] { (void)cache.fetch(request("/silent-cache"), {}, leftCancel); }); });
    await([] { return count(stats(), "counts", "/silent-cache") == 1; });
    auto right = std::async(std::launch::async, [&] { expectError("cancelled", [&] { (void)cache.fetch(request("/silent-cache"), {}, rightCancel); }); });
    await([&] { return cache.stats().joins > before; });
    leftCancel.cancel();
    left.get();
    require(count(stats(), "closed", "/silent-cache") == 0, "First subscriber cancellation closes a surviving request");
    rightCancel.cancel();
    right.get();
    await([] { return count(stats(), "closed", "/silent-cache") == 1; });
    auto etag = request("/etag");
    const auto initial = cache.fetch(etag);
    const auto validated = cache.fetch(etag);
    require(initial.response->bytes == bytes("etag") && validated.freshness == Freshness::Revalidated && validated.response->bytes == bytes("etag"), "Real HTTP 304 loses the cached body");
}

DownloadPlan plan(std::string name, std::string endpoint = "/files/fixture.jar") {
    DownloadPlan result;
    result.projectId = "p1";
    result.versionId = "v1";
    result.file = {"https://cdn.modrinth.com/data/p1/versions/v1/fixture.jar", std::move(name), config.at("size").get<std::uint64_t>(), true,
        config.at("sha1").get<std::string>(), config.at("sha512").get<std::string>()};
    result.candidates = {{request(std::move(endpoint)), true}};
    return result;
}

struct LeaseToken {
    explicit LeaseToken(std::atomic<bool>& value) : active(value) { active = true; }
    ~LeaseToken() { active = false; }
    std::atomic<bool>& active;
};

void realResourceInstallation() {
    TemporaryRoot root;
    auto selected = context(root.path);
    std::atomic<bool> active{false};
    ResourceServices authority;
    authority.context = [&](const InstanceIdentity& identity) { require(identity == selected.identity, "Installation fixture identity mismatch"); return selected; };
    authority.acquireMutation = [&](const InstanceIdentity&, const std::string&) { return std::make_shared<LeaseToken>(active); };
    auto registry = std::make_shared<tasks::Registry>();
    const auto resources = bindResourceInstaller(client, registry, authority);
    const auto downloaded = plan("fixture.jar");
    {
        const auto lease = resources.acquireMutation(selected.identity, selected.revision);
        const auto receipt = resources.install(selected, downloaded, lease, {}, [&](const Progress&) { require(active.load(), "Mutation lease is released during download"); });
        require(active && !receipt.alreadyPresent && !receipt.taskId.empty() && hashResourceFile(receipt.destination).sha256 == config.at("sha256").get<std::string>(), "Shared installation fails integrity or task receipt");
    }
    require(!active, "Installation retains its mutation lease after completion");
    const auto before = count(stats(), "counts", "/files/fixture.jar");
    const auto reused = resources.install(selected, downloaded, resources.acquireMutation(selected.identity, selected.revision), {}, {});
    require(reused.alreadyPresent && count(stats(), "counts", "/files/fixture.jar") == before, "Verified existing installation redownloads");
    write(selected.modsRoot / "conflict.jar", "preserve original");
    expectError("conflict", [&] { (void)resources.install(selected, plan("conflict.jar"), resources.acquireMutation(selected.identity, selected.revision), {}, {}); });
    auto invalid = plan("invalid.jar", "/invalid-archive");
    invalid.file.sha1 = config.at("invalid_sha1").get<std::string>();
    invalid.file.sha512 = config.at("invalid_sha512").get<std::string>();
    expectError("archive_failed", [&] { (void)resources.install(selected, invalid, resources.acquireMutation(selected.identity, selected.revision), {}, {}); });
    require(!std::filesystem::exists(selected.modsRoot / "invalid.jar"), "Invalid archive is published after verified download");
    auto wrongHash = plan("wrong.jar");
    wrongHash.file.sha1 = std::string(40, '0');
    expectError("integrity", [&] { (void)resources.install(selected, wrongHash, resources.acquireMutation(selected.identity, selected.revision), {}, {}); });
    require(!std::filesystem::exists(selected.modsRoot / "wrong.jar"), "Wrong hash reaches final target");
    const auto beforeRevision = selected;
    bool changed = false;
    expectError("conflict", [&] {
        (void)resources.install(beforeRevision, plan("revision.jar"), resources.acquireMutation(selected.identity, selected.revision), {}, [&](const Progress&) {
            require(active.load(), "Authority lease is not held during compatibility recheck");
            if (!changed) { selected.revision = "new-revision"; changed = true; }
        });
    });
    require(!std::filesystem::exists(selected.modsRoot / "revision.jar"), "Revision race publishes the resource");
    Cancellation cancel;
    auto operation = std::async(std::launch::async, [&] {
        expectError("cancelled", [&] { (void)resources.install(selected, plan("cancelled.jar", "/silent-install"), resources.acquireMutation(selected.identity, selected.revision), cancel, {}); });
    });
    await([] { return count(stats(), "counts", "/silent-install") == 1; });
    cancel.cancel();
    operation.get();
    await([] { return count(stats(), "closed", "/silent-install") == 1; });
    require(!std::filesystem::exists(selected.modsRoot / "cancelled.jar"), "Cancelled task publishes its staging file");
    for (const auto& snapshot : registry->snapshots()) require(snapshot.status != tasks::TaskStatus::Running && snapshot.status != tasks::TaskStatus::Committing, "Installation leaves a nonterminal task");
}

void realCallerTaskResourceDownload() {
    TemporaryRoot root;
    const auto modsRoot = root.path / "mods";
    std::filesystem::create_directory(modsRoot);
    auto registry = std::make_shared<tasks::Registry>();
    tasks::TaskSpec specification;
    specification.kind = tasks::TaskKind::Resource;
    specification.target = modsRoot / "caller-owned.jar";
    specification.network = client->snapshot();
    specification.network.user_agent = "Nickel/community-task-snapshot";
    const auto task = registry->begin(specification);
    const auto staged = task->staging() / "resource.download";
    auto pinned = plan("caller-owned.jar", "/files/helper.jar");
    pinned.candidates.front().request.headers.clear();
    std::atomic<bool> active{false};
    unsigned progressEvents = 0;
    {
        const auto lease = std::make_shared<LeaseToken>(active);
        downloadResourceStaged(*client, task->network(), pinned, staged, {}, task->cancellation(), [&](const transport::TransferProgress& value) {
            require(active.load(), "Caller-owned download loses mutation authority");
            ++progressEvents;
            task->progress(value);
        });
        require(active && progressEvents && registry->snapshots().size() == 1 &&
            task->snapshot().status == tasks::TaskStatus::Running,
            "Staged download creates a nested Task, commits prematurely or drops progress");
        require(!std::filesystem::exists(specification.target) &&
            hashResourceFile(staged).sha256 == config.at("sha256").get<std::string>(),
            "Caller-owned download publishes early or loses integrity");
        require(stats().at("headers").at("/files/helper.jar").at("user-agent") == specification.network.user_agent,
            "Staged download ignores the caller's immutable network snapshot");
        task->commit_files({{"resource.download", specification.target, {}, {}}});
        require(active && hashResourceFile(specification.target).sha256 == config.at("sha256").get<std::string>(),
            "Caller-owned final Task commit loses authority or verified content");
    }
    require(!active && registry->snapshots().size() == 1, "Caller-owned download retains a lease or extra Task");
    const auto before = count(stats(), "counts", "/files/helper.jar");
    const auto occupied = root.path / "occupied.download";
    write(occupied, "preserve caller file");
    const auto original = hashResourceFile(occupied).sha256;
    expectError("conflict", [&] { downloadResourceStaged(*client, specification.network, pinned, occupied); });
    ModLimits invalidLimit;
    invalidLimit.maximumArchiveBytes = 0;
    expectError("invalid_input", [&] { downloadResourceStaged(*client, specification.network, pinned, root.path / "limit.download", invalidLimit); });
    expectError("policy_denied", [&] { downloadResourceStaged(*client, specification.network, pinned, Path("relative.download")); });
    Cancellation cancelled;
    cancelled.cancel();
    expectError("cancelled", [&] { downloadResourceStaged(*client, specification.network, pinned, root.path / "cancelled.download", {}, cancelled); });
    require(count(stats(), "counts", "/files/helper.jar") == before && hashResourceFile(occupied).sha256 == original,
        "Staged download preflight performs IO or overwrites the caller's file");
    auto wrongHash = pinned;
    wrongHash.file.sha1 = std::string(40, '0');
    const auto rejected = root.path / "rejected.download";
    expectError("integrity", [&] { downloadResourceStaged(*client, specification.network, wrongHash, rejected); });
    require(!std::filesystem::exists(rejected), "Wrong-hash staged download leaves an unverified file");
    auto invalidArchive = plan("invalid.jar", "/invalid-archive");
    invalidArchive.file.sha1 = config.at("invalid_sha1").get<std::string>();
    invalidArchive.file.sha512 = config.at("invalid_sha512").get<std::string>();
    expectError("archive_failed", [&] { downloadResourceStaged(*client, specification.network, invalidArchive, rejected); });
    require(!std::filesystem::exists(rejected), "Invalid archive survives staging validation");
    auto redirected = plan("redirected.jar", "/redirect-private");
    expectError("policy_denied", [&] { downloadResourceStaged(*client, specification.network, redirected, rejected); });
    require(!std::filesystem::exists(rejected), "Denied redirect leaves an unapproved staging file");
    Cancellation physical;
    const auto silentBefore = count(stats(), "counts", "/silent-helper");
    const auto closedBefore = count(stats(), "closed", "/silent-helper");
    auto operation = std::async(std::launch::async, [&] {
        expectError("cancelled", [&] { downloadResourceStaged(*client, specification.network, plan("cancelled.jar", "/silent-helper"), rejected, {}, physical); });
    });
    await([&] { return count(stats(), "counts", "/silent-helper") == silentBefore + 1; });
    physical.cancel();
    operation.get();
    await([&] { return count(stats(), "closed", "/silent-helper") == closedBefore + 1; });
    require(!std::filesystem::exists(rejected), "Physically cancelled staged download leaves a file");
}


void realCallerTaskModPreflight() {
    TemporaryRoot root;
    auto selected = context(root.path);
    std::filesystem::create_directory(selected.modsRoot);
    const auto original = selected.modsRoot / "caller-original.jar.disabled";
    write(original, "identified caller Mod");
    std::atomic<bool> active{false};
    unsigned acquired = 0;
    ModServices services;
    services.resources.context = [&](const InstanceIdentity&) { return selected; };
    services.resources.acquireMutation = [&](const InstanceIdentity&, const std::string&) {
        require(!active.exchange(true), "Caller Mod update reacquires its non-reentrant mutation lease");
        ++acquired;
        return std::make_shared<LeaseToken>(active);
    };
    auto registry = std::make_shared<tasks::Registry>();
    services = bindModUpdater(client, registry, std::move(services));
    ProviderSettings settings;
    settings.apiPolicy = SourcePolicy::MirrorOnly;
    settings.modrinthMirror = config.at("http").get<std::string>() + "/mr/v2";
    settings.testLoopbackOrigins = {config.at("http").get<std::string>()};
    CachedHttp cache(fetch);
    CommunityService community(cache, settings);
    ModUpdate update;
    update.fileName = "caller-original.jar.disabled";
    update.originalSha256 = hashResourceFile(original).sha256;
    update.instance = selected.identity;
    update.instanceRevision = selected.revision;
    update.identified = true;
    update.available = plan("fixture.jar");
    const auto begin = [&] {
        tasks::TaskSpec specification;
        specification.kind = tasks::TaskKind::Resource;
        specification.target = selected.resourceRoot;
        specification.network = client->snapshot();
        specification.network.user_agent = "Palladium/caller-mod-preflight";
        return registry->begin(specification);
    };
    const auto failed = [&](const std::shared_ptr<tasks::Task>& task, const ModUpdateResult& result,
                            std::string_view code, bool cancelled = false) {
        require(result.receipts.empty() && result.error && result.error->code == code &&
            result.cancelled == cancelled && !active && !result.finalCommitted && !result.durabilityPending,
            "Prepublication caller Mod result loses error, cancellation or mutation authority");
        require(task->snapshot().status == (cancelled ? tasks::TaskStatus::Cancelled : tasks::TaskStatus::Failed) &&
            !std::filesystem::exists(task->workspace()), "Caller Mod failure leaves a live Task or workspace");
    };
    {
        const auto task = begin();
        const auto before = stats();
        auto unbound = services;
        unbound.replaceInTask = {};
        ModsService mods(std::move(unbound), &community);
        const auto result = mods.updateConfirmed(selected.identity, {update}, *task);
        failed(task, result, "unsupported");
        require(stats().at("counts").value("/mr/v2/project/p1", 0) == before.at("counts").value("/mr/v2/project/p1", 0) &&
            acquired == 0, "Missing caller publication binding performs provider IO or acquires authority");
    }
    unsigned replacementCalls = 0;
    services.replaceInTask = [&](const InstanceContext&, const std::string&, const std::string&,
                                  const DownloadPlan&, std::shared_ptr<void>, const std::string&, tasks::Task&, bool,
                                  const ProgressCallback&) -> ModUpdateReceipt {
        ++replacementCalls;
        throw ErrorException(Error{"controlled_prepublication", "No publication in this fixture", {{"phase", "before-publication"}}});
    };
    {
        const auto task = begin();
        const auto previous = client->snapshot();
        auto future = previous;
        future.user_agent = "Palladium/future-mod-preflight";
        client->set_snapshot(future);
        try {
            const auto prepared = services.prepareInTask(*update.available, settings, *task);
            require(prepared.file.sha1 == update.available->file.sha1 && prepared.file.sha512 == update.available->file.sha512,
                "Caller Mod preparation loses confirmed provider identity");
            const auto headers = stats().at("headers");
            for (const auto* path : {"/mr/v2/project/p1", "/mr/v2/version/v1"})
                require(headers.at(path).at("user-agent") == task->network().user_agent,
                    "Caller Mod preparation follows mutable defaults instead of its operation snapshot");
            task->rollback("controlled_prepublication", "Preparation fixture does not publish");
        } catch (...) {
            client->set_snapshot(previous);
            throw;
        }
        client->set_snapshot(previous);
    }
    {
        const auto task = begin();
        task->cancel();
        ModsService mods(services, &community);
        failed(task, mods.updateConfirmed(selected.identity, {update}, *task), "cancelled", true);
        require(replacementCalls == 0 && acquired == 0, "Cancelled caller Mod begins preparation or replacement");
    }
    {
        const auto task = begin();
        write(original, "changed since identification");
        ModsService mods(services, &community);
        failed(task, mods.updateConfirmed(selected.identity, {update}, *task), "conflict");
        require(replacementCalls == 0 && acquired == 1, "Stale original reaches caller Mod publication");
    }
    write(original, "identified caller Mod");
    {
        const auto task = begin();
        const auto expectedToken = task->cancellation().stop_token();
        const auto expectedId = task->id();
        services.replaceInTask = [&](const InstanceContext&, const std::string& name, const std::string& hash,
                                      const DownloadPlan& prepared, std::shared_ptr<void> lease, const std::string& stepId,
                                      tasks::Task& operation, bool final, const ProgressCallback&) -> ModUpdateReceipt {
            ++replacementCalls;
            require(lease && active && operation.id() == expectedId &&
                operation.cancellation().stop_token() == expectedToken && final && stepId == "mod-0" &&
                name == update.fileName && hash == update.originalSha256 && prepared.file.fileName.ends_with(".disabled"),
                "Caller Mod preparation changes Task/token, final-step selection, local identity or disabled state");
            throw ErrorException(Error{"controlled_prepublication", "No publication in this fixture", {{"phase", "before-publication"}}});
        };
        const auto countBefore = registry->snapshots().size();
        ModsService mods(services, &community);
        const auto result = mods.updateConfirmed(selected.identity, {update}, *task);
        failed(task, result, "controlled_prepublication");
        require(result.error->details.at("phase") == "before-publication" && replacementCalls == 1 && acquired == 2 &&
            registry->snapshots().size() == countBefore && hashResourceFile(original).sha256 == update.originalSha256,
            "Caller Mod failure drops canonical details, creates child Tasks or mutates the original");
    }
}


void realCallerTaskModPublication() {
    enum class Mode { Success, TaskCancel, UiCancel, TokenCancel, DownloadFailure, ChangedHash,
        ChangedIdentity, ProgressFailure, FinalProgressFailure, FinalReceiptFailure, PrefixReceiptFailure, Hook, FinalGateCancel };
    struct Case { const char* name; Mode mode; std::string_view point{}; bool final = false; };
    std::vector<Case> cases{
        {"two installed leaves and trailing unavailable selection", Mode::Success},
        {"first-step Task cancellation", Mode::TaskCancel},
        {"first-step UI cancellation", Mode::UiCancel},
        {"first-step accepted token cancellation", Mode::TokenCancel},
        {"second verified download failure", Mode::DownloadFailure},
        {"second original hash changed during download", Mode::ChangedHash},
        {"second equal-byte native identity substitution", Mode::ChangedIdentity},
        {"nonfinal progress reporting failure", Mode::ProgressFailure},
        {"final progress failure and late cancellation", Mode::FinalProgressFailure},
        {"actual final receipt survives binding reporting failure", Mode::FinalReceiptFailure},
        {"actual prefix survives binding reporting failure", Mode::PrefixReceiptFailure}};
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
    for (bool final : {false, true}) {
        for (const auto point : {"files.published", "step.marker", "step.committed", "cleanup"}) {
            if (!final && std::string_view(point) == "cleanup") continue;
            cases.push_back({final ? "final production publication fault" : "nonfinal production publication fault", Mode::Hook, point, final});
        }
    }
    cases.push_back({"final publication holds canonical cancellation gate", Mode::FinalGateCancel});
#endif
    for (const auto& test : cases) {
        TemporaryRoot root;
        auto selected = context(root.path / "instance");
        std::filesystem::create_directory(selected.resourceRoot);
        std::filesystem::create_directory(selected.modsRoot);
        const Strings names{"installed.jar", "second.jar.disabled"};
        const auto initial = pathFromUtf8(config.at("initial_jar").get<std::string>());
        for (const auto& name : names) detail::copyExclusive(initial, selected.modsRoot, name, 512 * 1024, {});
        const auto originalHash = config.at("initial_sha256").get<std::string>();
        const auto nextHash = config.at("sha256").get<std::string>();
        require(originalHash != nextHash, "Caller Mods fixture does not distinguish original and replacement bytes");
        std::atomic<bool> active{false};
        unsigned acquired = 0;
        ModServices services;
        services.resources.context = [&](const InstanceIdentity& identity) {
            require(identity == selected.identity, "Caller Mods context identity changes");
            return selected;
        };
        services.resources.acquireMutation = [&](const InstanceIdentity& identity, const std::string& revision) {
            require(identity == selected.identity && revision == selected.revision && !active.load(),
                "Caller Mods reenters mutation authority or loses the confirmed revision");
            ++acquired;
            return std::make_shared<LeaseToken>(active);
        };
        auto registry = std::make_shared<tasks::Registry>();
        services = bindModUpdater(client, registry, std::move(services));
        ProviderSettings settings;
        settings.apiPolicy = SourcePolicy::MirrorOnly;
        settings.modrinthMirror = config.at("http").get<std::string>() + "/mods-mr/v2";
        settings.curseforgeMirror = config.at("http").get<std::string>() + "/mods-cf/v1";
        settings.testLoopbackOrigins = {config.at("http").get<std::string>()};
        CachedHttp cache(fetch);
        CommunityService community(cache, settings);
        ModsService discovery(services, &community);
        auto updates = discovery.checkUpdates(selected.identity, names);
        require(updates.size() == 2 && acquired == 0, "Real caller Mods identification acquires publication authority");
        for (std::size_t index = 0; index != updates.size(); ++index) {
            const auto& update = updates[index];
            require(update.fileName == names[index] && update.identified && update.available &&
                update.originalSha256 == originalHash && update.instance == selected.identity && update.instanceRevision == selected.revision &&
                update.available->source == Source::Modrinth && update.available->projectId == "p1" && update.available->versionId == "v1" &&
                update.available->file.sha1 == config.at("sha1").get<std::string>() &&
                update.available->file.sha512 == config.at("sha512").get<std::string>(),
                "Real checkUpdates loses leaf, approved provider hashes, project, version or original identity");
        }
        if (test.mode == Mode::Success) {
            auto skipped = updates.back();
            skipped.fileName = "unavailable.jar";
            skipped.available.reset();
            detail::copyExclusive(initial, selected.modsRoot, skipped.fileName, 512 * 1024, {});
            updates.push_back(std::move(skipped));
        }
        const auto countBefore = registry->snapshots().size();
        tasks::TaskSpec specification;
        specification.kind = tasks::TaskKind::Resource;
        specification.target = selected.resourceRoot;
        specification.label = "Caller Mods publication";
        specification.network = client->snapshot();
        specification.network.user_agent = "Palladium/caller-mod-publication";
        Cancellation accepted;
        specification.cancellation = accepted;
        const auto task = registry->begin(specification);
        auto token = registry->cancellation_for(task->id());
        require(token && token->stop_token() == accepted.stop_token() &&
            token->stop_token() == task->cancellation().stop_token(), "Caller Mods does not receive its canonical accepted token");
        unsigned replacements = 0;
        const auto publish = services.replaceInTask;
        services.replaceInTask = [&](const InstanceContext& current, const std::string& name, const std::string& hash,
                                      const DownloadPlan& approved, std::shared_ptr<void> lease, const std::string& stepId,
                                      tasks::Task& operation, bool final, const ProgressCallback& progress) -> ModUpdateReceipt {
            const auto index = replacements++;
            require(index < 2 && &operation == task.get() && operation.id() == task->id() &&
                operation.cancellation().stop_token() == token->stop_token() && active && lease &&
                name == names[index] && hash == originalHash && stepId == "mod-" + std::to_string(index) && final == (index == 1) &&
                (index == 0 ? !approved.file.fileName.ends_with(".disabled") : approved.file.fileName.ends_with(".disabled")),
                "Production caller Mods binding changes Task/token, ordinal step, local leaf or enabled state");
            auto receipt = publish(current, name, hash, approved, std::move(lease), stepId, operation, final, progress);
            if ((test.mode == Mode::FinalReceiptFailure && final) || (test.mode == Mode::PrefixReceiptFailure && !final))
                throw ErrorException(Error{"report_failed", "Controlled failure after real production publication"});
            return receipt;
        };
        const auto downloadsBefore = count(stats(), "counts", "/files/caller-mod-update.jar");
        bool prefixReported = false, finalVerified = false, changed = false, hookReached = false, gateVerified = false;
        const ProgressCallback progress = [&](const Progress& value) {
            require(active.load(), "Caller Mods releases authority during preparation, publication or reporting");
            if (value.stage == "download" && replacements == 2 && !changed) {
                if (test.mode == Mode::ChangedHash) {
                    write(selected.modsRoot / names[1], "External original modification during download");
                    changed = true;
                } else if (test.mode == Mode::ChangedIdentity) {
                    const auto target = selected.modsRoot / names[1];
                    const auto previous = tasks::lease_file(target, {});
                    std::filesystem::rename(target, selected.modsRoot / "displaced.jar.disabled");
                    detail::copyExclusive(initial, selected.modsRoot, names[1], 512 * 1024, {});
                    const auto substituted = tasks::lease_file(target, {});
                    require(previous.identity != substituted.identity && previous.sha256 == substituted.sha256,
                        "Native identity fixture failed to preserve equal bytes while replacing the file object");
                    changed = true;
                }
            }
            if (value.stage != "committed") return;
            const auto actual = task->committed_files();
            require(actual.size() == value.completedBytes && task->snapshot().files_done == actual.size() &&
                task->snapshot().files_total == 2, "Caller Mods progress invents files instead of using real Task receipts");
            if (value.completedBytes == 1) {
                prefixReported = true;
                require(!actual.front().final, "First same-project installed leaf incorrectly terminates the operation");
                if (test.mode == Mode::TaskCancel) require(task->cancel(), "Live prefix Task cancellation is rejected");
                if (test.mode == Mode::UiCancel) require(registry->cancel(task->id()), "Live prefix UI cancellation is rejected");
                if (test.mode == Mode::TokenCancel) token->cancel();
                if (test.mode == Mode::DownloadFailure) {
                    auto arm = request("/fail-next-mod-download");
                    arm.method = "POST";
                    require(fetch(arm, {}).status == 200, "Cannot arm the independent second-download integrity fault");
                }
                if (test.mode == Mode::ProgressFailure) throw ErrorException(Error{"report_failed", "Controlled nonfinal progress failure"});
            } else {
                const auto gate = transport::detail::cancellation_gate(task->cancellation());
                std::unique_lock lock(*gate, std::try_to_lock);
                require(lock.owns_lock(), "Final committed reporting still holds the publication gate");
                lock.unlock();
                require(actual.back().final && task->snapshot().status == tasks::TaskStatus::Succeeded &&
                    !task->cancel() && !registry->cancel(task->id()), "Final real publication accepts late Task/UI cancellation");
                token->cancel();
                finalVerified = true;
                if (test.mode == Mode::FinalProgressFailure) throw ErrorException(Error{"report_failed", "Controlled final progress failure"});
            }
        };
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
        struct HookScope {
            std::function<void(std::string_view, const Path&)> previous = std::move(tasks::detail::testing::checkpoint);
            ~HookScope() { tasks::detail::testing::checkpoint = std::move(previous); }
        } hook;
        unsigned hits = 0;
        if (test.mode == Mode::Hook) {
            tasks::detail::testing::checkpoint = [&](std::string_view point, const Path&) {
                if (point == test.point && ++hits == (test.point == "cleanup" ? 1U : test.final ? 2U : 1U)) {
                    hookReached = true;
                    throw 9;
                }
            };
        } else if (test.mode == Mode::FinalGateCancel) {
            tasks::detail::testing::checkpoint = [&](std::string_view point, const Path& path) {
                if (point != "files.published" || path != selected.modsRoot / names[1]) return;
                hookReached = true;
                const auto state = task->snapshot();
                bool rejected = false;
                std::jthread contender([&] { rejected = !task->cancel() && !registry->cancel(task->id()); });
                contender.join();
                gateVerified = state.status == tasks::TaskStatus::Committing && !registry->cancellation_for(task->id()) && rejected;
                token->cancel();
            };
        }
#endif
        const auto defaultNetwork = client->snapshot();
        auto future = defaultNetwork;
        future.user_agent = "Palladium/future-mod-publication";
        client->set_snapshot(future);
        ModUpdateResult result;
        try {
            ModsService mods(services, &community);
            result = mods.updateConfirmed(selected.identity, updates, *task, progress);
        } catch (...) {
            client->set_snapshot(defaultNetwork);
            throw;
        }
        client->set_snapshot(defaultNetwork);
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
        tasks::detail::testing::checkpoint = {};
#endif
        const bool pending = test.mode == Mode::Hook && (test.point == "files.published" || test.point == "step.marker");
        const bool cancelled = test.mode == Mode::TaskCancel || test.mode == Mode::UiCancel || test.mode == Mode::TokenCancel;
        const bool partial = cancelled || test.mode == Mode::DownloadFailure || test.mode == Mode::ChangedHash ||
            test.mode == Mode::ChangedIdentity || test.mode == Mode::ProgressFailure || test.mode == Mode::PrefixReceiptFailure || (pending && !test.final);
        const std::size_t expected = partial ? 1 : 2;
        const auto actual = task->committed_files();
        require(actual.size() == expected && result.receipts.size() == expected && result.finalCommitted == !partial &&
            result.durabilityPending == pending && result.cancelled == cancelled && acquired == 1 && !active &&
            registry->snapshots().size() == countBefore + 1 && task->snapshot().files_done == expected && task->snapshot().files_total == 2 &&
            task->snapshot().status == (cancelled ? tasks::TaskStatus::Cancelled : partial ? tasks::TaskStatus::Failed : tasks::TaskStatus::Succeeded),
            "Caller Mods loses durable prefix, final/pending status, one Task or mutation lease lifecycle");
        for (std::size_t index = 0; index != expected; ++index) {
            const auto& receipt = result.receipts[index];
            require(receipt.taskId == task->id() && receipt.fileName == names[index] && receipt.destination == selected.modsRoot / names[index] &&
                receipt.stepId == "mod-" + std::to_string(index) && receipt.stepId == actual[index].step_id && receipt.sha256 == nextHash &&
                receipt.sha256 == actual[index].published.sha256 && receipt.final == actual[index].final &&
                receipt.durabilityPending == actual[index].durability_pending && hashResourceFile(receipt.destination).sha256 == nextHash,
                "Domain receipts differ from the canonical Task's actual published file facts");
        }
        require(prefixReported || test.mode == Mode::PrefixReceiptFailure, "Real nonfinal progress is not observed");
        if (!partial) require(!result.error && (finalVerified || test.mode == Mode::FinalReceiptFailure),
            "Final success or pending durability becomes failure after reporting or raw cancellation");
        else {
            const auto code = cancelled ? "cancelled" : pending ? "persistence_pending" : test.mode == Mode::DownloadFailure ? "integrity" :
                (test.mode == Mode::ChangedHash || test.mode == Mode::ChangedIdentity) ? "revision_conflict" : "report_failed";
            require(result.error && result.error->code == code, "Partial caller Mods outcome loses its canonical error code");
            if (test.mode != Mode::ChangedHash) require(hashResourceFile(selected.modsRoot / names[1]).sha256 == originalHash,
                "Unpublished suffix is lost or falsely replaced");
        }
        const auto expectedDownloads = partial && test.mode != Mode::DownloadFailure && test.mode != Mode::ChangedHash && test.mode != Mode::ChangedIdentity ? 1 : 2;
        require(count(stats(), "counts", "/files/caller-mod-update.jar") == downloadsBefore + expectedDownloads,
            "Caller Mods retries a published step or starts a cancelled/pending suffix");
        for (const auto* path : {"/mods-mr/v2/project/p1", "/mods-mr/v2/version/v1", "/files/caller-mod-update.jar"})
            require(stats().at("headers").at(path).at("user-agent") == task->network().user_agent,
                "Caller Mods provider and file transfers ignore the shared operation network snapshot");
        unsigned begins = 0, terminals = 0;
        for (const auto& event : registry->since(0).events) {
            require(event.snapshot.id == task->id(), "Caller Mods emits an event for a hidden child Task");
            if (event.kind == tasks::EventKind::Began) ++begins;
            if (event.kind == tasks::EventKind::Terminal) ++terminals;
        }
        require(begins == 1 && terminals == 1 && !registry->conflicts(selected.resourceRoot),
            "Caller Mods Task keeps claims or emits repeated/missing terminal events");
        if (test.mode == Mode::Hook || test.mode == Mode::FinalGateCancel) require(hookReached, "Requested real production publication hook was not reached");
        if (test.mode == Mode::FinalGateCancel) require(gateVerified, "Final production publication does not hold its canonical Task cancellation gate");
        if (pending || test.point == "cleanup") {
            require(std::filesystem::exists(task->workspace()), "Pending production publication loses its durable journal");
            const auto recovery = registry->recover(root.path, {});
            require(recovery.size() == 1 && recovery.front().recovered && recovery.front().committed == !partial &&
                recovery.front().committed_files.size() == expected && !std::filesystem::exists(task->workspace()),
                "Production pending recovery loses the actual committed prefix or final outcome");
        } else require(!std::filesystem::exists(task->workspace()), "Caller Mods leaves an unowned or live workspace");
        if (test.mode == Mode::Success) require(hashResourceFile(selected.modsRoot / "unavailable.jar").sha256 == originalHash,
            "Trailing unavailable selection is published or counted as the final step");
        std::cout << "PASS caller Mods matrix " << test.name;
        if (!test.point.empty()) std::cout << " [" << test.point << "]";
        std::cout << " files=" << expected << " final=" << result.finalCommitted << " pending=" << result.durabilityPending << '\n';
    }
}



void realGuardedModReplacement() {
    TemporaryRoot root;
    auto selected = context(root.path);
    std::filesystem::create_directory(selected.modsRoot);
    const auto original = selected.modsRoot / "old-name.jar.disabled";
    write(original, "identified original revision");
    const auto originalHash = hashResourceFile(original);
    std::atomic<bool> active{false};
    ModServices services;
    services.resources.context = [&](const InstanceIdentity&) { return selected; };
    services.resources.acquireMutation = [&](const InstanceIdentity&, const std::string&) { return std::make_shared<LeaseToken>(active); };
    auto registry = std::make_shared<tasks::Registry>();
    services = bindModUpdater(client, registry, services);
    const auto replacement = plan("new-provider-name.jar.disabled");
    const auto receipt = services.replace(selected, "old-name.jar.disabled", originalHash.sha256, replacement,
        services.resources.acquireMutation(selected.identity, selected.revision), {}, [&](const Progress&) { require(active.load(), "Update drops its unified lease before commit"); });
    require(receipt.fileName == "old-name.jar.disabled" && receipt.destination == original && hashResourceFile(original).sha256 == config.at("sha256").get<std::string>(), "Guarded update loses installed name/state or new content");
    require(!std::filesystem::exists(selected.modsRoot / "new-provider-name.jar.disabled"), "Update leaves two active versions");
    write(original, "before second confirmation");
    const auto confirmed = hashResourceFile(original);
    write(original, "changed after confirmation");
    const auto before = count(stats(), "counts", "/files/fixture.jar");
    expectError("conflict", [&] { (void)services.replace(selected, "old-name.jar.disabled", confirmed.sha256, replacement, services.resources.acquireMutation(selected.identity, selected.revision), {}, {}); });
    require(count(stats(), "counts", "/files/fixture.jar") == before, "Stale identification hash starts a replacement download");
    write(original, "race before download");
    const auto race = hashResourceFile(original);
    bool changed = false;
    expectError("revision_conflict", [&] {
        (void)services.replace(selected, "old-name.jar.disabled", race.sha256, replacement,
            services.resources.acquireMutation(selected.identity, selected.revision), {}, [&](const Progress&) {
                if (!changed) { write(original, "external mutation while downloading"); changed = true; }
            });
    });
    require(changed && hashResourceFile(original).sha256 != config.at("sha256").get<std::string>(), "Final commit overwrites a changed original");
    write(original, "cancel must preserve this original");
    const auto cancelledHash = hashResourceFile(original);
    Cancellation cancel;
    expectError("cancelled", [&] {
        (void)services.replace(selected, "old-name.jar.disabled", cancelledHash.sha256, replacement,
            services.resources.acquireMutation(selected.identity, selected.revision), cancel, [&](const Progress&) { cancel.cancel(); });
    });
    require(hashResourceFile(original).sha256 == cancelledHash.sha256, "Cancelled replacement moves or removes the original outside the journal");
}

void realReplacementClaimsAndRollback() {
    TemporaryRoot root;
    const auto mods = root.path / "mods";
    std::filesystem::create_directory(mods);
    tasks::Registry registry;
    const auto begin = [&](const Path& target, std::vector<Path> additional = {}) {
        tasks::TaskSpec specification;
        specification.kind = tasks::TaskKind::Resource;
        specification.target = target;
        specification.additional_targets = std::move(additional);
        return registry.begin(specification);
    };
    const auto read = [](const Path& path) {
        const auto value = tasks::detail::read_bounded(path, 1024);
        return std::string(reinterpret_cast<const char*>(value.data()), value.size());
    };
    const auto terminal = [&](const std::shared_ptr<tasks::Task>& task, tasks::TaskStatus status) {
        require(task->snapshot().status == status && !std::filesystem::exists(task->workspace()),
            "Replacement leaves an incorrect terminal or owned workspace");
        unsigned events = 0;
        for (const auto& event : registry.since(0).events)
            if (event.snapshot.id == task->id() && event.kind == tasks::EventKind::Terminal) ++events;
        require(events == 1, "Replacement emits multiple or missing terminal events");
    };
    for (const auto* suffix : {"", ".disabled", ".old"}) {
        const auto original = mods / (std::string("same.jar") + suffix);
        write(original, "original");
        const auto previous = tasks::lease_file(original, {});
        const auto task = begin(mods);
        write(task->staging() / "next", "updated");
        task->commit_replacement(original, previous.sha256, "next", original);
        require(read(original) == "updated", "In-place replacement loses state suffix or content");
        terminal(task, tasks::TaskStatus::Succeeded);
    }
    for (const auto* suffix : {".disabled", ".old"}) {
        const auto original = mods / (std::string("from.jar") + suffix);
        const auto destination = mods / (std::string("to.jar") + suffix);
        const auto retained = mods / (std::string("saved.jar") + suffix);
        write(original, "retain original");
        const auto previous = tasks::lease_file(original, {});
        const auto task = begin(original, {destination, retained});
        require(registry.conflicts(destination) && registry.conflicts(retained), "Renamed replacement claims are not unified");
        expectError("target_conflict", [&] { (void)begin(destination); });
        write(task->staging() / "next", "renamed update");
        task->commit_replacement(original, previous.sha256, "next", destination, retained);
        require(!std::filesystem::exists(original) && read(destination) == "renamed update" && read(retained) == "retain original",
            "Renamed/retained replacement loses either revision or state suffix");
        terminal(task, tasks::TaskStatus::Succeeded);
    }
    {
        const auto original = mods / "changed.jar";
        write(original, "identified");
        const auto previous = tasks::lease_file(original, {});
        write(original, "modified");
        const auto task = begin(mods);
        write(task->staging() / "next", "must not publish");
        expectError("revision_conflict", [&] { task->commit_replacement(original, previous.sha256, "next", original); });
        task->rollback();
        require(read(original) == "modified", "Stale identification SHA overwrites current content");
        terminal(task, tasks::TaskStatus::Failed);
    }
    {
        const auto original = mods / "identity.jar";
        write(original, "same bytes");
        const auto previous = tasks::lease_file(original, {});
        std::filesystem::rename(original, mods / "identity-before.jar");
        write(original, "same bytes");
        require(tasks::lease_file(original, {}).identity != previous.identity, "Identity substitution fixture did not replace the file");
        const auto task = begin(mods);
        write(task->staging() / "next", "must not publish");
        expectError("revision_conflict", [&] { task->commit_files({{"next", original, previous, {}}}); });
        require(read(original) == "same bytes" && read(mods / "identity-before.jar") == "same bytes", "Identity conflict consumes a revision");
        terminal(task, tasks::TaskStatus::Failed);
    }
    {
        const auto original = mods / "batch.jar", foreign = mods / "foreign.jar";
        write(original, "batch original");
        write(foreign, "external contender");
        const auto previous = tasks::lease_file(original, {});
        const auto task = begin(mods);
        write(task->staging() / "next", "first planned update");
        write(task->staging() / "later", "later planned update");
        expectError("target_conflict", [&] {
            task->commit_files({{"next", original, previous, {}}, {"later", foreign, {}, {}}});
        });
        require(read(original) == "batch original" && read(foreign) == "external contender", "Batch preflight conflict partially moves the original");
        terminal(task, tasks::TaskStatus::Failed);
    }
    {
        const auto original = mods / "cancel-direct.jar";
        write(original, "cancel original");
        const auto previous = tasks::lease_file(original, {});
        const auto task = begin(mods);
        write(task->staging() / "next", "cancelled update");
        tasks::Registry independent;
        const auto recovery = independent.recover(root.path, {});
        require(recovery.size() == 1 && !recovery.front().recovered && std::filesystem::exists(task->workspace()),
            "Independent recovery consumes an active locked replacement workspace");
        task->cancel();
        expectError("cancelled", [&] { task->commit_replacement(original, previous.sha256, "next", original); });
        task->rollback();
        require(read(original) == "cancel original", "Cancelled replacement moves the original");
        terminal(task, tasks::TaskStatus::Cancelled);
    }
    bool budgetRejected = false;
    try { requireSpace(root.path, std::uintmax_t(-1)); }
    catch (const std::runtime_error&) { budgetRejected = true; }
    require(budgetRejected, "Impossible fixture budget is not rejected before writes");
}

ProviderSettings providerSettings() {
    ProviderSettings settings;
    settings.apiPolicy = SourcePolicy::MirrorOnly;
    settings.modrinthMirror = config.at("http").get<std::string>() + "/mr/v2";
    settings.curseforgeMirror = config.at("http").get<std::string>() + "/cf/v1";
    settings.testLoopbackOrigins = {config.at("http").get<std::string>()};
    return settings;
}

void realDescriptorResolution() {
    CachedHttp cache(fetch);
    auto community = std::make_shared<CommunityService>(cache, providerSettings());
    const auto file = pathFromUtf8(config.at("jar").get<std::string>());
    const auto before = count(stats(), "counts", "/files/fixture.jar");
    auto services = bindExportResolver({}, community, true);
    require(bool(services.resolve_resource) && bool(services.validate_resource_download_url), "Export binder omits a required approval service");
    const auto descriptor = services.resolve_resource(file, "mods/fixture.jar", {});
    require(descriptor && descriptor->path == "mods/fixture.jar" && descriptor->size == config.at("size").get<std::uint64_t>() && descriptor->downloads.front().ends_with("/matched.jar"), "Real provider metadata selects primary rather than exact secondary file");
    require(descriptor->sha1 == config.at("sha1").get<std::string>() && descriptor->sha512 == config.at("sha512").get<std::string>(), "Export descriptor loses exact local hashes");
    services.validate_resource_download_url(descriptor->downloads.front());
    expectError("policy_denied", [&] { services.validate_resource_download_url("https://example.org/arbitrary.jar"); });
    expectError("policy_denied", [&] { services.validate_resource_download_url("https://edge.forgecdn.net/files/1/1/fixture.jar"); });
    const auto zero = community->lookupCurseForgeFingerprint(0);
    require(zero && zero->id == "101", "Zero fingerprint is mistaken for missing fingerprint");
    auto fallback = providerSettings();
    fallback.modrinthMirror = config.at("http").get<std::string>() + "/mr-missing/v2";
    services = bindExportResolver({}, std::make_shared<CommunityService>(cache, fallback), false);
    const auto curseforge = services.resolve_resource(file, "mods/fixture.jar", {});
    require(curseforge && curseforge->downloads.front().starts_with("https://edge.forgecdn.net/") &&
        curseforge->sha1 == descriptor->sha1 && curseforge->sha512 == descriptor->sha512,
        "Bound resolver cannot fall back to an exact approved CurseForge file");
    services.validate_resource_download_url(curseforge->downloads.front());
    expectError("policy_denied", [&] { services.validate_resource_download_url("https://example.org/arbitrary.jar"); });
    std::stop_source stopped;
    stopped.request_stop();
    expectError("cancelled", [&] { (void)services.resolve_resource(file, "mods/fixture.jar", stopped.get_token()); });
    require(count(stats(), "counts", "/files/fixture.jar") == before, "Metadata export resolver downloads the resource content");
}

void realPackExport() {
    TemporaryRoot root;
    const auto phase = [&](const char* value) {
        std::cerr << "PACK " << value << " (fixture path characters=" << root.path.native().size() << ")\n";
    };
    phase("instance reservation");
    const auto game = root.path / "game";
    std::filesystem::create_directory(game);
    auto transaction = instances::InstanceTransaction::reserve(game, "Fixture", "1.20.1", "fabric", "0.16.9");
    const auto directory = transaction.directory();
    const auto version = directory / "versions" / "1.20.1";
    std::filesystem::create_directories(version);
    const Json descriptor{{"id", "1.20.1"}, {"type", "release"}, {"mainClass", "net.minecraft.client.main.Main"}};
    write(version / "1.20.1.json", descriptor.dump());
    write(version / "1.20.1.jar", "synthetic game artifact");
    phase("instance complete and commit");
    transaction.complete({"1.20.1", "fabric", "0.16.9", descriptor, nullptr});
    transaction.commit("Fixture", Json::object());
    std::filesystem::create_directory(directory / "mods");
    detail::copyExclusive(pathFromUtf8(config.at("jar").get<std::string>()), directory / "mods", "fixture.jar", 512 * 1024, {});
    phase("controlled onboarding");
    instances::OnboardingGate gate(root.path / "config", "0.1.0");
    gate.accept_eula(true, instances::eula_version, 1700000000);
    gate.acknowledge_changelog("0.1.0");
    require(gate.finish([] {}), "Controlled onboarding does not finish");
    CachedHttp cache(fetch);
    auto community = std::make_shared<CommunityService>(cache, providerSettings());
    packs::ExportServices services;
    services.archive = packs::native_archive_backend();
    services = bindExportResolver(std::move(services), community, true);
    packs::ExportRequest request;
    request.game_root = game;
    request.instance_id = "Fixture";
    request.name = "Controlled export";
    request.version = "1.0";
    request.selections = {"mods/fixture.jar"};
    request.bundle_resources = false;
    request.modrinth_only = true;
    const auto external = root.path / "external.mrpack";
    phase("exact Modrinth descriptor export");
    packs::export_pack(request, external, Json::object(), services, gate);
    auto entries = services.archive->inspect(external, {}, {});
    const auto manifest = packs::parse_mrpack_manifest(services.archive->read(external, "modrinth.index.json", 32768, {}));
    require(manifest.files.size() == 1 && manifest.files.front().downloads.front().ends_with("matched.jar"), "Real pack export loses the secondary descriptor");
    require(std::none_of(entries.begin(), entries.end(), [](const auto& entry) { return entry.name == "overrides/mods/fixture.jar"; }), "Unbundled resource is included as override");
    services.archive->release(external);
    request.bundle_resources = true;
    services.resolve_resource = [](const Path&, std::string_view, std::stop_token) -> std::optional<packs::PackFile> { throw std::runtime_error("Bundled export must not resolve external descriptors"); };
    const auto bundled = root.path / "bundled.mrpack";
    phase("bundled overrides export");
    packs::export_pack(request, bundled, Json::object(), services, gate);
    entries = services.archive->inspect(bundled, {}, {});
    require(std::any_of(entries.begin(), entries.end(), [](const auto& entry) { return entry.name == "overrides/mods/fixture.jar"; }), "Bundled resource is missing its override");
    services.archive->release(bundled);
    request.bundle_resources = false;
    request.modrinth_only = false;
    auto fallback = providerSettings();
    fallback.modrinthMirror = config.at("http").get<std::string>() + "/mr-missing/v2";
    services = bindExportResolver(std::move(services), std::make_shared<CommunityService>(cache, fallback), false);
    const auto curseforge = root.path / "curseforge.mrpack";
    phase("approved CurseForge descriptor export");
    packs::export_pack(request, curseforge, Json::object(), services, gate);
    (void)services.archive->inspect(curseforge, {}, {});
    require(services.archive->read(curseforge, "modrinth.index.json", 32768, {}).find("edge.forgecdn.net") != std::string::npos, "Non-Modrinth-only export rejects an approved CurseForge descriptor");
    services.archive->release(curseforge);
    request.modrinth_only = true;
    phase("Modrinth-only provider rejection");
    try { packs::export_pack(request, root.path / "denied.mrpack", Json::object(), services, gate); }
    catch (const instances::Error& error) { require(error.code() == "unsafe_url", "Wrong provider rejection differs"); return; }
    throw std::runtime_error("Modrinth-only export accepts a CurseForge descriptor");
}
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    try {
        require(argc == 2, "Supply the controlled loopback fixture configuration");
        const auto raw = tasks::detail::read_bounded(pathFromUtf8(argv[1]), 32768);
        const auto* first = reinterpret_cast<const std::uint8_t*>(raw.data());
        config = detail::parseBoundedJson(std::span<const std::uint8_t>(first, raw.size()), 32768);
        transport::NetworkSnapshot snapshot;
        snapshot.connect_timeout = 2s;
        snapshot.idle_timeout = 5s;
        snapshot.total_timeout = 8s;
        client = std::make_shared<transport::HttpClient>(snapshot);
        fetch = makeHttpFetch(client);
        const std::array<std::pair<const char*, void(*)()>, 12> cases{{
            {"real shared HTTP policy and budgets", realHttpPolicy}, {"one operation provider/file/body identity and future defaults", realOperationSnapshotIdentity},
            {"real socket physical cancellation", realPhysicalCancellation},
            {"real cache coalescing and last-subscriber abort", realCacheSubscribers}, {"shared task resource installation", realResourceInstallation},
            {"caller-owned Task staging and immutable network snapshot", realCallerTaskResourceDownload},
            {"caller-owned Mods prepublication failure and provider snapshot", realCallerTaskModPreflight},
            {"production caller-owned Mods steps, durable prefix and final gate", realCallerTaskModPublication},
            {"hash-guarded shared Mod replacement", realGuardedModReplacement}, {"renamed retained replacement, claims and rollback", realReplacementClaimsAndRollback},
            {"exact secondary descriptor, export binder and zero fingerprint", realDescriptorResolution}, {"real pack export resolver and provider policy", realPackExport}}};
        unsigned failures = 0;
        for (const auto& [name, test] : cases) {
            std::cerr << "START " << name << '\n';
            try { test(); std::cout << "PASS " << name << '\n'; }
            catch (const ErrorException& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.error().code << ": " << error.what() << '\n'; }
            catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
        }
        return failures ? 1 : 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
