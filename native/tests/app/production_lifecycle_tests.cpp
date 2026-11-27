#include "production_fixture.hpp"

#include <QTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using nickel::Cancellation;
using nickel::Json;
using nickel::app::AppServicesTestProbe;
using nickel::app::ApplicationState;
using nickel::app::test::ProductionFixture;
using nickel::app::test::require;
using nickel::app::test::wait_until;
using nickel::ui::UiAction;
using nickel::ui::UiQuery;

namespace {

struct ReleaseOnExit {
    std::shared_ptr<std::atomic<bool>> release;
    ~ReleaseOnExit() { release->store(true); }
};

void no_owned_launch(const ProductionFixture& fixture) {
    std::lock_guard lock(fixture.state->state_mutex);
    require(fixture.state->launches.empty(), "Rejected startup registered an owned game");
    require(fixture.state->startup_launch_id.empty(), "Rejected startup allocated a launch identity");
}

void first_use_requires_finish() {
    ProductionFixture fixture;
    const auto scope = fixture.add_legacy_instance("consent-fixture");
    const auto initial = fixture.query_ok(UiQuery::Onboarding);
    const auto reject_launch = [&] {
        const auto reply = fixture.action(UiAction::LaunchGame, scope);
        require(reply.error.code == "onboarding_required" && !reply.error.retryable,
            "LaunchGame did not expose the safe onboarding refusal");
        require(reply.error.detail.isEmpty(), "Onboarding refusal exposed private details");
        no_owned_launch(fixture);
        require(fixture.state->tasks->snapshots().empty(), "Rejected startup created a preparation task");
    };
    require(!fixture.query_ok(UiQuery::Bootstrap).at("startupReady").get<bool>(), "Bootstrap bypassed first use");
    reject_launch();
    require(fixture.action(UiAction::FinishOnboarding).error.code == "onboarding_required",
        "FinishOnboarding accepted missing consent");
    require(fixture.action(UiAction::AcceptEula, {{"checked", false}, {"eulaVersion", initial.at("eulaVersion")}})
        .error.code == "onboarding_required", "Unchecked consent changed the gate");
    fixture.action_ok(UiAction::AcceptEula, {{"checked", true}, {"eulaVersion", initial.at("eulaVersion")}});
    reject_launch();
    require(fixture.action(UiAction::AcknowledgeChangelog, {{"version", "obsolete-fixture-version"}})
        .error.code == "onboarding_required", "Stale changelog did not reject safely");
    fixture.action_ok(UiAction::AcknowledgeChangelog, {{"version", initial.at("currentVersion")}});
    require(!fixture.query_ok(UiQuery::Bootstrap).at("startupReady").get<bool>(),
        "Bootstrap dispatched after acknowledgement but before FinishOnboarding");
    reject_launch();
    fixture.action_ok(UiAction::FinishOnboarding);
    fixture.action_ok(UiAction::FinishOnboarding);
    require(fixture.query_ok(UiQuery::Onboarding).at("startupReady").get<bool>(), "Explicit finish did not complete");
    no_owned_launch(fixture);

    ProductionFixture rejected;
    const auto before_reject = rejected.query_ok(UiQuery::Onboarding);
    require(rejected.action(UiAction::FinishOnboarding).error.code == "onboarding_required",
        "Initial FinishOnboarding refusal changed before RejectEula");
    const auto rejected_status = rejected.action_ok(UiAction::RejectEula);
    require(rejected_status.at("exiting").get<bool>() && !rejected_status.at("startupReady").get<bool>(),
        "RejectEula did not close its real startup gate");
    for (const auto action : {UiAction::FinishOnboarding, UiAction::AcknowledgeChangelog}) {
        const auto reply = rejected.action(action,
            action == UiAction::AcknowledgeChangelog ? Json{{"version", before_reject.at("currentVersion")}} : Json::object());
        require(reply.error.code == "onboarding_required" && !reply.error.retryable && reply.error.detail.isEmpty(),
            "Rejected onboarding exposed internal instead of a safe consent refusal");
    }
    const auto after_reject = rejected.query_ok(UiQuery::Onboarding);
    require(after_reject.at("exiting").get<bool>() && !after_reject.at("startupReady").get<bool>()
        && after_reject.at("startupState") == before_reject.at("startupState")
        && after_reject.at("eulaVersion") == before_reject.at("eulaVersion")
        && after_reject.at("currentVersion") == before_reject.at("currentVersion")
        && after_reject.at("changelog") == before_reject.at("changelog"),
        "Rejected FinishOnboarding changed startup state or its canonical agreement documents");
    no_owned_launch(rejected);
    require(rejected.state->tasks->snapshots().empty(), "Rejected finish registered a preparation task");
    std::cout << "first-use / explicit finish / RejectEula then Finish / no owned launch: PASS\n";
}

void startup_retry_and_persisted_resume() {
    ProductionFixture fixture({"--launch-instance", "missing-value"});
    const auto status = fixture.query_ok(UiQuery::Onboarding);
    fixture.action_ok(UiAction::AcceptEula, {{"checked", true}, {"eulaVersion", status.at("eulaVersion")}});
    fixture.action_ok(UiAction::AcknowledgeChangelog, {{"version", status.at("currentVersion")}});
    require(fixture.action(UiAction::FinishOnboarding).error.isError(), "Malformed real CLI startup reported success");
    const auto failed = fixture.query_ok(UiQuery::Onboarding);
    require(failed.at("startupState") == "pending" && !failed.at("startupReady").get<bool>(),
        "Unclaimed startup failure was not retryable");
    no_owned_launch(fixture);
    fixture.state->options.startup_arguments.clear();
    fixture.action_ok(UiAction::FinishOnboarding);
    require(fixture.query_ok(UiQuery::Onboarding).at("startupReady").get<bool>(), "Finished gate could not retry unclaimed startup");
    const auto options = fixture.state->options;
    fixture.services.reset();
    fixture.state.reset();
    nickel::app::AppServices resumed(options);
    const auto state = AppServicesTestProbe::state(resumed);
    std::optional<nickel::ui::UiReply> bootstrap;
    QObject::connect(&resumed, &nickel::ui::UiServices::queryReady, &resumed,
        [&](const nickel::ui::UiReply& reply) { bootstrap = reply; });
    resumed.request(ProductionFixture::next_id(), UiQuery::Bootstrap, {});
    wait_until([&] { return bootstrap.has_value(); });
    require(!bootstrap->error.isError(), "Persisted startup bootstrap failed");
    require(nickel::app::from_public_json(bootstrap->data).at("startupReady").get<bool>(),
        "Existing current consent did not resume on the next application session");
    require(state->tasks->snapshots().empty(), "No-launch persisted startup created a task");
    std::cout << "real CLI failure / unclaimed retry / persisted next-session bootstrap: PASS\n";
}

void monitor_chain_keeps_its_slot() {
    ProductionFixture fixture;
    struct Control {
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
        std::atomic<bool> successor{false};
        std::atomic<bool> duplicate_rejected{false};
        std::atomic<bool> overlapping{false};
        std::atomic<unsigned> active{0};
        std::atomic<unsigned> callbacks{0};
        std::atomic<unsigned> fillers{0};
    };
    const auto control = std::make_shared<Control>();
    const auto release = std::shared_ptr<std::atomic<bool>>(control, &control->release);
    ReleaseOnExit release_on_exit{release};
    const auto chain = std::make_shared<ApplicationState::BackgroundWork>();
    const std::weak_ptr<ApplicationState::BackgroundWork> weak_chain = chain;
    const std::weak_ptr<ApplicationState> weak_state = fixture.state;
    *chain = [control, weak_chain, weak_state](const Cancellation& shutdown) {
        if (control->active.fetch_add(1) != 0) control->overlapping.store(true);
        const auto step = control->callbacks.fetch_add(1);
        if (step == 0) {
            control->entered.store(true);
            while (!control->release.load() && !shutdown.cancelled()) std::this_thread::sleep_for(1ms);
        }
        if (step < 3 && !shutdown.cancelled()) {
            const auto state = weak_state.lock();
            const auto next = weak_chain.lock();
            const bool queued = state && next && state->schedule_monitor(0ms, *next);
            if (!queued) control->successor.store(false);
            else if (step == 0) {
                control->successor.store(true);
                control->duplicate_rejected.store(!state->schedule_monitor(0ms,
                    [control](const Cancellation&) { control->overlapping.store(true); }));
                std::this_thread::sleep_for(80ms);
            }
        }
        control->active.fetch_sub(1);
    };
    require(fixture.state->schedule_monitor(0ms, *chain), "Initial monitor was not admitted");
    for (unsigned index = 0; index != 15; ++index)
        require(fixture.state->schedule_monitor(500ms,
            [control](const Cancellation&) { control->fillers.fetch_add(1); }), "Monitor registration limit is below 16");
    require(!fixture.state->schedule_monitor(0ms, [](const Cancellation&) {}), "Seventeenth monitor was admitted");
    wait_until([&] { return control->entered.load(); });
    require(AppServicesTestProbe::monitor_registrations(*fixture.services) == 16,
        "Executing monitor released its registration before successor admission");
    require(fixture.query_ok(UiQuery::RuntimeInformation, {{"requestNonce", "monitor_control"}}).at("requestNonce")
        == "monitor_control", "Monitor consumed both application workers");
    release->store(true);
    wait_until([&] { return control->callbacks.load() == 4 && control->active.load() == 0; });
    require(control->successor.load(), "Monitor successor lost its slot at capacity");
    require(control->duplicate_rejected.load(), "One monitor admitted a second successor");
    require(!control->overlapping.load(), "Zero-delay monitor successor overlapped its predecessor");
    wait_until([&] { return control->fillers.load() == 15
        && AppServicesTestProbe::monitor_registrations(*fixture.services) == 0; });
    require(!fixture.state->schedule_monitor(-1ms, *chain), "Negative monitor delay was admitted");
    require(!fixture.state->schedule_monitor(std::chrono::hours(1) + 1ms, *chain), "Unbounded monitor delay was admitted");
    std::cout << "16 retained monitor slots / unique successor / zero-delay serialization / short query: PASS\n";
}

void ordinary_saturation_and_monitor_shutdown() {
    ProductionFixture fixture;
    const auto release = std::make_shared<std::atomic<bool>>(false);
    ReleaseOnExit release_on_exit{release};
    const auto active = std::make_shared<std::atomic<unsigned>>(0);
    const auto completed = std::make_shared<std::atomic<unsigned>>(0);
    const auto monitored = std::make_shared<std::atomic<bool>>(false);
    for (unsigned index = 0; index != 2; ++index)
        require(AppServicesTestProbe::submit_work(*fixture.services,
            [release, active, completed](const Cancellation& shutdown) {
                active->fetch_add(1);
                while (!release->load() && !shutdown.cancelled()) std::this_thread::sleep_for(1ms);
                completed->fetch_add(1);
            }), "Existing worker was not admitted");
    wait_until([&] { return active->load() == 2; });
    for (unsigned index = 0; index != 48; ++index)
        require(AppServicesTestProbe::submit_work(*fixture.services,
            [completed](const Cancellation&) { completed->fetch_add(1); }), "Ordinary pending capacity is below 48");
    require(!AppServicesTestProbe::submit_work(*fixture.services, [](const Cancellation&) {}),
        "Ordinary work consumed a reserved monitor slot");
    require(fixture.state->schedule_monitor(0ms, [monitored](const Cancellation&) { monitored->store(true); }),
        "Ordinary saturation rejected reserved monitor admission");
    QTest::qWait(20);
    require(!monitored->load(), "Blocked workers ran monitor work outside their queue");
    release->store(true);
    wait_until([&] { return completed->load() == 50 && monitored->load(); });
    wait_until([&] { return AppServicesTestProbe::monitor_registrations(*fixture.services) == 0; });
    const auto called = std::make_shared<std::atomic<bool>>(false);
    auto capture = std::make_shared<unsigned>(42);
    const std::weak_ptr<unsigned> weak_capture = capture;
    require(fixture.state->schedule_monitor(std::chrono::hours(1),
        [capture, called](const Cancellation&) { called->store(true); }), "Valid delayed monitor was not retained");
    capture.reset();
    fixture.services.reset();
    QCoreApplication::processEvents();
    require(weak_capture.expired() && !called->load(), "Shutdown retained or executed a pending monitor capture");
    require(!fixture.state->schedule_monitor(0ms, [](const Cancellation&) {}), "Shutdown accepted a monitor");
    std::cout << "48 ordinary pending / reserved monitor admission / shutdown capture release: PASS\n";
}

Json accepted_download(const Json& downloads) {
    const auto& rows = downloads.at("items");
    const auto found = std::find_if(rows.begin(), rows.end(), [](const Json& row) { return row.contains("operationId"); });
    require(found != rows.end(), "Accepted pre-task operation was missing from public Downloads");
    return *found;
}

void long_admission_rejection_has_one_reply(unsigned mode) {
    ProductionFixture fixture;
    const auto release = std::make_shared<std::atomic<bool>>(false);
    ReleaseOnExit release_on_exit{release};
    const auto entered = std::make_shared<std::atomic<bool>>(false);
    const auto completed = std::make_shared<std::atomic<unsigned>>(0);
    if (mode == 0) {
        fixture.state->schedule_operation([release, entered, completed](const Cancellation& shutdown) {
            entered->store(true);
            while (!release->load() && !shutdown.cancelled()) std::this_thread::sleep_for(1ms);
            completed->fetch_add(1);
        }, Cancellation{});
        wait_until([&] { return entered->load(); });
        for (unsigned index = 0; index < 63; ++index) {
            fixture.state->schedule_operation([completed](const Cancellation&) { completed->fetch_add(1); }, Cancellation{});
        }
    } else if (mode == 1) AppServicesTestProbe::close_operations(*fixture.services);
    else fixture.state->set_scheduler({});
    const auto request = ProductionFixture::next_id();
    unsigned replies = 0;
    const auto connection = QObject::connect(fixture.services.get(), &nickel::ui::UiServices::commandFinished,
        fixture.services.get(), [&](const nickel::ui::UiReply& reply) { if (reply.id == request) ++replies; });
    fixture.services->submit(request, UiAction::ImportPack, {});
    wait_until([&] { return fixture.commands.contains(request); });
    const auto& reply = fixture.commands.at(request);
    require(reply.error.code == (mode == 1 ? "cancelled" : "busy"), "Long admission rejection lost its typed safe error");
    require(reply.error.detail.isEmpty(), "Long admission rejection exposed private details");
    const auto terminal = accepted_download(fixture.state->downloads_snapshot());
    require(terminal.at("terminal").get<bool>() && terminal.at("status") == (mode == 1 ? "cancelled" : "error")
        && !terminal.contains("taskId"), "Rejected long admission stranded its accepted operation or fabricated a Task");
    require(!fixture.state->pending_operations() && fixture.state->tasks->snapshots().empty(),
        "Rejected long admission retained pending business authority");
    release->store(true);
    if (mode == 0) wait_until([&] { return completed->load() == 64; });
    QTest::qWait(20);
    require(replies == 1, "Long admission rejection delivered zero or duplicate replies");
    QObject::disconnect(connection);
    std::cout << (mode == 0 ? "full 64-entry operation FIFO / one safe reply / accepted terminal / no Task: PASS\n"
        : mode == 1 ? "closed operation FIFO / one cancelled reply / accepted terminal / no Task: PASS\n"
        : "missing dispatcher / one busy reply / accepted terminal / no Task: PASS\n");
}

void resource_authorization_keeps_queries_responsive() {
    ProductionFixture fixture;
    fixture.finish_onboarding();
    const auto release = std::make_shared<std::atomic<bool>>(false);
    ReleaseOnExit release_on_exit{release};
    const auto entered = std::make_shared<std::atomic<bool>>(false);
    fixture.state->schedule_operation([release, entered](const Cancellation& shutdown) {
        entered->store(true);
        while (!release->load() && !shutdown.cancelled()) std::this_thread::sleep_for(1ms);
    }, Cancellation{});
    wait_until([&] { return entered->load(); });
    for (const auto action : {UiAction::InstallProject, UiAction::DownloadProjectFile}) {
        QElapsedTimer timer;
        timer.start();
        const auto reply = fixture.action(action);
        require(reply.error.code == "invalid_argument", "Malformed resource authorization did not fail safely");
        require(timer.elapsed() < 2000, "Resource authorization waited behind a long business operation");
        require(!release->load(), "Resource authorization unblocked unrelated business work");
    }
    const auto downloads = fixture.query_ok(UiQuery::Downloads);
    require(downloads.at("items").size() == 2, "Resource admission lost its accepted-operation receipts");
    for (const auto& row : downloads.at("items")) {
        require(row.at("terminal").get<bool>() && row.at("status") == "error"
            && !row.contains("taskId"), "Rejected authorization fabricated or stranded a resource task");
    }
    require(fixture.state->tasks->snapshots().empty(), "Rejected resource authorization created a canonical task");
    release->store(true);
    std::cout << "resource short authorization / busy real FIFO / safe replies within two seconds / no fabricated task: PASS\n";
}

void accepted_cleanup_outlives_request(bool cancel_business) {
    ProductionFixture fixture;
    fixture.finish_onboarding();
    fixture.add_legacy_instance();
    const auto log = fixture.game / "logs" / "accepted-fixture.log";
    std::filesystem::create_directories(log.parent_path());
    nickel::atomic_write_text(log, "durable cleanup fixture\n");
    const auto preview = fixture.query_ok(UiQuery::JunkPreview);
    const auto release = std::make_shared<std::atomic<bool>>(false);
    ReleaseOnExit release_on_exit{release};
    const auto entered = std::make_shared<std::atomic<bool>>(false);
    fixture.state->schedule_operation([release, entered](const Cancellation& shutdown) {
        entered->store(true);
        while (!release->load() && !shutdown.cancelled()) std::this_thread::sleep_for(1ms);
    }, Cancellation{});
    wait_until([&] { return entered->load(); });
    const auto request = ProductionFixture::next_id();
    fixture.services->submit(request, UiAction::CleanJunk,
        nickel::app::to_public_json(Json{{"previewId", preview.at("previewId")}}).toObject());
    const auto accepted = accepted_download(fixture.query_ok(UiQuery::Downloads));
    require(accepted.at("status") == "queued" && !accepted.contains("taskId"), "Cleanup fabricated a pre-admission task");
    fixture.services->cancelRequest(request);
    if (cancel_business) fixture.action_ok(UiAction::CancelDownload, {{"id", accepted.at("id")}});
    release->store(true);
    wait_until([&] {
        const auto row = accepted_download(fixture.query_ok(UiQuery::Downloads));
        return row.at("terminal").get<bool>();
    });
    const auto terminal = accepted_download(fixture.query_ok(UiQuery::Downloads));
    require(terminal.at("status") == (cancel_business ? "cancelled" : "completed"), "Accepted cleanup published the wrong terminal state");
    require(std::filesystem::exists(log) == cancel_business, "Request cancellation changed independently accepted deletion authority");
    require(fixture.state->tasks->snapshots().empty(), "Synchronous cleanup fabricated a canonical task");
    QTest::qWait(20);
    require(!fixture.commands.contains(request), "Cancelled cleanup request delivered a stale reply");
    std::cout << (cancel_business ? "early public CancelDownload / pre-task authority / no deletion: PASS\n"
        : "public CleanJunk / cancelRequest / durable deletion / suppressed reply: PASS\n");
}

void cleanup_late_public_cancel_preserves_result(bool with_file) {
    ProductionFixture fixture;
    fixture.finish_onboarding();
    fixture.add_legacy_instance();
    const auto log = fixture.game / "logs" / "publication-fixture.log";
    const std::string content = "committed cleanup publication fixture\n";
    if (with_file) {
        std::filesystem::create_directories(log.parent_path());
        nickel::atomic_write_text(log, content);
    }
    const auto preview = fixture.query_ok(UiQuery::JunkPreview);
    require(with_file || preview.at("fileCount") == 0, "Empty cleanup fixture unexpectedly has selected leaves");
    const auto delivery = AppServicesTestProbe::sink(*fixture.services);
    const auto release = std::make_shared<std::atomic<bool>>(false);
    ReleaseOnExit release_on_exit{release};
    const auto published = std::make_shared<std::atomic<bool>>(false);
    const auto operation_id = std::make_shared<std::string>();
    const std::weak_ptr<ApplicationState> state = fixture.state;
    fixture.state->set_sink([delivery, release, published, operation_id, state](nickel::ui::UiTopic topic, Json value) {
        if (topic == nickel::ui::UiTopic::Downloads && !published->load()) {
            const auto& rows = value.at("items");
            const auto found = std::find_if(rows.begin(), rows.end(), [](const Json& row) {
                return row.contains("operationId") && row.at("terminal").get<bool>() && row.at("status") == "completed";
            });
            if (found != rows.end()) {
                *operation_id = found->at("operationId").get<std::string>();
                published->store(true);
                while (!release->load()) {
                    const auto owner = state.lock();
                    if (!owner || owner->stopping.load()) break;
                    std::this_thread::sleep_for(1ms);
                }
            }
        }
        delivery(topic, std::move(value));
    });
    const auto request = ProductionFixture::next_id();
    fixture.services->submit(request, UiAction::CleanJunk,
        nickel::app::to_public_json(Json{{"previewId", preview.at("previewId")}}).toObject());
    wait_until([&] { return published->load(); });
    require(!fixture.commands.contains(request), "Cleanup reply was delivered before its publication barrier");
    if (with_file) require(!std::filesystem::exists(log), "Cleanup reported committed publication before removing its leaf");
    fixture.action_ok(UiAction::CancelDownload, {{"id", *operation_id}});
    require(accepted_download(fixture.query_ok(UiQuery::Downloads)).at("status") == "completed",
        "Late public cancellation relabelled durable cleanup as cancelled");
    release->store(true);
    wait_until([&] { return fixture.commands.contains(request); });
    const auto& reply = fixture.commands.at(request);
    require(!reply.error.isError(), "Late public cancellation overwrote the committed cleanup reply");
    const auto result = nickel::app::from_public_json(reply.data);
    nickel::app::test::require_cleanup_result(result);
    require(!result.at("cancelled").get<bool>() && !result.at("locationConflict").get<bool>(),
        "Late public cancellation changed canonical cleanup status fields");
    if (with_file) {
        require(result.at("removedCount").get<std::uint64_t>() >= 1
            && result.at("freedBytes").get<std::uint64_t>() >= content.size(),
            "Late cancellation discarded already removed bytes or leaves");
    } else require(result.at("removedCount") == 0 && result.at("freedBytes") == 0,
        "Empty cleanup fabricated committed results");
    require(fixture.state->tasks->snapshots().empty(), "Synchronous cleanup added an unrelated canonical Task");
    fixture.state->set_sink(delivery);
    std::cout << (with_file ? "cleanup committed publication / late public cancel / eight-field durable reply: PASS\n"
        : "empty cleanup publication / late public cancel / eight-field zero result: PASS\n");
}

void accepted_launch_cancelled_before_worker() {
    ProductionFixture fixture;
    fixture.finish_onboarding();
    auto scope = fixture.add_legacy_instance("pre-worker-launch-fixture");
    scope["operationId"] = "pre-worker-launch-operation";
    const auto release = std::make_shared<std::atomic<bool>>(false);
    ReleaseOnExit release_on_exit{release};
    const auto entered = std::make_shared<std::atomic<unsigned>>(0);
    for (unsigned index = 0; index < 2; ++index) {
        require(AppServicesTestProbe::submit_work(*fixture.services, [release, entered](const Cancellation& shutdown) {
            entered->fetch_add(1);
            while (!release->load() && !shutdown.cancelled()) std::this_thread::sleep_for(1ms);
        }), "Cannot reserve both real workers for launch admission");
    }
    wait_until([&] { return entered->load() == 2; });
    const auto request = ProductionFixture::next_id();
    fixture.services->submit(request, UiAction::LaunchGame, nickel::app::to_public_json(scope).toObject());
    const auto before = accepted_download(fixture.state->downloads_snapshot());
    require(before.at("id") == scope.at("operationId") && before.at("status") == "queued"
        && !before.contains("taskId"), "LaunchGame was not accepted independently before its request worker");
    fixture.services->cancelRequest(request);
    const auto cancel_request = ProductionFixture::next_id();
    fixture.services->submit(cancel_request, UiAction::CancelDownload,
        nickel::app::to_public_json(Json{{"id", scope.at("operationId")}}).toObject());
    require(accepted_download(fixture.state->downloads_snapshot()).at("status") == "cancelling",
        "Public early operation-ID cancellation did not reach the launch business token");
    release->store(true);
    wait_until([&] { return fixture.commands.contains(cancel_request); });
    require(!fixture.commands.at(cancel_request).error.isError(), "Early public launch cancellation failed");
    wait_until([&] { return accepted_download(fixture.state->downloads_snapshot()).at("terminal").get<bool>(); });
    const auto terminal = accepted_download(fixture.query_ok(UiQuery::Downloads));
    require(terminal.at("status") == "cancelled", "Cancelled pre-worker launch published the wrong terminal state");
    no_owned_launch(fixture);
    require(fixture.state->tasks->snapshots().empty(), "Pre-worker cancellation fabricated a preparation task");
    QTest::qWait(20);
    require(!fixture.commands.contains(request), "Revoked LaunchGame request delivered a stale reply");
    std::cout << "accepted LaunchGame / pre-worker public ID cancel / no task or owned game / revoked reply: PASS\n";
}

void launch_delivery_rejects_late_snapshots() {
    ProductionFixture fixture;
    const auto delivery = AppServicesTestProbe::sink(*fixture.services);
    require(static_cast<bool>(delivery), "Production service delivery is missing");
    std::vector<Json> captured;
    fixture.state->set_sink([&](nickel::ui::UiTopic topic, Json value) {
        require(topic == nickel::ui::UiTopic::Launch, "Unexpected publication in launch delivery fixture");
        (void)fixture.state->launch_snapshot();
        captured.push_back(std::move(value));
    });
    const auto scope = fixture.add_legacy_instance("launch-publication-fixture");
    const auto identity = fixture.state->identity(scope);
    const auto context = fixture.state->context(identity);
    const auto launch = std::make_shared<nickel::app::LaunchOperation>(identity, "publication-launch",
        fixture.game, context.resourceRoot);
    launch->preparing.store(false);
    const Json running{{"launchId", launch->launch_id}, {"state", "running"}, {"phase", "running"}};
    const Json finished{{"launchId", launch->launch_id}, {"state", "finished"}, {"phase", "finished"}};
    {
        std::lock_guard lock(fixture.state->state_mutex);
        launch->public_state = running;
        fixture.state->launches.emplace(launch->launch_id, launch);
        fixture.state->last_launch_id = launch->launch_id;
    }
    fixture.state->publish(nickel::ui::UiTopic::Launch, running);
    {
        std::lock_guard lock(fixture.state->state_mutex);
        launch->public_state = finished;
    }
    fixture.state->publish(nickel::ui::UiTopic::Launch, finished);
    fixture.state->publish(nickel::ui::UiTopic::Launch, running);
    require(captured.size() == 3 && captured.at(2).at("state") == "finished",
        "Late pre-terminal publication was not refreshed under the state lock");
    std::vector<Json> observed;
    QObject::connect(fixture.services.get(), &nickel::ui::UiServices::stateChanged, fixture.services.get(),
        [&](nickel::ui::UiTopic topic, const QJsonObject& value) {
            if (topic != nickel::ui::UiTopic::Launch) return;
            (void)fixture.state->launch_snapshot();
            observed.push_back(nickel::app::from_public_json(value));
        });
    delivery(nickel::ui::UiTopic::Launch, captured.at(2));
    delivery(nickel::ui::UiTopic::Launch, captured.at(0));
    delivery(nickel::ui::UiTopic::Launch, captured.at(1));
    const auto idle_delivery = [&] {
        std::optional<nickel::ui::UiReply> reply;
        const auto request = ProductionFixture::next_id();
        const auto connection = QObject::connect(fixture.services.get(), &nickel::ui::UiServices::queryReady,
            fixture.services.get(), [&](const nickel::ui::UiReply& value) { if (value.id == request) reply = value; });
        fixture.services->request(request, UiQuery::RuntimeInformation, {});
        wait_until([&] { return reply.has_value(); });
        QObject::disconnect(connection);
        require(!reply->error.isError(), "Launch delivery barrier failed");
    };
    idle_delivery();
    require(observed.size() == 1 && observed.front().at("state") == "finished",
        "Out-of-order delivery replayed a stale launch snapshot");
    require(!observed.front().contains(nickel::app::launch_delivery_ticket_key), "Internal launch ticket leaked to public JSON");
    observed.clear();
    captured.clear();
    fixture.state->publish(nickel::ui::UiTopic::Launch, finished);
    const Json idle{{"launchId", launch->launch_id}, {"state", "idle"}, {"phase", "finished"}};
    {
        std::lock_guard lock(fixture.state->state_mutex);
        launch->public_state = idle;
    }
    fixture.state->publish(nickel::ui::UiTopic::Launch, idle);
    delivery(nickel::ui::UiTopic::Launch, captured.at(0));
    delivery(nickel::ui::UiTopic::Launch, captured.at(1));
    idle_delivery();
    require(observed.size() == 1 && observed.front().at("state") == "idle",
        "Terminal publication overwrote a later cleared launch state");
    fixture.state->set_sink(delivery);
    {
        std::lock_guard lock(fixture.state->state_mutex);
        fixture.state->launches.erase(launch->launch_id);
        fixture.state->last_launch_id.clear();
    }
    std::cout << "real ServiceDelivery / monotonic late snapshots / terminal clear / reentrant sinks and signals: PASS\n";
}

void same_task_commit_cancel_and_shutdown() {
    ProductionFixture fixture;
    const auto operation = fixture.state->begin_operation({{"operationId", "commit-handoff-fixture"}});
    const auto target = fixture.root / "transactions" / "published.txt";
    std::filesystem::create_directories(target.parent_path());
    nickel::tasks::TaskSpec spec;
    spec.kind = nickel::tasks::TaskKind::Runtime;
    spec.target = target;
    spec.label = "Real transaction fixture";
    spec.network = fixture.state->http->snapshot();
    spec.cancellation = operation->cancellation;
    const auto task = fixture.state->tasks->begin(spec);
    require(!fixture.state->operation_task(Cancellation{}), "Unknown token returned a task");
    bool mismatch = false;
    try { fixture.state->register_operation_task(Cancellation{}, task); }
    catch (const std::exception& error) { mismatch = nickel::app::public_error(error).code == "integrity"; }
    require(mismatch, "Task registration accepted another stop-token identity");
    fixture.state->register_operation_task(task->cancellation(), task);
    require(fixture.state->operation_task(operation->cancellation) == task, "Accepted operation did not retain exact canonical task identity");
    fixture.state->start_operation(operation);
    fixture.state->finish_operation(operation, {{"taskId", task->id()}});
    const auto release = std::make_shared<std::atomic<bool>>(false);
    ReleaseOnExit release_on_exit{release};
    const auto entered = std::make_shared<std::atomic<bool>>(false);
    const auto failed = std::make_shared<std::atomic<bool>>(false);
    fixture.state->schedule_operation([task, target, release, entered, failed](const Cancellation&) {
        try {
            task->commit_external([&] {
                entered->store(true);
                while (!release->load()) std::this_thread::sleep_for(1ms);
                nickel::atomic_write_text(target, "committed fixture\n");
            });
        } catch (...) { failed->store(true); }
    }, task->cancellation(), task);
    wait_until([&] { return entered->load(); });
    require(fixture.action(UiAction::CancelDownload, {{"id", operation->operation_id}}).error.code == "busy",
        "Public cancellation did not report the canonical commitment refusal");
    require(!task->cancellation().cancelled(), "Public cancellation bypassed the task commitment gate");
    require(!fixture.state->cancel_operation(task->id()), "Canonical task cancelled across irreversible commitment");
    fixture.state->shutdown();
    require(!task->cancellation().cancelled(), "Shutdown raw-cancelled the committed task token");
    release->store(true);
    wait_until([&] { return task->snapshot().status == nickel::tasks::TaskStatus::Succeeded || failed->load(); });
    require(!failed->load() && std::filesystem::exists(target), "Committed real file was lost on shutdown");
    require(task->snapshot().status == nickel::tasks::TaskStatus::Succeeded, "Durable commitment was relabelled as cancelled");
    std::cout << "canonical task identity / public operation handoff / commitment-cancel-shutdown: PASS\n";
}

void same_root_logs_keep_private_secrets() {
    ProductionFixture fixture;
    const auto scope = fixture.add_legacy_instance();
    const auto context = fixture.state->context(fixture.state->identity(scope));
    const auto store = fixture.state->log_store_for_root(context.resourceRoot);
    require(store == fixture.state->log_store_for_root(context.resourceRoot), "Same root constructed a second secret state");
    const std::string secret = "fixture-private-secret-not-a-provider-token";
    nickel::diagnostics::OutputSession output(*store);
    output.add_secret(secret);
    output.bytes(nickel::diagnostics::ProcessStream::stdout_stream, "opaque value " + secret + "\n");
    output.eof(nickel::diagnostics::ProcessStream::stdout_stream);
    output.eof(nickel::diagnostics::ProcessStream::stderr_stream);
    require(output.finish(0ms), "Authoritative fixture EOF did not finish output");
    nickel::atomic_write_text(store->paths().latest, "raw fixture value " + secret + "\n");
    require(fixture.query_ok(UiQuery::Logs, scope).dump().find(secret) == std::string::npos,
        "Unscoped same-root Logs lost the launch's registered secret");
    const auto other = fixture.root / "other-log-root";
    std::filesystem::create_directories(other);
    const auto independent = fixture.state->log_store_for_root(other);
    require(independent->redact_report("raw fixture value " + secret).text.find(secret) != std::string::npos,
        "Different roots unexpectedly shared private launch secrets");
    std::cout << "same-root shared LaunchLogStore / registered opaque secret / isolated roots: PASS\n";
}

} // namespace

void run_production_lifecycle_tests() {
    first_use_requires_finish();
    startup_retry_and_persisted_resume();
    monitor_chain_keeps_its_slot();
    ordinary_saturation_and_monitor_shutdown();
    long_admission_rejection_has_one_reply(0);
    long_admission_rejection_has_one_reply(1);
    long_admission_rejection_has_one_reply(2);
    resource_authorization_keeps_queries_responsive();
    accepted_cleanup_outlives_request(false);
    accepted_cleanup_outlives_request(true);
    cleanup_late_public_cancel_preserves_result(false);
    cleanup_late_public_cancel_preserves_result(true);
    accepted_launch_cancelled_before_worker();
    launch_delivery_rejects_late_snapshots();
    same_task_commit_cancel_and_shutdown();
    same_root_logs_keep_private_secrets();
}
