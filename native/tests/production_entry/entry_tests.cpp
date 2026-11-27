#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "app/AcceptancePaths.hpp"
#include "app/PerformanceProbe.hpp"
#include "core/config/windows_security.hpp"
#include "tests/app/production_fixture.hpp"

#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
using nickel::Json;
using nickel::app::test::ProductionFixture;
using nickel::app::test::require;
using nickel::app::test::wait_until;
using nickel::ui::UiAction;
using nickel::ui::UiQuery;
using namespace std::chrono_literals;

void private_directory(const std::filesystem::path& path) {
#ifdef _WIN32
    nickel::foundation_detail::PrivateSecurity security(true);
    require(CreateDirectoryW(nickel::foundation_detail::native_filesystem_path(path).c_str(), security.get()) != FALSE,
            "Cannot exclusively create a private test directory");
#else
    require(::mkdir(path.c_str(), 0700) == 0, "Cannot exclusively create a private test directory");
#endif
}

template<class Function>
void rejected(Function function, const char* message) {
    bool failed = false;
    try { function(); } catch (...) { failed = true; }
    require(failed, message);
}

class PathFixture final {
public:
    explicit PathFixture(const std::filesystem::path& parent) {
        options.fixture_root = parent / ("entry-paths-" + nickel::tasks::detail::unique_id());
        private_directory(options.fixture_root);
        for (const auto* name : {"system", "bin", "game", "cache", "temp", "evidence"})
            private_directory(options.fixture_root / name);
        options.config_root = options.fixture_root / "system" / nickel::path_from_utf8(nickel::application_identifier);
        private_directory(options.config_root);
        options.game_root = options.fixture_root / "game";
        options.cache_root = options.fixture_root / "cache";
        options.temporary_root = options.fixture_root / "temp";
        options.executable = options.fixture_root / "bin" / "path-contract-only.bin";
        // This file exercises guarded hash validation; it is never a trusted launcher capability.
        nickel::atomic_write_text(options.executable, "owned-path-contract-input\n");
        options.executable_sha256 = nickel::tasks::detail::capture_regular_file(options.fixture_root, options.executable).sha256;
        nickel::atomic_write_text(options.config_root / "settings.json", "{}");
        nickel::atomic_write_text(options.config_root / "game.json",
            Json{{"gameDir", nickel::path_to_utf8(options.game_root)}}.dump());
        context_file = options.fixture_root / "evidence" / "immutable.context.json";
        events_file = context_file.parent_path() / "production-events.jsonl";
        request = {{"schemaVersion", 1}, {"protocol", "nickel-evidence-driver-v1"}, {"role", "native"},
            {"observationAdapter", "native-production-markers-v1"}, {"runId", "entry_run"}, {"scenarioId", "entry_scenario"},
            {"requestNonce", "entry_run_nonce"}, {"phaseRequestNonce", "entry_ready_nonce"}, {"phase", "ready"},
            {"sourceCommit", std::string(40, 'a')}, {"sourceSnapshotSha256", std::string(64, 'b')},
            {"buildSourceSha256", std::string(64, 'c')}, {"driverSourceSha256", std::string(64, 'd')},
            {"inputCorpusSha256", std::string(64, 'e')}, {"executableSha256", options.executable_sha256},
            {"fixtureRoot", nickel::path_to_utf8(options.fixture_root)}, {"productionEvents", nickel::path_to_utf8(events_file)},
            {"explicitAcceptedPathContext", {{"protocol", "explicit-accepted-path-context-v1"},
                {"systemKnownFolderAccess", "disabled"}, {"runFixture", nickel::path_to_utf8(options.fixture_root)},
                {"executableSha256", options.executable_sha256}, {"dataRoots", {
                    {"config", nickel::path_to_utf8(options.config_root)}, {"game", nickel::path_to_utf8(options.game_root)},
                    {"cache", nickel::path_to_utf8(options.cache_root)}, {"temp", nickel::path_to_utf8(options.temporary_root)}}}}}};
        save();
    }
    void save() const { nickel::atomic_write_text(context_file, request.dump()); }
    nickel::app::AcceptancePathOptions options;
    Json request;
    std::filesystem::path context_file;
    std::filesystem::path events_file;
};

