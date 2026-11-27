#include "app/application_state.hpp"

#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"
#include "core/instances/domain_io.hpp"
#include "core/minecraft/maven.hpp"
#include "core/minecraft/runtime.hpp"
#include "core/minecraft/services_native.hpp"
#include "core/network/secure_memory.hpp"
#include "core/tasks/secure_fs.hpp"
#include "platform/memory.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stop_token>
#include <utility>

namespace nickel::app {
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

Json launch_target(const LaunchOperation& launch) {
    return {{"root", instances::path_to_utf8(launch.game_root)},
            {"versionId", launch.identity.instanceId}, {"launchId", launch.launch_id}};
}

void publish_launch_state(ApplicationState& state, const std::shared_ptr<LaunchOperation>& launch) {
    Json snapshot;
    {
        std::lock_guard lock(state.state_mutex);
        const auto found = state.launches.find(launch->launch_id);
        if (found == state.launches.end() || found->second != launch) return;
        snapshot = launch->public_state;
    }
    state.publish(ui::UiTopic::Launch, std::move(snapshot));
}

minecraft::Services owned_output_services(minecraft::Services services,
                                          const std::shared_ptr<diagnostics::OutputSession>& output,
                                          const std::shared_ptr<tasks::Task>& task) {
    const auto spawn = services.spawn;
    if (!spawn || !task) fail_static("unsupported");
    services.spawn = [spawn, output, task](const minecraft::ProcessSpec& specification,
                                          const minecraft::OutputCallback& previous) {
        if (specification.purpose != minecraft::ProcessKind::Game) return spawn(specification, previous);
        std::shared_ptr<minecraft::OwnedProcess> process;
        // Only real Game spawn crosses the task commit gate; all preparation stays cancellable.
        task->commit_external([&] {
            process = spawn(specification, [output](bool error, std::string_view bytes) {
                output->bytes(error ? diagnostics::ProcessStream::stderr_stream
                                    : diagnostics::ProcessStream::stdout_stream, bytes);
            });
            if (!process || !process->pid()) fail_static("internal");
        });
        return process;
    };
    const auto operation = services.forOperation;
    if (operation) {
        services.forOperation = [operation, output, task](const Cancellation& cancellation) {
            return owned_output_services(operation(cancellation), output, task);
        };
    }
    return services;
}

minecraft::InstalledVersion installed_version(const instances::ResolvedLaunchTarget& target,
                                              fs::path& client_jar, const Cancellation& cancellation) {
    cancellation.check();
    minecraft::validate_version_id(target.installed_version_id);
    if (!target.descriptor.is_object()
        || bounded_text(target.descriptor, "id", 512) != target.installed_version_id)
        fail_static("integrity");
    const auto metadata = minecraft::resolve_version(target.installed_version_id, target.install_root,
        [&](std::string_view id) {
            cancellation.check();
            if (id == target.installed_version_id) return target.descriptor;
            const auto path = minecraft::path_inside(target.install_root,
                "versions/" + std::string(id) + '/' + std::string(id) + ".json");
            return minecraft::parse_metadata(instances::detail::read_bounded(path, instances::descriptor_max_bytes),
                                             instances::descriptor_max_bytes);
        });
    client_jar = metadata.clientJar;
    instances::reject_links(client_jar);
    if (!fs::is_regular_file(client_jar)) fail_static("not_found");
    if (!instances::valid_minecraft_version(target.minecraft_version)) fail_static("integrity");
    if (instances::valid_minecraft_version(metadata.baseVersionId)
        && metadata.baseVersionId != target.minecraft_version) fail_static("integrity");
    minecraft::InstalledVersion version;
    version.versionId = metadata.versionId;
    version.minecraftVersion = target.minecraft_version;
    version.root = target.install_root;
    version.gameDirectory = target.resource_root;
    version.profilePath = target.descriptor_path;
    version.profile = metadata.metadata;
    const auto loader = target.entry.loader;
    if (loader != "vanilla" && loader != "legacy" && loader != "unknown" && !loader.empty()) {
        version.loaders.push_back({minecraft::parse_loader_kind(loader),
            target.entry.loader_version.value_or(target.profile ? target.profile->loader_version : std::string{})});
    }
    cancellation.check();
    return version;
}

void require_account_policy(const accounts::LaunchIdentity& identity,
                            const instances::EffectiveSettings& settings) {
    const bool offline = identity.type == accounts::AccountType::offline;
    const bool microsoft = identity.type == accounts::AccountType::microsoft;
    if ((settings.server_login == "offline" && !offline)
        || (settings.server_login == "microsoft" && !microsoft)
        || (settings.server_login == "thirdParty" && (offline || microsoft)))
        fail_static("authentication_required");
    if (!settings.auth_server.empty() && !offline && !microsoft) {
        const auto expected = accounts::yggdrasil_base(accounts::validate_server(settings.auth_server));
        if (!identity.yggdrasil_api || *identity.yggdrasil_api != expected)
            fail_static("authentication_required");
    }
}

void prepare_identity_runtime(ApplicationState& state, const accounts::LaunchIdentity& identity,
                              const fs::path& client_jar, std::string_view minecraft_version,
                              const instances::EffectiveSettings& settings,
                              const fs::path& runtime, const Cancellation& cancellation) {
    cancellation.check();
    media::OfflineSkinRequest request;
    request.runtime_directory = runtime;
    request.client_jar = client_jar;
    request.minecraft_version = minecraft_version;
    if (identity.type == accounts::AccountType::offline) {
        request.mode = media::parse_offline_skin_mode(settings.offline_skin);
        request.name = settings.offline_skin_name;
        if (!settings.offline_skin_path.empty()) request.path = instances::path_from_utf8(settings.offline_skin_path);
        request.slim = settings.offline_skin_slim;
    }
    // Random mode also disables our exact pack option identifiers for online identities.
    (void)state.offline_skins.prepare(request, cancellation);
    cancellation.check();
}

void require_launch_onboarding(const instances::OnboardingGate& gate) {
    gate.require_finished();
}

bool schedule_launch_monitor(ApplicationState& state, const std::shared_ptr<LaunchOperation>& launch) {
    const auto weak = state.weak_from_this();
    return state.schedule_monitor(100ms, [weak, launch](const Cancellation& shutdown) {
        if (const auto owner = weak.lock()) owner->poll_game(launch, shutdown);
    });
}

void retain_game_state(ApplicationState& state, const std::shared_ptr<LaunchOperation>& launch,
                       const std::shared_ptr<minecraft::GameSession>& game, std::uint64_t pid) {
    std::lock_guard lock(state.state_mutex);
    const auto found = state.launches.find(launch->launch_id);
    if (found == state.launches.end() || found->second != launch) return;
    if (game) launch->game = game;
    launch->preparing.store(false);
    // The monitor can reach real EOF before the started callback or preparation handler returns.
    const auto phase = launch->public_state.value("phase", std::string{});
    if (phase == "finished") return;
    launch->public_state.update({{"state", "running"}, {"phase", phase == "draining" ? "draining" : "running"},
                                {"canCancel", false}});
    if (pid) {
        launch->public_state["pid"] = pid;
        launch->public_state["gameSpawned"] = true;
    }
}

void freeze_launch_failure(ApplicationState& state, const std::shared_ptr<LaunchOperation>& launch,
                           std::string_view code, std::optional<int> exit_code) {
    std::shared_ptr<diagnostics::OutputSession> output;
    std::shared_ptr<diagnostics::LaunchLogStore> store;
    diagnostics::DiagnosticInput input;
    {
        std::lock_guard lock(state.state_mutex);
        const auto found = state.launches.find(launch->launch_id);
        if (found == state.launches.end() || found->second != launch
            || launch->public_state.value("phase", "") == "finished") return;
        output = launch->output;
        store = launch->log_store;
        input = launch->diagnostic_context;
    }
    // An exit code is supplied only after GameSession's authoritative dual-EOF wait.
    const bool output_complete = exit_code.has_value() && output && output->finished();
    if (output && !output->finished()) {
        output->producer_disappeared();
        (void)output->finish(0ms);
    }
    input.exit_code = exit_code;
    if (output) input.stdout_tail = output->capture_text();
    std::optional<diagnostics::ErrorReportSnapshot> snapshot;
    std::optional<diagnostics::SafeReport> excerpt;
    auto diagnostic = diagnostics::analyze(input);
    try {
        if (!store || (output && !output->safe())) fail_static("integrity");
        snapshot = diagnostics::capture_error_report(
            {launch->game_root, launch->identity.instanceId, launch->launch_id}, *store, input);
        diagnostic = snapshot->diagnosis();
        try { excerpt = diagnostics::safe_report(*store, input); } catch (const std::exception&) {}
    } catch (const std::exception&) {
        // Unsafe, unavailable or over-quota sources never become complete ready reports.
    }
    {
        std::lock_guard lock(state.state_mutex);
        const auto found = state.launches.find(launch->launch_id);
        if (found == state.launches.end() || found->second != launch
            || launch->public_state.value("phase", "") == "finished") return;
        launch->failure_snapshot = std::move(snapshot);
        launch->failure_report = std::move(excerpt);
        launch->public_state.update({{"state", "failed"}, {"phase", "finished"}, {"canCancel", false},
            {"exitCode", exit_code ? Json(*exit_code) : Json(nullptr)},
            {"error", {{"code", public_error(code).code.toStdString()}, {"message", safe_public_message(code)}}},
            {"diagnostic", diagnostics::diagnostic_brief_to_json(diagnostic)},
            {"reportTarget", launch_target(*launch)}, {"outputComplete", output_complete},
            {"rootExited", exit_code.has_value()}, {"reportReady", launch->failure_snapshot.has_value()}});
        launch->preparing.store(false);
        // A pre-spawn failure retains its slot until the already-admitted monitor acknowledges it.
        if (exit_code) launch->lease.reset();
    }
    publish_launch_state(state, launch);
    state.publish(ui::UiTopic::Logs, {{"target", launch_target(*launch)}, {"finished", true}});
}

void retain_incomplete_drain(ApplicationState& state, const std::shared_ptr<LaunchOperation>& launch) {
    bool changed;
    {
        std::lock_guard lock(state.state_mutex);
        const auto found = state.launches.find(launch->launch_id);
        if (found == state.launches.end() || found->second != launch
            || launch->public_state.value("phase", "") == "finished") return;
        changed = launch->public_state.value("phase", "") != "draining"
            || launch->public_state.value("state", "") != "running";
        launch->preparing.store(false);
        launch->public_state.update({{"state", "running"}, {"phase", "draining"}, {"canCancel", false},
                                    {"exitCode", nullptr}, {"reportReady", false},
                                    {"rootExited", true}, {"outputComplete", false}});
    }
    // Budget expiry is not EOF or unsafe output; retain the sole output owner and resource lease.
    if (changed) publish_launch_state(state, launch);
}


} // namespace

