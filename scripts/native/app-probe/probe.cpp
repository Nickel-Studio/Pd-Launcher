#include "WorkerQueue.hpp"
#include "core_adapters.hpp"
#include "settings.hpp"

#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using namespace nickel;
using namespace std::chrono_literals;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Callback>
void requires_error(Callback callback, const char* message) {
    bool failed = false;
    try { callback(); } catch (const ErrorException&) { failed = true; }
    require(failed, message);
}

void test_queue() {
    app::WorkerQueue queue(1, 1);
    std::promise<void> entered;
    std::promise<void> stopped;
    auto entry = entered.get_future();
    auto stop = stopped.get_future();
    require(queue.submit([&](const Cancellation& cancellation) {
        entered.set_value();
        while (!cancellation.cancelled()) std::this_thread::sleep_for(1ms);
        stopped.set_value();
    }), "Initial queue submission failed");
    require(entry.wait_for(2s) == std::future_status::ready, "Worker did not run");
    bool discarded_ran = false;
    require(queue.submit([&](const Cancellation&) { discarded_ran = true; }), "Pending queue submission failed");
    require(queue.pending() == 1, "Queue pending count is not bounded");
    require(!queue.submit([](const Cancellation&) {}), "Queue accepted work over capacity");
    queue.stop();
    require(!queue.submit([](const Cancellation&) {}), "Stopped queue accepted work");
    require(stop.wait_for(2s) == std::future_status::ready, "Active work did not observe cancellation");
    require(!discarded_ran && queue.pending() == 0, "Stop did not discard unstarted work");
    app::WorkerQueue survivor(1, 4);
    std::promise<void> resumed;
    auto resumed_future = resumed.get_future();
    require(survivor.submit([](const Cancellation&) { throw std::runtime_error("Controlled failure"); }), "Exception fixture enqueue failed");
    require(survivor.submit([&](const Cancellation&) { resumed.set_value(); }), "Follow-up enqueue failed");
    require(resumed_future.wait_for(2s) == std::future_status::ready, "Request exception killed the worker");
}

void test_accounts(const std::filesystem::path& root) {
    JsonRepository repository(root);
    Vault vault(root);
    app::RepositoryAccountStorage storage(repository);
    app::VaultAdapter secrets(vault);
    accounts::AccountService service(storage, secrets);
    const auto account = service.add_offline("FixturePlayer");
    service.set_active(account.id);
    require(service.active_id() == account.id, "Active identity was not persisted");
    require(Json::parse(storage.read_accounts()).is_array(), "Account storage changed the legacy array shape");
    const auto public_json = Json::parse(accounts::public_account_json(account));
    require(!public_json.contains("accessToken") && !public_json.contains("password"), "Public account leaked private fields");
    const auto sealed = secrets.seal("inert interoperability fixture");
    require(sealed.starts_with("v1:") && secrets.open(sealed) == "inert interoperability fixture", "Real vault adapter did not round-trip");
    const auto accounts_before = storage.read_accounts();
    const auto active_before = storage.read_active();
    bool transaction_failed = false;
    try {
        storage.transaction([&] {
            storage.write_accounts("[]");
            storage.write_active("{\"id\":\"controlled-uncommitted\"}");
            throw std::runtime_error("Controlled precommit failure");
        });
    } catch (const accounts::IdentityError&) {
        transaction_failed = true;
    }
    require(transaction_failed, "Canonical account transaction did not propagate failure");
    require(storage.read_accounts() == accounts_before && storage.read_active() == active_before,
            "Failed account transaction changed accounts or active identity");
    service.remove(account.id);
    require(service.list().empty(), "Offline account was not removed");
}

