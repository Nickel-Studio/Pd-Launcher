#pragma once

#include "app/AppServices.hpp"
#include "app/application_state.hpp"
#include "app/service/public_dto.hpp"
#include "core/config/atomic_file.hpp"
#include "core/packs/native_services.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace nickel::app {

struct AppServicesTestProbe {
    static std::shared_ptr<ApplicationState> state(AppServices& services) { return services.state_; }
    static bool submit_work(AppServices& services, ApplicationState::BackgroundWork work) {
        return services.submitWork(std::move(work));
    }
    static std::size_t monitor_registrations(const AppServices& services) {
        return services.monitor_pending_->load();
    }
    static ApplicationState::StateSink sink(const AppServices& services) {
        std::lock_guard lock(services.state_->sink_mutex_);
        return services.state_->sink_;
    }
    static void close_operations(AppServices& services) { services.state_->operations_->close(); }
};

namespace test {

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline void require_cleanup_result(const Json& value) {
    require(value.is_object(), "Cleanup omitted its canonical result object");
    for (const auto* name : {"freedBytes", "removedCount", "failedCount", "missingCount", "conflictCount", "cancelledCount"}) {
        const auto& field = value.at(name);
        require(field.is_number_unsigned() || (field.is_number_integer() && field.get<std::int64_t>() >= 0),
            "Cleanup result changed a canonical count field");
    }
    require(value.at("cancelled").is_boolean() && value.at("locationConflict").is_boolean(),
        "Cleanup result omitted its canonical cancellation or location status");
}

template<class Predicate>
void wait_until(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() >= timeout.count()) throw std::runtime_error("Production fixture timed out");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

class ProductionFixture final {
public:
    explicit ProductionFixture(std::vector<std::string> startup_arguments = {},
                               std::function<void(const std::filesystem::path&)> prepare_executable = {})
        : directory_(QDir::tempPath() + QStringLiteral("/nickel-app-production-XXXXXX")) {
        require(directory_.isValid(), "Cannot create an owned production fixture");
        root = nickel::path_from_utf8(directory_.path().toUtf8().toStdString());
        const auto executable = root / "bin" / "fixture.exe";
        std::filesystem::create_directories(executable.parent_path());
        std::filesystem::create_directories(root / "system");
        std::filesystem::create_directories(root / "data");
        // Prepare negative disk inputs before the real constructor captures its OS-only capability.
        if (prepare_executable) prepare_executable(executable);
        AppServicesOptions options;
        options.paths = {executable, root / "system", root / "data", false};
        options.executable = executable;
        options.startup_arguments = std::move(startup_arguments);
        services = std::make_unique<AppServices>(std::move(options));
        state = AppServicesTestProbe::state(*services);
        config = state->repository.root();
        game = state->paths.default_game_directory();
        require(config == root / "bin" / "Nickel", "Fixture selected an unexpected config root");
        require(game == root / "bin" / ".minecraft", "Fixture selected an unexpected game root");
        QObject::connect(services.get(), &ui::UiServices::queryReady, services.get(),
            [this](const ui::UiReply& reply) {
                require(QThread::currentThread() == services->thread(), "Query delivered off the QObject thread");
                queries.emplace(reply.id, reply);
                ++query_count;
            });
        QObject::connect(services.get(), &ui::UiServices::commandFinished, services.get(),
            [this](const ui::UiReply& reply) {
                require(QThread::currentThread() == services->thread(), "Action delivered off the QObject thread");
                commands.emplace(reply.id, reply);
            });
    }

    ~ProductionFixture() {
        services.reset();
        state.reset();
    }

    ProductionFixture(const ProductionFixture&) = delete;
    ProductionFixture& operator=(const ProductionFixture&) = delete;

    static ui::RequestId next_id() {
        static std::atomic<ui::RequestId> next{1000};
        return next.fetch_add(1);
    }

    ui::UiReply query(ui::UiQuery kind, const Json& scope = Json::object()) {
        const auto id = next_id();
        services->request(id, kind, to_public_json(scope).toObject());
        return take_query(id);
    }

    ui::UiReply take_query(ui::RequestId id) {
        wait_until([&] { return queries.contains(id); });
        auto reply = queries.at(id);
        queries.erase(id);
        return reply;
    }

    ui::UiReply action(ui::UiAction kind, const Json& arguments = Json::object()) {
        const auto id = next_id();
        services->submit(id, kind, to_public_json(arguments).toObject());
        wait_until([&] { return commands.contains(id); });
        auto reply = commands.at(id);
        commands.erase(id);
        return reply;
    }

    Json query_ok(ui::UiQuery kind, const Json& scope = Json::object()) {
        const auto reply = query(kind, scope);
        if (reply.error.isError()) throw std::runtime_error(reply.error.code.toStdString());
        return from_public_json(reply.data);
    }

    Json action_ok(ui::UiAction kind, const Json& arguments = Json::object()) {
        const auto reply = action(kind, arguments);
        if (reply.error.isError()) throw std::runtime_error(reply.error.code.toStdString());
        return from_public_json(reply.data);
    }

    void finish_onboarding() {
        const auto status = query_ok(ui::UiQuery::Onboarding);
        action_ok(ui::UiAction::AcceptEula, {{"checked", true}, {"eulaVersion", status.at("eulaVersion")}});
        action_ok(ui::UiAction::AcknowledgeChangelog, {{"version", status.at("currentVersion")}});
        action_ok(ui::UiAction::FinishOnboarding);
        require(query_ok(ui::UiQuery::Onboarding).at("startupReady").get<bool>(), "Startup did not complete");
    }

    Json add_legacy_instance(const std::string& id = "1.20.1") {
        instances::validate_name(id);
        const auto directory = game / "versions" / nickel::path_from_utf8(id);
        std::filesystem::create_directories(directory);
        const bool version_like = !id.empty() && id.front() >= '0' && id.front() <= '9';
        const std::string minecraft_version = version_like ? id : "1.20.1";
        const Json descriptor{{"id", id}, {"type", "release"}, {"clientVersion", minecraft_version},
            {"mainClass", "net.minecraft.client.main.Main"}, {"libraries", Json::array()},
            {"minecraftArguments", "--username ${auth_player_name} --version ${version_name}"},
            {"javaVersion", {{"majorVersion", 21}}}};
        atomic_write_text(directory / nickel::path_from_utf8(id + ".json"), descriptor.dump());
        const auto backend = packs::native_archive_backend();
        const std::vector<packs::ArchiveInput> members{
            {"META-INF/MANIFEST.MF", std::nullopt, "Manifest-Version: 1.0\r\n\r\n"}};
        backend->write(directory / nickel::path_from_utf8(id + ".jar"), members, {}, {});
        return {{"root", instances::path_to_utf8(game)}, {"versionId", id}};
    }

    std::filesystem::path root;
    std::filesystem::path config;
    std::filesystem::path game;
    std::unique_ptr<AppServices> services;
    std::shared_ptr<ApplicationState> state;
    std::map<ui::RequestId, ui::UiReply> queries;
    std::map<ui::RequestId, ui::UiReply> commands;
    std::size_t query_count = 0;

private:
    QTemporaryDir directory_;
};

} // namespace test
} // namespace nickel::app
