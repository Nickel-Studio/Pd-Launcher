#include "app/AppServices.hpp"
#include "app/service/snapshots.hpp"
#include "core/accounts/accounts.hpp"
#include "core/archive/archive.hpp"
#include "core/config/repository.hpp"
#include "core/config/vault.hpp"
#include "core/instances/domain_io.hpp"
#include "core/instances/settings.hpp"
#include "core/instances/startup.hpp"
#include "core/media/offline_skin.hpp"
#include "core/media/skin_png.hpp"
#include "core/tasks/tasks.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace {
namespace fs = std::filesystem;
namespace app = nickel::app;
namespace instances = nickel::instances;
namespace ui = nickel::ui;
using nickel::Json;
using nickel::tasks::TaskStatus;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

QString qpath(const fs::path& path) {
    return QString::fromStdString(instances::path_to_utf8(path));
}

void write_file(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(static_cast<bool>(output), "Cannot write owned launch fixture");
}

std::string shell_path(const fs::path& path) {
    const auto text = instances::path_to_utf8(path);
#ifdef _WIN32
    require(text.find_first_of("\"%!?\r\n") == std::string::npos, "Fixture shell path has unsafe expansion characters");
    return '"' + text + '"';
#else
    std::string quoted = "'";
    for (const auto c : text) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    return quoted + '\'';
#endif
}

bool preparation_running(unsigned long pid) {
    require(pid != 0, "Preparation fixture did not publish its actual PID");
#ifdef _WIN32
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        require(GetLastError() == ERROR_INVALID_PARAMETER, "Cannot inspect the owned preparation process");
        return false;
    }
    const auto result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    require(result != WAIT_FAILED, "Cannot poll the owned preparation process");
    return result == WAIT_TIMEOUT;
#else
    const auto result = kill(static_cast<pid_t>(pid), 0);
    require(result == 0 || errno == ESRCH, "Cannot inspect the owned preparation process");
    return result == 0;
#endif
}


struct Fixture {
    fs::path root;
    fs::path game;
    fs::path client;
    fs::path starts;
    app::AppServicesOptions options;
    std::string outer_id;