void unicode_arguments_and_fail_closed(const std::filesystem::path& root) {
    const QStringList ordinary{QStringLiteral("pd-launcher"), QStringLiteral("--launch-instance"),
        QStringLiteral("\u542f\u52a8\u5668 \u00e9\U0001f680"), QStringLiteral(""), QStringLiteral("last value")};
    const auto normal = nickel::app::parse_production_arguments(ordinary);
    require(!normal.acceptance_context && !normal.performance, "Normal startup unexpectedly selected acceptance");
    std::vector<std::string> expected;
    for (qsizetype index = 1; index < ordinary.size(); ++index) expected.push_back(ordinary.at(index).toUtf8().toStdString());
    require(normal.business_arguments == expected, "Ordinary Unicode arguments changed value or ordering");
    auto measured = ordinary;
    measured.insert(2, QStringLiteral("--performance"));
    measured.insert(3, QStringLiteral("--acceptance-context"));
    measured.insert(4, QString::fromStdString(nickel::path_to_utf8(root / "not-created.context.json")));
    const auto parsed = nickel::app::parse_production_arguments(measured);
    require(parsed.performance && parsed.acceptance_context.has_value() && parsed.business_arguments == expected,
            "Acceptance flags or their value leaked into business arguments");
    for (const auto& arguments : std::vector<QStringList>{
            {ordinary.front(), QStringLiteral("--performance")},
            {ordinary.front(), QStringLiteral("--acceptance-context")},
            {ordinary.front(), QStringLiteral("--acceptance-context=")},
            {ordinary.front(), QStringLiteral("--acceptance-root"), QStringLiteral("anything")},
            {ordinary.front(), QStringLiteral("--performance=1")},
            {ordinary.front(), QStringLiteral("--acceptance-context"), QStringLiteral("relative.json")}})
        rejected([&] { (void)nickel::app::parse_production_arguments(arguments); }, "Malformed opt-in fell back to normal startup");
    std::cout << "Unicode argv / complete flag stripping / malformed opt-in: PASS\n";
}

void guarded_context_contract(const std::filesystem::path& root) {
    PathFixture fixture(root);
    const auto accepted = nickel::app::AcceptancePaths::from_context(fixture.context_file, fixture.options.executable);
    require(accepted.context().system_config_base == fixture.options.config_root.parent_path(), "Invented configuration path protocol");
    require(nickel::PathResolver(accepted.context()).config_root(false) == fixture.options.config_root, "Legal resolver precedence changed");
    accepted.verify();
    auto wrong = fixture.options;
    wrong.executable_sha256 = std::string(64, '0');
    rejected([&] { nickel::app::AcceptancePaths unsafe(wrong); }, "Wrong executable SHA-256 was accepted");
    wrong = fixture.options;
    wrong.game_root = root;
    rejected([&] { nickel::app::AcceptancePaths unsafe(wrong); }, "A data root escaped its run fixture");
    fixture.request["role"] = "legacy";
    fixture.save();
    rejected([&] { (void)nickel::app::AcceptancePaths::from_context(fixture.context_file, fixture.options.executable); },
             "Wrong role reached native acceptance");
    fixture.request["role"] = "native";
    fixture.save();
    rejected([&] { accepted.verify(); }, "A replaced immutable context retained its original authority");
    nickel::atomic_write_text(fixture.options.config_root / "instances.json",
        Json{{"roots", Json::array({nickel::path_to_utf8(root)})}}.dump());
    rejected([&] { nickel::app::AcceptancePaths unsafe(fixture.options); }, "Persisted game-root escape was accepted");
    std::cout << "Legal explicit PathContext / executable identity / immutable context / persisted root containment: PASS\n";
}

