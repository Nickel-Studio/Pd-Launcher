#include "app/application_state.hpp"
#include "app/WorkerQueue.hpp"
#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"
#include "core/resources/utility.hpp"
#include "core/tasks/secure_fs.hpp"
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
#include "core/tasks/test_hooks.hpp"
#endif

#include <QCoreApplication>
#include <QEventLoop>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace nickel::app {
struct AppServicesTestProbe {
    static std::shared_ptr<ApplicationState> state(AppServices& services) { return services.state_; }
};
} // namespace nickel::app

namespace {
namespace fs = std::filesystem;
using nickel::Cancellation;
using nickel::ErrorException;
using nickel::Json;
using nickel::app::ApplicationState;
using nickel::app::OperationResult;
using nickel::ui::UiAction;
using nickel::ui::UiQuery;
using nickel::ui::UiTopic;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class Work>
std::string failure_code(Work&& work) {
    try { work(); }
    catch (const ErrorException& failure) { return failure.error().code; }
    throw std::runtime_error("Operation unexpectedly succeeded");
}

std::string read(const fs::path& path) {
    const auto bytes = nickel::tasks::detail::read_bounded(path, 1024 * 1024);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void write(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    auto file = nickel::tasks::detail::SecureFile::create_new(path);
    file.write(std::as_bytes(std::span(value.data(), value.size())));
    file.flush();
}

template<class Ready>
bool pump_until(Ready&& ready, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return ready();
}

struct Fixture {
    fs::path directory;
    fs::path game;
    fs::path instance;
    nickel::tasks::detail::FileIdentity directory_identity;
    std::shared_ptr<ApplicationState> state;
    std::unique_ptr<nickel::app::AppServices> services;
    QObject callback_context;
    std::deque<ApplicationState::BackgroundWork> dispatches;
    std::vector<std::pair<UiTopic, Json>> events;
    std::map<nickel::ui::RequestId, nickel::ui::UiReply> query_replies;
    std::map<nickel::ui::RequestId, nickel::ui::UiReply> command_replies;
    Json scope;

    explicit Fixture(bool production_dispatch = false) {
        directory = fs::current_path() / (".resource-actions-" + nickel::tasks::detail::unique_id());
        nickel::tasks::detail::create_directory_new(directory);
        directory_identity = nickel::tasks::detail::directory_identity(directory);
        const auto executable = directory / "bin" / "launcher.exe";
        write(executable, "fixture-owned executable location; never executed");
        nickel::app::AppServicesOptions options;
        options.executable = executable;
        options.paths.executable = executable;
        options.paths.system_config_base = directory / "config";
        options.paths.app_data_base = directory / "data";
        if (production_dispatch) {
            services = std::make_unique<nickel::app::AppServices>(std::move(options));
            state = nickel::app::AppServicesTestProbe::state(*services);
        } else state = std::make_shared<ApplicationState>(std::move(options));
        game = state->roots.ensure_default();
        instance = game / "versions" / "fixture";
        fs::create_directories(instance);
        write(instance / "fixture.json", Json{{"id", "1.20.1"}, {"type", "release"},
            {"mainClass", "net.minecraft.client.main.Main"}, {"libraries", Json::array()},
            {"arguments", {{"game", Json::array()}, {"jvm", Json::array()}}},
            {"nickelCustom", {{"name", "Fixture display name, not its directory ID"}}}}.dump());
        scope = {{"root", nickel::instances::path_to_utf8(game)}, {"versionId", "fixture"}};
        const auto context = state->context(state->identity(scope));
        for (const auto& path : {context.resourceRoot, context.modsRoot, context.resourcepacksRoot,
                                context.shaderpacksRoot, context.datapacksRoot}) fs::create_directories(path);
        state->onboarding.accept_eula(true, nickel::instances::eula_version, 1700000000);
        state->onboarding.acknowledge_changelog(nickel::app::build_version());
        if (services) {
            QObject::connect(services.get(), &nickel::ui::UiServices::queryReady, &callback_context,
                [this](const nickel::ui::UiReply& reply) { query_replies.insert_or_assign(reply.id, reply); });
            QObject::connect(services.get(), &nickel::ui::UiServices::commandFinished, &callback_context,
                [this](const nickel::ui::UiReply& reply) { command_replies.insert_or_assign(reply.id, reply); });
            QObject::connect(services.get(), &nickel::ui::UiServices::stateChanged, &callback_context,
                [this](UiTopic topic, const QJsonObject& snapshot) {
                    events.emplace_back(topic, nickel::app::from_public_json(snapshot));
                });
        } else {
            state->set_scheduler([this](std::chrono::milliseconds delay, ApplicationState::BackgroundWork work) {
                require(delay == std::chrono::milliseconds(0), "Long work must use the canonical zero-delay drainer");
                dispatches.push_back(std::move(work));
                return true;
            });
            state->set_sink([this](UiTopic topic, Json value) { events.emplace_back(topic, std::move(value)); });
        }
    }

    ~Fixture() {
        if (services) {
            QObject::disconnect(services.get(), nullptr, &callback_context, nullptr);
            services.reset();
        } else if (state) {
            state->set_sink({});
            state->set_scheduler({});
            state->shutdown();
        }
        dispatches.clear();
        state.reset();
        try { nickel::tasks::detail::remove_owned_tree(directory, directory_identity); }
        catch (...) {}
    }

    nickel::resources::InstanceContext context() const { return state->context(state->identity(scope)); }

    Json args(Json fields = Json::object()) const {
        auto value = scope;
        value.update(fields);
        return value;
    }

    OperationResult action(UiAction selected, const Json& fields = Json::object(), const Cancellation& cancel = {}) {
        return state->action_resources(selected, args(fields), cancel);
    }

    OperationResult dispatch_action(nickel::ui::RequestId id, UiAction selected, const Json& fields = Json::object()) {
        require(bool(services), "Public dispatch requires the actual production AppServices fixture");
        services->submit(id, selected, nickel::app::to_public_json(args(fields)).toObject());
        require(pump_until([&] { return command_replies.contains(id); }, std::chrono::seconds(2)),
                "Production dispatch must return its command receipt without waiting for long work");
        const auto& reply = command_replies.at(id);
        require(!reply.error.isError(), "Production command receipt must not contain an error");
        OperationResult result;
        result.data = nickel::app::from_public_json(reply.data);
        return result;
    }

    void drain() {
        while (!dispatches.empty()) {
            auto work = std::move(dispatches.front());
            dispatches.pop_front();
            work(Cancellation{});
        }
    }

#ifdef NICKEL_TRANSPORT_TEST_HOOKS
    void drain_on_worker(std::function<void(std::string_view, const fs::path&)> checkpoint) {
        const auto caller = std::this_thread::get_id();
        while (!dispatches.empty()) {
            auto work = std::move(dispatches.front());
            dispatches.pop_front();
            auto completed = std::make_shared<std::promise<void>>();
            auto future = completed->get_future();
            auto workers = std::make_unique<nickel::app::WorkerQueue>(2, 16);
            require(workers->submit([work = std::move(work), checkpoint, completed, caller](const Cancellation& cancel) mutable {
                try {
                    require(std::this_thread::get_id() != caller, "Fault checkpoints must execute on the actual business worker");
                    struct RestoreCheckpoint {
                        std::function<void(std::string_view, const fs::path&)> previous;
                        ~RestoreCheckpoint() { nickel::tasks::detail::testing::checkpoint = std::move(previous); }
                    } restore{std::move(nickel::tasks::detail::testing::checkpoint)};
                    nickel::tasks::detail::testing::checkpoint = std::move(checkpoint);
                    work(cancel);
                    completed->set_value();
                } catch (...) { completed->set_exception(std::current_exception()); }
            }), "The production WorkerQueue must accept the real operation drainer");
            require(future.wait_for(std::chrono::seconds(30)) == std::future_status::ready,
                    "Worker fault regression must finish within its bounded deadline");
            workers.reset();
            future.get();
        }
    }
#endif

    nickel::tasks::Snapshot task(const OperationResult& result) const {
        const auto id = result.data.at("taskId").get<std::string>();
        const auto snapshots = state->tasks->snapshots();
        const auto found = std::find_if(snapshots.begin(), snapshots.end(), [&](const auto& item) { return item.id == id; });
        require(found != snapshots.end(), "Returned task ID must be in the real registry");
        return *found;
    }

    void succeeded(const OperationResult& result) {
        require(result.data.at("queued") == true, "Initial reply must be explicitly queued");
        if (services) {
            require(pump_until([&] {
                const auto status = task(result).status;
                return status == nickel::tasks::TaskStatus::Succeeded || status == nickel::tasks::TaskStatus::Cancelled
                    || status == nickel::tasks::TaskStatus::Failed;
            }, std::chrono::seconds(15)), "Production task must reach a terminal state within the bounded fixture deadline");
        } else drain();
        const auto snapshot = task(result);
        require(snapshot.status == nickel::tasks::TaskStatus::Succeeded,
                "Production operation must finish with durable success; actual=" + snapshot.error_code + ": " + snapshot.message);
    }

    void jar(const fs::path& target) {
        const std::vector<nickel::packs::ArchiveInput> contents{
            {"fabric.mod.json", {}, R"({"schemaVersion":1,"id":"fixturemod","version":"1.0","name":"Fixture Mod"})"},
            {"META-INF/MANIFEST.MF", {}, "Manifest-Version: 1.0\n"},
            {"fixture.txt", {}, "production archive fixture"}};
        state->archive->write(target, contents, {}, {});
        state->archive->release(target);
    }

    fs::path mod(std::string_view name = "fixture.jar") {
        const auto target = context().modsRoot / nickel::resources::pathFromUtf8(std::string(name));
        jar(target);
        return target;
    }

    fs::path save(std::string_view name) {
        const auto parent = directory / "exports";
        fs::create_directories(parent);
        return parent / nickel::resources::pathFromUtf8(std::string(name));
    }
};

void wrong_scope_matrix() {
    Fixture fixture;
    const std::array<UiAction, 11> actions{UiAction::InstallProject, UiAction::DownloadProjectFile,
        UiAction::ImportResources, UiAction::DeleteResource, UiAction::SetModEnabled, UiAction::RenameMod,
        UiAction::DeleteMods, UiAction::CheckModUpdates, UiAction::UpdateMods, UiAction::ExportMods, UiAction::ExportPack};
    fixture.mod();
    const auto destination = fixture.save("wrong.zip");
    auto value = fixture.args({{"provider", "modrinth"}, {"kind", "mod"}, {"projectId", "fixture1"},
        {"projectVersionId", "version1"}, {"path", nickel::resources::utf8Path(destination)},
        {"paths", Json::array()}, {"ids", Json::array({"fixture.jar"})}, {"enabled", true},
        {"id", "fixture.jar"}, {"name", "renamed.jar"}, {"version", "1"},
        {"format", "mrpack"}, {"files", Json::array()}});
    value["root"] = nickel::resources::utf8Path(fixture.directory / "unregistered");
    for (const auto selected : actions) {
        const auto code = failure_code([&] { fixture.state->action_resources(selected, value, {}); });
        require(code == "unauthorized" || code == "not_found" || code == "invalid_argument" || code == "policy_denied",
                "Wrong root must fail before production work starts");
        require(fixture.dispatches.empty(), "Wrong scope must never enter the operation scheduler");
        require(fixture.state->tasks->snapshots().empty(), "Wrong scope must never register a task");
    }
    for (const auto& root : std::array<fs::path, 3>{fs::path{"."}, fs::path{"versions"}, fixture.game.relative_path()}) {
        require(!root.is_absolute(), "Relative-root fixture must not provide an absolute path");
        require(failure_code([&] {
            fixture.action(UiAction::InstallProject,
                {{"root", nickel::resources::utf8Path(root)}, {"versionId", ""}, {"kind", "modpack"},
                 {"provider", "modrinth"}, {"projectId", "fixture1"}, {"projectVersionId", "approved1"},
                 {"instanceName", "relative-pack"}});
        }) == "unauthorized", "Modpack installation must reject relative root input before resolving authority");
        require(fixture.dispatches.empty() && fixture.state->tasks->snapshots().empty(),
                "Rejected relative roots must not create or queue a Task");
    }
    auto wrong_id = fixture.args({{"ids", Json::array({"fixture.jar"})}});
    wrong_id["versionId"] = "Fixture display name, not its directory ID";
    require(!failure_code([&] { fixture.state->action_resources(UiAction::DeleteMods,
        wrong_id, {}); }).empty(), "Display name must not replace directory identity");
}

void payload_override_matrix() {
    Fixture fixture;
    for (const auto* key : {"url", "downloadUrl", "downloads", "source_path", "destination", "launcher", "executable", "originalSha256"}) {
        auto value = fixture.args();
        value[key] = "https://unapproved.invalid/payload";
        require(failure_code([&] { fixture.state->action_resources(UiAction::InstallProject, value, {}); }) == "unauthorized",
                "Public arguments must not override approved provider/download authority");
    }
}

void import_all_local_kinds() {
    Fixture fixture;
    const auto source = fixture.directory / "incoming";
    fs::create_directories(source);
    fixture.jar(source / "local.jar");
    const auto create = [&](const fs::path& file, std::string_view member) {
        const std::vector<nickel::packs::ArchiveInput> inputs{
            {"pack.mcmeta", {}, R"({"pack":{"pack_format":15,"description":"Fixture"}})"},
            {std::string(member), {}, "fixture payload"}};
        fixture.state->archive->write(file, inputs, {}, {});
        fixture.state->archive->release(file);
    };
    create(source / "resources.zip", "assets/fixture/lang/en_us.json");
    create(source / "data.zip", "data/fixture/functions/hello.mcfunction");
    create(source / "shaders.zip", "shaders/fixture.glsl");
    Json paths = Json::array();
    for (const auto* name : {"local.jar", "resources.zip", "data.zip", "shaders.zip"})
        paths.push_back(nickel::resources::utf8Path(source / name));
    const auto result = fixture.action(UiAction::ImportResources, {{"paths", paths}});
    require(!fs::exists(fixture.context().modsRoot / "local.jar"), "Queue admission is not an installation receipt");
    fixture.succeeded(result);
    const auto context = fixture.context();
    for (const auto& pair : {std::pair{context.modsRoot, "local.jar"}, {context.resourcepacksRoot, "resources.zip"},
                            {context.datapacksRoot, "data.zip"}, {context.shaderpacksRoot, "shaders.zip"}})
        require(read(pair.first / pair.second) == read(source / pair.second), "Import must preserve exact archive bytes and kind");
    require(std::any_of(fixture.events.begin(), fixture.events.end(), [](const auto& event) { return event.first == UiTopic::Mods; }),
            "Committed Mod import must publish its immutable scoped snapshot");
}

void import_replacement_rejected() {
    Fixture fixture;
    const auto incoming = fixture.directory / "incoming" / "replace.jar";
    fs::create_directories(incoming.parent_path());
    fixture.jar(incoming);
    const auto bytes = read(incoming);
    const auto before = nickel::tasks::lease_file(incoming, {});
    const auto result = fixture.action(UiAction::ImportResources,
        {{"paths", Json::array({nickel::resources::utf8Path(incoming)})}});
    fs::rename(incoming, incoming.parent_path() / "captured.jar");
    write(incoming, bytes);
    require(nickel::tasks::lease_file(incoming, {}).identity != before.identity, "Adversarial fixture must actually replace the inode");
    fixture.drain();
    require(fixture.task(result).status == nickel::tasks::TaskStatus::Failed, "Replaced import source must fail");
    require(!fs::exists(fixture.context().modsRoot / "replace.jar"), "Replaced import source must never publish");
}

void import_same_inode_changed_bytes_rejected() {
    Fixture fixture;
    const auto incoming = fixture.directory / "incoming" / "changed.jar";
    fs::create_directories(incoming.parent_path());
    fixture.jar(incoming);
    const auto before = nickel::tasks::lease_file(incoming, {});
    const auto result = fixture.action(UiAction::ImportResources,
        {{"paths", Json::array({nickel::resources::utf8Path(incoming)})}});
    {
        std::fstream file(incoming, std::ios::binary | std::ios::in | std::ios::out);
        require(bool(file), "Adversarial fixture must open the existing file for in-place mutation");
        file.put('X');
        file.flush();
        require(bool(file), "Adversarial fixture must change the original file bytes");
    }
    const auto after = nickel::tasks::lease_file(incoming, {});
    require(before.identity == after.identity && before.size == after.size && before.sha256 != after.sha256,
            "Fixture must change bytes without replacing the inode or size");
    fixture.drain();
    require(fixture.task(result).status == nickel::tasks::TaskStatus::Failed,
            "Import must reject content changed after queue admission");
    require(!fs::exists(fixture.context().modsRoot / "changed.jar"), "Changed input must never publish");
}

void import_relative_path_rejected() {
    Fixture fixture;
    const auto incoming = fixture.directory / "incoming" / "relative.jar";
    fs::create_directories(incoming.parent_path());
    fixture.jar(incoming);
    const auto relative = incoming.lexically_relative(fs::current_path());
    require(!relative.empty() && !relative.is_absolute(), "Fixture input must be relative");
    require(failure_code([&] { fixture.action(UiAction::ImportResources,
        {{"paths", Json::array({nickel::resources::utf8Path(relative)})}}); }) == "unauthorized",
        "Relative import sources must not acquire implicit filesystem authority");
    require(fixture.dispatches.empty() && fixture.state->tasks->snapshots().empty(),
            "Unauthorized input must fail before creating a task");
}


void import_batch_atomic_collision() {
    Fixture fixture;
    const auto incoming = fixture.directory / "incoming";
    fs::create_directories(incoming);
    fixture.jar(incoming / "first.jar");
    fixture.jar(incoming / "second.jar");
    fixture.mod("second.jar");
    const auto original = read(fixture.context().modsRoot / "second.jar");
    const auto result = fixture.action(UiAction::ImportResources,
        {{"paths", Json::array({nickel::resources::utf8Path(incoming / "first.jar"), nickel::resources::utf8Path(incoming / "second.jar")})}});
    fixture.drain();
    require(fixture.task(result).status == nickel::tasks::TaskStatus::Failed, "Batch collision must report failure");
    require(!fs::exists(fixture.context().modsRoot / "first.jar"), "Batch validation must precede every publication");
    require(read(fixture.context().modsRoot / "second.jar") == original, "Collision must never overwrite existing content");
}

void rename_native_identity_and_commit() {
    Fixture fixture;
    const auto original = fixture.mod();
    const auto bytes = read(original);
    const auto result = fixture.action(UiAction::RenameMod, {{"id", "fixture.jar"}, {"name", "renamed.jar"}});
    fixture.succeeded(result);
    require(!fs::exists(original), "Rename must remove the leased original only in the transaction");
    require(read(fixture.context().modsRoot / "renamed.jar") == bytes, "Rename must publish exact bytes");
}

void rename_same_bytes_new_inode_rejected() {
    Fixture fixture;
    const auto original = fixture.mod();
    const auto before = nickel::tasks::lease_file(original, {});
    const auto bytes = read(original);
    const auto result = fixture.action(UiAction::RenameMod, {{"id", "fixture.jar"}, {"name", "renamed.jar"}});
    fs::rename(original, fixture.context().modsRoot / "original-held.jar");
    write(original, bytes);
    const auto after = nickel::tasks::lease_file(original, {});
    require(before.sha256 == after.sha256 && before.identity != after.identity, "Fixture must preserve bytes but replace native identity");
    fixture.drain();
    require(fixture.task(result).status == nickel::tasks::TaskStatus::Failed, "Stale rename must fail despite identical bytes");
    require(read(original) == bytes && !fs::exists(fixture.context().modsRoot / "renamed.jar"), "Stale rename must preserve replacement");
}

void cancel_request_and_canonical_task() {
    for (const bool pack : {false, true}) {
        for (const auto mode : {0, 1, 2}) {
            Fixture fixture;
            const auto original = fixture.mod();
            const auto pack_root = fixture.directory / "empty-pack-root";
            if (pack) {
                fs::create_directory(pack_root);
                auto registry = fixture.state->roots.get();
                registry.roots.push_back(nickel::resources::utf8Path(pack_root));
                fixture.state->roots.set(registry);
                fixture.scope["root"] = nickel::resources::utf8Path(pack_root);
                fixture.scope["versionId"] = "";
                require(!fs::exists(pack_root / "versions"), "New registered pack root must start without its target parent");
            }
            const auto destination = pack ? pack_root / "versions" / "queued-pack"
                : fixture.context().modsRoot / "renamed.jar";
            const auto operation = fixture.state->begin_operation(Json{{"operationId", "resource-cancel"}});
            auto cancellation = operation->cancellation;
            const Json fields = pack
                ? Json{{"provider", "modrinth"}, {"kind", "modpack"}, {"projectId", "fixture1"},
                       {"projectVersionId", "approved1"}, {"instanceName", "queued-pack"}, {"operationId", operation->operation_id}}
                : Json{{"id", "fixture.jar"}, {"name", "renamed.jar"}, {"operationId", operation->operation_id}};
            const auto result = fixture.action(pack ? UiAction::InstallProject : UiAction::RenameMod, fields, cancellation);
            const auto task = fixture.state->operation_task(cancellation);
            require(task && task->id() == result.data.at("taskId").get<std::string>()
                    && task->cancellation().stop_token() == cancellation.stop_token(),
                    "Task registration must immediately expose the identical canonical task and token");
            require(operation->task_id == task->id() && fixture.state->tasks->snapshots().size() == 1
                    && fixture.dispatches.size() == 1 && result.data.at("queued") == true,
                    "Operation handoff must precede the sole canonical task's queued business work");
            if (pack) {
                require(task->snapshot().kind == nickel::tasks::TaskKind::Modpack
                        && task->snapshot().target == destination,
                        "A queued pack must reserve its installation directory using the single Modpack Task");
                require(fs::is_directory(destination.parent_path()) && !fs::exists(destination),
                        "Pinned namespace preparation must create only the checked versions parent before returning its receipt");
            }
            fixture.state->finish_operation(operation, result.data);
            if (mode == 1) cancellation.cancel();
            else if (mode == 2) require(fixture.state->cancel_operation(operation->operation_id),
                                       "UI operation identity must cancel its already registered task");
            else require(fixture.state->tasks->cancel(task->id()), "Canonical task must accept queued cancellation");
            fixture.drain();
            require(fixture.task(result).status == nickel::tasks::TaskStatus::Cancelled,
                    "Queued cancellation must have a real cancelled terminal");
            require(fs::exists(original) && !fs::exists(destination),
                    "Cancellation before commit must preserve originals and not publish a pack instance");
        }
    }
}

void early_cancel_creates_no_task() {
    Fixture fixture;
    Cancellation cancellation;
    cancellation.cancel();
    const std::array<UiAction, 11> actions{UiAction::InstallProject, UiAction::DownloadProjectFile,
        UiAction::ImportResources, UiAction::DeleteResource, UiAction::SetModEnabled, UiAction::RenameMod,
        UiAction::DeleteMods, UiAction::CheckModUpdates, UiAction::UpdateMods, UiAction::ExportMods, UiAction::ExportPack};
    for (const auto selected : actions) {
        require(failure_code([&] { fixture.state->action_resources(selected, fixture.scope, cancellation); }) == "cancelled",
                "Pre-cancelled requests must fail before payload validation or task creation");
    }
    require(fixture.dispatches.empty() && fixture.state->tasks->snapshots().empty(),
            "Early cancellation must not queue or register resource work");
}

void shutdown_pending_task_is_cancelled_without_business() {
    Fixture fixture;
    const auto original = fixture.mod();
    const auto destination = fixture.context().modsRoot / "renamed.jar";
    const auto bytes = read(original);
    Cancellation cancellation;
    const auto result = fixture.action(UiAction::RenameMod,
        {{"id", "fixture.jar"}, {"name", "renamed.jar"}}, cancellation);
    require(fixture.dispatches.size() == 1, "Pending task must not have executed yet");
    fixture.state->shutdown();
    const auto snapshot = fixture.task(result);
    require(cancellation.cancelled() && snapshot.status == nickel::tasks::TaskStatus::Cancelled
            && snapshot.error_code == "cancelled", "Queue close must share the task token and publish a cancelled terminal");
    require(!fixture.state->tasks->conflicts(original), "Cancelled pending work must release task target authority");
    fixture.drain();
    require(read(original) == bytes && !fs::exists(destination), "Shutdown must not run pending rename business code");
}

void late_cancel_preserves_durable_publication() {
    Fixture fixture;
    const auto original = fixture.mod();
    const auto destination = fixture.context().modsRoot / "renamed.jar";
    Cancellation cancellation;
    fixture.state->set_sink([&](UiTopic topic, Json value) {
        fixture.events.emplace_back(topic, std::move(value));
        if (topic == UiTopic::Mods) cancellation.cancel();
    });
    const auto result = fixture.action(UiAction::RenameMod,
        {{"id", "fixture.jar"}, {"name", "renamed.jar"}}, cancellation);
    fixture.drain();
    require(cancellation.cancelled() && fixture.task(result).status == nickel::tasks::TaskStatus::Succeeded,
            "A post-commit request cancellation must not change durable success");
    require(!fs::exists(original) && fs::exists(destination), "Durably renamed files must survive late cancellation");
    require(std::any_of(fixture.events.begin(), fixture.events.end(), [](const auto& event) {
        return event.first == UiTopic::Resources;
    }), "Late cancellation must not suppress the remaining committed resource snapshot");
}


void revision_changed_before_commit() {
    Fixture fixture;
    fixture.mod();
    const auto result = fixture.action(UiAction::RenameMod, {{"id", "fixture.jar"}, {"name", "renamed.jar"}});
    const auto descriptor = fixture.instance / "fixture.json";
    nickel::tasks::detail::write_atomic(descriptor, Json{{"id", "1.20.2"}, {"mainClass", "fixture.Main"}, {"libraries", Json::array()}}.dump());
    fixture.drain();
    require(fixture.task(result).status == nickel::tasks::TaskStatus::Failed, "Frozen instance revision must be rechecked");
    require(fs::exists(fixture.context().modsRoot / "fixture.jar") && !fs::exists(fixture.context().modsRoot / "renamed.jar"),
            "Revision conflict must not mutate the resource");
}

void domain_mod_state_uses_own_lease() {
    Fixture fixture;
    fixture.mod();
    fixture.action(UiAction::SetModEnabled, {{"ids", Json::array({"fixture.jar"})}, {"enabled", false}});
    require(fs::exists(fixture.context().modsRoot / "fixture.jar.disabled"), "Domain mutation must not reacquire an outer App lease");
    fixture.action(UiAction::SetModEnabled, {{"ids", Json::array({"fixture.jar.disabled"})}, {"enabled", true}});
    require(fs::exists(fixture.context().modsRoot / "fixture.jar"), "Domain Mod enable must commit");
    const auto result = fixture.action(UiAction::DeleteMods, {{"ids", Json::array({"fixture.jar"})}});
    require(result.data.at("deleted") == 1 && !fs::exists(fixture.context().modsRoot / "fixture.jar"), "Domain Mod delete must report actual count");
}

void delete_only_authorized_regular_leaves() {
    Fixture fixture;
    const auto context = fixture.context();
    write(context.resourcepacksRoot / "selected.zip", "selected fixture leaf");
    write(context.resourcepacksRoot / "kept.zip", "unselected fixture leaf");
    fs::create_directories(context.resourcepacksRoot / "directory.zip");
    require(!failure_code([&] { fixture.action(UiAction::DeleteResource, {{"ids", Json::array({"resourcepack:directory.zip"})}}); }).empty(),
            "Resource deletion must reject directories");
    const auto result = fixture.action(UiAction::DeleteResource, {{"ids", Json::array({"resourcepack:selected.zip"})}});
    require(result.data.at("deletedIds") == Json::array({"resourcepack:selected.zip"}), "Delete must report only actual selected leaves");
    require(!fs::exists(context.resourcepacksRoot / "selected.zip") && fs::exists(context.resourcepacksRoot / "kept.zip"),
            "Leaf deletion must not recurse or delete unselected resources");
}

#ifdef _WIN32
void delete_partial_failure_reports_durable_subset() {
    Fixture fixture;
    const auto context = fixture.context();
    const auto removed = context.resourcepacksRoot / "first.zip";
    const auto blocked = context.resourcepacksRoot / "second.zip";
    const auto kept = context.resourcepacksRoot / "unselected.zip";
    write(removed, "first authorized leaf");
    write(blocked, "second authorized leaf");
    write(kept, "unselected resource");
    // This real read handle permits capture but denies Windows DELETE sharing.
    auto pin = nickel::tasks::detail::SecureFile::open_read(blocked);
    bool partial = false;
    try {
        fixture.action(UiAction::DeleteResource,
            {{"ids", Json::array({"resourcepack:first.zip", "resourcepack:second.zip"})}});
    } catch (const ErrorException& failure) {
        require(failure.error().code == "partial_failure", "A later delete failure must report partial success");
        const auto& details = failure.error().details;
        require(details.at("partial") == true && details.at("failureCode") == "revision_conflict",
                "Partial error must preserve the underlying publication conflict");
        require(details.at("deletedIds") == Json::array({"resourcepack:first.zip"}),
                "Partial error must report only the durably removed subset");
        partial = true;
    }
    require(partial, "The pinned second leaf must prevent batch completion");
    require(!fs::exists(removed) && read(blocked) == "second authorized leaf" && read(kept) == "unselected resource",
            "Partial deletion must preserve the failed and unselected leaves");
    const auto published = std::find_if(fixture.events.begin(), fixture.events.end(), [](const auto& event) {
        return event.first == UiTopic::Resources;
    });
    require(published != fixture.events.end(), "Partial durable deletion must publish the actual resource snapshot");
    const auto& items = published->second.at("items");
    require(items.size() == 2 && std::none_of(items.begin(), items.end(), [](const auto& item) {
        return item.at("id") == "resourcepack:first.zip";
    }), "Partial snapshot must exclude the removed leaf and retain both surviving leaves");
    pin.close();
    const auto retry = fixture.action(UiAction::DeleteResource, {{"ids", Json::array({"resourcepack:second.zip"})}});
    require(retry.data.at("deletedIds") == Json::array({"resourcepack:second.zip"}) && !fs::exists(blocked),
            "Partial failure must release the mutation lease so the surviving leaf can be retried");
}
#endif

void export_is_nonoverwriting_and_absolute() {
    Fixture fixture;
    fixture.mod();
    const auto relative_error = failure_code([&] {
        fixture.action(UiAction::ExportMods, {{"ids", Json::array({"fixture.jar"})}, {"path", "relative.zip"}});
    });
    require(relative_error == "unauthorized", "Relative save paths must not be silently absolutized; actual=" + relative_error);
    const auto destination = fixture.save("existing.zip");
    write(destination, "keep original export");
    const auto existing_error = failure_code([&] {
        fixture.action(UiAction::ExportMods,
            {{"ids", Json::array({"fixture.jar"})}, {"path", nickel::resources::utf8Path(destination)}});
    });
    require(existing_error == "conflict", "Existing export must fail closed; actual=" + existing_error);
    require(read(destination) == "keep original export", "Existing export bytes must remain unchanged");
}

void export_collision_after_queue() {
    Fixture fixture;
    fixture.mod();
    const auto destination = fixture.save("racing.zip");
    const auto result = fixture.action(UiAction::ExportMods,
        {{"ids", Json::array({"fixture.jar"})}, {"path", nickel::resources::utf8Path(destination)}});
    write(destination, "new external owner");
    fixture.drain();
    require(fixture.task(result).status == nickel::tasks::TaskStatus::Failed, "Post-admission destination collision must fail commit");
    require(read(destination) == "new external owner", "Export must not overwrite post-admission external file");
}

void export_mods_real_archive() {
    Fixture fixture;
    const auto source = fixture.mod();
    const auto destination = fixture.save("mods.zip");
    const auto result = fixture.action(UiAction::ExportMods,
        {{"ids", Json::array({"fixture.jar"})}, {"path", nickel::resources::utf8Path(destination)}});
    fixture.succeeded(result);
    const auto entries = fixture.state->archive->inspect(destination, {}, {});
    require(entries.size() == 1 && entries.front().name == "fixture.jar", "Export must produce a real Mods ZIP");
    require(fixture.state->archive->read(destination, "fixture.jar", 1024 * 1024, {}) == read(source), "Exported Mod bytes must match leased source");
    fixture.state->archive->release(destination);
}

void export_pack_real_formats_and_public_tokens() {
    Fixture fixture;
    for (const auto* format : {"mrpack", "portable"}) {
        const auto destination = fixture.save(std::string(format) + ".zip");
        const auto result = fixture.action(UiAction::ExportPack, {{"path", nickel::resources::utf8Path(destination)},
            {"name", "Fixture pack"}, {"version", "1"}, {"format", format}, {"files", Json::array()},
            {"includeLauncher", false}, {"includePersonalization", false}});
        fixture.succeeded(result);
        const auto entries = fixture.state->archive->inspect(destination, {}, {});
        const auto expected = std::string_view(format) == "mrpack" ? "modrinth.index.json" : "nickel.portable.json";
        require(std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.name == expected; }),
                "Export format must produce its real verified manifest");
        fixture.state->archive->release(destination);
    }
    const auto destination = fixture.save("bad-token.mrpack");
    const auto result = fixture.action(UiAction::ExportPack, {{"path", nickel::resources::utf8Path(destination)},
        {"name", "Fixture pack"}, {"version", "1"}, {"format", "mrpack"},
        {"files", Json::array({nickel::resources::utf8Path(fixture.instance / "fixture.json")})}});
    fixture.drain();
    require(fixture.task(result).status == nickel::tasks::TaskStatus::Failed && !fs::exists(destination),
            "Private filesystem source paths must not be accepted as public ExportTree tokens");
}