    Fixture(const fs::path& executable, const fs::path& parent, std::string id,
            std::string mode, bool isolation = true, bool named = false,
            std::chrono::milliseconds delay = 1s)
        : root(parent / ("launch-" + instances::detail::random_id())), game(root / "game"),
          starts(root / "game-starts.txt"), outer_id(std::move(id)) {
        fs::create_directories(parent);
        require(fs::create_directory(root), "Launch fixture parent is not unused");
        options.executable = root / "launcher.exe";
        options.paths.executable = options.executable;
        options.paths.system_config_base = root / "system-config";
        options.paths.app_data_base = root / "app-data";
        write_file(options.executable, "controlled-application-path\n");
        const auto outer = game / "versions" / instances::path_from_utf8(outer_id);
        const auto install_root = named ? outer / "instances" / instances::path_from_utf8(outer_id) : game;
        const auto installed_id = named ? std::string("1.20.6") : outer_id;
        const auto version_directory = install_root / "versions" / instances::path_from_utf8(installed_id);
        client = version_directory / instances::path_from_utf8(installed_id + ".jar");
        const auto resource = isolation ? outer : game;
        fs::create_directories(resource);
        fs::create_directories(version_directory);
        const auto seed = root / "client-seed";
        const auto class_file = seed / "FixtureMain.class";
        const unsigned char header[]{0xca, 0xfe, 0xba, 0xbe, 0x00, 0x00, 0x00, 0x41};
        write_file(class_file, std::string_view(reinterpret_cast<const char*>(header), sizeof(header)));
        write_file(seed / "MANIFEST.MF", "Manifest-Version: 1.0\nMain-Class: FixtureMain\n\n");
        write_file(seed / "version.json", "{\"id\":\"1.20.6\",\"pack_version\":{\"resource\":32}}\n");
        nickel::media::RgbaImage skin;
        skin.width = skin.height = 64;
        skin.pixels.assign(64 * 64 * 4, 255);
        const auto png = nickel::media::encode_skin_png(skin);
        write_file(seed / "skin.png", std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
        nickel::Cancellation cancellation;
        nickel::archive::Limits limits;
        limits.max_total_bytes = limits.max_archive_bytes = 4 * 1024 * 1024;
        nickel::archive::Archive::write_zip(client,
            {{"FixtureMain.class", class_file}, {"META-INF/MANIFEST.MF", seed / "MANIFEST.MF"},
             {"version.json", seed / "version.json"},
             {"assets/minecraft/textures/entity/player/wide/steve.png", seed / "skin.png"},
             {"assets/minecraft/textures/entity/player/slim/alex.png", seed / "skin.png"}}, limits, cancellation);
        Json metadata{{"id", installed_id}, {"mainClass", "FixtureMain"}, {"type", "release"},
            {"jar", installed_id}, {"javaVersion", {{"majorVersion", 21}}}, {"libraries", Json::array()},
            {"arguments", {{"jvm", Json::array()}, {"game", Json::array({"--version", "${version_name}",
                "--fixture-mode", mode, "--fixture-delay", std::to_string(delay.count()),
                "--fixture-record", instances::path_to_utf8(starts),
                "--fixture-expect-client", instances::path_to_utf8(client),
                "--fixture-expect-directory", instances::path_to_utf8(resource),
                "--fixture-expect-version", installed_id})}}}};
        write_file(version_directory / instances::path_from_utf8(installed_id + ".json"), metadata.dump());
        if (named) {
            instances::NamedProfile profile;
            profile.name = outer_id;
            profile.minecraft_version = "1.20.6";
            profile.ready = true;
            instances::save_profile(outer, profile);
            auto wrapper = metadata;
            wrapper["id"] = outer_id;
            wrapper["inheritsFrom"] = "1.20.6";
            wrapper["nickelProfile"] = {{"minecraftVersion", "1.20.6"}, {"loader", nullptr}, {"loaderVersion", ""}};
            write_file(outer / instances::path_from_utf8(outer_id + ".json"), wrapper.dump());
        }
        nickel::PathResolver paths(options.paths);
        nickel::JsonRepository repository(paths.config_root());
        repository.write("game.json", {{"gameDir", instances::path_to_utf8(game)}, {"javaPath", instances::path_to_utf8(executable)},
            {"versionIsolation", isolation}, {"memoryMode", "custom"}, {"memoryMb", 512},
            {"memoryManagement", "g1"}, {"offlineSkin", "random"}, {"disableWrapper", true}, {"disableLwjglAgent", true}});
        instances::InstanceRegistry registry(repository.root(), game);
        instances::Registry registered;
        registered.roots.push_back(instances::path_to_utf8(game));
        registered.active = registered.roots.front();
        registry.set(registered);
        nickel::Vault vault(repository.root());
        nickel::accounts::AccountService accounts(repository, vault);
        const auto account = accounts.add_offline("FixturePlayer");
        accounts.set_active(account.id);
    }

    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    QJsonObject scope() const { return {{"root", qpath(game)}, {"versionId", QString::fromStdString(outer_id)}}; }
    fs::path resource(bool isolation = true) const { return isolation ? game / "versions" / instances::path_from_utf8(outer_id) : game; }
    void accept_onboarding() const {
        nickel::PathResolver paths(options.paths);
        instances::OnboardingGate gate(paths.config_root(), app::build_version());
        gate.accept_eula(true, instances::eula_version, app::unix_milliseconds() / 1000);
        gate.acknowledge_changelog(app::build_version());
    }
    std::size_t start_count() const {
        if (!fs::exists(starts)) return 0;
        const auto text = instances::detail::read_bounded(starts, 4096);
        return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
    }
};

class Probe {
public:
    explicit Probe(const app::AppServicesOptions& options) : services(options), caller(QThread::currentThread()) {
        const auto reply = [this](const ui::UiReply& value) {
            require(QThread::currentThread() == caller, "Production reply ran off the Qt caller thread");
            require(delivered.insert(value.id).second, "Production request delivered more than once");
            replies.emplace(value.id, value);
        };
        QObject::connect(&services, &ui::UiServices::commandFinished, &services, reply);
        QObject::connect(&services, &ui::UiServices::queryReady, &services, reply);
        QObject::connect(&services, &ui::UiServices::stateChanged, &services,
            [this](ui::UiTopic topic, const QJsonObject& value) {
                require(QThread::currentThread() == caller, "Production event ran off the Qt caller thread");
                const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
                require(!bytes.contains("fixture-private-value"), "Secret leaked through a production DTO");
                if (topic == ui::UiTopic::Launch) {
                    const auto id = value.value("launchId").toString();
                    const auto phase = value.value("phase").toString();
                    require(!terminal_launches.contains(id) || phase == "finished",
                            "A late preparation callback resurrected a finished game");
                    if (phase == "finished") terminal_launches.insert(id);
                    launch = value;
                    if (value.value("state").toString() == "running") started.insert(id);
                }
                if (topic == ui::UiTopic::Logs && value.value("line").isString()) {
                    lines.push_back(value.value("line").toString());
                    if (lines.size() > 4100) lines.erase(lines.begin());
                }
            });
        const auto onboarding = query(ui::UiQuery::Bootstrap).value("onboarding").toObject();
        if (!onboarding.value("needsEula").toBool(true) && !onboarding.value("needsChangelog").toBool(true))
            success(ui::UiAction::FinishOnboarding);
    }

