#include "app/AppServices.hpp"
#include "app/application_state.hpp"
#include "app/service/public_dto.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/path_guard.hpp"
#include "core/packs/internal.hpp"
#include "core/packs/native_services.hpp"
#include "core/tasks/secure_fs.hpp"
#include "platform/process.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef PD_TRUSTED_EXPORT_SOURCE_COMMIT
#error The builder must bind the immutable production source commit.
#endif
#ifndef PD_TRUSTED_EXPORT_SOURCE_SHA256
#error The builder must bind the actual production source manifest SHA256.
#endif

namespace nickel::app {
struct AppServicesTestProbe {
    static std::shared_ptr<ApplicationState> state(AppServices& services) { return services.state_; }
};
}

namespace {
namespace fs = std::filesystem;
using nickel::Cancellation;
using nickel::ErrorException;
using nickel::Json;
using nickel::app::ApplicationState;
using nickel::ui::UiAction;
using nickel::ui::UiQuery;
using nickel::ui::UiReply;
using nickel::tasks::TaskStatus;
namespace safe = nickel::tasks::detail;

std::uint64_t assertions = 0;

void check(bool condition, std::string_view message) {
    ++assertions;
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class Work>
std::string denied(Work&& work) {
    try { work(); }
    catch (const ErrorException& error) { return error.error().code; }
    throw std::runtime_error("Negative operation unexpectedly succeeded");
}

void child_of(const fs::path& root, const fs::path& child) {
    const auto relative = child.lexically_relative(root);
    check(!relative.empty() && relative != "." && !relative.is_absolute(), "Path is not an owned child");
    for (const auto& part : relative) check(part != "..", "Path escaped isolated fixture");
}

void write_new(const fs::path& path, std::string_view text) {
    fs::create_directories(safe::io_path(path.parent_path()));
    auto output = safe::SecureFile::create_new(path);
    output.write(std::as_bytes(std::span(text.data(), text.size())));
    output.flush();
}

std::string read_small(const fs::path& path) {
    const auto bytes = safe::read_bounded(path, 4 * 1024 * 1024);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string fake_pe(std::uint16_t machine = 0x8664, bool dll = false) {
    std::string bytes(1024, '\0');
    bytes.replace(0, 2, "MZ");
    const auto u16 = [&](std::size_t at, std::uint16_t value) {
        bytes[at] = static_cast<char>(value & 255U);
        bytes[at + 1] = static_cast<char>(value >> 8);
    };
    const auto u32 = [&](std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i != 4; ++i) bytes[at + i] = static_cast<char>((value >> (i * 8)) & 255U);
    };
    u32(0x3c, 0x80);
    bytes.replace(0x80, 4, std::string("PE\0\0", 4));
    u16(0x84, machine);
    u16(0x86, 1);
    u16(0x94, 0xf0);
    u16(0x96, static_cast<std::uint16_t>(2U | (dll ? 0x2000U : 0U)));
    u16(0x98, machine == 0x14c ? 0x10b : 0x20b);
    u16(0x98 + 68, 2);
    u32(0x98 + 0xf0 + 16, 128);
    u32(0x98 + 0xf0 + 20, 512);
    return bytes;
}

void capacity() {
#ifdef _WIN32
    for (const auto& [volume, minimum] : std::array<std::pair<const wchar_t*, std::uint64_t>, 2>{
            std::pair{L"C:\\", 1024ULL * 1024 * 1024},
            std::pair{L"D:\\", 256ULL * 1024 * 1024}}) {
        ULARGE_INTEGER free{};
        check(GetDiskFreeSpaceExW(volume, &free, nullptr, nullptr) != 0, "Cannot verify volume capacity");
        check(free.QuadPart >= minimum, "C/D safety reserve is below the test floor");
    }
#endif
}

bool terminal(TaskStatus status) {
    return status == TaskStatus::Succeeded || status == TaskStatus::Failed || status == TaskStatus::Cancelled;
}

template<class Ready>
void wait_for(Ready ready) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!ready()) {
        check(std::chrono::steady_clock::now() < end, "Bounded production test timed out");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
}

class Fixture {
public:
    explicit Fixture(const fs::path& base, std::string image = "Inert untrusted file; never executed",
                     std::string_view kind = "file") {
        capacity();
        root = base / safe::utf8_path("case-" + safe::unique_id());
        safe::create_directory_new(root);
        executable = root / "bin" / "user-selected.exe";
        fs::create_directories(safe::io_path(executable.parent_path()));
        if (kind == "directory") fs::create_directory(safe::io_path(executable));
        else if (kind == "copy-test") {
            check(nickel::atomic_copy_no_replace(nickel::current_executable(), executable, 512ULL * 1024 * 1024),
                  "Could not create owned copy of the test image");
        } else if (kind != "missing") write_new(executable, image);
        write_new(root / "bin/Nickel/settings.json", "{}");
        write_new(root / "bin/Nickel/game.json", "{}");
        fs::create_directories(safe::io_path(root / "system"));
        fs::create_directories(safe::io_path(root / "data"));
        fs::create_directories(safe::io_path(root / "exports"));
        nickel::app::AppServicesOptions options;
        options.executable = executable;
        options.paths = {executable, root / "system", root / "data", false};
        services = std::make_unique<nickel::app::AppServices>(std::move(options));
        state = nickel::app::AppServicesTestProbe::state(*services);
        check(state->repository.root() == root / "bin/Nickel", "AppServices escaped the explicit config root");
        game = state->roots.ensure_default();
        child_of(root, game);
        check(state->options.executable == executable, "Fixture selected a different untrusted options path");
        QObject::connect(services.get(), &nickel::ui::UiServices::queryReady, &callbacks,
            [this](const UiReply& reply) { queries.insert_or_assign(reply.id, reply); });
        QObject::connect(services.get(), &nickel::ui::UiServices::commandFinished, &callbacks,
            [this](const UiReply& reply) { commands.insert_or_assign(reply.id, reply); });
        const auto instance = game / "versions/fixture";
        fs::create_directories(safe::io_path(instance));
        write_new(instance / "fixture.json", Json{{"id", "1.20.1"}, {"type", "release"},
            {"mainClass", "net.minecraft.client.main.Main"}, {"libraries", Json::array()},
            {"arguments", {{"game", Json::array()}, {"jvm", Json::array()}}}}.dump());
        scope = {{"root", nickel::path_to_utf8(game)}, {"versionId", "fixture"}};
        context = state->context(state->identity(scope));
        fs::create_directories(safe::io_path(context.resourceRoot));
        const auto status = query(UiQuery::Onboarding);
        action_ok(UiAction::AcceptEula, {{"checked", true}, {"eulaVersion", status.at("eulaVersion")}});
        action_ok(UiAction::AcknowledgeChangelog, {{"version", status.at("currentVersion")}});
        action_ok(UiAction::FinishOnboarding);
        check(query(UiQuery::Onboarding).at("startupReady") == true, "Production first-use gate did not finish");
        check(state->launches.empty(), "The isolated test must not auto-launch a game");
    }