OperationResult ApplicationState::action_launch(ui::UiAction action, const Json& args,
                                                const Cancellation& cancellation) {
    cancellation.check();
    if (action == ui::UiAction::CancelLaunch) {
        const auto id = bounded_text(args, "launchId", 128);
        minecraft::validate_launch_id(id);
        std::shared_ptr<LaunchOperation> launch;
        std::shared_ptr<minecraft::Launcher> launcher;
        std::shared_ptr<tasks::Task> task;
        {
            std::lock_guard lock(state_mutex);
            const auto found = launches.find(id);
            if (found == launches.end()) fail_static("unauthorized");
            launch = found->second;
            if (!launch->preparing.load() || launch->public_state.value("state", "") == "running")
                return {Json{{"launchId", id}, {"cancelled", false}}, {}, {}};
            launcher = launch->launcher;
            task = launch->preparation_task;
        }
        // Canonical cancellation orders early assignment against spawn and invokes callbacks outside state_mutex.
        if (!cancel_background(launch->cancellation, task))
            return {Json{{"launchId", id}, {"cancelled", false}}, {}, {}};
        if (launcher) (void)launcher->cancel_prepare(id);
        {
            std::lock_guard lock(state_mutex);
            if (launch->preparing.load() && launch->public_state.value("state", "") != "running") {
                launch->public_state["phase"] = "cancelling";
                launch->public_state["canCancel"] = false;
            }
        }
        publish_launch_state(*this, launch);
        return {Json{{"launchId", id}, {"cancelled", true}}, {}, {}};
    }
    if (action != ui::UiAction::LaunchGame) fail_static("invalid_argument");
    require_launch_onboarding(onboarding);
    const auto root = registered_root(bounded_text(args, "root", 32768));
    const auto outer_id = bounded_text(args, "versionId", 512);
    instances::validate_name(outer_id);
    const resources::InstanceIdentity selected{instances::root_identity(instances::path_to_utf8(root)), outer_id};
    auto authority = std::make_shared<MutationGate::Lease>(mutations.instance(selected.rootId, outer_id, cancellation));
    if (running(selected)) fail_static("busy");
    const auto memory = physical_memory();
    const auto target = instances::resolve_launch_target(root, outer_id, settings("game"),
        instances::MemoryContext{memory.total_mb, memory.available_mb});
    const auto supplied = bounded_text(args, "launchId", 128, false);
    const auto id = supplied.empty() ? "launch-" + instances::detail::random_id() : supplied;
    minecraft::validate_launch_id(id);
    auto launch = std::make_shared<LaunchOperation>(selected, id, target.request.game_root, target.resource_root);
    launch->cancellation = cancellation;
    launch->lease = std::move(authority);
    launch->diagnostic_context.minecraft_version = target.minecraft_version;
    launch->diagnostic_context.loader = target.entry.loader;
    if (target.entry.loader == "optifine") launch->diagnostic_context.optifine = target.entry.loader_version;
    launch->public_state = launch_target(*launch);
    launch->public_state.update({{"state", "preparing"}, {"phase", "preparing"}, {"canCancel", true},
        {"minecraftVersion", target.minecraft_version}, {"pid", nullptr}, {"exitCode", nullptr},
        {"error", nullptr}, {"reportTarget", nullptr}, {"diagnostic", nullptr}, {"reportReady", false},
        {"launchGeneration", "generation-" + instances::detail::random_id()},
        {"launcherVisibility", target.settings.visibility}, {"gameSpawned", false},
        {"gameWindowSeen", false}, {"rootExited", false}, {"outputComplete", false}});
    const auto account_id = accounts.active_id();
    if (account_id.empty()) fail_static("authentication_required");
    const transport::detail::Erased<transport::NetworkSnapshot> network(http->snapshot());
    std::optional<accounts::MsaConfig> configuration;
    std::vector<std::shared_ptr<LaunchOperation>> retired;
    std::string previous_last_launch;
    {
        std::lock_guard lock(state_mutex);
        if (stopping.load()) fail_static("cancelled");
        if (launches.contains(id)) fail_static("invalid_argument");
        for (auto it = launches.begin(); it != launches.end();) {
            const auto& prior = it->second;
            const bool cleared = prior->public_state.value("state", "") != "failed"
                || prior->public_state.value("error", Json(nullptr)).is_null();
            if (!prior->preparing.load() && !prior->draining.load() && !prior->lease
                && prior->public_state.value("phase", "") == "finished" && cleared
                && !prior->failure_report && !prior->failure_snapshot) {
                retired.push_back(std::move(it->second));
                it = launches.erase(it);
            } else ++it;
        }
        if (launches.size() >= 16) fail_static("busy");
        for (const auto& [previous_id, previous] : launches) {
            if (previous->preparing.load()) fail_static("busy");
            if (previous->public_state.value("phase", "") != "finished"
                && tasks::detail::paths_overlap(previous->resource_root, launch->resource_root))
                fail_static("busy");
        }
        configuration = microsoft;
        previous_last_launch = last_launch_id;
        launches.emplace(id, launch);
        last_launch_id = id;
    }
    retired.clear();
    const auto state = shared_from_this();
    std::shared_ptr<tasks::Task> task;
    bool monitor_admitted = false;
    Json accepted;
    try {
        task = operation_task(launch->cancellation);
        if (!task) {
            tasks::TaskSpec task_spec;
            transport::detail::EraseGuard network_guard(task_spec.network);
            task_spec.kind = tasks::TaskKind::Runtime;
            task_spec.target = target.install_root;
            if (target.resource_root != target.install_root) task_spec.additional_targets.push_back(target.resource_root);
            task_spec.label = "Prepare Minecraft";
            task_spec.network = network.get();
            task_spec.cancellation = launch->cancellation;
            task = tasks->begin(task_spec);
        }
        {
            std::lock_guard lock(state_mutex);
            launch->preparation_task = task;
        }
        register_operation_task(launch->cancellation, task);
        {
            std::lock_guard lock(state_mutex);
            launch->public_state["preparationTaskId"] = task->id();
            launch->public_state["taskId"] = task->id();
            accepted = launch->public_state;
        }
        // Reserve durable monitor admission before any queued handler can spawn a Game.
        monitor_admitted = schedule_launch_monitor(*this, launch);
        if (!monitor_admitted) fail_static("busy");
        auto prepare = [state, launch, task, target, account_id, configuration, memory]
                       (const Cancellation& shutdown) {
            try {
                launch->cancellation.check();
                shutdown.check();
                if (state->stopping.load()) fail_static("cancelled");
                require_launch_onboarding(state->onboarding);
                auto store = state->log_store_for_root(launch->resource_root);
                {
                    std::lock_guard lock(state->state_mutex);
                    launch->log_store = store;
                }
                minecraft::NativeServiceOptions service_options;
                service_options.task = task;
                auto services = minecraft::make_native_services(state->http, service_options);
                minecraft::LaunchRequest request;
                transport::detail::EraseGuard token_guard(request.identity.accessToken);
                request.launchId = launch->launch_id;
                fs::path client_jar;
                request.version = installed_version(target, client_jar, launch->cancellation);
                request.settings = minecraft::launch_settings_from_json(effective_settings_json(target.settings));
                request.settings.physicalMemoryMb = memory.total_mb;
                const auto identity = state->auth.prepare_launch(account_id, configuration, launch->cancellation);
                require_account_policy(identity, target.settings);
                request.identity.type = std::string(accounts::account_type_name(identity.type));
                request.identity.name = identity.name;
                request.identity.uuid = identity.uuid;
                request.identity.userType = identity.user_type;
                request.identity.accessToken = identity.access_token.view();
                request.identity.xuid = identity.xuid;
                request.identity.yggdrasilApi = identity.yggdrasil_api.value_or("");
                if (identity.type == accounts::AccountType::microsoft && configuration)
                    request.identity.clientId = configuration->client_id;
                request.settings.bypassOffline = identity.type == accounts::AccountType::offline;
                const auto check = minecraft::cancellation_check(services, launch->cancellation);
                std::optional<unsigned> metadata_major;
                const auto java_version = request.version.profile.find("javaVersion");
                if (java_version != request.version.profile.end() && java_version->is_object()) {
                    const auto major = java_version->find("majorVersion");
                    if (major != java_version->end() && major->is_number_unsigned()) metadata_major = major->get<unsigned>();
                }
                const auto requirement = minecraft::loader_java_requirement(target.minecraft_version,
                    request.version.loaders, request.version.profile.value("releaseTime", std::string{}), metadata_major);
                fs::path java;
                if (!target.settings.java_path.empty()) java = instances::path_from_utf8(target.settings.java_path);
                else {
                    if (!services.discoverJava) fail_static("unsupported");
                    request.javaCandidates = services.discoverJava(check);
                    const auto selected_java = minecraft::select_java(requirement, request.javaCandidates);
                    if (!selected_java.path) fail_static("not_found");
                    java = *selected_java.path;
                }
                request.java = minecraft::probe_java(services, java, check);
                request.version.java = minecraft::validate_manual_java(requirement, *request.java);
                {
                    std::lock_guard lock(state->state_mutex);
                    launch->diagnostic_context.java_major = request.java->major;
                    launch->diagnostic_context.java_vendor = request.java->vendor;
                }
                task->stage("offline-skin");
                prepare_identity_runtime(*state, identity, client_jar, target.minecraft_version,
                    target.settings, request.version.gameDirectory, launch->cancellation);
                const std::weak_ptr<ApplicationState> weak_state = state;
                const auto scope = launch_target(*launch);
                auto output = std::make_shared<diagnostics::OutputSession>(*store,
                    [weak_state, scope](const diagnostics::SafeOutputEvent& event) {
                        if (const auto owner = weak_state.lock()) {
                            auto value = scope;
                            value.update({{"stream", event.stream == diagnostics::ProcessStream::stderr_stream ? "stderr" : "stdout"},
                                          {"line", event.line}});
                            owner->publish(ui::UiTopic::Logs, std::move(value));
                        }
                    });
                if (!identity.access_token.empty() && identity.access_token.view() != "0") output->add_secret(identity.access_token.view());
                if (identity.client_token && !identity.client_token->empty()) output->add_secret(identity.client_token->view());
                if (!task->network().proxy.password.empty()) output->add_secret(task->network().proxy.password);
                services = owned_output_services(std::move(services), output, task);
                auto launcher = std::make_shared<minecraft::Launcher>(std::move(services));
                {
                    std::lock_guard lock(state->state_mutex);
                    launch->output = output;
                    launch->launcher = launcher;
                }
                task->stage("launch");
                const std::weak_ptr<LaunchOperation> weak_launch = launch;
                const auto events = [weak_state, weak_launch, output, id = launch->launch_id](const minecraft::LaunchEvent& event) {
                    if (event.launchId != id) return;
                    if (event.kind == "stdout-eof" || event.kind == "stderr-eof") {
                        output->eof(event.kind == "stderr-eof" ? diagnostics::ProcessStream::stderr_stream
                                                              : diagnostics::ProcessStream::stdout_stream);
                        return;
                    }
                    if (event.kind == "stdout" || event.kind == "stderr" || event.kind == "exited"
                        || event.kind == "failed" || event.kind == "cancelled") return;
                    const auto owner = weak_state.lock();
                    const auto current = weak_launch.lock();
                    if (!owner || !current) return;
                    if (event.kind == "started") retain_game_state(*owner, current, {}, event.pid);
                    else {
                        std::lock_guard lock(owner->state_mutex);
                        if (current->public_state.value("phase", "") != "finished")
                            current->public_state["message"] = diagnostics::bounded_safe_line(event.message);
                    }
                    publish_launch_state(*owner, current);
                };
                const auto game = launcher->launch(request, launch->cancellation, events);
                retain_game_state(*state, launch, game, game->pid());
                state->publish(ui::UiTopic::Downloads, state->downloads_snapshot());
                publish_launch_state(*state, launch);
            } catch (const std::exception& error) {
                const auto failure = public_error(error).code.toStdString();
                std::shared_ptr<minecraft::GameSession> game;
                std::shared_ptr<minecraft::Launcher> launcher;
                {
                    std::lock_guard lock(state->state_mutex);
                    game = launch->game;
                    launcher = launch->launcher;
                }
                if (!game && launcher) game = launcher->session(launch->launch_id);
                const bool committed = game || task->snapshot().status == tasks::TaskStatus::Succeeded;
                if (!committed) {
                    try { task->rollback(failure, safe_public_message(failure)); } catch (const std::exception&) {}
                }
                if (committed) {
                    retain_game_state(*state, launch, game, game ? game->pid() : 0);
                    if (!game) {
                        std::lock_guard lock(state->state_mutex);
                        launch->public_state["monitorError"] = safe_public_message("internal");
                    }
                    publish_launch_state(*state, launch);
                } else if (launch->cancellation.cancelled() || shutdown.cancelled() || state->stopping.load()) {
                    std::shared_ptr<diagnostics::OutputSession> output;
                    {
                        std::lock_guard lock(state->state_mutex);
                        output = launch->output;
                    }
                    if (output) {
                        output->producer_disappeared();
                        (void)output->finish(0ms);
                    }
                    {
                        std::lock_guard lock(state->state_mutex);
                        launch->preparing.store(false);
                        launch->public_state.update({{"state", "cancelled"}, {"phase", "finished"}, {"canCancel", false}});
                    }
                    publish_launch_state(*state, launch);
                } else freeze_launch_failure(*state, launch, failure, {});
                state->publish(ui::UiTopic::Downloads, state->downloads_snapshot());
            }
        };
        schedule_operation(std::move(prepare), launch->cancellation, task);
    } catch (...) {
        if (task) {
            try { (void)task->cancel(); } catch (...) {}
            try { task->rollback("cancelled", safe_public_message("cancelled")); } catch (...) {}
        } else (void)cancel_background(launch->cancellation);
        std::shared_ptr<LaunchOperation> rejected;
        {
            std::lock_guard lock(state_mutex);
            launch->preparing.store(false);
            launch->public_state.update({{"state", "cancelled"}, {"phase", "finished"}, {"canCancel", false}});
            const auto found = launches.find(id);
            if (found != launches.end() && found->second == launch && !monitor_admitted) {
                launch->lease.reset();
                rejected = std::move(found->second);
                launches.erase(found);
            }
            // Retain rejected admitted monitors until their callback releases the slot; no stale slot can starve a successor.
            if (last_launch_id == id) last_launch_id = previous_last_launch;
        }
        throw;
    }
    publish_launch_state(*this, launch);
    return {accepted, {}, {}};
}