    template<class Predicate>
    void until(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate()) {
            require(std::chrono::steady_clock::now() < deadline, "Production launch event deadline expired");
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            std::this_thread::sleep_for(2ms);
        }
    }
    ui::UiReply command(ui::UiAction action, const QJsonObject& args = {}) {
        const auto id = ++sequence;
        services.submit(id, action, args);
        require(!replies.contains(id), "Production command completed synchronously");
        until([&] { return replies.contains(id); });
        auto reply = replies.at(id);
        replies.erase(id);
        return reply;
    }
    QJsonObject success(ui::UiAction action, const QJsonObject& args = {}) {
        const auto reply = command(action, args);
        if (reply.error.isError()) throw std::runtime_error("Production command failed: " + reply.error.code.toStdString());
        return reply.data.toObject();
    }
    QJsonObject query(ui::UiQuery query, const QJsonObject& args = {}) {
        const auto id = ++sequence;
        services.request(id, query, args);
        require(!replies.contains(id), "Production query completed synchronously");
        until([&] { return replies.contains(id); });
        auto reply = replies.at(id);
        replies.erase(id);
        if (reply.error.isError()) throw std::runtime_error("Production query failed: " + reply.error.code.toStdString());
        return reply.data.toObject();
    }
    QString start(const Fixture& fixture) {
        const auto accepted = success(ui::UiAction::LaunchGame, fixture.scope());
        require(accepted.value("state").toString() == "preparing", "Launch did not return preparation acceptance");
        const auto id = accepted.value("launchId").toString();
        require(!id.isEmpty(), "Launch acceptance lost owned ID");
        require(!accepted.value("taskId").toString().isEmpty()
            && accepted.value("taskId") == accepted.value("preparationTaskId"), "Launch acceptance lost its canonical task");
        return id;
    }
    QJsonObject finished(const QString& id) {
        until([&] { return launch.value("launchId").toString() == id && launch.value("phase").toString() == "finished"; }, 15s);
        return launch;
    }
    void preparation_terminal(const QJsonObject& launch_state, TaskStatus expected) {
        const auto id = launch_state.value("preparationTaskId").toString();
        require(!id.isEmpty(), "Production launch did not expose a preparation task ID");
        const auto downloads = query(ui::UiQuery::Downloads).value("items").toArray();
        for (const auto& row : downloads) {
            const auto task = row.toObject();
            if (task.value("id").toString() == id) {
                const auto status = task.value("taskStatus");
                const auto terminal = task.value("terminal");
                const auto can_cancel = task.value("canCancel");
                // The canonical task status is an enum number; the UI status is a separate string.
                if (!status.isDouble() || status.toInt(-1) != static_cast<int>(expected)
                    || !terminal.isBool() || !terminal.toBool()
                    || !can_cancel.isBool() || can_cancel.toBool()) {
                    throw std::runtime_error("Preparation task has no truthful terminal state: expected="
                        + std::to_string(static_cast<int>(expected)) + " actual=" + std::to_string(status.toInt(-1))
                        + " terminal=" + (terminal.toBool() ? "true" : "false")
                        + " canCancel=" + (can_cancel.toBool() ? "true" : "false"));
                }
                return;
            }
        }
        throw std::runtime_error("Production preparation task vanished from the registry");
    }
    void no_preparation() {
        for (const auto& value : query(ui::UiQuery::Downloads).value("items").toArray()) {
            const auto row = value.toObject();
            require(row.value("taskId").toString().isEmpty() && !row.contains("taskStatus"),
                    "Consent rejection started a preparation task");
            require(row.value("terminal").toBool() && !row.value("canCancel").toBool(true),
                    "Consent rejection retained a live business operation");
        }
    }