void trusted_launcher_material_missing_fails_closed() {
    Fixture fixture;
    const auto destination = fixture.save("launcher.zip");
    require(failure_code([&] { fixture.action(UiAction::ExportPack,
        {{"path", nickel::resources::utf8Path(destination)}, {"name", "Fixture pack"}, {"version", "1"},
         {"format", "portable"}, {"files", Json::array()}, {"includeLauncher", true}}); }) == "missing_service",
        "Portable launcher export must not invent trusted release artifacts");
    require(fixture.state->tasks->snapshots().empty() && !fs::exists(destination), "Missing trust materials must fail before scheduling");
}

void updates_require_captured_confirmation() {
    Fixture fixture;
    fixture.mod();
    const auto actual = failure_code([&] {
        fixture.action(UiAction::UpdateMods, {{"ids", Json::array({"fixture.jar"})}});
    });
    require(actual == "conflict", "UpdateMods must reject unconfirmed arbitrary download requests; actual=" + actual);
    require(fixture.state->tasks->snapshots().empty(), "Unconfirmed updates must not create replacement work");
}

void queued_work_does_not_block_queries() {
    Fixture fixture;
    fixture.mod();
    const auto result = fixture.action(UiAction::RenameMod, {{"id", "fixture.jar"}, {"name", "renamed.jar"}});
    require(fixture.dispatches.size() == 1, "Canonical scheduler must admit one long-operation drainer");
    const auto begin = std::chrono::steady_clock::now();
    const auto query = fixture.state->query(UiQuery::InstalledResources, fixture.scope, {});
    require(query.data.at("versionId") == "fixture", "Queries must retain exact scope during queued work");
    require(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2), "Queued long work must not synchronously block queries");
    fixture.succeeded(result);
}