void ApplicationState::poll_game(const std::shared_ptr<LaunchOperation>& launch, const Cancellation& shutdown) {
    if (stopping.load() || shutdown.cancelled()) return;
    std::shared_ptr<minecraft::GameSession> game;
    std::shared_ptr<minecraft::Launcher> launcher;
    std::shared_ptr<diagnostics::OutputSession> output;
    bool retry_drain;
    {
        std::lock_guard lock(state_mutex);
        const auto found = launches.find(launch->launch_id);
        if (found == launches.end() || found->second != launch) return;
        if (launch->public_state.value("phase", "") == "finished") {
            launch->lease.reset();
            return;
        }
        if (launch->draining.exchange(true)) return;
        game = launch->game;
        launcher = launch->launcher;
        output = launch->output;
        retry_drain = launch->public_state.value("phase", "") == "draining";
    }
    {
        struct Release {
            std::shared_ptr<LaunchOperation> launch;
            ~Release() { launch->draining.store(false); }
        } release{launch};
        try {
            if (!game && launcher) {
                game = launcher->session(launch->launch_id);
                if (game) retain_game_state(*this, launch, game, game->pid());
            }
            if (game && !game->running()) {
                retain_incomplete_drain(*this, launch);
                std::optional<int> code;
                // Only the first observed exit spends 10s; subsequent durable polls stay nonblocking.
                try { code = game->wait(retry_drain ? 0ms : 10s); }
                catch (const std::exception&) {}
                if (code) {
                    const bool safe = output && output->finish(0ms);
                    if (*code != 0 || !safe) {
                        freeze_launch_failure(*this, launch, safe ? "internal" : "integrity", *code);
                    } else {
                        {
                            std::lock_guard lock(state_mutex);
                            launch->preparing.store(false);
                            launch->lease.reset();
                            launch->public_state.update({{"state", "exited"}, {"phase", "finished"}, {"exitCode", *code},
                                                        {"canCancel", false}, {"reportReady", false}});
                        }
                        publish_launch_state(*this, launch);
                        publish(ui::UiTopic::Logs, {{"target", launch_target(*launch)}, {"finished", true}});
                    }
                }
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(state_mutex);
            launch->public_state["monitorError"] = safe_public_message(public_error(error).code.toStdString());
        }
    }
    {
        std::lock_guard lock(state_mutex);
        const auto found = launches.find(launch->launch_id);
        if (found == launches.end() || found->second != launch) return;
        if (launch->public_state.value("phase", "") == "finished") {
            launch->lease.reset();
            return;
        }
    }
    if (!schedule_launch_monitor(*this, launch) && !stopping.load() && !shutdown.cancelled()) {
        std::lock_guard lock(state_mutex);
        launch->public_state["monitorError"] = safe_public_message("busy");
    }
}

} // namespace nickel::app