    ~Fixture() {
        QObject::disconnect(services.get(), nullptr, &callbacks, nullptr);
        services.reset();
        state.reset();
        // Keep owned fixtures and failed artifacts for independent inspection.
    }

    UiReply action(UiAction selected, const Json& fields) {
        const auto id = ++request_id;
        services->submit(id, selected, nickel::app::to_public_json(fields).toObject());
        wait_for([&] { return commands.contains(id); });
        const auto reply = commands.at(id);
        commands.erase(id);
        return reply;
    }

    Json action_ok(UiAction selected, const Json& fields = Json::object()) {
        const auto reply = action(selected, fields);
        check(!reply.error.isError(), "Production public action unexpectedly failed");
        return nickel::app::from_public_json(reply.data);
    }

    Json query(UiQuery selected, const Json& fields = Json::object()) {
        const auto id = ++request_id;
        services->request(id, selected, nickel::app::to_public_json(fields).toObject());
        wait_for([&] { return queries.contains(id); });
        const auto reply = queries.at(id);
        queries.erase(id);
        check(!reply.error.isError(), "Production public query unexpectedly failed");
        return nickel::app::from_public_json(reply.data);
    }

    Json export_args(const fs::path& path, bool include_launcher = true) const {
        auto result = scope;
        result.update({{"path", nickel::path_to_utf8(path)}, {"name", "Trusted export negative fixture"},
                       {"version", "1"}, {"format", "portable"}, {"files", Json::array()},
                       {"bundleResources", true}, {"modrinthOnly", false},
                       {"includeLauncher", include_launcher}, {"includePersonalization", false}});
        return result;
    }