fs::path network_data() {
    const auto data = qgetenv("NICKEL_RESOURCE_TEST_DATA");
    require(!data.isEmpty(), "Controlled network tests require the isolated fixture wrapper");
    const auto path = nickel::resources::pathFromUtf8(data.toStdString());
    require(path.is_absolute() && fs::is_directory(path), "Network fixture data must be an owned absolute directory");
    return path;
}

void configure_network(Fixture& fixture) {
    const auto proxy = qgetenv("NICKEL_RESOURCE_TEST_PROXY");
    const auto ca = qgetenv("NICKEL_RESOURCE_TEST_CA");
    require(!proxy.isEmpty() && !ca.isEmpty(), "Network tests require a controlled proxy and pinned temporary CA");
    auto snapshot = fixture.state->http->snapshot();
    snapshot.proxy.mode = nickel::transport::ProxySnapshot::Mode::HttpConnect;
    snapshot.proxy.endpoint = proxy.toStdString();
    snapshot.proxy.allow_loopback_proxy = true;
    snapshot.ca_bundle = nickel::resources::pathFromUtf8(ca.toStdString());
    snapshot.connect_timeout = std::chrono::seconds(3);
    snapshot.idle_timeout = std::chrono::seconds(3);
    snapshot.total_timeout = std::chrono::seconds(12);
    fixture.state->http->set_snapshot(snapshot);
    auto providers = fixture.state->community->settings();
    providers.apiPolicy = nickel::resources::SourcePolicy::OfficialFirst;
    providers.filePolicy = nickel::resources::SourcePolicy::OfficialFirst;
    providers.modrinthMirror.clear();
    providers.curseforgeMirror.clear();
    providers.fileMirror.clear();
    fixture.state->community->setSettings(std::move(providers));
}