    app::AppServices services;
    QThread* caller;
    QJsonObject launch;
    std::vector<QString> lines;
    std::set<QString> started;
    std::set<QString> terminal_launches;
private:
    ui::RequestId sequence = 0;
    std::map<ui::RequestId, ui::UiReply> replies;
    std::set<ui::RequestId> delivered;
};

void manual_launch_requires_complete_consent(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(executable, parent, "1.20.6", "hold", true, false, 40ms);
    Probe probe(fixture.options);
    const auto bootstrap = probe.query(ui::UiQuery::Bootstrap);
    require(bootstrap.value("onboarding").toObject().value("needsEula").toBool(), "Fresh manual launch lost its consent gate");
    require(!fixture.scope().contains("launchId"), "Consent regression must exercise the legal optional launch ID");
    const auto rejected = probe.command(ui::UiAction::LaunchGame, fixture.scope());
    require(rejected.error.code == "onboarding_required", "Unaccepted manual launch did not expose the canonical consent error");
    require(fixture.start_count() == 0 && probe.started.empty(), "Manual launch spawned before consent");
    probe.no_preparation();
    probe.success(ui::UiAction::AcceptEula, {{"checked", true},
        {"eulaVersion", QString::fromUtf8(instances::eula_version.data(), static_cast<qsizetype>(instances::eula_version.size()))}});
    const auto changelog = probe.command(ui::UiAction::LaunchGame, fixture.scope());
    require(changelog.error.code == "onboarding_required", "Pending changelog did not expose the canonical consent error");
    require(fixture.start_count() == 0 && probe.started.empty(), "Manual launch spawned before changelog acknowledgement");
    probe.no_preparation();
    probe.success(ui::UiAction::AcknowledgeChangelog, {{"version", QString::fromStdString(app::build_version())}});
    require(!probe.query(ui::UiQuery::Bootstrap).value("startupReady").toBool(), "Bootstrap finished first-use without explicit confirmation");
    const auto unfinished = probe.command(ui::UiAction::LaunchGame, fixture.scope());
    require(unfinished.error.code == "onboarding_required", "Accepted records bypassed explicit FinishOnboarding");
    require(fixture.start_count() == 0 && probe.started.empty(), "Manual launch spawned before explicit first-use finish");
    probe.no_preparation();
    probe.success(ui::UiAction::FinishOnboarding);
    const auto id = probe.start(fixture);
    const auto finished = probe.finished(id);
    require(finished.value("state").toString() == "exited" && fixture.start_count() == 1,
            "Manual launch did not resume once after complete consent");
    probe.preparation_terminal(finished, TaskStatus::Succeeded);
}

void fast_exit_stays_terminal_under_query_pressure(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(executable, parent, "1.20.6", "hold", true, false, 0ms);
    fixture.accept_onboarding();
    Probe probe(fixture.options);
    std::set<QString> ids;
    for (int attempt = 0; attempt != 20; ++attempt) {
        const auto id = probe.start(fixture);
        require(ids.insert(id).second, "Retry reused an owned launch ID");
        const auto finished = probe.finished(id);
        require(finished.value("state").toString() == "exited" && finished.value("exitCode").toInt(-1) == 0,
                "A quick natural exit lost its truthful terminal state");
        probe.preparation_terminal(finished, TaskStatus::Succeeded);
        probe.query(ui::UiQuery::Bootstrap);
        require(probe.launch.value("launchId").toString() == id && probe.launch.value("phase").toString() == "finished",
                "A late callback restored a quick-exited game to running");
    }
    require(fixture.start_count() == 20 && probe.terminal_launches.size() == 20,
            "Quick-exit retry capacity or exactly-once terminal ownership failed");
}