    nickel::tasks::Snapshot snapshot(std::string_view task_id) const {
        const auto snapshots = state->tasks->snapshots();
        const auto found = std::find_if(snapshots.begin(), snapshots.end(), [&](const auto& item) { return item.id == task_id; });
        check(found != snapshots.end(), "Production task receipt is absent from the real registry");
        return *found;
    }

    nickel::tasks::Snapshot finish(const UiReply& reply) {
        check(!reply.error.isError(), "Expected a queued real Task, not an admission error");
        const auto receipt = nickel::app::from_public_json(reply.data);
        check(receipt.at("queued") == true && receipt.at("taskId").is_string(), "Export omitted its canonical Task receipt");
        const auto id = receipt.at("taskId").get<std::string>();
        wait_for([&] { return terminal(snapshot(id).status); });
        return snapshot(id);
    }

    fs::path save(std::string_view name) const { return root / "exports" / safe::utf8_path(name); }

    fs::path root;
    fs::path executable;
    fs::path game;
    Json scope;
    nickel::resources::InstanceContext context;
    std::unique_ptr<nickel::app::AppServices> services;
    std::shared_ptr<ApplicationState> state;
private:
    QObject callbacks;
    std::map<nickel::ui::RequestId, UiReply> queries;
    std::map<nickel::ui::RequestId, UiReply> commands;
    nickel::ui::RequestId request_id = 100;
};

Json no_options_trust(Fixture& fixture) {
    const auto target = fixture.save("must-not-publish.zip");
    const auto args = fixture.export_args(target);
    const auto raw = denied([&] { fixture.state->action_resources(UiAction::ExportPack, args, Cancellation{}); });
    check(raw == "missing_service" || raw == "unsupported" || raw == "integrity" || raw == "unauthorized"
          || raw == "untrusted_launcher", "Wrong failure reason: options trust check was not reached");
    check(fixture.state->tasks->snapshots().empty(), "Untrusted options must fail before starting an export Task");
    const auto reply = fixture.action(UiAction::ExportPack, args);
    check(reply.error.isError(), "Public AppServices trusted an arbitrary executable option");
    check(reply.error.code == nickel::app::public_error(raw).code, "Public and production-domain denials disagree");
    check(fixture.state->tasks->snapshots().empty() && !fs::exists(safe::io_path(target)), "Denied launcher export published or scheduled work");
    return {{"rawDenial", raw}, {"publicDenial", reply.error.code.toStdString()},
            {"arbitraryOptionTrusted", false}, {"destinationExists", false}, {"fixture", nickel::path_to_utf8(fixture.root)}};
}

class BoundaryArchive final : public nickel::packs::ArchiveBackend {
public:
    BoundaryArchive(std::shared_ptr<nickel::packs::ArchiveBackend> delegate, std::function<void()> after)
        : delegate_(std::move(delegate)), after_(std::move(after)) {}
    std::vector<nickel::packs::ArchiveEntry> inspect(const fs::path& file, const nickel::packs::ArchiveLimits& limits,
                                                   std::stop_token cancel) override { return delegate_->inspect(file, limits, cancel); }
    std::string read(const fs::path& file, std::string_view name, std::uint64_t maximum,
                     std::stop_token cancel) override { return delegate_->read(file, name, maximum, cancel); }
    void extract_file(const fs::path& file, std::string_view name, const fs::path& target, std::uint64_t size,
                      std::stop_token cancel) override { delegate_->extract_file(file, name, target, size, cancel); }
    void write(const fs::path& target, std::span<const nickel::packs::ArchiveInput> inputs,
               const nickel::packs::ArchiveLimits& limits, std::stop_token cancel) override {
        delegate_->write(target, inputs, limits, cancel);
        if (!fired && std::any_of(inputs.begin(), inputs.end(), [](const auto& input) { return input.name == "nickel.portable.json"; })) {
            fired = true;
            after_();
        }
    }
    void release(const fs::path& file) noexcept override { delegate_->release(file); }
    bool fired = false;
private:
    std::shared_ptr<nickel::packs::ArchiveBackend> delegate_;
    std::function<void()> after_;
};

Json publication_cancel(const fs::path& base) {
    Fixture fixture(base);
    const auto target = fixture.save("cancelled.zip");
    bool cancel_accepted = false;
    auto archive = std::make_shared<BoundaryArchive>(fixture.state->archive, [&] {
        for (const auto& task : fixture.state->tasks->snapshots()) {
            if (task.target == target && !terminal(task.status)) cancel_accepted = fixture.state->tasks->cancel(task.id);
        }
    });
    fixture.state->archive = archive;
    const auto final = fixture.finish(fixture.action(UiAction::ExportPack, fixture.export_args(target, false)));
    check(archive->fired && cancel_accepted, "Deterministic post-write/pre-publication cancellation was not exercised");
    check(final.status == TaskStatus::Cancelled && final.cancel_requested, "Cancellation did not reach canonical Cancelled");
    check(!fs::exists(safe::io_path(target)), "Cancellation published the final export");
    return {{"task", nickel::tasks::to_json(final)}, {"archiveBoundaryFired", true}, {"destinationExists", false},
            {"launcherCapabilityTested", false}, {"fixture", nickel::path_to_utf8(fixture.root)}};
}

Json publication_collision(const fs::path& base) {
    Fixture fixture(base);
    const auto target = fixture.save("external-owner.zip");
    auto archive = std::make_shared<BoundaryArchive>(fixture.state->archive, [&] { write_new(target, "external owner must survive"); });
    fixture.state->archive = archive;
    const auto final = fixture.finish(fixture.action(UiAction::ExportPack, fixture.export_args(target, false)));
    check(archive->fired && final.status == TaskStatus::Failed, "Post-admission collision was not rejected");
    check(read_small(target) == "external owner must survive", "Export overwrote an externally published destination");
    return {{"task", nickel::tasks::to_json(final)}, {"externalBytesPreserved", true},
            {"launcherCapabilityTested", false}, {"fixture", nickel::path_to_utf8(fixture.root)}};
}

Json selection_denied(const fs::path& base, bool absolute) {
    Fixture fixture(base);
    const auto target = fixture.save("selection.zip");
    auto args = fixture.export_args(target, false);
    args["files"] = Json::array({absolute ? nickel::path_to_utf8(fixture.game / "versions/fixture/fixture.json") : "launcher_accounts.json"});
    const auto final = fixture.finish(fixture.action(UiAction::ExportPack, args));
    check(final.status == TaskStatus::Failed && final.error_code == "unauthorized", "Non-public/protected source token was not rejected by authorization");
    check(!fs::exists(safe::io_path(target)), "Unauthorized source token published a package");
    return {{"task", nickel::tasks::to_json(final)}, {"destinationExists", false}, {"fixture", nickel::path_to_utf8(fixture.root)}};
}

Json content_denied(const fs::path& base) {
    Fixture fixture(base);
    const auto config = fixture.context.resourceRoot / "config/fixture.json";
    write_new(config, R"({"accessToken":"SYNTHETIC_ISOLATED_PRIVATE_VALUE"})");
    const auto target = fixture.save("secret.zip");
    auto args = fixture.export_args(target, false);
    args["files"] = Json::array({"@dir/config/config"});
    const auto final = fixture.finish(fixture.action(UiAction::ExportPack, args));
    check(final.status == TaskStatus::Failed && final.error_code == "unsafe_content", "Selected credential-bearing config did not fail closed");
    check(!fs::exists(safe::io_path(target)), "Secret-bearing source was published");
    check(read_small(config).find("SYNTHETIC_ISOLATED_PRIVATE_VALUE") != std::string::npos, "Negative export modified its source");
    return {{"task", nickel::tasks::to_json(final)}, {"destinationExists", false}, {"fixture", nickel::path_to_utf8(fixture.root)}};
}

Json payload_denied(const fs::path& base, std::string_view key) {
    Fixture fixture(base);
    const auto target = fixture.save("override.zip");
    auto args = fixture.export_args(target);
    args[std::string(key)] = key == "originalSha256" ? Json(std::string(64, 'a')) : Json(nickel::path_to_utf8(fixture.executable));
    const auto reply = fixture.action(UiAction::ExportPack, args);
    check(reply.error.code == "unauthorized", "Public DTO accepted a path/hash trust override");
    check(!fs::exists(safe::io_path(target)) && fixture.state->tasks->snapshots().empty(), "Override denial scheduled or published an export");
    return {{"publicDenial", reply.error.code.toStdString()}, {"destinationExists", false}, {"fixture", nickel::path_to_utf8(fixture.root)}};
}

Json notice_denied(const fs::path& base, std::string_view variant) {
    Fixture fixture(base);
    const auto payload = fixture.root / "negative-pack.mrpack";
    nickel::packs::PackManifest pack;
    pack.name = "Inert notice-negative pack";
    pack.version = "1";
    pack.minecraft_version = "1.20.1";
    const std::vector<nickel::packs::ArchiveInput> payload_inputs{
        {"modrinth.index.json", {}, nickel::packs::mrpack_manifest_to_json(pack).dump()}};
    fixture.state->archive->write(payload, payload_inputs, {}, {});
    fixture.state->archive->release(payload);
    const auto scratch = fixture.root / "negative-material-workspace";
    fs::create_directories(safe::io_path(scratch));
    nickel::packs::ExportServices inputs;
    inputs.archive = fixture.state->archive;
    inputs.launcher = fixture.executable;
    inputs.launcher_os = "windows";
    inputs.launcher_arch = "x86_64";
    const auto inert_sha = nickel::packs::hash_file(fixture.executable, "sha256");
    // This untrusted negative-only adapter reaches the existing notice policy;
    // it never represents a production receipt or a positive launcher test.
    inputs.validate_launcher_artifact = [inert_sha](const fs::path& file, std::stop_token stop) {
        if (nickel::packs::hash_file(file, "sha256", stop) != inert_sha)
            throw ErrorException(nickel::Error{"integrity", "Inert negative input unexpectedly changed"});
    };
    for (const auto* name : {"LICENSE", "LICENSE-ASSETS", "NOTICE.md", "SOURCE.txt"})
        inputs.launcher_notices.push_back({name, {}, "Inert negative-only material; not release legal"});
    std::string expected;
    if (variant == "outside") { inputs.launcher_notices.push_back({"Nickel/accounts.json", {}, "not legal"}); expected = "unsafe_content"; }
    else if (variant == "file") { inputs.launcher_notices.front().file = fixture.executable; expected = "unsafe_content"; }
    else if (variant == "duplicate") { inputs.launcher_notices.push_back(inputs.launcher_notices.front()); expected = "limit"; }
    else if (variant == "missing-source") { inputs.launcher_notices.pop_back(); expected = "missing_service"; }
    else if (variant == "traversal") { inputs.launcher_notices.push_back({"LICENSES/../other", {}, "not legal"}); expected = "unsafe_path"; }
    else if (variant == "overquota") { inputs.launcher_notices.front().bytes.assign(4 * 1024 * 1024 + 1, 'x'); expected = "limit"; }
    else throw std::runtime_error("Unknown notice-negative variant");
    nickel::packs::ExportRequest request;
    request.name = pack.name;
    request.version = pack.version;
    request.instance_id = "fixture";
    request.include_launcher = true;
    request.format = nickel::packs::ExportFormat::portable;
    const auto target = fixture.save("negative-materials.zip");
    const auto raw = denied([&] { nickel::packs::detail::write_portable(request, payload, {}, target, scratch, inputs, {}); });
    check(raw == expected, "Notice policy failed for the wrong reason");
    check(!fs::exists(safe::io_path(target)), "Invalid notice material wrote an envelope");
    return {{"rawDenial", raw}, {"scope", "packs-negative-notice-policy-only"}, {"productionReceiptUsed", false},
            {"launcherCapabilityTested", false}, {"destinationExists", false}, {"fixture", nickel::path_to_utf8(fixture.root)}};
}

Json run_case(std::string_view id, const std::function<Json()>& work) {
    assertions = 0;
    const auto begin = std::chrono::steady_clock::now();
    Json result{{"id", id}, {"status", "FAIL"}, {"executed", true}};
    try { result["evidence"] = work(); result["status"] = "PASS"; }
    catch (const ErrorException& error) { result["errorCode"] = error.error().code; result["failure"] = error.what(); }
    catch (const std::exception& error) { result["failure"] = error.what(); }
    result["assertions"] = assertions;
    result["elapsedMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    std::cout << result.dump() << std::endl;
    return result;
}

std::string option(const QStringList& arguments, std::string_view key) {
    const auto index = arguments.indexOf(QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size())));
    if (index < 0 || index + 1 >= arguments.size()) throw std::runtime_error("Required owned runner argument is missing");
    return arguments.at(index + 1).toUtf8().toStdString();
}
}

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        const auto arguments = QCoreApplication::arguments();
        const auto output = safe::checked_absolute(safe::utf8_path(option(arguments, "--output")));
        const auto base = safe::checked_absolute(safe::utf8_path(option(arguments, "--fixture-root")));
        const auto graph_sha = option(arguments, "--graph-sha256");
        child_of(output.parent_path(), base);
        check(!fs::exists(safe::io_path(output)) && !fs::exists(safe::io_path(base)), "Runner output/fixtures must be unused");
        check(graph_sha.size() == 64 && std::all_of(graph_sha.begin(), graph_sha.end(), [](char c) {
            return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9'); }), "Graph SHA256 is not a full digest");
        for (const auto* name : {"APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "TEMP", "TMP"}) {
            const auto value = qgetenv(name);
            check(!value.isEmpty(), "Runner did not isolate the process environment");
            child_of(output.parent_path(), safe::checked_absolute(safe::utf8_path(value.toStdString())));
        }
        safe::create_directory_new(base);
        Json cases = Json::array();
        const auto add = [&](std::string_view id, const std::function<Json()>& work) { cases.push_back(run_case(id, work)); };
        add("options-text-executable-no-trust", [&] { Fixture f(base); return no_options_trust(f); });
        add("options-missing-executable-no-trust", [&] { Fixture f(base, {}, "missing"); return no_options_trust(f); });
        add("options-directory-executable-no-trust", [&] { Fixture f(base, {}, "directory"); return no_options_trust(f); });
        add("options-wrong-os-image-no-trust", [&] { Fixture f(base, std::string("\x7f" "ELF") + std::string(1020, '\0')); return no_options_trust(f); });
        add("options-wrong-arch-image-no-trust", [&] { Fixture f(base, fake_pe(0xaa64)); return no_options_trust(f); });
        add("options-dll-image-no-trust", [&] { Fixture f(base, fake_pe(0x8664, true)); return no_options_trust(f); });
        add("options-copy-of-test-image-no-trust", [&] { Fixture f(base, {}, "copy-test"); return no_options_trust(f); });
        add("options-replaced-exe-no-trust", [&] {
            Fixture f(base, fake_pe());
            const auto before = safe::capture_regular_file(f.root, f.executable);
            safe::rename_no_replace(f.executable, f.executable.parent_path() / "original.exe");
            write_new(f.executable, fake_pe());
            const auto after = safe::capture_regular_file(f.root, f.executable);
            check(before.file_identity != after.file_identity && before.sha256 == after.sha256, "Same-byte identity replacement attack was not real");
            auto result = no_options_trust(f);
            result["beforeSha256"] = before.sha256;
            result["afterSha256"] = after.sha256;
            result["fileIdentityChanged"] = true;
            return result;
        });
        add("options-same-size-tampered-exe-no-trust", [&] {
            Fixture f(base, fake_pe());
            const auto before = safe::capture_regular_file(f.root, f.executable);
            auto file = safe::SecureFile::open_owned(f.executable, before.file_identity);
            file.seek(700);
            const std::array<std::byte, 1> modified{std::byte{0x5a}};
            file.write(modified);
            file.flush();
            file.close();
            const auto after = safe::capture_regular_file(f.root, f.executable);
            check(before.file_identity == after.file_identity && before.size == after.size && before.sha256 != after.sha256, "Same-size same-file SHA mutation attack was not real");
            auto result = no_options_trust(f);
            result["beforeSha256"] = before.sha256;
            result["afterSha256"] = after.sha256;
            return result;
        });
        for (const auto& [id, field] : std::array<std::pair<const char*, const char*>, 5>{
                std::pair{"payload-launcher-path-denied", "launcher"},
                std::pair{"payload-executable-path-denied", "executable"},
                std::pair{"payload-artifact-denied", "artifact"},
                std::pair{"payload-original-sha-denied", "originalSha256"},
                std::pair{"payload-source-path-denied", "source_path"}})
            add(id, [&] { return payload_denied(base, field); });
        add("source-absolute-selection-denied", [&] { return selection_denied(base, true); });
        add("source-protected-selection-denied", [&] { return selection_denied(base, false); });
        add("cancel-before-publication", [&] { return publication_cancel(base); });
        add("destination-collision-preserved", [&] { return publication_collision(base); });
        add("content-secret-whitelist-denied", [&] { return content_denied(base); });
        for (const auto& [id, variant] : std::array<std::pair<const char*, const char*>, 6>{
                std::pair{"notice-outside-whitelist-denied", "outside"}, std::pair{"notice-file-input-denied", "file"},
                std::pair{"notice-duplicate-denied", "duplicate"}, std::pair{"notice-missing-source-denied", "missing-source"},
                std::pair{"notice-traversal-denied", "traversal"}, std::pair{"notice-overquota-denied", "overquota"}})
            add(id, [&] { return notice_denied(base, variant); });
        Json report{{"schema", "pd.trusted-export.cases.v1"}, {"sourceCommit", PD_TRUSTED_EXPORT_SOURCE_COMMIT},
                    {"sourceSha256", PD_TRUSTED_EXPORT_SOURCE_SHA256}, {"graphSha256", graph_sha},
                    {"processImage", nickel::path_to_utf8(nickel::current_executable())},
                    {"processImageSha256", nickel::packs::hash_file(nickel::current_executable(), "sha256")},
                    {"productionTarget", "Nickel::AppServices"}, {"actualPdRoundTripPass", false},
                    {"receiptNegativeStatus", "NOT_RUN"}, {"actualPdStatus", "NOT_RUN"}, {"cases", cases}};
        write_new(output, report.dump(2) + "\n");
        const auto failures = std::count_if(cases.begin(), cases.end(), [](const auto& value) { return value.at("status") == "FAIL"; });
        std::cout << "production-negative cases=" << cases.size() << " failed=" << failures
                  << "; receipt/actualPd NOT_RUN (not tested by an arbitrary test image)\n";
        capacity();
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Harness failed closed: " << error.what() << '\n';
        return 1;
    }
}