Json project_arguments(std::string_view version) {
    return {{"provider", "modrinth"}, {"kind", "mod"}, {"projectId", "fixture1"}, {"projectVersionId", version}};
}

void network_install_and_download_approved_plan() {
    Fixture fixture;
    configure_network(fixture);
    const auto expected = read(network_data() / "approved.jar");
    const auto installation = fixture.action(UiAction::InstallProject, project_arguments("approved1"));
    fixture.succeeded(installation);
    const auto installed = fixture.context().modsRoot / "approved.jar";
    require(read(installed) == expected, "InstallProject must commit provider-approved real HTTP archive bytes");
    const auto lease = nickel::tasks::lease_file(installed, {});
    const auto repeated = fixture.action(UiAction::InstallProject, project_arguments("approved1"));
    fixture.succeeded(repeated);
    require(nickel::tasks::lease_file(installed, {}).identity == lease.identity,
            "An already verified resource must complete without replacing the installed inode");
    const auto destination = fixture.save("author-selected.jar");
    auto arguments = project_arguments("approved1");
    arguments["path"] = nickel::resources::utf8Path(destination);
    const auto download = fixture.action(UiAction::DownloadProjectFile, arguments);
    fixture.succeeded(download);
    require(read(destination) == expected, "Save-as download must use the same approved verified plan");
    require(fixture.state->tasks->snapshots().size() == 3, "Each action must return exactly its real canonical task");
}