void test_network_configuration() {
    const auto defaults = app::network_configuration(Json::object());
    require(defaults.limits.connections == 10 && defaults.limits.bytes_per_second == 0, "Network defaults changed");
    require(defaults.snapshot.user_agent == "Palladium/0.1.0 (studio.nickel.launcher)", "Production HTTP identity differs from the generated metadata");
    const auto configured = app::network_configuration(Json::parse(R"({"versionListSource":"official_first","fileDownloadSource":"mirror_first","communityApiSource":"official_auto","maxDownloadThreads":1000,"downloadLimitBps":9999999999})"));
    require(configured.snapshot.user_agent == defaults.snapshot.user_agent, "Settings replaced the trusted application HTTP identity");
    require(configured.snapshot.version_list_source == transport::SourcePolicy::OfficialFirst, "List source lost independence");
    require(configured.snapshot.file_download_source == transport::SourcePolicy::MirrorFirst, "File source lost independence");
    require(configured.limits.connections == 64 && configured.limits.bytes_per_second == 128ULL * 1024 * 1024, "Network bounds were not clamped");
    const Json ui_settings{{"maxDownloadThreads", std::int64_t{4}}, {"downloadLimitBps", std::int64_t{4096}}};
    require(ui_settings.at("maxDownloadThreads").is_number_integer()
            && !ui_settings.at("maxDownloadThreads").is_number_unsigned(), "Signed UI fixture lost its storage type");
    const auto ui_configured = app::network_configuration(ui_settings);
    require(ui_configured.limits.connections == 4 && ui_configured.limits.bytes_per_second == 4096,
            "Nonnegative signed UI integers were ignored");
    const auto zero = app::network_configuration(Json{{"maxDownloadThreads", std::int64_t{0}}, {"downloadLimitBps", std::int64_t{0}}});
    require(zero.limits.connections == 1 && zero.limits.bytes_per_second == 0, "Signed zero did not preserve the documented bounds");
    for (const auto& maximum : {Json(std::numeric_limits<std::int64_t>::max()), Json(std::numeric_limits<std::uint64_t>::max())}) {
        const auto limited = app::network_configuration(Json{{"maxDownloadThreads", maximum}, {"downloadLimitBps", maximum}});
        require(limited.limits.connections == 64 && limited.limits.bytes_per_second == 128ULL * 1024 * 1024,
                "Large signed or unsigned network integers overflowed the bounds");
    }
    for (const auto& invalid : {Json(-1), Json(4.5), Json(true), Json("4"), Json(nullptr)}) {
        const auto fallback = app::network_configuration(Json{{"maxDownloadThreads", invalid}, {"downloadLimitBps", invalid}});
        require(fallback.limits.connections == 10 && fallback.limits.bytes_per_second == 0,
                "Invalid network integers were coerced or wrapped");
    }
    requires_error([] { app::network_configuration(Json{{"versionListSource", "invalid"}}); }, "Invalid source policy was silently accepted");
}

