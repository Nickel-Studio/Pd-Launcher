#include "production_fixture.hpp"

#include "app/service/trusted_launcher.hpp"
#include "core/network/hash.hpp"
#include "core/tasks/secure_fs.hpp"
#include "platform/process.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using nickel::Cancellation;
using nickel::Json;
using nickel::app::test::ProductionFixture;
using nickel::app::test::require;
using nickel::app::test::wait_until;
using nickel::ui::UiAction;
using nickel::ui::UiQuery;
namespace safe = nickel::tasks::detail;
namespace fs = std::filesystem;

static_assert(!std::is_default_constructible_v<nickel::app::TrustedLauncherReceipt>);
static_assert(!std::is_copy_constructible_v<nickel::app::TrustedLauncherReceipt>);

Json export_arguments(ProductionFixture& fixture, const fs::path& destination, bool launcher) {
    auto arguments = fixture.add_legacy_instance();
    arguments.update({{"path", nickel::instances::path_to_utf8(destination)}, {"name", "Factory boundary"},
        {"version", "1.0"}, {"files", Json::array()}, {"format", "portable"},
        {"includeLauncher", launcher}, {"includePersonalization", false},
        {"bundleResources", true}, {"modrinthOnly", false}});
    return arguments;
}

void require_untrusted_factory(ProductionFixture& fixture) {
    for (const bool modrinth_only : {false, true}) {
        const auto services = fixture.state->export_services(modrinth_only, Cancellation{});
        require(services.archive && services.resolve_resource && services.validate_resource_download_url,
            "Fail-closed factory lost the real archive or community adapters");
        require(!services.launcher && !services.validate_launcher_artifact && services.launcher_notices.empty()
            && services.launcher_os.empty() && services.launcher_arch.empty(),
            "Untrusted options minted or partially exposed a launcher capability");
    }
}

std::string pe_negative(bool amd64, bool dll) {
    std::string bytes(512, '\0');
    const auto word = [&](std::size_t offset, std::uint16_t value) {
        bytes.at(offset) = static_cast<char>(value & 0xff);
        bytes.at(offset + 1) = static_cast<char>(value >> 8);
    };
    const auto dword = [&](std::size_t offset, std::uint32_t value) {
        word(offset, static_cast<std::uint16_t>(value));
        word(offset + 2, static_cast<std::uint16_t>(value >> 16));
    };
    word(0, 0x5a4d);
    dword(0x3c, 64);
    dword(64, 0x00004550);
    word(68, amd64 ? 0x8664 : 0x014c);
    word(70, 1);
    word(84, 240);
    word(86, static_cast<std::uint16_t>(0x0002 | (dll ? 0x2000 : 0)));
    word(88, amd64 ? 0x020b : 0x010b);
    return bytes;
}

void copy_test_image(const fs::path& destination) {
    const auto source = nickel::current_executable();
    const auto size = fs::file_size(safe::io_path(source));
    require(size > 0 && size <= 128ULL * 1024 * 1024, "Test image exceeds the owned negative-copy budget");
    require(fs::copy_file(safe::io_path(source), safe::io_path(destination)), "Cannot copy test image for a negative case");
}

void no_image_input_grants_trust() {
    using Setup = std::function<void(const fs::path&)>;
    const std::vector<std::pair<std::string, Setup>> cases{
        {"missing", {}},
        {"text", [](const auto& path) { nickel::atomic_write_text(path, "not an executable\n"); }},
        {"directory", [](const auto& path) { fs::create_directory(safe::io_path(path)); }},
        {"wrong-arch", [](const auto& path) { nickel::atomic_write_text(path, pe_negative(false, false)); }},
        {"dll", [](const auto& path) { nickel::atomic_write_text(path, pe_negative(true, true)); }},
        {"pe-without-version-stamp", [](const auto& path) { nickel::atomic_write_text(path, pe_negative(true, false)); }},
        {"copy-of-test-image", copy_test_image},
        {"replaced-image", [](const auto& path) {
            copy_test_image(path);
            safe::FileIdentity identity;
            { auto before = safe::SecureFile::open_read(path); identity = before.identity(); }
            // Tamper in place: replacing the freshly copied PE via an atomic rename can be
            // denied by scanner oplocks, while the negative input (an invalid replaced image) is identical.
            auto file = safe::SecureFile::open_owned(path, identity);
            const std::string replacement = "replaced before AppServices construction\n";
            file.seek(0);
            file.write(std::as_bytes(std::span(replacement.data(), replacement.size())));
            file.flush();
        }},
        {"same-size-tampered-image", [](const auto& path) {
            copy_test_image(path);
            safe::FileIdentity identity;
            { auto before = safe::SecureFile::open_read(path); identity = before.identity(); }
            auto file = safe::SecureFile::open_owned(path, identity);
            file.seek(0);
            const std::array<std::byte, 1> byte{std::byte{0}};
            file.write(byte);
            file.flush();
        }}
    };
    require(!nickel::app::capture_trusted_launcher(), "A fixture process was accepted as the actual trusted Pd image");
    for (const auto& [name, setup] : cases) {
        ProductionFixture fixture({}, setup);
        fixture.finish_onboarding();
        require_untrusted_factory(fixture);
        const auto destination = fixture.root / "exports" / "denied.zip";
        fs::create_directories(safe::io_path(destination.parent_path()));
        const auto arguments = export_arguments(fixture, destination, true);
        const auto before = fixture.state->tasks->snapshots().size();
        const auto reply = fixture.action(UiAction::ExportPack, arguments);
        require(reply.error.code == "missing_service" && reply.error.detail.isEmpty(),
            "Real public ExportPack did not fail closed for an untrusted launcher");
        require(!fs::exists(safe::io_path(destination)) && fixture.state->tasks->snapshots().size() == before,
            "Rejected launcher export created a task or published an output");
        std::cout << "production factory/public ExportPack negative " << name << ": PASS\n";
    }
}