void network_bad_hash_never_publishes() {
    Fixture fixture;
    configure_network(fixture);
    const auto destination = fixture.save("bad-hash.jar");
    auto arguments = project_arguments("broken01");
    arguments["path"] = nickel::resources::utf8Path(destination);
    const auto result = fixture.action(UiAction::DownloadProjectFile, arguments);
    fixture.drain();
    const auto snapshot = fixture.task(result);
    require(snapshot.status == nickel::tasks::TaskStatus::Failed && snapshot.error_code == "integrity",
            "A same-size corrupt HTTP artifact must fail its pinned provider hash");
    require(!fs::exists(destination), "Hash failure must never publish a save-as file");
}

fs::path capture_network_update(Fixture& fixture, const nickel::resources::Strings& names = {"installed.jar"}) {
    require(!names.empty(), "Update fixture must select at least one real installed leaf");
    configure_network(fixture);
    auto descriptor = Json::parse(read(fixture.instance / "fixture.json"));
    descriptor["nickelProfile"] = {{"minecraftVersion", "1.20.1"}, {"loader", "fabric"}, {"loaderVersion", "0.16.0"}};
    nickel::tasks::detail::write_atomic(fixture.instance / "fixture.json", descriptor.dump());
    const auto root = fixture.context().modsRoot;
    const auto initial = read(network_data() / "initial.jar");
    for (const auto& name : names) write(root / nickel::resources::pathFromUtf8(name), initial);
    const Json fields{{"ids", names}};
    const auto result = fixture.services ? fixture.dispatch_action(1, UiAction::CheckModUpdates, fields)
        : fixture.action(UiAction::CheckModUpdates, fields);
    fixture.succeeded(result);
    const auto selected = fixture.state->identity(fixture.scope);
    const auto key = Json::array({selected.rootId, selected.instanceId}).dump();
    const auto captured = [&] {
        std::lock_guard lock(fixture.state->state_mutex);
        const auto found = fixture.state->mod_updates.find(key);
        return found != fixture.state->mod_updates.end() && found->second.size() == names.size()
            && std::all_of(found->second.begin(), found->second.end(), [&](const auto& update) {
                return update.identified && bool(update.available)
                    && std::find(names.begin(), names.end(), update.fileName) != names.end();
            });
    };
    require(pump_until(captured, std::chrono::seconds(2)),
            "Actual provider hash identification must capture every compatible update confirmation");
    return root / nickel::resources::pathFromUtf8(names.front());
}