void preparation_cancel_controls_the_canonical_task(const fs::path& executable, const fs::path& parent) {
    for (const bool through_downloads : {false, true}) {
        Fixture fixture(executable, parent, "1.20.6", "hold", true, false, 40ms);
        fixture.accept_onboarding();
        Probe probe(fixture.options);
        const auto ready = fixture.root / "preparation-ready.txt";
        const auto command = shell_path(executable) + " --fixture-mode preparation --fixture-delay 20000 --fixture-ready " + shell_path(ready);
        probe.success(ui::UiAction::PatchSettings, {{"section", "game"},
            {"patch", QJsonObject{{"preLaunchCommand", QString::fromStdString(command)}}}});
        auto args = fixture.scope();
        const auto operation_id = QString::fromStdString("prepare-" + instances::detail::random_id());
        args.insert("operationId", operation_id);
        const auto accepted = probe.success(ui::UiAction::LaunchGame, args);
        const auto id = accepted.value("launchId").toString();
        const auto task_id = accepted.value("preparationTaskId").toString();
        require(accepted.value("state").toString() == "preparing" && !id.isEmpty() && !task_id.isEmpty()
            && accepted.value("taskId").toString() == task_id && accepted.value("operationId").toString() == operation_id,
            "Preparation acceptance did not bind one canonical task to its business operation");
        probe.until([&] {
            return fs::is_regular_file(ready) && instances::detail::read_bounded(ready, 64).find('\n') != std::string::npos;
        });
        const auto pid = std::stoul(instances::detail::read_bounded(ready, 64));
        require(preparation_running(pid), "Cancellation fixture did not reach a real owned preparation process");
        const auto items = probe.query(ui::UiQuery::Downloads).value("items").toArray();
        std::size_t associated = 0;
        for (const auto& item : items) {
            const auto row = item.toObject();
            if (row.value("operationId").toString() != operation_id) continue;
            ++associated;
            require(row.value("id").toString() == task_id && row.value("taskId").toString() == task_id
                && row.value("canCancel").toBool() && !row.value("terminal").toBool(),
                "Downloads did not expose the actual cancellable preparation task");
        }
        require(associated == 1 && fixture.start_count() == 0 && probe.started.empty(),
                "Preparation duplicated its task or spawned a game before cancellation");
        const auto cancelled_at = std::chrono::steady_clock::now();
        const auto reply = through_downloads
            ? probe.command(ui::UiAction::CancelDownload, {{"id", operation_id}})
            : probe.command(ui::UiAction::CancelLaunch, {{"launchId", id}});
        require(!reply.error.isError(), "Actual preparation cancellation was rejected");
        const auto finished = probe.finished(id);
        require(finished.value("state").toString() == "cancelled"
            && !finished.value("reportReady").toBool() && fixture.start_count() == 0 && probe.started.empty(),
            "Cancelled preparation became a game or a failure report");
        probe.until([&] { return !preparation_running(pid); }, 5s);
        require(std::chrono::steady_clock::now() - cancelled_at < 10s, "Preparation cancellation waited for the fixture's natural exit");
        probe.preparation_terminal(finished, TaskStatus::Cancelled);
        probe.until([&] {
            const auto mutation = probe.command(ui::UiAction::RenameRoot, {{"root", qpath(fixture.game)}, {"label", "Cancelled fixture"}});
            require(!mutation.error.isError() || mutation.error.code == "busy", "Cancelled preparation corrupted root authority");
            return !mutation.error.isError();
        });
        probe.success(ui::UiAction::PatchSettings, {{"section", "game"}, {"patch", QJsonObject{{"preLaunchCommand", ""}}}});
        const auto retry_id = probe.start(fixture);
        const auto retry = probe.finished(retry_id);
        require(retry_id != id && retry.value("state").toString() == "exited" && fixture.start_count() == 1,
                "Cancelled preparation did not release ownership for a new real launch");
        probe.preparation_terminal(retry, TaskStatus::Succeeded);
    }
}



void running_cancel_is_preparation_only(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(executable, parent, "1.20.6", "hold");
    fixture.accept_onboarding();
    Probe probe(fixture.options);
    const auto id = probe.start(fixture);
    probe.until([&] { return probe.started.contains(id); });
    require(!probe.launch.value("canCancel").toBool(true), "Running game advertises preparation cancellation");
    const auto cancelled = probe.success(ui::UiAction::CancelLaunch, {{"launchId", id}});
    require(!cancelled.value("cancelled").toBool(true), "CancelLaunch accepted a running game");
    const auto finished = probe.finished(id);
    require(finished.value("state").toString() == "exited" && finished.value("exitCode").toInt(-1) == 0,
            "Running game was stopped or lost its natural exit");
    probe.preparation_terminal(finished, TaskStatus::Succeeded);
    require(fixture.start_count() == 1, "Running launch spawned more than one game");
}

