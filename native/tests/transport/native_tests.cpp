#include "core/network/network.hpp"
#include "core/tasks/secure_fs.hpp"
#include "core/archive/archive.hpp"
#include "core/common/error.hpp"
#include "core/network/secure_memory.hpp"
#include <algorithm>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

namespace {
namespace fs = std::filesystem;
namespace sd = nickel::tasks::detail;
namespace nt = nickel::transport;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Function> void rejected(Function&& function, const char* message, std::string_view code = {}) {
    bool caught{};
    try { function(); } catch (const nickel::ErrorException& error) {
        if (!code.empty()) require(error.error().code == code, "wrong typed error code");
        caught = true;
    }
    require(caught, message);
}
template<class Predicate> void wait_until(Predicate&& predicate, const char* message) {
    const auto end = Clock::now() + 3s;
    while (!predicate()) { require(Clock::now() < end, message); std::this_thread::sleep_for(5ms); }
}
nt::HttpRequest request(std::string url, std::uint64_t limit = 65536) {
    nt::HttpRequest value; value.url = std::move(url); value.trust = nt::TrustPolicy::ControlledLoopback;
    value.max_response_bytes = limit; return value;
}
std::vector<std::byte> bytes(std::string_view text) {
    const auto raw = std::as_bytes(std::span(text.data(), text.size())); return {raw.begin(), raw.end()};
}
nt::Json stats(nt::HttpClient& client, const std::string& base) {
    nickel::Cancellation cancel; return client.perform(request(base + "/stats"), cancel).json();
}
nt::DownloadRequest artifact(const fs::path& root, const std::string& base, std::string endpoint, std::string file) {
    nt::DownloadRequest value; value.destination = root / sd::utf8_path(file);
    value.candidates = {{base + endpoint, false, {}}}; value.trust = nt::TrustPolicy::ControlledLoopback;
    value.expected_size = fs::file_size(root / "payload.bin"); value.max_bytes = *value.expected_size + 1024;
    value.digests = nt::detail::hash_file(root / "payload.bin", {nt::HashAlgorithm::Sha256, nt::HashAlgorithm::Sha1});
    value.retry = {2, 5ms, 20ms}; value.parallel_ranges = 1; return value;
}
void http_tests(nt::HttpClient& client, const nt::Json& config, const fs::path& root) {
    const auto base = config.at("http").get<std::string>();
    nickel::Cancellation cancel;
    auto response = client.perform(request(base + "/json"), cancel);
    require(response.status == 200 && response.json().at("ok") == true, "HTTP JSON GET failed");
    auto head = request(base + "/head", 1); head.method = nt::HttpMethod::Head;
    response = client.perform(head, cancel);
    require(response.status == 200 && response.body.empty() && response.content_length == 16777216, "HEAD incorrectly applied body cap");
    for (const auto method : {nt::HttpMethod::Post, nt::HttpMethod::Put, nt::HttpMethod::Patch, nt::HttpMethod::Delete}) {
        auto echo = request(base + "/echo"); echo.method = method; echo.body = bytes("typed-body");
        echo.headers = {{"Content-Type", "application/octet-stream"}};
        require(client.perform(echo, cancel).text() == "typed-body", "HTTP method body lost");
    }
    require(client.perform(request(base + "/chunked"), cancel).text() == "chunked body", "HTTP chunked payload failed");
    require(client.perform(request(base + "/redirect"), cancel).json().at("ok") == true, "same endpoint redirect failed");
    auto redirects = request(base + "/redirect"); redirects.redirect_policy = nt::RedirectPolicy::None;
    require(client.perform(redirects, cancel).status == 302, "disabled redirect followed");
    require(client.perform(request(base + "/folded-header"), cancel).header("x-header") == "value folded", "curl-normalized harmless header changed");
    for (const auto* endpoint : {"/redirect-private", "/redirect-loop", "/duplicate-length", "/ambiguous-framing", "/encoded", "/truncated", "/big-header", "/folded-length"}) {
        std::cerr << "HTTP hostile phase: " << endpoint << '\n';
        rejected([&] { client.perform(request(base + endpoint), cancel); }, "hostile HTTP response accepted");
    }
    auto cross = request(base + "/redirect-cross"); cross.redirect_policy = nt::RedirectPolicy::SameOrigin;
    rejected([&] { client.perform(cross, cancel); }, "SameOrigin crossed authority");
    cross.redirect_policy = nt::RedirectPolicy::PublicHttps; cross.headers = {{"Authorization", "fixture-only"}};
    rejected([&] { client.perform(cross, cancel); }, "cross-origin credential forwarded");
    auto allow = request(base + "/json"); allow.allowed_origins = {config.at("http2").get<std::string>()};
    rejected([&] { client.perform(allow, cancel); }, "origin allowlist ignored");
    auto insecure = request(base + "/json"); insecure.trust = nt::TrustPolicy::PublicHttps;
    rejected([&] { client.perform(insecure, cancel); }, "production trust allowed HTTP loopback");
    rejected([&] { client.perform(request(base + "/big", 8), cancel); }, "HTTP body cap ignored");
    rejected([&] { client.perform(request(base + "/invalid-json"), cancel).json(); }, "invalid JSON accepted", "invalid_json");
    auto retry = request(base + "/retry"); retry.retry = {3, 1ms, 20ms};
    require(client.perform(retry, cancel).status == 200, "GET retry failed");
    auto post_retry = request(base + "/post-retry"); post_retry.method = nt::HttpMethod::Post; post_retry.retry.max_attempts = 3;
    require(client.perform(post_retry, cancel).status == 503, "POST was automatically retried");
    nt::NetworkSnapshot short_snapshot = client.snapshot(); short_snapshot.idle_timeout = 120ms; short_snapshot.total_timeout = 400ms;
    nt::HttpClient short_client(short_snapshot);
    rejected([&] { short_client.perform(request(base + "/silent"), cancel); }, "idle timeout not enforced");
    auto timed = request(base + "/silent"); timed.timeout = 80ms;
    rejected([&] { client.perform(timed, cancel); }, "per-request deadline not enforced");
    std::stop_source stop;
    auto pending = std::async(std::launch::async, [&] { rejected([&] { client.perform(request(base + "/silent?stop"), stop.get_token()); }, "stop token did not cancel", "cancelled"); });
    std::this_thread::sleep_for(40ms); const auto cancelled_at = Clock::now(); stop.request_stop(); pending.get();
    require(Clock::now() - cancelled_at < 800ms, "physical HTTP cancellation too slow");
    auto tls_request = request(config.at("https").get<std::string>() + "/json");
    require(client.perform(tls_request, cancel).json().at("ok") == true, "valid CA/hostname TLS failed");
    for (const auto* key : {"untrusted_https", "hostname_https", "expired_https"}) {
        rejected([&] { client.perform(request(config.at(key).get<std::string>() + "/json"), cancel); }, "TLS validation bypassed");
    }
    rejected([&] { client.perform(request(config.at("https").get<std::string>() + "/redirect-http"), cancel); }, "TLS downgrade accepted");
    nt::NetworkSnapshot no_ca; nt::HttpClient untrusted(no_ca);
    rejected([&] { untrusted.perform(tls_request, cancel); }, "test CA implicitly trusted without bundle");
    nt::NetworkSnapshot proxy = client.snapshot(); proxy.proxy.mode = nt::ProxySnapshot::Mode::HttpConnect;
    proxy.proxy.endpoint = config.at("proxy").get<std::string>(); proxy.proxy.allow_loopback_proxy = true;
    nt::HttpClient proxied(proxy);
    require(proxied.perform(tls_request, cancel).status == 200, "validated HTTP CONNECT failed");
    proxy.proxy.endpoint = config.at("reject_proxy").get<std::string>(); nt::HttpClient denied(proxy);
    rejected([&] { denied.perform(tls_request, cancel); }, "proxy CONNECT rejection ignored");
    proxy.proxy.endpoint = config.at("proxy").get<std::string>(); proxy.proxy.allow_loopback_proxy = false; nt::HttpClient unsafe_proxy(proxy);
    rejected([&] { unsafe_proxy.perform(tls_request, cancel); }, "loopback proxy implicitly permitted");
    auto candidates = std::vector<nt::RequestCandidate>{{base + "/failure", false, {}}, {base + "/json", true, {}}};
    require(client.fetch_candidates(candidates, nt::SourceDomain::CommunityApi, request(base), cancel).status == 200, "HTTP source fallback failed");
    (void)root;
    std::cout << "PASS real HTTP methods/headers/chunking/redirects/caps/timeouts/retries/stop-token/TLS-chain-hostname-expiry/CONNECT\n";
}
void cache_tests(nt::HttpClient& client, const std::string& base) {
    nickel::Cancellation cancel;
    auto cached = request(base + "/cache");
    require(client.get_cached(cached, cancel).status == 200, "cache initial GET failed");
    require(client.get_cached(cached, cancel).transferred_bytes == 0 && client.cache_stats().hits != 0, "fresh cache not reused");
    auto no_store = request(base + "/no-store");
    client.get_cached(no_store, cancel); require(client.get_cached(no_store, cancel).transferred_bytes > 0, "no-store cached");
    auto revalidate = request(base + "/etag");
    require(client.get_cached(revalidate, cancel).text() == "etag-body", "ETag initial GET failed");
    require(client.get_cached(revalidate, cancel).text() == "etag-body", "ETag revalidation lost body");
    require(stats(client, base).at("etag304").get<int>() > 0, "cache did not send conditional ETag");
    auto slow = request(base + "/slow-cache"); nickel::Cancellation first_cancel, second_cancel;
    auto first = std::async(std::launch::async, [&] { rejected([&] { client.get_cached(slow, first_cancel); }, "one cancelled subscriber returned", "cancelled"); });
    wait_until([&] { return client.budget()->stats().active_connections > 0; }, "coalesced transfer did not start");
    auto second = std::async(std::launch::async, [&] { return client.get_cached(slow, second_cancel); });
    wait_until([&] { return client.cache_stats().coalesced > 0; }, "second subscriber not coalesced");
    first_cancel.cancel(); first.get(); require(second.get().text() == "slow-cache", "cancelled subscriber aborted live peer");
    auto abort_request = request(base + "/abort-cache"); nickel::Cancellation abort1, abort2;
    auto a = std::async(std::launch::async, [&] { rejected([&] { client.get_cached(abort_request, abort1); }, "last subscriber not cancelled", "cancelled"); });
    std::this_thread::sleep_for(30ms);
    auto b = std::async(std::launch::async, [&] { rejected([&] { client.get_cached(abort_request, abort2); }, "second abort subscriber not cancelled", "cancelled"); });
    std::this_thread::sleep_for(30ms); const auto at = Clock::now(); abort1.cancel(); abort2.cancel(); a.get(); b.get();
    wait_until([&] { return client.budget()->stats().active_connections == 0; }, "physical cache socket remains open");
    require(Clock::now() - at < 800ms, "last-subscriber socket abort too slow");
    require(client.get_cached(abort_request, cancel).text() == "retry-cache", "cancelled flight poisoned new subscriber");
    for (int index = 0; index < 30; ++index) {
        auto quick = request(base + "/cache?race=" + std::to_string(index));
        nickel::Cancellation race;
        auto work = std::async(std::launch::async, [&] { try { client.get_cached(quick, race); } catch (const nickel::ErrorException& error) { require(error.error().code == "cancelled", "cache race unexpected error"); } });
        if (index % 2 == 0) race.cancel(); work.get();
    }
    require(client.cache_stats().resident_bytes <= 32ULL * 1024 * 1024, "cache byte quota exceeded");
    std::cout << "PASS real byte-LRU/cache-control/ETag304/coalescing/individual-last-cancel/flight-retry/races\n";
}
void snapshot_tests(nt::HttpClient& client, const nt::Json& config) {
    const auto base = config.at("http").get<std::string>();
    nickel::Cancellation cancel;
    auto a = client.snapshot(), b = a;
    a.user_agent = "Nickel/snapshot-A"; b.user_agent = "Nickel/snapshot-B";
    a.version_list_source = nt::SourcePolicy::OfficialFirst;
    b.version_list_source = nt::SourcePolicy::MirrorFirst;
    client.set_snapshot(a);
    auto redirect = request(base + "/snapshot-redirect/capture");
    auto pending = std::async(std::launch::async, [&] { return client.perform(redirect, cancel); });
    wait_until([&] { return stats(client, base).value("/snapshot-redirect/capture", 0) == 1; }, "snapshot redirect did not start");
    client.set_snapshot(b);
    require(client.perform(request(base + "/snapshot/future"), cancel).json().at("agent") == b.user_agent,
            "setter did not update future default requests");
    require(pending.get().json().at("agent") == a.user_agent, "setter changed a running redirect snapshot");
    require(redirect.url == base + "/snapshot-redirect/capture", "request URL ownership was changed");
    const std::vector<nt::RequestCandidate> candidates{{base + "/snapshot/official", false, {}},
                                                       {base + "/snapshot/mirror", true, {}}};
    for (int index = 0; index < 8; ++index) {
        const auto input = request(base + "/snapshot/explicit");
        auto first = std::async(std::launch::async, [&] { return client.execute(input, a, cancel).json(); });
        auto second = std::async(std::launch::async, [&] {
            std::vector<std::byte> body;
            client.stream(input, b, cancel, [&](auto data) { body.insert(body.end(), data.begin(), data.end()); });
            return nt::Json::parse(body);
        });
        auto official = std::async(std::launch::async, [&] {
            return client.fetch_candidates(candidates, nt::SourceDomain::VersionList, input, a, cancel).json();
        });
        auto mirror = std::async(std::launch::async, [&] {
            return client.fetch_candidates(candidates, nt::SourceDomain::VersionList, input, b, cancel).json();
        });
        client.set_snapshot(index % 2 ? a : b);
        require(first.get().at("agent") == a.user_agent && second.get().at("agent") == b.user_agent,
                "explicit operations on one client shared mutable defaults");
        const auto selected_a = official.get(), selected_b = mirror.get();
        require(selected_a.at("agent") == a.user_agent && selected_a.at("path") == "/snapshot/official" &&
                selected_b.at("agent") == b.user_agent && selected_b.at("path") == "/snapshot/mirror",
                "explicit candidate source ordering did not retain its operation snapshot");
    }
    auto short_snapshot = a; short_snapshot.total_timeout = 60ms;
    auto delayed = request(base + "/snapshot-delay/deadline");
    auto longer = std::async(std::launch::async, [&] { return client.execute(delayed, b, cancel).json(); });
    rejected([&] { client.execute(delayed, short_snapshot, cancel); }, "explicit short deadline was ignored", "http_failed");
    require(longer.get().at("agent") == b.user_agent, "one operation deadline cancelled another snapshot");
    client.clear_cache(); client.set_snapshot(a);
    auto cached = request(base + "/snapshot-flight/generation");
    auto old = std::async(std::launch::async, [&] { return client.get_cached(cached, cancel); });
    wait_until([&] { return stats(client, base).value("/snapshot-flight/generation", 0) == 1; }, "old generation did not start");
    const auto at = Clock::now(); client.set_snapshot(b);
    require(Clock::now() - at < 200ms, "snapshot setter waited for an old flight");
    require(client.get_cached(cached, cancel).json().at("agent") == b.user_agent, "new generation coalesced with old snapshot");
    require(old.get().json().at("agent") == a.user_agent, "snapshot setter cancelled an old live subscriber");
    const auto hit = client.get_cached(cached, cancel);
    require(hit.transferred_bytes == 0 && hit.json().at("agent") == b.user_agent && client.cache_stats().entries == 1,
            "old flight repopulated current cache generation");
    auto fields = b;
    const auto fresh = request(base + "/snapshot/cache-fields");
    const auto check_field = [&] {
        client.set_snapshot(fields);
        require(client.get_cached(fresh, cancel).transferred_bytes > 0, "changed snapshot reused a stale entity");
        require(client.get_cached(fresh, cancel).transferred_bytes == 0, "new snapshot entity was not reusable");
    };
    fields.version_list_source = nt::SourcePolicy::OfficialFirst; check_field();
    fields.file_download_source = nt::SourcePolicy::OfficialFirst; check_field();
    fields.community_api_source = nt::SourcePolicy::MirrorFirst; check_field();
    fields.connect_timeout += 1ms; check_field(); fields.idle_timeout += 1ms; check_field();
    fields.total_timeout += 1ms; check_field();
    fields.ca_bundle = sd::utf8_path(config.at("alternate_ca_bundle").get<std::string>()); check_field();
    fields.proxy.username = "snapshot-fixture-user"; check_field();
    fields.proxy.password = "snapshot-fixture-password"; check_field();
    fields.proxy.allow_loopback_proxy = true; check_field();
    fields.proxy.mode = nt::ProxySnapshot::Mode::HttpConnect;
    fields.proxy.endpoint = config.at("proxy").get<std::string>(); check_field();
    fields.proxy.password = "snapshot-fixture-password-2"; check_field();
    require(!fields.proxy.username.empty() && !fields.proxy.password.empty(), "setter erased caller-owned snapshot");
    auto invalid = b; invalid.total_timeout = 0ms;
    rejected([&] { client.set_snapshot(invalid); }, "invalid snapshot replaced usable defaults", "http_failed");
    require(client.snapshot().proxy.password == fields.proxy.password, "failed setter changed defaults");
    client.set_snapshot(b); client.clear_cache();
    cache_tests(client, base);
    require(client.budget() == nt::SharedBudget::process(), "snapshot execution replaced the shared budget");
    std::cout << "PASS same-client explicit execute/stream/candidate policies/deadline isolation/default capture/redirect/settings race/cache generation/full snapshot invalidation/old-flight survival/subscriber cancellation\n";
}
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
struct ErasureAudit {
    static inline thread_local ErasureAudit* current{};
    std::uint64_t count{}, bytes{}, partial{};
    bool zero{true};
    ErasureAudit() { current = this; nt::detail::testing::after_erase = &observe; }
    ~ErasureAudit() { nt::detail::testing::after_erase = nullptr; current = nullptr; }
    static void observe(std::span<const std::byte> buffer) noexcept {
        if (!current) return;
        ++current->count; current->bytes += buffer.size();
        if (buffer.size() == 65537) ++current->partial;
        for (auto value : buffer) if (value != std::byte{}) current->zero = false;
    }
};
void erasure_tests(nt::HttpClient& client, const nt::Json& config) {
    const auto base = config.at("http").get<std::string>();
    nickel::Cancellation cancel;
    auto snapshot = client.snapshot();
    snapshot.proxy.username = std::string(183, 'u'); snapshot.proxy.password = std::string(197, 'p');
    auto echo = request(base + "/echo"); echo.method = nt::HttpMethod::Post;
    echo.headers = {{"Authorization", "Bearer " + std::string(503, 'h')}, {"Content-Type", "application/octet-stream"}};
    echo.body = bytes(std::string(4099, 'b'));
    const auto original_body = echo.body; const auto original_headers = echo.headers;
    ErasureAudit audit;
    const auto response = client.execute(echo, snapshot, cancel);
    require(response.body == original_body && echo.body == original_body && echo.headers == original_headers &&
            snapshot.proxy.password == std::string(197, 'p'), "erasure modified caller-owned DTOs");
    require(audit.count > 0 && audit.bytes > 4099 && audit.zero, "successful operation did not zero owned storage");
    auto partial = request(base + "/snapshot-partial", 131072);
    rejected([&] { client.execute(partial, snapshot, cancel); }, "truncated partial response accepted", "http_failed");
    require(audit.partial > 0 && audit.zero, "partial response storage was not zeroed on failure");
    client.clear_cache();
    require(audit.zero, "cache retirement reported nonzero erased storage");
    std::cout << "PASS direct success/failure internal erasure including truncated partial response and caller-buffer preservation; worker TLS observer is not global\n";
}
#endif
void snapshot_download_tests(nt::HttpClient& client, const nt::Json& config, const fs::path& root) {
    const auto base = config.at("http").get<std::string>();
    const auto defaults = client.snapshot();
    auto a = defaults, b = defaults;
    a.user_agent = "Pd/snapshot-download-A"; b.user_agent = "Pd/snapshot-download-B";
    a.file_download_source = nt::SourcePolicy::OfficialFirst;
    b.file_download_source = nt::SourcePolicy::MirrorFirst;
    nickel::Cancellation cancel;
    const auto verified = [](const nt::DownloadReceipt& receipt, const nt::DownloadRequest& input) {
        require(receipt.destination == input.destination && receipt.size == *input.expected_size &&
                !receipt.reused_existing && !receipt.digests.empty() &&
                nt::detail::digest_equal(receipt.digests.front().hex, input.digests.front().hex),
                "snapshot download did not publish the verified artifact");
        const auto actual = nt::detail::hash_file(input.destination, {nt::HashAlgorithm::Sha256});
        require(nt::detail::digest_equal(actual.front().hex, input.digests.front().hex),
                "snapshot download published bytes for another user agent");
    };
    auto first_input = artifact(root, base, "/snapshot-artifact-delay/official-A", "snapshot-ranges.bin");
    first_input.candidates.push_back({base + "/snapshot-artifact/mirror-A", true, {}});
    first_input.parallel_ranges = 4; first_input.range_threshold = 1;
    auto second_input = artifact(root, base, "/snapshot-artifact/official-B", "snapshot-mirror.bin");
    second_input.candidates.push_back({base + "/snapshot-artifact/mirror-B", true, {}});
    client.set_snapshot(b);
    auto first = std::async(std::launch::async, [&] { return client.download(first_input, a, cancel); });
    wait_until([&] { return stats(client, base).value("/snapshot-artifact-delay/official-A", 0) == 1; },
               "explicit snapshot range probe did not start");
    auto second = std::async(std::launch::async, [&] { return client.download(second_input, b, cancel); });
    for (int index = 0; index < 8; ++index) client.set_snapshot(index % 2 ? a : b);
    const auto first_receipt = first.get(), second_receipt = second.get();
    verified(first_receipt, first_input); verified(second_receipt, second_input);
    require(first_receipt.used_ranges, "explicit snapshot download skipped real range workers");
    const auto selected = stats(client, base);
    require(selected.value("/snapshot-artifact/mirror-A", 0) == 0 &&
            selected.value("/snapshot-artifact/official-B", 0) == 0 &&
            selected.value("/snapshot-artifact/mirror-B", 0) == 1,
            "download candidate ordering followed changed defaults");

    auto captured = artifact(root, base, "/snapshot-artifact-delay/default-A", "snapshot-default.bin");
    client.set_snapshot(a);
    auto pending = std::async(std::launch::async, [&] { return client.download(captured, cancel); });
    wait_until([&] { return stats(client, base).value("/snapshot-artifact-delay/default-A", 0) == 1; },
               "default snapshot download did not start");
    client.set_snapshot(b); verified(pending.get(), captured);

    auto fallback = artifact(root, base, "/snapshot-artifact/failure-A", "snapshot-fallback.bin");
    fallback.candidates.push_back({base + "/snapshot-artifact/fallback-A", true, {}});
    client.set_snapshot(a);
    auto falling_back = std::async(std::launch::async, [&] { return client.download(fallback, a, cancel); });
    wait_until([&] { return stats(client, base).value("/snapshot-artifact/failure-A", 0) == 1; },
               "snapshot fallback first candidate did not start");
    client.set_snapshot(b); verified(falling_back.get(), fallback);
    require(stats(client, base).value("/snapshot-artifact/fallback-A", 0) == 1,
            "snapshot download did not use its fallback candidate");

    auto survivor = artifact(root, base, "/snapshot-artifact-delay/survivor-B", "snapshot-survivor.bin");
    auto live = std::async(std::launch::async, [&] { return client.download(survivor, b, cancel); });
    auto resumed = artifact(root, base, "/snapshot-artifact/slow-A", "snapshot-resume.bin");
    nickel::Cancellation stopped;
    rejected([&] {
        client.download(resumed, a, stopped, [&](const auto& progress) {
            if (progress.completed_bytes >= 65536) stopped.cancel();
        });
    }, "snapshot cancellation published a partial download", "cancelled");
    require(!fs::exists(resumed.destination), "snapshot cancelled destination exists");
    verified(live.get(), survivor);
    const auto resumed_receipt = client.download(resumed, a, cancel);
    verified(resumed_receipt, resumed);
    require(resumed_receipt.resumed, "explicit snapshot partial download did not resume");

    auto deadline = artifact(root, base, "/snapshot-artifact-delay/deadline-A", "snapshot-deadline.bin");
    deadline.retry.max_attempts = 1;
    auto short_snapshot = a; short_snapshot.total_timeout = 60ms;
    rejected([&] { client.download(deadline, short_snapshot, cancel); },
             "explicit download snapshot deadline was ignored", "http_failed");
    require(!fs::exists(deadline.destination), "deadline-failed snapshot download published");
    require(a.user_agent == "Pd/snapshot-download-A" && b.user_agent == "Pd/snapshot-download-B" &&
            first_input.candidates.front().url == base + "/snapshot-artifact-delay/official-A",
            "download changed caller-owned snapshots or request DTOs");
    client.set_snapshot(defaults);
    require(client.budget() == nt::SharedBudget::process() && client.budget()->stats().active_connections == 0,
            "snapshot download replaced or leaked the shared budget");
    std::cout << "PASS Pd same-client explicit download snapshot/range workers/candidate policy/default capture/fallback/resume/isolated cancellation/deadline/setter race/verified publication\n";
}

void download_tests(nt::HttpClient& client, const fs::path& root, const std::string& base) {
    nickel::Cancellation cancel;
    auto simple = artifact(root, base, "/payload", "simple.bin");
    auto receipt = client.download(simple, cancel);
    require(receipt.size == simple.expected_size && !receipt.reused_existing && !receipt.used_ranges, "single download receipt incorrect");
    require(client.download(simple, cancel).reused_existing, "verified file reuse failed");
    auto ranges = artifact(root, base, "/payload", "ranges.bin"); ranges.parallel_ranges = 4; ranges.range_threshold = 1;
    receipt = client.download(ranges, cancel); require(receipt.used_ranges, "parallel ranges not used");
    auto ignored = artifact(root, base, "/ignore-range", "ignored.bin"); ignored.parallel_ranges = 4; ignored.range_threshold = 1;
    require(!client.download(ignored, cancel).used_ranges, "ignored range did not fall back");
    auto invalid_range = artifact(root, base, "/bad-range", "bad-range.bin"); invalid_range.parallel_ranges = 4; invalid_range.range_threshold = 1;
    require(!client.download(invalid_range, cancel).used_ranges, "invalid range did not fall back");
    auto resume = artifact(root, base, "/slow-payload", "resume.bin"); nickel::Cancellation stopped;
    rejected([&] { client.download(resume, stopped, [&](const auto& progress) { if (progress.completed_bytes >= 65536) stopped.cancel(); }); }, "cancelled download published", "cancelled");
    require(!fs::exists(resume.destination), "cancelled destination exists");
    require(client.download(resume, cancel).resumed, "valid partial did not resume");
    auto stale = artifact(root, base, "/stale-resume", "stale.bin"); nickel::Cancellation stale_cancel;
    rejected([&] { client.download(stale, stale_cancel, [&](const auto& p) { if (p.completed_bytes >= 65536) stale_cancel.cancel(); }); }, "stale setup cancellation failed", "cancelled");
    require(!client.download(stale, cancel).resumed, "changed ETag resume was not restarted");
    auto fallback = artifact(root, base, "/failure", "fallback.bin"); fallback.candidates.push_back({base + "/payload", true, {}});
    require(client.download(fallback, cancel).size == fallback.expected_size, "download source fallback failed");
    auto bad_hash = artifact(root, base, "/payload", "hash-bad.bin"); bad_hash.digests.front().hex[0] = bad_hash.digests.front().hex[0] == '0' ? '1' : '0';
    rejected([&] { client.download(bad_hash, cancel); }, "bad digest published"); require(!fs::exists(bad_hash.destination), "digest-failed destination exists");
    auto wrong_size = artifact(root, base, "/payload", "size-bad.bin"); ++*wrong_size.expected_size;
    rejected([&] { client.download(wrong_size, cancel); }, "wrong expected size published");
    auto tampered = artifact(root, base, "/slow-payload", "tampered.bin"); nickel::Cancellation tamper_cancel;
    rejected([&] { client.download(tampered, tamper_cancel, [&](const auto& p) { if (p.completed_bytes >= 65536) tamper_cancel.cancel(); }); }, "tamper setup failed", "cancelled");
    for (const auto& dir : fs::directory_iterator(root)) {
        if (!dir.is_directory() || !dir.path().filename().string().starts_with(".nickel-download-")) continue;
        const auto journal = nt::Json::parse(sd::read_bounded(dir.path() / "journal.json", 1024 * 1024));
        if (journal.at("destination").get<std::string>() != sd::path_utf8(tampered.destination)) continue;
        auto file = sd::SecureFile::open_owned(dir.path() / "artifact.part", {journal.at("fileIdentity").at("volume").get<std::uint64_t>(), journal.at("fileIdentity").at("index").get<std::uint64_t>()});
        file.seek(0); file.write(bytes("modified")); file.flush();
    }
    require(client.download(tampered, cancel).size == tampered.expected_size, "tampered partial was not safely retried");
    auto jar = artifact(root, base, "/invalid-archive", "broken.jar"); jar.expected_size = 8; jar.max_bytes = 1024;
    nt::detail::IncrementalHash hash(nt::HashAlgorithm::Sha256); hash.update(bytes("not-a-zip")); jar.digests = {{nt::HashAlgorithm::Sha256, hash.finish()}};
    rejected([&] { client.download(jar, cancel); }, "invalid archive download published"); require(!fs::exists(jar.destination), "invalid JAR target exists");
    auto deep = root / "long-download";
    while (sd::path_utf8(deep).size() < 420) deep /= "segment-" + std::string(48, 'x');
    { sd::ParentGuard parents(deep, true); }
    auto long_download = artifact(root, base, "/payload", "long.bin"); long_download.destination = deep / "long.bin";
    require(client.download(long_download, cancel).size == long_download.expected_size, "long-path download failed");
    require(client.download(long_download, cancel).reused_existing, "long-path verified reuse failed");
    long_download.destination = deep / "resume.bin"; long_download.candidates.front().url = base + "/slow-payload";
    nickel::Cancellation long_cancel;
    rejected([&] { client.download(long_download, long_cancel, [&](const auto& p) { if (p.completed_bytes >= 65536) long_cancel.cancel(); }); }, "long-path cancellation published", "cancelled");
    require(client.download(long_download, cancel).resumed, "long-path partial recovery failed");
    std::cout << "PASS streaming/resume/reuse long logical path characters=" << sd::path_utf8(deep).size() << '\n';
    require(client.budget()->stats().active_connections == 0, "download socket budget leaked");
    std::cout << "PASS real streaming/hash/size/reuse/parallel-ranges/fallback/resume/stale-ETag/tampered-partial/abort/JAR\n";
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "fixture configuration required"); const auto file = sd::checked_absolute(sd::utf8_path(argv[1]));
        const auto config = nt::Json::parse(sd::read_bounded(file, 1024 * 1024));
        nt::NetworkSnapshot snapshot; snapshot.ca_bundle = sd::utf8_path(config.at("ca_bundle").get<std::string>());
        snapshot.connect_timeout = 2s; snapshot.idle_timeout = 3s; snapshot.total_timeout = 10s;
        nt::HttpClient client(snapshot); const auto root = file.parent_path();
        if (config.value("snapshot_only", false)) {
            snapshot_tests(client, config);
            snapshot_download_tests(client, config, root);
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
            erasure_tests(client, config);
#endif
            return 0;
        }
        http_tests(client, config, root); cache_tests(client, config.at("http").get<std::string>());
        download_tests(client, root, config.at("http").get<std::string>()); return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL transport runtime: " << error.what() << '\n'; return 1; }
}