void network_confirmed_update_commits_exact_bytes() {
    for (const bool multiple : {false, true}) {
        Fixture fixture;
        const nickel::resources::Strings names = multiple
            ? nickel::resources::Strings{"installed.jar", "second.jar.disabled"}
            : nickel::resources::Strings{"installed.jar"};
        const auto original = capture_network_update(fixture, names);
        const auto prior_tasks = fixture.state->tasks->snapshots().size();
        const auto operation = fixture.state->begin_operation(Json{{"operationId", "resource-confirmed-update"}});
        const auto result = fixture.action(UiAction::UpdateMods,
            {{"ids", names}, {"operationId", operation->operation_id}}, operation->cancellation);
        const auto task = fixture.state->operation_task(operation->cancellation);
        require(task && task->id() == result.data.at("taskId").get<std::string>()
                && task->cancellation().stop_token() == operation->cancellation.stop_token(),
                "Confirmed update must register the sole canonical task before its business work is queued");
        require(task->snapshot().target == fixture.context().resourceRoot,
                "The registered update task must reserve the actual resource publication root");
        require(result.data.at("perFileCommit") == true, "Update receipt must disclose the shared per-file transaction contract");
        fixture.state->finish_operation(operation, result.data);
        fixture.succeeded(result);
        const auto receipts = task->committed_files();
        require(receipts.size() == names.size() && task->snapshot().files_done == names.size(),
                "One canonical task must retain one durable receipt for each actual file step");
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto destination = original.parent_path() / nickel::resources::pathFromUtf8(names[index]);
            const auto current = nickel::tasks::lease_file(destination, {});
            const auto& receipt = receipts[index];
            require(read(destination) == read(network_data() / "approved.jar"),
                    "Every confirmed leaf must contain the exact approved HTTP archive bytes");
            require(receipt.final == (index + 1 == names.size()) && !receipt.durability_pending
                    && receipt.published.destination == destination && receipt.published.identity == current.identity
                    && receipt.published.size == current.size && receipt.published.sha256 == current.sha256,
                    "Real file-step receipts must bind the published identity/hash and only the last real final step");
            if (index) require(receipt.step_id != receipts[index - 1].step_id,
                               "Different installed leaves of the same project must have distinct stable step identities");
        }
        require(!fs::exists(original.parent_path() / "approved.jar")
                && !fs::exists(original.parent_path() / "approved.jar.disabled")
                && !fs::exists(original.parent_path() / "second.jar"),
                "Provider filename and disabled state must not create a second logical installed Mod");
        require(fixture.state->tasks->snapshots().size() == prior_tasks + 1,
                "Confirmed replacements must use the registered task, not unregistered per-file tasks");
        require(!fixture.state->cancel_operation(operation->operation_id) && !fixture.state->tasks->cancel(task->id()),
                "A durably completed update must reject late cancellation through either public identity");
        require(fixture.task(result).status == nickel::tasks::TaskStatus::Succeeded,
                "Late cancellation must not relabel a fully published update as cancelled");
    }
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
    enum class Boundary { CancelPrefix, FailSecond, PendingPrefix, PendingFinal };
    for (const auto boundary : {Boundary::CancelPrefix, Boundary::FailSecond, Boundary::PendingPrefix, Boundary::PendingFinal}) {
        Fixture fixture;
        const nickel::resources::Strings names{"installed.jar", "second.jar.disabled"};
        const auto original = capture_network_update(fixture, names);
        const auto initial = read(network_data() / "initial.jar");
        const auto expected = read(network_data() / "approved.jar");
        const auto prior_tasks = fixture.state->tasks->snapshots().size();
        const auto operation = fixture.state->begin_operation(Json{{"operationId", "resource-update-boundary"}});
        const auto result = fixture.action(UiAction::UpdateMods,
            {{"ids", names}, {"operationId", operation->operation_id}}, operation->cancellation);
        const auto task = fixture.state->operation_task(operation->cancellation);
        require(task && task->id() == result.data.at("taskId").get<std::string>()
                && task->cancellation().stop_token() == operation->cancellation.stop_token(),
                "Every fault/cancel subcase must retain the exact registered accepted Task");
        fixture.state->finish_operation(operation, result.data);
        bool reached = false;
        const auto checkpoint = [&](std::string_view point, const fs::path& path) {
            if (point != "files.prepared" && point != "files.backed_up" && point != "step.marker") return;
            if (point == "files.backed_up" ? path.parent_path() != task->workspace() / "backup"
                    : path != task->workspace()) return;
            const auto done = task->committed_files().size();
            if (boundary == Boundary::CancelPrefix && point == "files.prepared" && done == 1) {
                reached = task->cancel();
            } else if (boundary == Boundary::FailSecond && point == "files.backed_up" && done == 1) {
                reached = true;
                throw ErrorException(nickel::Error{"io", "Controlled second-step prepublication failure"});
            } else if ((boundary == Boundary::PendingPrefix || boundary == Boundary::PendingFinal)
                    && point == "step.marker" && done == (boundary == Boundary::PendingFinal ? 1U : 0U)) {
                reached = true;
                throw std::runtime_error("Controlled post-publication marker persistence failure");
            }
        };
        fixture.drain_on_worker(checkpoint);
        require(reached, "Mandatory update boundary must reach the real Task checkpoint, not a replacement service");
        const bool final = boundary == Boundary::PendingFinal;
        const bool pending = final || boundary == Boundary::PendingPrefix;
        const auto receipts = task->committed_files();
        require(receipts.size() == (final ? 2U : 1U) && task->snapshot().files_done == receipts.size(),
                "Cancellation, suffix failure and pending durability must retain the exact durable prefix count");
        require(read(original) == expected
                && read(original.parent_path() / nickel::resources::pathFromUtf8(names[1])) == (final ? expected : initial),
                "Stopping a suffix must never restore its committed prefix or publish an uncommitted second leaf");
        require(receipts.front().final == false && receipts.back().final == final
                && receipts.back().durability_pending == pending,
                "Durable receipts must preserve final and persistence-pending facts after rollback/reporting");
        const auto status = boundary == Boundary::CancelPrefix ? nickel::tasks::TaskStatus::Cancelled
            : final ? nickel::tasks::TaskStatus::Succeeded : nickel::tasks::TaskStatus::Failed;
        require(task->snapshot().status == status && (!pending || task->snapshot().error_code == "persistence_pending"),
                "Only the last real publication may finish success; pending nonfinal publication must stop the suffix");
        require(fixture.state->tasks->snapshots().size() == prior_tasks + 1,
                "Fault/cancel updates must not create child Tasks");
        const auto delta = fixture.state->tasks->since(0);
        require(std::count_if(delta.events.begin(), delta.events.end(), [&](const auto& event) {
            return event.kind == nickel::tasks::EventKind::Terminal && event.snapshot.id == task->id();
        }) == 1, "The accepted Task must publish exactly one terminal outcome");
        task->rollback("reporting_failed", "A reporting failure cannot erase a committed prefix");
        require(task->snapshot().status == status && task->committed_files().size() == receipts.size()
                && read(original) == expected, "Late rollback must not change durable step facts");
        if (pending) {
            const auto recovered = fixture.state->tasks->recover(task->workspace().parent_path(), {});
            const auto found = std::find_if(recovered.begin(), recovered.end(), [&](const auto& entry) {
                return entry.task_id == task->id();
            });
            require(found != recovered.end() && found->recovered && found->committed == final
                    && found->committed_files.size() == receipts.size() && read(original) == expected,
                    "Actual journal recovery must retain pending committed files and the true final flag");
        }
        require(!fixture.state->tasks->cancel(task->id()), "A terminal update must reject late public Task cancellation");
        const auto context = fixture.context();
        auto lease = fixture.state->acquire_mutation(context.identity, context.revision);
        require(bool(lease), "The domain's sole mutation lease must be released after success, failure or cancellation");
    }