void real_skin_uses_resolved_named_client(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(executable, parent, "OuterID", "hold", true, true);
    fixture.accept_onboarding();
    Probe probe(fixture.options);
    probe.success(ui::UiAction::PatchSettings, {{"section", "game"}, {"patch", QJsonObject{{"offlineSkin", "steve"}}}});
    const auto id = probe.start(fixture);
    const auto finished = probe.finished(id);
    require(finished.value("state").toString() == "exited", "Named client or offline skin failed production preparation");
    const auto pack = fixture.resource() / "resourcepacks" / std::string(nickel::media::offline_skin_pack_folder);
    require(fs::is_regular_file(pack / "pack.mcmeta"), "Production skin preparation created no owned resource pack");
    require(fs::is_regular_file(pack / "assets/minecraft/textures/entity/player/wide/steve.png"),
            "Production skin did not use actual resolved client resources");
    const auto options = instances::detail::read_bounded(fixture.resource() / "options.txt", 2 * 1024 * 1024);
    require(options.find(nickel::media::offline_skin_pack_id) != std::string::npos, "Production skin pack was not enabled");
    probe.preparation_terminal(finished, TaskStatus::Succeeded);
}

void failure_scope_and_full_zip_stay_frozen(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(executable, parent, "1.20.6", "crash", false);
    fixture.accept_onboarding();
    Probe probe(fixture.options);
    const auto id = probe.start(fixture);
    const auto failed = probe.finished(id);
    require(failed.value("state").toString() == "failed" && failed.value("reportReady").toBool(),
            "Production crash did not capture a complete safe report");
    const auto target = failed.value("reportTarget").toObject();
    require(target.value("root").toString() == qpath(fixture.game) && target.value("versionId").toString() == "1.20.6"
        && target.value("launchId").toString() == id, "Production failure target lost frozen launch scope");
    require(failed.value("diagnostic").toObject().value("category").toString() == "outOfMemory",
            "Production failure did not analyze actual crash evidence");
    probe.success(ui::UiAction::PatchSettings, {{"section", "game"}, {"patch", QJsonObject{{"versionIsolation", true}}}});
    write_file(fixture.game / "logs/latest.log", "route-changed-after-failure\n");
    auto args = target;
    const auto destination = fixture.root / "app-error-report.zip";
    args.insert("path", qpath(destination));
    probe.success(ui::UiAction::ExportErrorReport, args);
    nickel::Cancellation cancellation;
    const auto bytes = nickel::archive::Archive::read_entry(destination, "logs/latest.log", 2 * 1024 * 1024, cancellation);
    const auto text = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    require(text.find("crash-source-begin") != std::string::npos && text.find("crash-source-end") != std::string::npos
        && text.find("complete-source-evidence-19999") != std::string::npos, "Production report exported only a log tail");
    require(text.find("fixture-private-value") == std::string::npos && text.find("route-changed-after-failure") == std::string::npos,
            "Production report leaked secrets or re-resolved changed settings");
    auto forged = args;
    forged.insert("versionId", "OtherID");
    forged.insert("path", qpath(fixture.root / "forged-report.zip"));
    require(probe.command(ui::UiAction::ExportErrorReport, forged).error.isError(), "Unowned failure scope exported a report");
    probe.preparation_terminal(failed, TaskStatus::Succeeded);
    probe.success(ui::UiAction::ClearLaunchError, target);
}

void unsafe_output_never_becomes_ready(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(executable, parent, "1.20.6", "unsafe");
    fixture.accept_onboarding();
    Probe probe(fixture.options);
    const auto id = probe.start(fixture);
    const auto failed = probe.finished(id);
    require(failed.value("state").toString() == "failed" && !failed.value("reportReady").toBool(),
            "Incomplete sensitive output became a ready report");
}