void normal_gate_privacy_cancellation_and_lifetime() {
    ProductionFixture fixture;
    const auto gate = fixture.query_ok(UiQuery::Onboarding);
    require(gate.at("needsEula").get<bool>() && !gate.at("startupReady").get<bool>(), "First-use gate was bypassed");
    const auto refusal = fixture.action(UiAction::FinishOnboarding);
    require(refusal.error.code == QStringLiteral("onboarding_required") && refusal.error.detail.isEmpty(),
            "FinishOnboarding without consent succeeded or leaked private detail");
    require(fixture.action(UiAction::AcceptEula, {{"checked", false}, {"eulaVersion", gate.at("eulaVersion")}}).error.isError(),
            "Unchecked EULA was accepted");
    fixture.finish_onboarding();
    fixture.action_ok(UiAction::FinishOnboarding);
    require(fixture.query_ok(UiQuery::Onboarding).at("startupState") == "completed", "Normal once-dispatch gate did not remain completed");
    require(fixture.state->http->budget() == fixture.state->budget, "Entry services cloned the shared network budget");
    // This fixture needs only a public account shell. Production refuses to manufacture an online
    // account without its stored credential state, so the shell is an offline identity; the public
    // privacy DTO is type-independent and still exercises email masking and credential omission.
    nickel::accounts::PublicAccount account;
    account.id = "entry-private-account"; account.name = "owned-fixture-account";
    account.uuid = nickel::accounts::offline_uuid(account.name); account.type = nickel::accounts::AccountType::offline;
    account.email = "private-entry@example.invalid";
    fixture.state->accounts.upsert_public(account);
    {
        auto manufactured = account;
        manufactured.id = "entry-manufactured-online";
        manufactured.type = nickel::accounts::AccountType::microsoft;
        bool refused = false;
        try { fixture.state->accounts.upsert_public(manufactured); }
        catch (const nickel::accounts::IdentityError& error) {
            refused = error.code() == nickel::accounts::IdentityErrorCode::missing_credentials;
        }
        require(refused, "A public-only shell fabricated a signed-in Microsoft account");
    }
    fixture.action_ok(UiAction::PatchSettings, {{"patch", {{"privacyMode", true}}}});
    const auto accounts = fixture.query_ok(UiQuery::Accounts).dump();
    require(accounts.find(*account.email) == std::string::npos && accounts.find("example.invalid") == std::string::npos
        && accounts.find("accessToken") == std::string::npos && accounts.find("refreshToken") == std::string::npos,
        "Public privacy DTO exposed an email or credential fields");
    const auto cancelled = ProductionFixture::next_id();
    fixture.services->request(cancelled, UiQuery::RuntimeInformation, {{"requestNonce", QStringLiteral("entry_cancelled")}});
    fixture.services->cancelRequest(cancelled);
    const auto replaced = ProductionFixture::next_id();
    fixture.services->request(replaced, UiQuery::RuntimeInformation, {{"requestNonce", QStringLiteral("entry_stale")}});
    fixture.services->request(replaced, UiQuery::RuntimeInformation, {{"requestNonce", QStringLiteral("entry_current")}});
    const auto reply = fixture.take_query(replaced);
    require(!reply.error.isError() && nickel::app::from_public_json(reply.data).at("requestNonce") == "entry_current",
            "Replaced request delivered a stale nonce");
    QTest::qWait(20);
    require(!fixture.queries.contains(cancelled), "Cancelled request delivered a queued reply");
    const auto called = std::make_shared<std::atomic<bool>>(false);
    auto capture = std::make_shared<unsigned>(42);
    const std::weak_ptr<unsigned> weak_capture = capture;
    require(fixture.state->schedule_monitor(1h, [capture, called](const nickel::Cancellation&) { called->store(true); }),
            "Real shared services refused a valid delayed monitor");
    capture.reset();
    fixture.services.reset();
    QCoreApplication::processEvents();
    require(weak_capture.expired() && !called->load(), "Shutdown retained or executed a pending capture");
    require(!fixture.state->schedule_monitor(0ms, [](const nickel::Cancellation&) {}), "Destroyed services accepted new work");
    std::cout << "Real normal EULA/finish / privacy / cancel-replace / shared-service shutdown lifetime: PASS\n";
}