void test_settings(const std::filesystem::path& root) {
    JsonRepository repository(root);
    auto budget = std::make_shared<transport::SharedBudget>();
    Cancellation cancellation;
    const auto original = Json::parse(R"({"unknown":18446744073709551615,"customTheme":{"hue":174,"saturation":39,"unknown":18446744073709551614},"nullable":true})");
    repository.write("settings.json", original);
    auto saved = app::apply_settings_patch(repository, "settings.json",
        Json{{"customTheme", {{"hue", 180}}}, {"nullable", nullptr}, {"maxDownloadThreads", std::int64_t{4}},
             {"downloadLimitBps", std::int64_t{4096}}}, budget, cancellation);
    require(saved.at("unknown").get<std::uint64_t>() == std::numeric_limits<std::uint64_t>::max(), "Top-level unknown integer lost precision");
    require(saved.at("customTheme").at("unknown").get<std::uint64_t>() == std::numeric_limits<std::uint64_t>::max() - 1, "Nested unknown integer lost precision");
    require(saved.at("customTheme").at("saturation") == 39 && saved.at("customTheme").at("hue") == 180, "Nested settings object was replaced");
    require(saved.contains("nullable") && saved.at("nullable").is_null(), "Explicit null was erased by a merge patch");
    require(budget->stats().limits.connections == 4 && budget->stats().limits.bytes_per_second == 4096,
            "Persisted signed UI network budget was not applied");
    saved = app::apply_settings_patch(repository, "settings.json",
        Json{{"customTheme", {{"lightness", 48}}}}, budget, cancellation);
    require(saved.at("customTheme").at("hue") == 180 && saved.at("customTheme").at("lightness") == 48, "Successive nested field patches clobbered each other");
    const auto before = repository.read_text("settings.json");
    requires_error([&] { app::apply_settings_patch(repository, "settings.json", Json{{"versionListSource", "invalid"}, {"maxDownloadThreads", 12u}}, budget, cancellation); }, "Invalid network settings were persisted");
    require(repository.read_text("settings.json") == before && budget->stats().limits.connections == 4
            && budget->stats().limits.bytes_per_second == 4096, "Failed settings save mutated persistent state or budget");
    Cancellation cancelled;
    cancelled.cancel();
    requires_error([&] { app::apply_settings_patch(repository, "settings.json", Json{{"maxDownloadThreads", 12u}}, budget, cancelled); }, "Cancelled settings save committed");
    require(repository.read_text("settings.json") == before && budget->stats().limits.connections == 4
            && budget->stats().limits.bytes_per_second == 4096, "Cancellation mutated persistent state or budget");
    requires_error([&] { app::apply_settings_patch(repository, "settings.json", Json{{"large", std::string(settings_max_bytes, 'x')}}, budget, cancellation); }, "Oversize settings patch committed");
    requires_error([&] { app::apply_settings_patch(repository, "accounts.json", Json::object(), budget, cancellation); }, "Settings port wrote a foreign document");
    const auto game = app::apply_settings_patch(repository, "game.json", Json{{"memoryMb", 4096u}}, budget, cancellation);
    require(game.at("memoryMb") == 4096 && budget->stats().limits.connections == 4
            && budget->stats().limits.bytes_per_second == 4096, "Game settings changed the shared network budget");
}


void test_http(const std::string& origin, const std::string& alternate) {
    auto client = std::make_shared<transport::HttpClient>();
    auto fetch = app::community_http(client);
    resources::HttpRequest request;
    request.url = origin + "/probe";
    request.maximumBytes = 1024;
    request.timeout = 3s;
    request.rules.testLoopbackOrigins.insert(origin);
    request.rules.testLoopbackOrigins.insert(alternate);
    Cancellation cancellation;
    const auto response = fetch(request, cancellation);
    require(response.status == 200 && std::string(response.bytes.begin(), response.bytes.end()) == "native-shared-http", "Real shared HTTP bridge failed");
    request.url = origin + "/redirect";
    request.followRedirects = true;
    require(fetch(request, cancellation).status == 200, "Approved relative redirect failed");
    request.url = origin + "/escape";
    requires_error([&] { fetch(request, cancellation); }, "Unapproved redirect origin was accepted");
    request.url = origin + "/cross-origin";
    request.headers["authorization"] = "inert-fixture-value";
    requires_error([&] { fetch(request, cancellation); }, "Credential-bearing cross-origin redirect was followed");
    request.headers.clear();
    request.url = origin + "/oversize";
    request.followRedirects = false;
    requires_error([&] { fetch(request, cancellation); }, "HTTP response exceeded its byte budget");
    request.url = origin + "/slow";
    Cancellation stop;
    std::jthread cancel([stop]() mutable { std::this_thread::sleep_for(100ms); stop.cancel(); });
    requires_error([&] { fetch(request, stop); }, "Physical HTTP cancellation was ignored");
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 4, "Probe needs a private root and two controlled origins");
        test_queue();
        const auto root = nickel::resources::pathFromUtf8(argv[1]);
        test_accounts(root);
        test_network_configuration();
        test_settings(root);
        test_http(argv[2], argv[3]);
        std::cout << R"({"workerQueue":true,"accountStorage":true,"vault":true,"networkPolicy":true,"nestedSettings":true,"integerPreservation":true,"persistedBudget":true,"sharedHttp":true,"redirectPolicy":true,"cancellation":true,"releaseAcceptance":false})" << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