void descendant_timeout_retains_resource_protection(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(executable, parent, "1.20.6", "descendant-pipes", false, false, 16s);
    fixture.accept_onboarding();
    instances::set_version_settings(fixture.game, fixture.outer_id, {{"versionIsolation", false}});
    const auto nested = fixture.game / "versions" / "1.20.5";
    const auto nested_client = nested / "1.20.5.jar";
    fs::create_directories(nested);
    fs::copy_file(fixture.client, nested_client);
    Json metadata{{"id", "1.20.5"}, {"mainClass", "FixtureMain"}, {"type", "release"},
        {"jar", "1.20.5"}, {"javaVersion", {{"majorVersion", 21}}}, {"libraries", Json::array()},
        {"arguments", {{"jvm", Json::array()}, {"game", Json::array({"--version", "${version_name}",
            "--fixture-mode", "hold", "--fixture-delay", "500",
            "--fixture-expect-client", instances::path_to_utf8(nested_client),
            "--fixture-expect-directory", instances::path_to_utf8(nested),
            "--fixture-expect-version", "1.20.5"})}}}};
    write_file(nested / "1.20.5.json", metadata.dump());
    instances::set_version_settings(fixture.game, "1.20.5", {{"versionIsolation", true}});
    const QJsonObject nested_scope{{"root", qpath(fixture.game)}, {"versionId", "1.20.5"}};
    Probe probe(fixture.options);
    const auto id = probe.start(fixture);
    probe.until([&] {
        return probe.launch.value("launchId").toString() == id
            && probe.launch.value("state").toString() == "running"
            && probe.launch.value("phase").toString() == "draining";
    });
    const auto drain_observed = std::chrono::steady_clock::now();
    probe.until([&] { return std::chrono::steady_clock::now() - drain_observed >= 11s; }, 14s);
    require(probe.launch.value("state").toString() == "running"
        && probe.launch.value("phase").toString() == "draining"
        && probe.launch.value("error").isNull(), "A finite drain budget became an unsafe terminal failure");
    require(!probe.launch.value("canCancel").toBool(true) && !probe.launch.value("reportReady").toBool(),
            "Held pipes advertised cancellation or a ready report");
    probe.preparation_terminal(probe.launch, TaskStatus::Succeeded);
    auto target = fixture.scope();
    target.insert("launchId", id);
    require(probe.command(ui::UiAction::LaunchGame, fixture.scope()).error.code == "busy",
            "Drain timeout released the same resource root before dual EOF");
    require(probe.command(ui::UiAction::LaunchGame, nested_scope).error.code == "busy",
            "Drain timeout admitted a different instance with an overlapping nested resource root");
    auto rename = fixture.scope();
    rename.insert("fullName", "MustNotChange");
    require(probe.command(ui::UiAction::RenameInstance, rename).error.code == "busy",
            "Held-pipe launch released its instance mutation lease");
    require(probe.command(ui::UiAction::RenameRoot, {{"root", qpath(fixture.game)}, {"label", "MustNotChange"}}).error.code == "busy",
            "Held-pipe launch released root-exclusive mutation protection");
    require(probe.command(ui::UiAction::ClearLaunchError, target).error.isError(),
            "A still-draining launch was cleared or retired");
    const auto cancelled = probe.success(ui::UiAction::CancelLaunch, {{"launchId", id}});
    require(!cancelled.value("cancelled").toBool(true), "CancelLaunch stopped a pipe-owning descendant");
    probe.success(ui::UiAction::PatchSettings, {{"section", "game"}, {"patch", QJsonObject{{"versionIsolation", true}}}});
    const auto finished = probe.finished(id);
    require(finished.value("state").toString() == "failed" && finished.value("exitCode").toInt(-1) == 17
        && finished.value("reportReady").toBool(), "Natural safe dual EOF did not restore failure report readiness");
    require(finished.value("reportTarget").toObject() == target, "Drain recovery lost frozen launch scope");
    probe.until([&] {
        return std::count(probe.lines.begin(), probe.lines.end(), QStringLiteral("descendant-stdout-tail")) == 1
            && std::count(probe.lines.begin(), probe.lines.end(), QStringLiteral("descendant-stderr-tail")) == 1;
    });
    require(fixture.start_count() == 1 && probe.started.size() == 1, "Held-pipe retries spawned a second game");
    probe.preparation_terminal(finished, TaskStatus::Succeeded);
    auto favorite = fixture.scope();
    favorite.insert("favorite", true);
    probe.success(ui::UiAction::SetInstanceFavorite, favorite);
    probe.success(ui::UiAction::RenameRoot, {{"root", qpath(fixture.game)}, {"label", "Finished fixture"}});
    probe.success(ui::UiAction::ClearLaunchError, target);
}