#else
    throw std::runtime_error("BLOCKED update boundary regressions require matching NICKEL_TRANSPORT_TEST_HOOKS on production transport and this fixture");
#endif
}

void network_update_query_retains_confirmation() {
    Fixture fixture(true);
    capture_network_update(fixture);
    fixture.services->request(2, UiQuery::Mods, nickel::app::to_public_json(fixture.scope).toObject());
    require(pump_until([&] { return fixture.query_replies.contains(2); }, std::chrono::seconds(2)),
            "A canonical Mods request must deliver through the actual AppServices dispatcher");
    const auto& reply = fixture.query_replies.at(2);
    require(!reply.error.isError(), "A later public Mods query must succeed");
    const auto snapshot = nickel::app::from_public_json(reply.data);
    const auto& rows = snapshot.at("items");
    require(snapshot.at("versionId") == "fixture" && std::any_of(rows.begin(), rows.end(), [](const auto& row) {
        return row.value("id", std::string{}) == "installed.jar" && row.value("updateAvailable", false);
    }), "A later canonical Mods query must retain the captured update availability and exact scope");
}

void network_changed_original_rejects_update() {
    Fixture fixture;
    const auto original = capture_network_update(fixture);
    const auto changed = read(network_data() / "approved.jar");
    nickel::tasks::detail::write_atomic(original, changed);
    const auto prior_tasks = fixture.state->tasks->snapshots().size();
    const auto operation = fixture.state->begin_operation(Json{{"operationId", "resource-update-changed-original"}});
    const auto result = fixture.action(UiAction::UpdateMods,
        {{"ids", Json::array({"installed.jar"})}, {"operationId", operation->operation_id}}, operation->cancellation);
    const auto task = fixture.state->operation_task(operation->cancellation);
    require(task && task->id() == result.data.at("taskId").get<std::string>()
            && task->cancellation().stop_token() == operation->cancellation.stop_token(),
            "A changed-original update must still register only the accepted canonical Task");
    fixture.state->finish_operation(operation, result.data);
    fixture.drain();
    require(task->snapshot().status == nickel::tasks::TaskStatus::Failed,
            "A changed original must invalidate captured update authorization");
    require(task->committed_files().empty() && task->snapshot().files_done == 0,
            "Invalid expected original bytes must not manufacture a committed file step");
    require(fixture.state->tasks->snapshots().size() == prior_tasks + 1 && !fixture.state->tasks->cancel(task->id()),
            "An invalid confirmed update must create no child Task and reject late cancellation");
    require(read(original) == changed, "Failed update must preserve the externally changed original");
    const auto context = fixture.context();
    auto lease = fixture.state->acquire_mutation(context.identity, context.revision);
    require(bool(lease), "Changed-original rejection must release the domain mutation lease");
}