void producer_and_queued_reply(nickel::app::PerformanceApplication& application, const std::filesystem::path& root) {
    PathFixture fixture(root);
    const auto accepted = nickel::app::AcceptancePaths::from_context(fixture.context_file, fixture.options.executable);
    nickel::app::PerformanceRecord record(accepted, nickel::app::PerformanceApplication::monotonic_nanoseconds());
    {
        nickel::app::AppServices services({accepted.context(), fixture.options.executable, {}});
        record.marker("servicesInitialized");
        nickel::app::PerformanceProbe probe(application, services);
        bool complete = false;
        probe.request_input_round_trip(QStringLiteral("entry_ready_nonce"), [&](const nickel::app::InputMeasurement& measurement) {
            require(measurement.successful && measurement.posted_ns <= measurement.dispatched_ns
                && measurement.dispatched_ns <= measurement.reply_ns, "Canonical production queued reply did not correlate");
            record.input(measurement); complete = true;
        });
        require(!complete, "Input reply was synchronous rather than GUI queued");
        wait_until([&] { return complete; });
        record.rendered_frame(QImage{});
    }
    record.finish(0);
    const auto text = nickel::read_text_bounded(fixture.events_file, 1024 * 1024);
    std::uint64_t sequence = 0;
    bool input = false, unavailable = false, exited = false;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find('\n', begin);
        require(end != std::string::npos, "Producer left a partial JSONL record");
        const auto event = Json::parse(text.substr(begin, end - begin));
        begin = end + 1;
        require(event.at("sequence").get<std::uint64_t>() == ++sequence && event.at("rootPid") == QCoreApplication::applicationPid(),
                "Producer sequence or actual process identity changed");
        require(event.at("product") == "Palladium" && event.at("executableName") == "pd-launcher", "Producer used a test product identity");
        require(event.at("sourceBindingsOrigin") == "runner-context-claims-not-build-attestation", "Runner source claims became attestation");
        if (event.at("event") == "inputReady") input = event.at("observation").at("successful").get<bool>();
        if (event.at("event") == "renderedFrameUnavailable") {
            const auto& fields = event.at("observation");
            unavailable = !fields.at("rendered").get<bool>() && !fields.at("presented").get<bool>() && !fields.at("ready").get<bool>();
        }
        if (event.at("event") == "exit") exited = event.at("observation").at("productionServicesDestroyed").get<bool>();
    }
    require(input && unavailable && exited, "Producer omitted actual queued input, truthful unavailable frame or service destruction");
    std::cout << "Production producer / real queued RuntimeInformation / truthful unavailable pixels / destruction order: PASS\n";
}

void malformed_context_entry_process(const std::filesystem::path& root) {
    const auto executable = qEnvironmentVariable("NICKEL_ENTRY_FIXTURE");
    require(!executable.isEmpty(), "The real-main bounded entry fixture executable must be supplied");
    QProcess process;
    process.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--acceptance-context"),
        QString::fromStdString(nickel::path_to_utf8(root / "missing.context.json")), QStringLiteral("--performance")});
    process.start();
    require(process.waitForStarted(5000) && process.waitForFinished(10000), "Malformed-context entry process did not fail promptly");
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() != 0, "Malformed context started production services");
    const auto error = process.readAllStandardError();
    require(error.contains("no fallback selected") && !error.contains(root.string().c_str()), "Entry failure was interactive or disclosed fixture paths");
    std::cout << "Actual main subprocess / missing context / no service fallback / noninteractive safe failure: PASS\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto environment_root = qEnvironmentVariable("NICKEL_ENTRY_TEST_ROOT");
        require(!environment_root.isEmpty(), "An explicit owned test-root environment is required");
        const auto parent = nickel::foundation_detail::checked_absolute_path(nickel::path_from_utf8(environment_root.toUtf8().toStdString()));
        require(parent.is_absolute() && parent != parent.root_path(), "Invalid test-root parent");
        if (!std::filesystem::exists(parent)) private_directory(parent);
        const auto root = parent / ("entry-run-" + nickel::tasks::detail::unique_id());
        private_directory(root); private_directory(root / "temp");
        const auto temporary = QByteArray::fromStdString(nickel::path_to_utf8(root / "temp"));
        for (const auto* name : {"TEMP", "TMP", "TMPDIR"}) require(qputenv(name, temporary), "Cannot isolate test temporary files");
        require(qputenv("QT_QPA_PLATFORM", "offscreen"), "Cannot select bounded offscreen test platform");
        nickel::app::PerformanceApplication application(argc, argv);
        unicode_arguments_and_fail_closed(root);
        guarded_context_contract(root);
        normal_gate_privacy_cancellation_and_lifetime();
        producer_and_queued_reply(application, root);
        malformed_context_entry_process(root);
        std::cout << "Entry library/fixture tests: PASS; full Pd current-image/presentation evidence remains separate.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Production entry test failed: " << failure.what() << '\n';
        return 1;
    }
}
