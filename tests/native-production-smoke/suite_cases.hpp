#pragma once

#include "qt_probe.hpp"
#include "app/AppServices.hpp"
#include "core/common/json.hpp"

#ifdef NICKEL_SMOKE_SAME_GRAPH
#include "tests/app/production_fixture.hpp"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#endif

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>

#include <algorithm>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace nickel::production_smoke {
namespace fs = std::filesystem;
using Factory = std::function<std::unique_ptr<app::AppServices>(const Json&)>;

inline fs::path path_of(const std::string& value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}

inline QString qstr(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

inline std::string path_text(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

inline void check(bool value, const char* description) {
    if (!value) {
        throw std::runtime_error(description);
    }
}

inline QByteArray read_fixture(const fs::path& path, qsizetype limit = 8 * 1024 * 1024) {
    QFile input(qstr(path_text(path)));
    check(input.open(QIODevice::ReadOnly), "fixture file read failed");
    check(input.size() <= limit, "fixture read exceeded quota");
    return input.readAll();
}

inline Json read_disk(const fs::path& path) {
    const auto bytes = read_fixture(path);
    return Json::parse(bytes.constData(), bytes.constData() + bytes.size());
}

inline std::string file_hash(const fs::path& path) {
    QFile input(qstr(path_text(path)));
    check(input.open(QIODevice::ReadOnly), "hash input read failed");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    check(digest.addData(&input), "hash input read failed during hashing");
    return digest.result().toHex().toStdString();
}

inline bool same_path(const QString& value, const fs::path& expected) {
    auto normalize = [](QString text) {
        text = QDir::cleanPath(QDir::fromNativeSeparators(text));
#ifdef _WIN32
        text = text.toCaseFolded();
#endif
        return text;
    };
    return normalize(value) == normalize(qstr(path_text(expected)));
}

struct CaseResult {
    std::string id;
    std::string verdict = "BLOCKED";
    std::string reason;
    std::string harnessStage = "not-started";
    bool executed = false;
    Json checks = Json::object();
    Json observations = Json::array();
    Json requests = Json::array();
    Json facts = Json::object();
    Json requiredQueries = Json::array();
    Json requiredActions = Json::array();

    void passed(const std::string& key) {
        check(checks.contains(key), "attempted to satisfy an undeclared matrix check");
        checks[key] = true;
    }

    void record(const ui::UiReply& reply) {
        check(observations.size() < 2048, "case reply evidence exceeded its quota");
        observations.push_back(Json::parse(QJsonDocument(public_reply(reply)).toJson(QJsonDocument::Compact).constData()));
    }

    void record_request(const QJsonObject& request) {
        check(requests.size() < 2048, "case request evidence exceeded its quota");
        requests.push_back(Json::parse(QJsonDocument(request).toJson(QJsonDocument::Compact).constData()));
    }

    [[nodiscard]] Json missing_operations() const {
        Json missing = Json::array();
        const auto scan = [&](const Json& required, const std::string& channel) {
            for (const auto& name : required) {
                const auto found = std::any_of(requests.begin(), requests.end(), [&](const auto& request) {
                    return request.at("channel").template get<std::string>() == channel
                        && request.at("name").template get<std::string>() == name.template get<std::string>();
                });
                if (!found) missing.push_back({{"channel", channel}, {"name", name}});
            }
        };
        scan(requiredQueries, "query");
        scan(requiredActions, "command");
        return missing;
    }
};

class Suite final {
public:
    Suite(Factory factory, Json fixtures, Json matrix)
        : factory_(std::move(factory)), fixtures_(std::move(fixtures)), matrix_(std::move(matrix)) {
        check(fixtures_.at("kind") == "synthetic-production-fixtures", "fixture manifest kind mismatch");
        check(fixtures_.at("owner") == "production-smoke", "fixture owner mismatch");
#ifdef NICKEL_SMOKE_SAME_GRAPH
        check(fixtures_.at("fixtureRole") == "runtime-instance", "immutable fixture templates must never be passed to AppServices");
#endif
        root_ = path_of(fixtures_.at("root").get<std::string>());
        check(path_text(root_).size() >= 310, "production fixture long path was shortened");
        const auto owner = read_disk(root_ / "fixture-owner.json");
        check(owner.at("token") == fixtures_.at("token"), "fixture owner token mismatch");
        for (const auto& row : fixtures_.at("files")) {
            const auto relative = path_of(row.at("path").get<std::string>());
            check(!relative.is_absolute(), "fixture manifest path escaped");
            for (const auto& part : relative) {
                check(part != ".." && part != ".", "fixture manifest has a dot path");
            }
            const auto file = root_ / relative;
            check(fs::is_regular_file(fs::symlink_status(file)), "fixture file became a link/non-file");
            const auto actual_size = fs::file_size(file);
            const auto expected_size = row.at("size").get<std::uintmax_t>();
            if (actual_size != expected_size) {
                throw std::runtime_error("runtime fixture size changed before AppServices: " + row.at("path").get<std::string>()
                    + "; expected " + std::to_string(expected_size) + ", actual " + std::to_string(actual_size));
            }
            check(file_hash(file) == row.at("sha256").get<std::string>(), "fixture source hash changed before run");
        }
        for (const auto& item : matrix_.at("cases")) {
            CaseResult result;
            result.id = item.at("id").get<std::string>();
            result.reason = "not executed: controlled production contract dependency";
            result.requiredQueries = item.at("queries");
            result.requiredActions = item.at("actions");
            for (const auto& key : item.at("checks")) {
                result.checks[key.get<std::string>()] = false;
            }
            check(results_.emplace(result.id, std::move(result)).second, "duplicate matrix case id");
        }
    }

    void run() {
        case_run("startup-root", [&] { startup_roots(); });
        case_run("startup-once", [&] { startup_once(); });
        case_run("settings-transaction", [&] { settings_transaction(); });
        case_run("accounts-offline", [&] { accounts_transaction(); });
        case_run("named-instance", [&] { named_instance(); });
        case_run("version-settings", [&] { version_settings(); });
        case_run("resource-import", [&] { resource_import(); });
        case_run("cleanup-identity", [&] { cleanup(); });
        case_run("download-lifecycle", [&] { downloads(); });
        case_run("nonce-stale", [&] { runtime_nonce(); });
        case_run("shutdown-queue", [&] { queued_destruction(); });
    }

    [[nodiscard]] Json report(std::string manifestHash, std::string binaryHash) const {
        Json cases = Json::array();
        for (const auto& [id, row] : results_) {
            static_cast<void>(id);
            cases.push_back({{"id", row.id}, {"verdict", row.verdict}, {"reason", row.reason},
                {"executed", row.executed}, {"harnessStage", row.harnessStage},
                {"checks", row.checks}, {"observations", row.observations},
                {"requests", row.requests}, {"facts", row.facts},
                {"missingOperations", row.missing_operations()}});
        }
        return {{"schema", 1}, {"kind", "production-appservices-fixture"},
                {"manifestSha256", std::move(manifestHash)}, {"binarySha256", std::move(binaryHash)},
                {"realAppServices", true}, {"realQObject", true}, {"cases", std::move(cases)},
                {"requestEvidence", "captured immediately before awaiting the real AppServices reply"},
                {"fixtureRoot", path_text(root_)}, {"fixtureRootCharacters", path_text(root_).size()},
                {"fullMatrixPassed", full_pass()}};
    }

    [[nodiscard]] bool full_pass() const {
        for (const auto& [id, row] : results_) {
            static_cast<void>(id);
            if (row.verdict != "PASS") {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool has_failure() const {
        for (const auto& [id, row] : results_) {
            static_cast<void>(id);
            if (row.verdict == "FAIL") return true;
        }
        return false;
    }

private:
    const Json& scenario(std::string_view name = "accepted") const {
        for (const auto& candidate : fixtures_.at("scenarios")) {
            if (candidate.at("name").get<std::string>() == std::string(name)) {
                return candidate;
            }
        }
        throw std::runtime_error("fixture scenario missing");
    }

    QJsonObject target(const Json& fixture) const {
        return {{"root", qstr(fixture.at("expectedGameRoot").get<std::string>())},
                {"versionId", qstr(fixture.at("namedVersionId").get<std::string>())}};
    }

    fs::path config(const Json& fixture) const { return path_of(fixture.at("expectedConfigRoot").get<std::string>()); }
    fs::path named(const Json& fixture) const { return path_of(fixture.at("expectedNamedDirectory").get<std::string>()); }

    void public_only(const QByteArray& text) const {
        for (const auto& sentinel : fixtures_.at("syntheticRedactionSentinels")) {
            check(!text.contains(QByteArray::fromStdString(sentinel.get<std::string>())), "public request/DTO/error leaked synthetic credentials");
        }
    }

    void public_only(const ui::UiReply& reply) const {
        public_only(QJsonDocument(public_reply(reply)).toJson(QJsonDocument::Compact));
    }

    void submitted(Probe& probe, ui::RequestId id, bool command) {
        const auto request = probe.submitted(id, command);
        public_only(QJsonDocument(request).toJson(QJsonDocument::Compact));
        current_->record_request(request);
        current_->harnessStage = "awaiting-" + request.value("name").toString().toStdString();
    }

    ui::UiReply observed(Probe& probe, ui::RequestId id, bool command) {
        auto reply = probe.await(id, command);
        public_only(reply);
        current_->record(reply);
        current_->harnessStage = "asserting-reply";
        return reply;
    }

    ui::UiReply query(Probe& probe, ui::UiQuery kind, const QJsonObject& scope = {}, bool expectError = false) {
        const auto id = probe.query(kind, scope);
        submitted(probe, id, false);
        auto reply = observed(probe, id, false);
        if (expectError) check(reply.error.isError(), "invalid production query was accepted");
        else require_ok(reply);
        return reply;
    }

    ui::UiReply action(Probe& probe, ui::UiAction kind, const QJsonObject& arguments = {}, bool expectError = false) {
        const auto id = probe.submit(kind, arguments);
        submitted(probe, id, true);
        auto reply = observed(probe, id, true);
        if (expectError) check(reply.error.isError(), "invalid production action was accepted");
        else require_ok(reply);
        return reply;
    }

    void require_code(const ui::UiReply& reply, const QString& expected, const char* description) const {
        if (reply.error.code != expected) {
            throw std::runtime_error(std::string(description) + ": expected " + expected.toStdString()
                + ", actual " + reply.error.code.toStdString());
        }
    }

    bool no_game_before_finish(app::AppServices& services, Probe& probe, const std::string& phase) {
        probe.drain(50);
        Json observation = {{"observationOrigin", "actual service state after a bounded 50 ms event drain"},
            {"productionStage", nullptr}, {"rawException", nullptr}, {"processSpawnHistoryObserved", false}};
#ifdef NICKEL_SMOKE_SAME_GRAPH
        const auto state = app::AppServicesTestProbe::state(services);
        std::size_t records = 0;
        std::size_t sessions = 0;
        {
            std::lock_guard lock(state->state_mutex);
            records = state->launches.size();
            for (const auto& [id, launch] : state->launches) {
                static_cast<void>(id);
                if (launch->game) ++sessions;
            }
            observation["launchRecords"] = records;
            observation["registeredGameSessions"] = sessions;
            observation["startupLaunchClaimed"] = state->startup_launch_claimed;
            observation["startupReady"] = state->startup_ready;
        }
#ifdef _WIN32
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        check(snapshot != INVALID_HANDLE_VALUE, "cannot observe the real process snapshot for pre-Finish noGame");
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        Json children = Json::array();
        bool ownProcessPresent = false;
        const auto ownPid = GetCurrentProcessId();
        if (Process32FirstW(snapshot, &entry)) {
            do {
                ownProcessPresent = ownProcessPresent || entry.th32ProcessID == ownPid;
                if (entry.th32ParentProcessID == ownPid) children.push_back({{"pid", entry.th32ProcessID},
                    {"parentPid", entry.th32ParentProcessID}, {"birthTime", nullptr}});
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        observation["processObservation"] = "actual Toolhelp32 parent-PID snapshot; not a cumulative spawn count";
        observation["observerPid"] = ownPid;
        observation["ownedDirectChildren"] = children;
        observation["observerPresentInSnapshot"] = ownProcessPresent;
        current_->facts["noGame-" + phase] = observation;
        check(ownProcessPresent && children.empty(), "owned child process observed before explicit FinishOnboarding");
#else
        observation["ownedDirectChildren"] = nullptr;
        current_->facts["noGame-" + phase] = observation;
#endif
        check(records == 0 && sessions == 0, "real AppServices registered launch/GameSession before explicit FinishOnboarding");
        return true;
#else
        static_cast<void>(services);
        observation["launchRecords"] = nullptr;
        observation["registeredGameSessions"] = nullptr;
        observation["ownedDirectChildren"] = nullptr;
        current_->facts["noGame-" + phase] = observation;
        return false;
#endif
    }


    template<class Work>
    void case_run(const std::string& id, Work work) {
        current_ = &results_.at(id);
        current_->executed = true;
        current_->harnessStage = "constructing-fixture";
        current_->reason.clear();
        try {
            work();
            bool complete = current_->missing_operations().empty();
            for (const auto& value : current_->checks) {
                complete = complete && value == true;
            }
            current_->verdict = complete ? "PASS" : "BLOCKED";
            current_->harnessStage = "subset-completed";
            if (!complete) {
                current_->reason = "production subset ran; remaining checks or declared port calls not yet executed";
            }
        } catch (const std::exception& error) {
            current_->verdict = "FAIL";
            current_->reason = error.what();
        }
        current_ = nullptr;
    }

    void startup_roots() {
        for (const auto name : {"accepted", "system-priority", "corrupt-proof"}) {
            const auto& fixture = scenario(name);
            auto services = factory_(fixture);
            Probe probe(*services);
            const auto bootstrap = query(probe, ui::UiQuery::Bootstrap).data.toObject();
            const auto roots = query(probe, ui::UiQuery::InstanceRoots).data.toObject();
            const auto expected = path_of(fixture.at("expectedGameRoot").get<std::string>());
            check(same_path(roots.value("active").toString(), expected), "InstanceRoots active disagrees with whole-root game selection");
            check(same_path(bootstrap.value("roots").toObject().value("active").toString(), expected), "Bootstrap roots differ from production InstanceRoots");
#ifdef NICKEL_SMOKE_SAME_GRAPH
            const auto state = app::AppServicesTestProbe::state(*services);
            check(same_path(qstr(path_text(state->repository.root())), config(fixture)), "real AppServices selected a different whole config root");
            check(same_path(qstr(path_text(state->paths.default_game_directory())), expected), "real PathResolver disagrees with the active registry game root");
#endif
            const auto accounts = query(probe, ui::UiQuery::Accounts).data.toObject();
            check(!QJsonDocument(accounts).toJson().contains("must-not-merge"), "configuration roots were merged");
            query(probe, ui::UiQuery::Settings, {{"section", "app"}});
            query(probe, ui::UiQuery::Instances, {{"root", qstr(path_text(expected))}});
            current_->facts[std::string("root-") + name] = {{"expectedGameRoot", path_text(expected)},
                {"settingsSha256", file_hash(config(fixture) / "settings.json")}};
            for (const auto& proof : fixture.at("foreignRootProofs")) {
                check(file_hash(path_of(proof.at("path").get<std::string>())) == proof.at("sha256").get<std::string>(), "unselected whole-root proof was changed");
            }
            if (fixture.value("corruptProof", false)) {
                const auto file = path_of(fixture.at("corruptProofFile").get<std::string>());
                check(file_hash(file) == fixture.at("corruptProofSha256").get<std::string>(), "ignored corrupt game proof changed");
                current_->passed("corrupt-proof");
            }
            probe.drain();
        }
        current_->passed("whole-root");
        current_->passed("game-root-consistency");
        {
            const auto& fixture = scenario("system-priority");
            auto services = factory_(fixture);
            Probe probe(*services);
            const auto bootstrap = query(probe, ui::UiQuery::Bootstrap).data.toObject();
            check(same_path(bootstrap.value("roots").toObject().value("active").toString(),
                path_of(fixture.at("expectedGameRoot").get<std::string>())), "whole-root selection changed on restart");
            current_->passed("restart");
        }
        const auto& fixture = scenario("unaccepted");
        auto services = factory_(fixture);
        Probe probe(*services);
        const auto bootstrap = query(probe, ui::UiQuery::Bootstrap).data.toObject();
        check(!bootstrap.value("startupReady").toBool(), "unaccepted startup reported ready");
        const auto before = file_hash(config(fixture) / "settings.json");
        const auto gated = action(probe, ui::UiAction::LaunchGame, target(fixture), true);
        probe.drain();
        const auto after = file_hash(config(fixture) / "settings.json");
        current_->facts["startup-gate"] = {{"settingsBeforeSha256", before}, {"settingsAfterSha256", after},
            {"actualCode", gated.error.code.toStdString()}, {"expectedCode", "onboarding_required"},
            {"stageOrigin", "harness assertion only"}, {"rawExceptionObserved", false}};
        check(after == before, "startup-gated launch changed settings");
        require_code(gated, QStringLiteral("onboarding_required"), "LaunchGame startup public-code mismatch");
        current_->passed("startup-gate");
    }

    void startup_once() {
        const auto& fixture = scenario("unaccepted");
        const auto record = config(fixture) / "onboarding.json";
        const auto before = file_hash(record);
        bool noGameObserved = true;
        {
            auto services = factory_(fixture);
            Probe probe(*services);
            const auto initial = query(probe, ui::UiQuery::Onboarding).data.toObject();
            check(initial.value("needsEula").toBool() && !initial.value("startupReady").toBool(), "first-use agreement was bypassed");
            noGameObserved = no_game_before_finish(*services, probe, "unaccepted") && noGameObserved;
            Json durableBefore = Json::object();
            for (const auto* file : {"onboarding.json", "settings.json", "accounts.json", "active_account.json", "game.json", "instances.json"}) {
                durableBefore[file] = file_hash(config(fixture) / file);
            }
            const auto firstId = probe.submit(ui::UiAction::FinishOnboarding);
            submitted(probe, firstId, true);
            const auto firstFinish = observed(probe, firstId, true);
            const auto afterFirst = query(probe, ui::UiQuery::Onboarding).data.toObject();
            Json durableAfter = Json::object();
            for (const auto& [file, digest] : durableBefore.items()) {
                static_cast<void>(digest);
                durableAfter[file] = file_hash(config(fixture) / file);
            }
            noGameObserved = no_game_before_finish(*services, probe, "first-Finish-rejected") && noGameObserved;
            current_->facts["first-FinishOnboarding"] = {
                {"actualCode", firstFinish.error.code.toStdString()}, {"expectedCode", "onboarding_required"},
                {"beforeSha256", durableBefore}, {"afterSha256", durableAfter},
                {"startupReadyAfter", afterFirst.value("startupReady").toBool()},
                {"productionStage", nullptr}, {"rawException", nullptr}};
            check(durableAfter == durableBefore, "first FinishOnboarding changed durable state before consent");
            check(!afterFirst.value("startupReady").toBool() && afterFirst.value("needsEula").toBool(), "first FinishOnboarding provisionally accepted consent");
            require_code(firstFinish, QStringLiteral("onboarding_required"), "first FinishOnboarding public-code mismatch");
            current_->passed("first-finish-gate");
            const auto rejected = action(probe, ui::UiAction::RejectEula).data.toObject();
            check(rejected.value("exiting").toBool() && !rejected.value("startupReady").toBool(), "RejectEula did not leave startup blocked");
            const auto afterReject = action(probe, ui::UiAction::FinishOnboarding, {}, true);
            require_code(afterReject, QStringLiteral("onboarding_required"), "rejected FinishOnboarding public-code mismatch");
            noGameObserved = no_game_before_finish(*services, probe, "Eula-rejected") && noGameObserved;
            check(file_hash(record) == before, "rejecting EULA durably accepted it");
        }
        current_->passed("reject");
        {
            auto services = factory_(fixture);
            Probe probe(*services);
            const auto initial = query(probe, ui::UiQuery::Onboarding).data.toObject();
            check(!initial.value("exiting").toBool(), "a prior object leaked rejection into the next lifecycle");
            action(probe, ui::UiAction::AcceptEula,
                   {{"checked", false}, {"eulaVersion", initial.value("eulaVersion")}}, true);
            check(file_hash(record) == before, "unchecked agreement changed durable state");
            const auto gate = [&](const std::string& phase, const QJsonObject& status) {
                check(status.value("startupReady").isBool() && !status.value("startupReady").toBool(), "consent phase completed startup before explicit FinishOnboarding");
                const auto committed = file_hash(record);
                const auto id = probe.submit(ui::UiAction::LaunchGame, target(fixture));
                submitted(probe, id, true);
                const auto reply = observed(probe, id, true);
                noGameObserved = no_game_before_finish(*services, probe, phase) && noGameObserved;
                check(file_hash(record) == committed, "pre-Finish gated LaunchGame changed durable consent");
                require_code(reply, QStringLiteral("onboarding_required"), "pre-Finish LaunchGame public-code mismatch");
            };
            action(probe, ui::UiAction::AcceptEula,
                   {{"checked", true}, {"eulaVersion", initial.value("eulaVersion")}});
            const auto accepted = query(probe, ui::UiQuery::Onboarding).data.toObject();
            check(!accepted.value("needsEula").toBool() && accepted.value("needsChangelog").toBool(), "AcceptEula unexpectedly acknowledged the changelog");
            gate("AcceptEula", accepted);
            action(probe, ui::UiAction::AcknowledgeChangelog, {{"version", initial.value("currentVersion")}});
            const auto acknowledged = query(probe, ui::UiQuery::Onboarding).data.toObject();
            check(!acknowledged.value("needsEula").toBool() && !acknowledged.value("needsChangelog").toBool(), "Ack did not durably record both consent phases");
            gate("AcknowledgeChangelog", acknowledged);
            const auto bootstrapped = query(probe, ui::UiQuery::Bootstrap).data.toObject();
            check(bootstrapped.value("startupReady").isBool() && !bootstrapped.value("startupReady").toBool(), "Bootstrap silently completed this session's explicit Finish gate");
            gate("Bootstrap-before-Finish", query(probe, ui::UiQuery::Onboarding).data.toObject());
            current_->passed("consent-three-phase");
            if (noGameObserved) current_->passed("no-game-before-finish");
            const auto saved = read_disk(record);
            check(saved.at("eulaVersion") == initial.value("eulaVersion").toString().toStdString(), "accepted agreement version was not durable");
            check(saved.at("eulaAcceptedAt").get<std::uint64_t>() > 0, "accepted agreement timestamp was not durable");
            check(saved.at("changelogSeenVersion") == initial.value("currentVersion").toString().toStdString(), "acknowledged changelog was not durable");
            const auto finished = action(probe, ui::UiAction::FinishOnboarding).data.toObject();
            check(finished.value("startupReady").toBool() && finished.value("startupState").toString() == QStringLiteral("completed"), "accepted startup did not complete");
            const auto committed = file_hash(record);
            action(probe, ui::UiAction::FinishOnboarding);
            query(probe, ui::UiQuery::Bootstrap);
            check(file_hash(record) == committed, "repeated startup changed the durable agreement");
        }
        auto services = factory_(fixture);
        Probe probe(*services);
        const auto restarted = query(probe, ui::UiQuery::Bootstrap).data.toObject();
        check(restarted.value("startupReady").toBool(), "accepted agreement did not survive AppServices restart");
        current_->passed("accept-durable");
        // CLI targeting, portable import and spawn-once remain separately required checks.
    }

    void settings_transaction() {
        const auto& fixture = scenario();
        const auto before = read_disk(config(fixture) / "settings.json");
        const auto unknown = before.at("future");
        {
            auto services = factory_(fixture);
            Probe probe(*services);
            action(probe, ui::UiAction::PatchSettings, {{"section", "app"},
                {"patch", QJsonObject{{"offlineUsername", "ProductionSmokeName"}, {"titlebarTint", false}}}});
            const auto saved = read_disk(config(fixture) / "settings.json");
            check(saved.at("future") == unknown, "PatchSettings lost unknown fields/exact integers");
            check(saved.at("offlineUsername") == "ProductionSmokeName", "successful PatchSettings was not durable");
            current_->passed("unknown-fields");
            current_->passed("integer-precision");
            const auto committed = file_hash(config(fixture) / "settings.json");
            action(probe, ui::UiAction::PatchSettings, {{"section", "invalid-section"},
                {"patch", QJsonObject{{"offlineUsername", "InvalidSmokeName"}}}}, true);
            check(file_hash(config(fixture) / "settings.json") == committed, "failed PatchSettings changed persisted state");
            current_->passed("failure-no-state");
        }
        auto services = factory_(fixture);
        Probe probe(*services);
        const auto state = query(probe, ui::UiQuery::Settings, {{"section", "app"}}).data.toObject();
        const auto values = state.value("values").isObject() ? state.value("values").toObject() : state;
        check(values.value("offlineUsername").toString() == "ProductionSmokeName", "restarted settings did not match persistence");
        current_->passed("restart");
        // Persist-before-budget needs observable real transfer barriers and is deliberately not inferred.
    }

    void accounts_transaction() {
        const auto& fixture = scenario();
        {
            auto services = factory_(fixture);
            Probe probe(*services);
            query(probe, ui::UiQuery::Accounts);
            action(probe, ui::UiAction::SetAccountType, {{"type", "offline"}});
            action(probe, ui::UiAction::SetActiveAccount, {{"id", "offline-two"}, {"type", "offline"}});
            check(read_disk(config(fixture) / "active_account.json").at("id") == "offline-two", "active account reply preceded durability");
            const auto publicState = query(probe, ui::UiQuery::Accounts);
            public_only(publicState);
            current_->passed("public-no-secrets");
            action(probe, ui::UiAction::RemoveAccount, {{"id", "offline-one"}});
            const auto stored = read_disk(config(fixture) / "accounts.json");
            for (const auto& account : stored) {
                check(account.at("id") != "offline-one", "RemoveAccount did not persist");
            }
            check(read_disk(config(fixture) / "active_account.json").at("id") == "offline-two", "RemoveAccount corrupted unrelated active id");
            current_->passed("active-consistency");
        }
        auto services = factory_(fixture);
        Probe probe(*services);
        query(probe, ui::UiQuery::Accounts);
        check(read_disk(config(fixture) / "active_account.json").at("id") == "offline-two", "account restart changed active id");
        current_->passed("durable");
        // Real multi-file conflict/failure injection is a distinct check, not supplied by a successful transaction.
    }

    void named_instance() {
        const auto& fixture = scenario();
        const auto descriptor = named(fixture) / (fixture.at("namedVersionId").get<std::string>() + ".json");
        const auto profileBefore = read_disk(named(fixture) / "nickel.profile.json");
        {
            auto services = factory_(fixture);
            Probe probe(*services);
            action(probe, ui::UiAction::SelectInstance, target(fixture));
            auto rename = target(fixture);
            rename.insert("patch", QJsonObject{{"name", "Different display label"}});
            action(probe, ui::UiAction::RenameInstance, rename);
            check(read_disk(descriptor).at("nickelCustom").at("name") == "Different display label", "display rename was not durable");
            check(read_disk(named(fixture) / "nickel.profile.json").at("name") == profileBefore.at("name"), "display label altered named instance identity");
            auto favorite = target(fixture);
            favorite.insert("favorite", true);
            action(probe, ui::UiAction::SetInstanceFavorite, favorite);
            auto icon = target(fixture);
            icon.insert("icon", "anvil");
            action(probe, ui::UiAction::SetInstanceIcon, icon);
            const auto detail = query(probe, ui::UiQuery::VersionDetail, target(fixture)).data.toObject();
            check(detail.value("versionId").toString() == qstr(fixture.at("namedVersionId").get<std::string>()), "VersionDetail identity lost named id");
            check(same_path(detail.value("root").toString(), path_of(fixture.at("expectedGameRoot").get<std::string>())), "VersionDetail root changed on rename");
            current_->passed("root-version-identity");
        }
        auto services = factory_(fixture);
        Probe probe(*services);
        const auto selected = query(probe, ui::UiQuery::Bootstrap).data.toObject().value("selectedInstance").toObject();
        check(selected.value("versionId").toString() == qstr(fixture.at("namedVersionId").get<std::string>()), "named selection was not durable across AppServices restart");
        check(same_path(selected.value("root").toString(), path_of(fixture.at("expectedGameRoot").get<std::string>())), "selected named root drifted");
        current_->passed("durable");
        // Effective installed directory, pending owner and conflict require install/import cases.
    }

    void version_settings() {
        const auto& fixture = scenario();
        const auto settingsFile = named(fixture) / "nickel.settings.json";
        const auto unknown = read_disk(settingsFile).at("futureInstance");
        auto services = factory_(fixture);
        Probe probe(*services);
        query(probe, ui::UiQuery::VersionSettings, target(fixture));
        auto arguments = target(fixture);
        arguments.insert("patch", QJsonObject{{"offlineUsername", "NamedOverride"}});
        action(probe, ui::UiAction::PatchVersionSettings, arguments);
        check(read_disk(settingsFile).at("futureInstance") == unknown, "version settings lost unknown exact integers");
        current_->passed("unknown-fields");
        query(probe, ui::UiQuery::VersionSettings, target(fixture));
        action(probe, ui::UiAction::ResetVersionSettings, target(fixture));
        query(probe, ui::UiQuery::VersionSettings, target(fixture));
        // Complete inheritance/legacy-readonly/runtime validation asserts wait on exact frozen DTO contract.
    }

    void resource_import() {
        const auto& fixture = scenario();
        const auto inputs = path_of(fixture.at("inputs").get<std::string>());
        const auto foreign = named(fixture) / "mods/foreign-preserve.bin";
        const auto foreignHash = file_hash(foreign);
        const auto validHash = file_hash(inputs / "resource-valid.zip");
        auto services = factory_(fixture);
        Probe probe(*services);
        auto arguments = target(fixture);
        arguments.insert("paths", QJsonArray{qstr(path_text(inputs / "resource-valid.zip")), qstr(path_text(inputs / "resource-invalid.zip"))});
        const auto id = probe.submit(ui::UiAction::ImportResources, arguments);
        submitted(probe, id, true);
        const auto result = observed(probe, id, true);
        current_->facts["import-command-accepted"] = !result.error.isError();
        check(file_hash(foreign) == foreignHash, "resource import changed an unrelated file");
        check(file_hash(inputs / "resource-valid.zip") == validHash, "resource import modified source archive");
        current_->passed("foreign-preserved");
        query(probe, ui::UiQuery::InstalledResources, target(fixture));
        query(probe, ui::UiQuery::Downloads);
        // An accepted command is not terminal task, partial success or cancellation evidence.
    }

    void cleanup() {
        const auto& fixture = scenario();
        const auto protectedFile = named(fixture) / "saves/foreign-world/level.dat";
        const auto protectedHash = file_hash(protectedFile);
        auto services = factory_(fixture);
        Probe probe(*services);
        const auto preview = query(probe, ui::UiQuery::JunkPreview).data.toObject();
        const auto id = preview.value("previewId").toString();
        check(!id.isEmpty(), "production JunkPreview omitted its authorization token");
        action(probe, ui::UiAction::CleanJunk, {{"previewId", id}});
        check(file_hash(protectedFile) == protectedHash, "production cleanup touched a protected save");
        current_->passed("preview-token");
        current_->passed("foreign-preserved");
        current_->passed("deep-path");
        // Identity substitution, selected junk partial and cancellation remain explicit separate checks.
    }

    void downloads() {
        const auto& fixture = scenario();
        auto services = factory_(fixture);
        Probe probe(*services);
        query(probe, ui::UiQuery::Downloads);
        action(probe, ui::UiAction::RefreshDownloads);
        query(probe, ui::UiQuery::Downloads);
        // An empty Downloads snapshot cannot satisfy queued/commit/cancel/history acceptance.
    }

    void runtime_nonce() {
        const auto& fixture = scenario();
        auto services = factory_(fixture);
        Probe probe(*services);
        const auto initial = query(probe, ui::UiQuery::RuntimeInformation,
            {{"requestNonce", "Pd_Runtime_A-123"}}).data.toObject();
        check(initial.value("requestNonce").toString() == QStringLiteral("Pd_Runtime_A-123"), "runtime nonce did not correlate");
        const auto absent = query(probe, ui::UiQuery::RuntimeInformation).data.toObject();
        check(!absent.contains("requestNonce"), "runtime query invented an absent nonce");
        for (const auto& scope : std::vector<QJsonObject>{
            {{"requestNonce", ""}}, {{"requestNonce", "bad/value"}},
            {{"requestNonce", QString(129, QLatin1Char('x'))}}, {{"requestNonce", 1}},
            {{"root", qstr(fixture.at("expectedGameRoot").get<std::string>())}},
            {{"versionId", qstr(fixture.at("namedVersionId").get<std::string>())}}}) {
            const auto invalid = query(probe, ui::UiQuery::RuntimeInformation, scope, true);
            require_code(invalid, QStringLiteral("invalid_argument"), "invalid runtime nonce/scope accepted");
        }
        current_->facts["runtime-nonce-validation"] = true;
        current_->passed("nonce");

        const auto before = query(probe, ui::UiQuery::Bootstrap).data.toObject().value("selectedInstance");
        const auto id = probe.query(ui::UiQuery::RuntimeInformation, {{"requestNonce", "Pd_Old_Scope"}});
        submitted(probe, id, false);
        auto replacement = target(fixture);
        replacement.insert("requestNonce", "Pd_New_Scope");
        probe.supersede_pending(id, ui::UiQuery::RuntimeInformation, replacement);
        submitted(probe, id, false);
        const auto latest = observed(probe, id, false);
        require_ok(latest);
        check(latest.data.toObject().value("requestNonce").toString() == QStringLiteral("Pd_New_Scope"), "stale nonce won a pending request-id replacement");
        probe.drain();
        check(probe.reply_count(id, false) == 1, "pending request-id replacement delivered a stale second reply");
        current_->passed("request-id-reuse");

        const auto abandoned = probe.query(ui::UiQuery::RuntimeInformation, {{"requestNonce", "Pd_Cancelled_Subscriber"}});
        submitted(probe, abandoned, false);
        const auto survivor = probe.query(ui::UiQuery::RuntimeInformation, {{"requestNonce", "Pd_Live_Subscriber"}});
        submitted(probe, survivor, false);
        probe.cancel(abandoned);
        current_->facts["cancelRequestId"] = abandoned;
        const auto independent = observed(probe, survivor, false);
        require_ok(independent);
        check(independent.data.toObject().value("requestNonce").toString() == QStringLiteral("Pd_Live_Subscriber"), "cancelling one subscriber cancelled an independent request");
        probe.drain();
        check(probe.reply_count(abandoned, false) == 0, "cancelled request delivered a late reply");
        check(probe.reply_count(survivor, false) == 1, "independent subscriber reply was not single-shot");
        current_->passed("subscriber-isolation");
        const auto after = query(probe, ui::UiQuery::Bootstrap).data.toObject().value("selectedInstance");
        check(after == before, "runtime nonce/replacement mutated selected instance state");
        current_->passed("old-scope-not-applied");
        // Community/body/character subscriptions remain declared and required for whole-case PASS.
    }


    void queued_destruction() {
        const auto& fixture = scenario();
        auto services = factory_(fixture);
        check(services->thread() == QThread::currentThread(), "production QObject thread mismatch");
        Probe probe(*services);
        auto receiver = std::make_unique<QObject>();
        const QPointer<QObject> receiverIdentity(receiver.get());
        QObject monitor;
        int callbacks = 0;
        int emissionsAtDestruction = 0;
        bool destroyOnReply = false;
        bool wrongThread = false;
        QObject::connect(services.get(), &ui::UiServices::queryReady, receiver.get(),
                         [&callbacks](const ui::UiReply&) { ++callbacks; }, Qt::QueuedConnection);
        QObject::connect(services.get(), &ui::UiServices::queryReady, &monitor,
                         [&](const ui::UiReply&) {
                             if (!destroyOnReply || !receiver) return;
                             if (QThread::currentThread() != monitor.thread()) {
                                 wrongThread = true;
                                 return;
                             }
                             ++emissionsAtDestruction;
                             receiver.reset();
                         }, Qt::DirectConnection);
        query(probe, ui::UiQuery::Bootstrap);
        probe.drain();
        check(callbacks == 1, "production queued callback positive control did not execute");
        destroyOnReply = true;
        query(probe, ui::UiQuery::Bootstrap);
        probe.drain();
        check(!wrongThread && emissionsAtDestruction == 1 && receiverIdentity.isNull(), "receiver was not destroyed after a real queued reply emission");
        check(callbacks == 1, "already queued production reply reached a destroyed receiver context");
        current_->facts["receiver-destruction"] = {{"positiveControlCallbacks", 1},
            {"realReplyEmissionsAtDestruction", emissionsAtDestruction}, {"callbacksAfterDrain", callbacks},
            {"receiverQPointerNull", receiverIdentity.isNull()}};
        current_->passed("no-late-QObject-callback");
        int pending = 0;
        for (int index = 0; index < 64; ++index) {
            const auto id = probe.query(ui::UiQuery::Bootstrap);
            submitted(probe, id, false);
            if (probe.reply_count(id, false) == 0) ++pending;
        }
        QElapsedTimer destruction;
        destruction.start();
        services.reset();
        const auto elapsed = destruction.elapsed();
        probe.drain(50);
        current_->facts["service-destruction"] = {{"submittedQueries", 64}, {"undeliveredAtReset", pending},
            {"destructorElapsedMs", elapsed}, {"observationBoundMs", 50},
            {"ownedWorkerJoinObserved", false}, {"queueSaturationObserved", false}};
        // Destruction without a crash is not saturation, post-shutdown rejection or joined-work proof.
    }

    Factory factory_;
    Json fixtures_;
    Json matrix_;
    fs::path root_;
    std::map<std::string, CaseResult> results_;
    CaseResult* current_ = nullptr;
};

} // namespace nickel::production_smoke
