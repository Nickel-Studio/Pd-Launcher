#include "production_fixture.hpp"

#include <QApplication>
#include <QImage>
#include <QPluginLoader>
#include <QTest>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <map>
#include <set>
#include <thread>

using namespace std::chrono_literals;
using nickel::Json;
using nickel::app::test::ProductionFixture;
using nickel::app::test::require;
using nickel::app::test::wait_until;
using nickel::ui::UiAction;
using nickel::ui::UiQuery;

void run_production_lifecycle_tests();
void run_production_factory_tests();

namespace {

void runtime_nonce_and_onboarding() {
    ProductionFixture fixture;
    const auto initial = fixture.query_ok(UiQuery::Onboarding);
    require(initial.at("needsEula").get<bool>() && !initial.at("startupReady").get<bool>(), "First-use gate was bypassed");
    const auto reply = fixture.query_ok(UiQuery::RuntimeInformation, {{"requestNonce", "input_ABC-123"}});
    require(reply.at("requestNonce") == "input_ABC-123", "Runtime nonce did not correlate");
    require(!fixture.query_ok(UiQuery::RuntimeInformation).contains("requestNonce"), "Absent nonce was invented");
    for (const auto& scope : std::vector<Json>{{{"requestNonce", ""}}, {{"requestNonce", "bad/value"}},
            {{"requestNonce", std::string(129, 'x')}}, {{"requestNonce", 1}}, {{"root", "missing"}}, {{"versionId", "1.20.1"}}}) {
        const auto invalid = fixture.query(UiQuery::RuntimeInformation, scope);
        require(invalid.error.code == QStringLiteral("invalid_argument"), "Invalid nonce or partial instance scope was accepted");
    }
    require(fixture.query(UiQuery::Settings, {{"section", "untrusted"}}).error.isError(), "Unknown settings section was accepted");
    fixture.finish_onboarding();
    fixture.action_ok(UiAction::FinishOnboarding);
    require(fixture.query_ok(UiQuery::Onboarding).at("startupState") == "completed", "Repeated onboarding changed startup state");
    const auto persisted = nickel::JsonRepository(fixture.config).read("onboarding.json");
    require(persisted.is_object(), "Onboarding was not durable");
    std::cout << "runtime nonce / partial authority / durable repeated onboarding: PASS\n";
}

void durable_settings_and_shared_network() {
    ProductionFixture fixture;
    fixture.action_ok(UiAction::PatchSettings, {{"patch", {{"future", {{"a", 1}, {"b", 2}}}}}});
    fixture.action_ok(UiAction::PatchSettings, {{"section", "app"}, {"patch", {
        {"maxDownloadThreads", 3}, {"downloadLimitBps", 4096}, {"communityApiSource", "mirror_first"},
        {"fileDownloadSource", "official_first"}, {"future", {{"a", 5}}}}}});
    const auto stored = nickel::JsonRepository(fixture.config).read("settings.json");
    require(stored.at("future").at("a") == 5 && stored.at("future").at("b") == 2, "Patch lost unknown sibling fields");
    require(fixture.state->http->budget() == fixture.state->budget, "HTTP cloned the shared network budget");
    const auto limits = fixture.state->budget->stats().limits;
    require(limits.connections == 3 && limits.bytes_per_second == 4096, "Committed network limits were not applied");
    require(fixture.state->http->snapshot().community_api_source == nickel::transport::SourcePolicy::MirrorFirst,
        "Committed HTTP snapshot was not refreshed");
    require(fixture.state->community->settings().apiPolicy == nickel::resources::SourcePolicy::MirrorFirst,
        "Community did not consume the committed snapshot");
    const auto before = stored;
    require(fixture.action(UiAction::PatchSettings, {{"patch", {{"communityApiSource", "untrusted"}}}}).error.isError(),
        "Invalid source policy was accepted");
    require(nickel::JsonRepository(fixture.config).read("settings.json") == before, "Failed validation changed durable settings");
    require(fixture.action(UiAction::PatchSettings, {{"patch", {{"verifyMicrosoftSsl", false}}}}).error.code
        == QStringLiteral("unauthorized"), "Microsoft TLS verification was weakened");
    require(fixture.action(UiAction::PatchSettings, {{"patch", {{"selectedInstance", nullptr}}}}).error.code
        == QStringLiteral("unauthorized"), "Settings bypassed selected-instance authority");
    std::cout << "durable field patches / shared HTTP-budget refresh / TLS and authority: PASS\n";
}

void accounts_atomic_sibling_rollback() {
    ProductionFixture fixture;
    const auto first = fixture.state->accounts.add_offline("First", "first-account");
    const auto second = fixture.state->accounts.add_offline("Second", "second-account");
    fixture.action_ok(UiAction::SetActiveAccount, {{"id", first.id}});
    require(fixture.state->accounts.active_id() == first.id, "Active account did not commit");
    require(nickel::JsonRepository(fixture.config).read("settings.json").at("accountType") == "offline",
        "Account type did not commit with the active account");
    require(fixture.action(UiAction::SetActiveAccount, {{"id", second.id}, {"type", "microsoft"}}).error.isError(),
        "Mismatched account type was accepted");
    const auto active_before = nickel::read_text_bounded(fixture.config / "active_account.json", 4096);
    const auto accounts_before = nickel::read_text_bounded(fixture.config / "accounts.json", nickel::accounts_max_bytes);
    const auto faults = std::vector<std::pair<nickel::JsonReadResult::State, std::string>>{
        {nickel::JsonReadResult::State::Oversized, std::string(nickel::settings_max_bytes + 1, 'x')},
        {nickel::JsonReadResult::State::Corrupt, "{"},
        {nickel::JsonReadResult::State::WrongShape, "[]"}};
    for (const auto& [expected_state, bytes] : faults) {
        nickel::atomic_write_text(fixture.config / "settings.json", bytes);
        require(fixture.state->repository.inspect("settings.json", Json::object()).state == expected_state,
            "Real sibling-write fault did not reach its canonical storage boundary");
        const auto reply = fixture.action(UiAction::SetActiveAccount, {{"id", second.id}});
        require(reply.error.isError(), "Failed sibling write reported success");
        require(reply.error.code == "storage" && reply.error.retryable,
            "Failed sibling write lost its typed storage refusal");
        require(reply.error.detail.isEmpty(), "Sibling failure exposed private storage details");
        require(fixture.state->accounts.active_id() == first.id, "Sibling settings failure changed the active account");
        require(nickel::read_text_bounded(fixture.config / "settings.json", nickel::accounts_max_bytes) == bytes,
            "Failed sibling write silently repaired or replaced the original settings");
        require(nickel::read_text_bounded(fixture.config / "active_account.json", 4096) == active_before
            && nickel::read_text_bounded(fixture.config / "accounts.json", nickel::accounts_max_bytes) == accounts_before,
            "Failed sibling write changed a durable account document");
    }
    nickel::atomic_write_text(fixture.config / "settings.json", Json{{"accountType", "offline"}}.dump());
    const auto public_accounts = fixture.query_ok(UiQuery::Accounts).dump();
    require(public_accounts.find("accessToken") == std::string::npos && public_accounts.find("clientToken") == std::string::npos,
        "Public account DTO exposed credentials");
    std::cout << "active account / oversized-corrupt-shape sibling refusal / exact durable retention / public DTO: PASS\n";
}

void stale_delivery_and_long_operation_control() {
    ProductionFixture fixture;
    const auto cancelled = ProductionFixture::next_id();
    fixture.services->request(cancelled, UiQuery::RuntimeInformation, {{"requestNonce", QStringLiteral("cancelled")}});
    fixture.services->cancelRequest(cancelled);
    const auto replaced = ProductionFixture::next_id();
    fixture.services->request(replaced, UiQuery::RuntimeInformation, {{"requestNonce", QStringLiteral("old")}});
    fixture.services->request(replaced, UiQuery::RuntimeInformation, {{"requestNonce", QStringLiteral("current")}});
    const auto current = fixture.take_query(replaced);
    require(!current.error.isError() && nickel::app::from_public_json(current.data).at("requestNonce") == "current",
        "Replaced request delivered a stale reply");
    QTest::qWait(20);
    require(!fixture.queries.contains(cancelled), "Cancelled request delivered a reply");

    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    nickel::Cancellation stop;
    fixture.state->schedule_operation([&](const nickel::Cancellation& cancellation) {
        started.store(true);
        while (!cancellation.cancelled()) std::this_thread::sleep_for(1ms);
        finished.store(true);
    }, stop);
    struct StopOnExit {
        nickel::Cancellation token;
        ~StopOnExit() { token.cancel(); }
    } release{stop};
    wait_until([&] { return started.load(); });
    require(fixture.query_ok(UiQuery::RuntimeInformation, {{"requestNonce", "short_control"}}).at("requestNonce") == "short_control",
        "Long work consumed both workers");
    stop.cancel();
    wait_until([&] { return finished.load(); });
    std::cout << "cancel / replace stale queued replies / real two-worker short control: PASS\n";
}

void cleanup_preview_identity_and_partial_conflict() {
    ProductionFixture fixture;
    fixture.finish_onboarding();
    fixture.add_legacy_instance();
    std::filesystem::create_directories(fixture.game / "logs");
    const auto first = fixture.game / "logs" / "old-a.log";
    const auto second = fixture.game / "logs" / "old-b.log";
    nickel::atomic_write_text(first, "first\n");
    nickel::atomic_write_text(second, "second\n");
    const auto preview = fixture.query_ok(UiQuery::JunkPreview);
    require(preview.at("fileCount") == preview.at("count") && preview.at("fileCount").get<std::uint64_t>() >= 2,
        "Junk preview omitted files or canonical count");
    require(fixture.action(UiAction::CleanJunk, {{"previewId", preview.at("previewId")}, {"paths", Json::array()}}).error.isError(),
        "Cleanup accepted caller file authority");
    fixture.action_ok(UiAction::PatchSettings, {{"patch", {{"cleanupRevision", 1}}}});
    const auto conflict = fixture.action(UiAction::CleanJunk, {{"previewId", preview.at("previewId")}});
    require(conflict.error.code == QStringLiteral("conflict"), "Changed settings did not invalidate preview");
    const auto conflict_data = nickel::app::from_public_json(conflict.data);
    nickel::app::test::require_cleanup_result(conflict_data);
    require(conflict_data.at("locationConflict").get<bool>() && conflict_data.at("freedBytes") == 0,
        "Conflict lost its partial result");
    require(std::filesystem::exists(first) && std::filesystem::exists(second), "Stale preview removed files");
    const auto next = fixture.action_ok(UiAction::ScanJunk);
    const auto clean = fixture.action_ok(UiAction::CleanJunk, {{"previewId", next.at("previewId")}});
    nickel::app::test::require_cleanup_result(clean);
    require(clean.at("removedCount").get<std::uint64_t>() >= 2 && clean.at("freedBytes").get<std::uint64_t>() >= 13,
        "Actual cleanup did not remove both owned logs");
    require(!std::filesystem::exists(first) && !std::filesystem::exists(second), "Actual cleanup left selected logs");
    std::cout << "cleanup preview ID / eight canonical fields / changed authority / real removal: PASS\n";
}

void descriptions_are_owned_and_bounded() {
    ProductionFixture fixture;
    std::vector<std::string> keys;
    for (unsigned index = 0; index != 8; ++index) {
        nickel::documents::FullDescription description;
        description.projectId = "project-" + std::to_string(index);
        description.canonicalBase = "https://modrinth.com/mod/example";
        description.format = nickel::documents::BodyFormat::Html;
        description.text = "<p>safe text " + std::to_string(index) + "</p><script>alert(1)</script>"
            "<a href=\"https://modrinth.com/mod/example\">project</a>";
        const auto dto = fixture.state->own_description(fixture.state->body_reader.convert(description));
        require(dto.contains("documentKey") && dto.contains("sanitizedHtml") && dto.contains("images") && dto.contains("links"),
            "Description DTO changed");
        require(dto.dump().find("https://modrinth.com") == std::string::npos && dto.at("sanitizedHtml").get<std::string>().find("<script")
            == std::string::npos, "Description exposed URLs or unsanitized script");
        keys.push_back(dto.at("documentKey").get<std::string>());
    }
    nickel::documents::FullDescription ninth;
    ninth.projectId = "ninth";
    ninth.text = "ninth";
    bool rejected = false;
    try { (void)fixture.state->own_description(fixture.state->body_reader.convert(ninth)); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "Description lease budget was unbounded");
    fixture.services->releaseDescription(QString::fromStdString(keys.front()));
    rejected = false;
    try { (void)fixture.state->description(keys.front()); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Released description remained accessible");
    require(fixture.state->own_description(fixture.state->body_reader.convert(ninth)).contains("documentKey"),
        "Released description did not return its budget");
    std::cout << "production safe body / opaque URLs / lease cap / release: PASS\n";
}

void shared_resource_handoff_protection() {
    ProductionFixture fixture;
    fixture.finish_onboarding();
    // Both legacy instances must share one effective resource root; isolation is on by default.
    fixture.action_ok(UiAction::PatchSettings, {{"section", "game"}, {"patch", {{"versionIsolation", false}}}});
    const auto selected_scope = fixture.add_legacy_instance();
    fixture.add_legacy_instance("1.20.2");
    const auto selected = fixture.state->identity(selected_scope);
    const auto other = fixture.state->identity({{"root", nickel::instances::path_to_utf8(fixture.game)}, {"versionId", "1.20.2"}});
    auto launch = std::make_shared<nickel::app::LaunchOperation>(other, "handoff-check", fixture.game,
        nickel::instances::resource_directory(fixture.game, other.instanceId, fixture.state->settings("game")));
    launch->preparing.store(false);
    launch->public_state = {{"state", "running"}, {"phase", "running"}};
    {
        std::lock_guard lock(fixture.state->state_mutex);
        fixture.state->launches.emplace(launch->launch_id, launch);
    }
    require(fixture.state->running(selected), "Shared resources were mutable during started-to-game handoff");
    {
        std::lock_guard lock(fixture.state->state_mutex);
        launch->public_state = {{"state", "exited"}, {"phase", "draining"}};
        launch->draining.store(true);
    }
    require(fixture.state->running(selected), "Shared resources were mutable while draining");
    {
        std::lock_guard lock(fixture.state->state_mutex);
        launch->draining.store(false);
        launch->public_state = {{"state", "exited"}, {"phase", "finished"}};
    }
    require(!fixture.state->running(selected), "Terminal launch retained a resource lock");
    std::cout << "handoff and draining shared resource-root authority: PASS\n";
}

void secure_instance_icon_media() {
    ProductionFixture fixture;
    fixture.finish_onboarding();
    const auto scope = fixture.add_legacy_instance();
    const auto sibling = fixture.add_legacy_instance("1.20.2");
    const auto selected_file = fixture.root / "selected-icon.png";
    QImage selected(3, 5, QImage::Format_RGBA8888);
    selected.fill(Qt::red);
    require(selected.save(QString::fromUtf8(nickel::instances::path_to_utf8(selected_file)), "PNG"),
        "Cannot write a real selected icon fixture");
    std::set<std::string> approved;
    std::map<std::string, QImage> received;
    unsigned media_count = 0;
    const auto approve = [&](const nickel::ui::UiReply& reply) {
        const auto data = nickel::app::from_public_json(reply.data);
        const auto add = [&](const Json& row) {
            if (row.is_object() && row.contains("iconKey")) approved.insert(row.at("iconKey").get<std::string>());
        };
        add(data);
        if (data.is_object() && data.contains("items")) for (const auto& row : data.at("items")) add(row);
    };
    QObject::connect(fixture.services.get(), &nickel::ui::UiServices::queryReady, fixture.services.get(), approve);
    QObject::connect(fixture.services.get(), &nickel::ui::UiServices::commandFinished, fixture.services.get(), approve);
    QObject::connect(fixture.services.get(), &nickel::ui::UiServices::mediaReady, fixture.services.get(),
        [&](const QString& key, const QImage& image) {
            require(QThread::currentThread() == fixture.services->thread(), "Managed icon delivered off the GUI thread");
            require(approved.contains(key.toStdString()), "Managed pixels arrived before their reply approved the key");
            require(image.width() == 32 && image.height() == 32, "Managed icon lost canonical normalization");
            received.insert_or_assign(key.toStdString(), image);
            ++media_count;
        });
    const auto set_icon = [&](const Json& target) {
        auto arguments = target;
        arguments["path"] = nickel::instances::path_to_utf8(selected_file);
        return fixture.action_ok(UiAction::SetInstanceIcon, arguments);
    };
    const auto first = set_icon(scope);
    const auto key = first.at("iconKey").get<std::string>();
    require(key.starts_with("instance:") && key.size() == 73 && first.at("icon") == key
        && first.at("iconSource") == "managed" && first.at("mediaScope") == scope,
        "Managed icon DTO omitted its opaque identity and authorized media scope");
    require(received.contains(key), "Managed icon mutation omitted its pixels");
    require(fixture.query_ok(UiQuery::VersionDetail, scope).at("iconKey") == key,
        "Managed icon detail did not retain its content-bound key");
    const auto listed = fixture.query_ok(UiQuery::Instances, {{"root", scope.at("root")}});
    bool found = false;
    for (const auto& row : listed.at("items")) if (row.at("id") == scope.at("versionId")) {
        found = row.at("iconKey") == key && row.at("mediaScope") == scope;
    }
    require(found, "Instances query did not consume the verified icon");
    require(set_icon(sibling).at("iconKey") != key, "Different instance identities shared one media key");
    selected.fill(Qt::blue);
    require(selected.save(QString::fromUtf8(nickel::instances::path_to_utf8(selected_file)), "PNG"),
        "Cannot update the selected icon fixture");
    const auto changed = set_icon(scope);
    require(changed.at("iconKey") != key, "Changed managed icon reused a stale pixel key");
    const auto directory = fixture.game / "versions" / nickel::path_from_utf8(scope.at("versionId").get<std::string>());
    const auto reference = nickel::instances::read_local_settings(fixture.game,
        scope.at("versionId").get<std::string>()).value.at("icon").get<std::string>();
    nickel::atomic_write_text(directory / nickel::path_from_utf8(reference), "changed managed bytes");
    const auto before = media_count;
    const auto rejected = fixture.query(UiQuery::VersionDetail, scope);
    require(rejected.error.isError() && rejected.error.detail.isEmpty() && media_count == before,
        "Changed managed bytes were published or exposed private failure details");
    nickel::atomic_write_text(directory / nickel::path_from_utf8(nickel::instances::settings_file),
        Json{{"icon", nickel::instances::path_to_utf8(selected_file)}}.dump());
    const auto legacy = fixture.state->query(UiQuery::VersionDetail, scope, nickel::Cancellation{});
    require(!legacy.data.contains("iconKey") && legacy.media.empty(), "Legacy arbitrary icon path was decoded");
    const auto reset = fixture.action_ok(UiAction::SetInstanceIcon, {{"root", scope.at("root")},
        {"versionId", scope.at("versionId")}, {"icon", nullptr}});
    require(!reset.contains("iconKey") && media_count == before, "Built-in icon reset retained managed media authority");
    std::cout << "verified managed icon / opaque identity / reply-before-pixels / legacy path and tamper rejection: PASS\n";
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    try {
        const bool factory_only = argc == 2 && std::string_view(argv[1]) == "--factory-only";
        if (!factory_only) {
            runtime_nonce_and_onboarding();
            durable_settings_and_shared_network();
            accounts_atomic_sibling_rollback();
            stale_delivery_and_long_operation_control();
            cleanup_preview_identity_and_partial_conflict();
            descriptions_are_owned_and_bounded();
            shared_resource_handoff_protection();
            secure_instance_icon_media();
            run_production_lifecycle_tests();
        }
        run_production_factory_tests();
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Production application test failed: " << failure.what() << '\n';
        return 1;
    }
}