void network_inflight_cancel_rolls_back() {
    Fixture fixture;
    configure_network(fixture);
    const auto signal = network_data() / "slow-started-1";
    Cancellation cancellation;
    std::atomic<bool> observed{false};
    const auto destination = fixture.save("cancelled.jar");
    auto arguments = project_arguments("slow0001");
    arguments["path"] = nickel::resources::utf8Path(destination);
    const auto result = fixture.action(UiAction::DownloadProjectFile, arguments, cancellation);
    std::jthread canceller([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (fs::exists(signal)) { observed.store(true); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        cancellation.cancel();
    });
    fixture.drain();
    canceller.join();
    require(observed.load(), "Cancellation fixture must observe an actual slow TLS transfer");
    require(fixture.task(result).status == nickel::tasks::TaskStatus::Cancelled && !fs::exists(destination),
            "In-flight request cancellation must roll back the shared task without publication");
}

void network_two_workers_keep_control_responsive() {
    Fixture fixture;
    configure_network(fixture);
    auto workers = std::make_unique<nickel::app::WorkerQueue>(2, 16);
    fixture.state->set_scheduler([&](std::chrono::milliseconds delay, ApplicationState::BackgroundWork work) {
        require(delay == std::chrono::milliseconds(0), "Long resource work must use the shared zero-delay drainer");
        return workers->submit(std::move(work));
    });
    Cancellation cancellation;
    const auto signal = network_data() / "slow-started-2";
    const auto destination = fixture.save("control.jar");
    auto arguments = project_arguments("slow0001");
    arguments["path"] = nickel::resources::utf8Path(destination);
    const auto result = fixture.action(UiAction::DownloadProjectFile, arguments, cancellation);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!fs::exists(signal) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto transferring = fs::exists(signal);
    auto queried = std::make_shared<std::promise<void>>();
    auto completed = queried->get_future();
    const auto admitted = workers->submit([state = fixture.state, queried](const Cancellation& cancel) {
        try {
            require(state->query(UiQuery::Downloads, {}, cancel).data.is_object(), "Control query must use the actual shared registry");
            queried->set_value();
        } catch (...) { queried->set_exception(std::current_exception()); }
    });
    const auto responsive = admitted && completed.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    cancellation.cancel();
    fixture.state->set_scheduler({});
    // WorkerQueue::stop does not join; its destructor must run before fixture state is inspected or released.
    workers.reset();
    require(transferring, "Control fixture must observe an actual slow TLS transfer before querying");
    require(responsive, "One long transfer must leave the second production worker responsive");
    completed.get();
    require(fixture.task(result).status == nickel::tasks::TaskStatus::Cancelled && !fs::exists(destination),
            "Worker shutdown and request cancellation must not publish a half download");
}

void network_appservices_dispatch_keeps_resources_responsive() {
    Fixture fixture(true);
    configure_network(fixture);
    fixture.mod();
    const auto signal = network_data() / "slow-started-3";
    const auto destination = fixture.save("public-control.jar");
    auto arguments = project_arguments("slow0001");
    arguments["path"] = nickel::resources::utf8Path(destination);
    arguments["operationId"] = "resource-public-control";
    const auto result = fixture.dispatch_action(1, UiAction::DownloadProjectFile, arguments);
    require(result.data.at("operationId") == arguments.at("operationId")
            && fixture.state->tasks->snapshots().size() == 1,
            "Public handoff must retain the UI operation identity and a single canonical task");
    require(result.data.at("queued") == true && pump_until([&] { return fs::exists(signal); }, std::chrono::seconds(10)),
            "Public dispatch must start an actual slow TLS transfer before responsiveness is measured");
    fixture.services->cancelRequest(1);
    const auto scope = nickel::app::to_public_json(fixture.scope).toObject();
    fixture.services->request(2, UiQuery::Mods, scope);
    fixture.services->request(3, UiQuery::InstalledResources, scope);
    const bool responsive = pump_until([&] {
        return fixture.query_replies.contains(2) && fixture.query_replies.contains(3);
    }, std::chrono::seconds(2));
    const bool still_transferring = fixture.task(result).status == nickel::tasks::TaskStatus::Running;
    fixture.services->submit(4, UiAction::CancelDownload,
        nickel::app::to_public_json(Json{{"id", result.data.at("operationId")}}).toObject());
    const bool cancel_replied = pump_until([&] { return fixture.command_replies.contains(4); }, std::chrono::seconds(2));
    const bool cancelled = pump_until([&] {
        return fixture.task(result).status == nickel::tasks::TaskStatus::Cancelled;
    }, std::chrono::seconds(3));
    // Closing the real service joins its workers before inspecting final fixture state.
    fixture.services.reset();
    require(responsive && still_transferring,
            "Local queries must remain responsive and cancelRequest must not cancel accepted business work");
    for (const nickel::ui::RequestId id : {2ULL, 3ULL}) {
        const auto& reply = fixture.query_replies.at(id);
        require(!reply.error.isError(), "Both public local resource queries must succeed during long work");
        const auto snapshot = nickel::app::from_public_json(reply.data);
        require(snapshot.at("versionId") == "fixture" && snapshot.at("root") == fixture.scope.at("root"),
                "Public local resource queries must preserve the exact authorized instance scope");
    }
    require(cancel_replied && !fixture.command_replies.at(4).error.isError() && cancelled
            && fixture.task(result).status == nickel::tasks::TaskStatus::Cancelled && !fs::exists(destination),
            "Public CancelDownload must resolve the UI operation identity to its sole canonical task");
}


} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    bool network = false;
    if (argc == 2 && std::string_view(argv[1]) == "--network") network = true;
    else if (argc != 1) { std::cerr << "Unexpected resource test arguments\n"; return 2; }
    std::vector<std::pair<const char*, std::function<void()>>> tests;
    if (network) {
        tests = {{"network_install_and_download_approved_plan", network_install_and_download_approved_plan},
            {"network_bad_hash_never_publishes", network_bad_hash_never_publishes},
            {"network_confirmed_update_commits_exact_bytes", network_confirmed_update_commits_exact_bytes},
            {"network_update_query_retains_confirmation", network_update_query_retains_confirmation},
            {"network_changed_original_rejects_update", network_changed_original_rejects_update},
            {"network_inflight_cancel_rolls_back", network_inflight_cancel_rolls_back},
            {"network_two_workers_keep_control_responsive", network_two_workers_keep_control_responsive},
            {"network_appservices_dispatch_keeps_resources_responsive", network_appservices_dispatch_keeps_resources_responsive}};
    } else {
        tests = {{"wrong_scope_matrix", wrong_scope_matrix},
            {"payload_override_matrix", payload_override_matrix},
            {"import_all_local_kinds", import_all_local_kinds},
            {"import_replacement_rejected", import_replacement_rejected},
            {"import_same_inode_changed_bytes_rejected", import_same_inode_changed_bytes_rejected},
            {"import_relative_path_rejected", import_relative_path_rejected},
            {"import_batch_atomic_collision", import_batch_atomic_collision},
            {"rename_native_identity_and_commit", rename_native_identity_and_commit},
            {"rename_same_bytes_new_inode_rejected", rename_same_bytes_new_inode_rejected},
            {"cancel_request_and_canonical_task", cancel_request_and_canonical_task},
            {"early_cancel_creates_no_task", early_cancel_creates_no_task},
            {"shutdown_pending_task_is_cancelled_without_business", shutdown_pending_task_is_cancelled_without_business},
            {"late_cancel_preserves_durable_publication", late_cancel_preserves_durable_publication},
            {"revision_changed_before_commit", revision_changed_before_commit},
            {"domain_mod_state_uses_own_lease", domain_mod_state_uses_own_lease},
            {"delete_only_authorized_regular_leaves", delete_only_authorized_regular_leaves},
#ifdef _WIN32
            {"delete_partial_failure_reports_durable_subset", delete_partial_failure_reports_durable_subset},
#endif
            {"export_is_nonoverwriting_and_absolute", export_is_nonoverwriting_and_absolute},
            {"export_collision_after_queue", export_collision_after_queue},
            {"export_mods_real_archive", export_mods_real_archive},
            {"export_pack_real_formats_and_public_tokens", export_pack_real_formats_and_public_tokens},
            {"trusted_launcher_material_missing_fails_closed", trusted_launcher_material_missing_fails_closed},
            {"updates_require_captured_confirmation", updates_require_captured_confirmation},
            {"queued_work_does_not_block_queries", queued_work_does_not_block_queries}};
    }
    unsigned failed = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const ErrorException& failure) { ++failed; std::cerr << "FAIL " << name << ": " << failure.error().code << '\n'; }
        catch (const std::exception& failure) { ++failed; std::cerr << "FAIL " << name << ": " << failure.what() << '\n'; }
    }
    std::cout << (tests.size() - failed) << '/' << tests.size() << " production resource-action tests passed\n";
    return failed ? 1 : 0;
}