void cli_waits_for_onboarding_and_dispatches_once(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(executable, parent, "OuterID", "hold", true, true);
    fixture.options.startup_arguments = {"--launch-instance", fixture.outer_id, "--game-root", instances::path_to_utf8(fixture.game)};
    Probe probe(fixture.options);
    const auto bootstrap = probe.query(ui::UiQuery::Bootstrap);
    require(bootstrap.value("onboarding").toObject().value("needsEula").toBool(), "Fresh CLI launch skipped EULA");
    require(fixture.start_count() == 0 && probe.started.empty(), "CLI spawned before onboarding completed");
    probe.success(ui::UiAction::AcceptEula, {{"checked", true}, {"eulaVersion", QString::fromUtf8(instances::eula_version.data())}});
    probe.success(ui::UiAction::AcknowledgeChangelog, {{"version", QString::fromStdString(app::build_version())}});
    require(!probe.query(ui::UiQuery::Bootstrap).value("startupReady").toBool(), "CLI Bootstrap bypassed explicit first-use finish");
    require(fixture.start_count() == 0 && probe.started.empty(), "CLI spawned before explicit FinishOnboarding");
    probe.success(ui::UiAction::FinishOnboarding);
    probe.until([&] { return !probe.started.empty(); });
    const auto id = *probe.started.begin();
    probe.success(ui::UiAction::FinishOnboarding);
    const auto ready = probe.query(ui::UiQuery::Bootstrap);
    require(ready.value("startupReady").toBool(), "CLI dispatch did not finish its onboarding gate");
    const auto finished = probe.finished(id);
    require(finished.value("state").toString() == "exited", "CLI did not use the production named launch path");
    probe.preparation_terminal(finished, TaskStatus::Succeeded);
    require(fixture.start_count() == 1 && probe.started.size() == 1, "Repeated startup consumers spawned twice");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        require(argc == 3 || argc == 4, "Expected controlled Java executable, fixture parent, and optional --case selector");
        const auto executable = fs::absolute(instances::path_from_utf8(argv[1]));
        const auto parent = fs::absolute(instances::path_from_utf8(argv[2]));
        const std::string selected = argc == 4 ? argv[3] : "--case=all";
        require(selected == "--case=all" || selected == "--case=held-pipe" || selected == "--case=consent"
            || selected == "--case=quick-exit" || selected == "--case=prepare-cancel", "Unknown production launch test case");
        const auto run = [](std::string_view name, const auto& test) {
            const auto start = std::chrono::steady_clock::now();
            std::cout << "BEGIN: " << name << '\n' << std::flush;
            test();
            const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            std::cout << "PASS: " << name << " elapsedMs=" << duration.count() << '\n' << std::flush;
        };
        if (selected == "--case=all" || selected == "--case=held-pipe")
            run("AppServices-held-pipe-16000ms", [&] { descendant_timeout_retains_resource_protection(executable, parent); });
        if (selected == "--case=all" || selected == "--case=consent")
            run("AppServices-manual-consent-optional-launchId", [&] { manual_launch_requires_complete_consent(executable, parent); });
        if (selected == "--case=all" || selected == "--case=quick-exit")
            run("AppServices-quick-exit-terminal-capacity", [&] { fast_exit_stays_terminal_under_query_pressure(executable, parent); });
        if (selected == "--case=all" || selected == "--case=prepare-cancel")
            run("AppServices-canonical-preparation-cancel", [&] { preparation_cancel_controls_the_canonical_task(executable, parent); });
        if (selected == "--case=all") {
            run("AppServices-running-cancel-preparation-only", [&] { running_cancel_is_preparation_only(executable, parent); });
            run("AppServices-resolved-named-client-skin", [&] { real_skin_uses_resolved_named_client(executable, parent); });
            run("AppServices-frozen-full-ZIP", [&] { failure_scope_and_full_zip_stay_frozen(executable, parent); });
            run("AppServices-unsafe-output-quarantine", [&] { unsafe_output_never_becomes_ready(executable, parent); });
            run("AppServices-CLI-onboarding-once", [&] { cli_waits_for_onboarding_and_dispatches_once(executable, parent); });
        }
        std::cout << "PASS: Palladium actual AppServices launch pipeline " << selected << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