void cancellation_and_collision_are_prepublication() {
    ProductionFixture fixture;
    fixture.finish_onboarding();
    require_untrusted_factory(fixture);
    Cancellation cancelled;
    cancelled.cancel();
    const auto cancelled_boundary = [&](const auto& work) {
        bool refused = false;
        try { work(); }
        catch (const nickel::ErrorException& failure) { refused = failure.error().code == "cancelled"; }
        require(refused, "Cancellation was swallowed by an optional trusted capability boundary");
    };
    cancelled_boundary([&] { (void)nickel::app::capture_trusted_launcher(cancelled); });
    cancelled_boundary([&] { (void)fixture.state->export_services(false, cancelled); });
    const auto destination = fixture.root / "exports" / "collision.zip";
    fs::create_directories(safe::io_path(destination.parent_path()));
    const auto arguments = export_arguments(fixture, destination, false);
    cancelled_boundary([&] { (void)fixture.state->action(UiAction::ExportPack, arguments, cancelled); });
    require(!fs::exists(safe::io_path(destination)) && fixture.state->tasks->snapshots().empty(),
        "Pre-cancelled export changed disk or task state");
    nickel::atomic_write_text(destination, "preserve collision bytes\n");
    const auto reply = fixture.action(UiAction::ExportPack, arguments);
    require(reply.error.code == "conflict" && reply.error.detail.isEmpty(), "Existing export target was not safely refused");
    require(nickel::read_text_bounded(destination, 4096) == "preserve collision bytes\n"
        && fixture.state->tasks->snapshots().empty(), "Collision changed the original destination or queued work");
    fixture.state->shutdown();
    cancelled_boundary([&] { (void)fixture.state->export_services(false, Cancellation{}); });
    std::cout << "factory cancellation / public export pre-cancel / collision preservation / shutdown: PASS\n";
}

void public_nonlauncher_export_is_terminal() {
    ProductionFixture fixture;
    fixture.finish_onboarding();
    const auto destination = fixture.root / "exports" / "without-launcher.zip";
    fs::create_directories(safe::io_path(destination.parent_path()));
    const auto arguments = export_arguments(fixture, destination, false);
    const auto accepted = fixture.action_ok(UiAction::ExportPack, arguments);
    require(accepted.at("queued").get<bool>(), "Non-launcher portable export did not use the real task queue");
    const auto task_id = accepted.at("taskId").get<std::string>();
    nickel::tasks::Snapshot terminal;
    wait_until([&] {
        const auto snapshots = fixture.state->tasks->snapshots();
        const auto found = std::find_if(snapshots.begin(), snapshots.end(), [&](const auto& row) { return row.id == task_id; });
        if (found == snapshots.end()) return false;
        terminal = *found;
        return terminal.status == nickel::tasks::TaskStatus::Succeeded || terminal.status == nickel::tasks::TaskStatus::Failed
            || terminal.status == nickel::tasks::TaskStatus::Cancelled;
    }, std::chrono::seconds(20));
    require(terminal.status == nickel::tasks::TaskStatus::Succeeded && fs::exists(safe::io_path(destination)),
        "Real non-launcher portable export did not durably succeed");
    const auto digest = nickel::packs::hash_file(destination, "sha256");
    const auto backend = nickel::packs::native_archive_backend();
    const auto entries = backend->inspect(destination, {}, {});
    require(std::none_of(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.name == "Nickel.exe" || entry.name == "Nickel-launcher";
    }), "Non-launcher archive unexpectedly included an executable");
    const auto manifest = nickel::packs::validate_portable_manifest(Json::parse(
        backend->read(destination, nickel::packs::portable_manifest_file, 1024 * 1024, {})));
    require(!manifest.contains("launcher") && manifest.at("schemaVersion") == 1
        && manifest.at("payload") == "Nickel/pack.mrpack", "Portable export changed schema 1 or invented trust");
    backend->release(destination);
    require(!fixture.state->cancel_operation(task_id), "Late task cancellation crossed a terminal publication");
    (void)fixture.action(UiAction::CancelDownload, {{"id", task_id}});
    fixture.query_ok(UiQuery::Downloads);
    fixture.state->shutdown();
    const auto snapshots = fixture.state->tasks->snapshots();
    const auto found = std::find_if(snapshots.begin(), snapshots.end(), [&](const auto& row) { return row.id == task_id; });
    require(found != snapshots.end() && found->status == nickel::tasks::TaskStatus::Succeeded
        && found->revision == terminal.revision && found->error_code == terminal.error_code
        && nickel::packs::hash_file(destination, "sha256") == digest,
        "Late cancel/shutdown relabelled or changed successful durable export");
    const auto delta = fixture.state->tasks->since(0);
    require(std::count_if(delta.events.begin(), delta.events.end(), [&](const auto& event) {
        return event.snapshot.id == task_id && event.kind == nickel::tasks::EventKind::Terminal;
    }) == 1, "Export emitted a second or missing terminal task event");
    std::cout << "public non-launcher portable export / schema1 / final publication / late cancel-shutdown: PASS\n";
}
} // namespace

void run_production_factory_tests() {
    no_image_input_grants_trust();
    cancellation_and_collision_are_prepublication();
    public_nonlauncher_export_is_terminal();
}
