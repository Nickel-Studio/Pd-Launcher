#include "core/diagnostics/diagnostics.hpp"
#include "core/packs/archive_bridge.hpp"
#include "core/instances/domain_io.hpp"
#include "core/diagnostics/report_detail.hpp"
#include "core/packs/internal.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <set>
#include <fstream>
#include <limits>
#include <type_traits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

using namespace nickel::diagnostics;
namespace packs = nickel::packs;
namespace io = nickel::instances::detail;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Fn> void rejected(Fn&& operation, std::string_view expected = {}) {
    try { operation(); }
    catch (const nickel::ErrorException& error) {
        if (!expected.empty()) require(error.error().code == expected, "Unexpected report rejection category");
        return;
    }
    throw std::runtime_error("Expected safe report rejection");
}
struct Fixture {
    fs::path root = fs::current_path() / ("report-" + io::random_id());
    Fixture() { require(io::create_directory_no_replace(root), "Synthetic report fixture identity conflict"); }
    ~Fixture() { try { if (io::path_exists(root)) io::remove_owned_tree(root); } catch (...) {} }
};
struct Interference final : packs::ArchiveBackend {
    std::shared_ptr<packs::ArchiveBackend> backend = packs::make_zip_backend();
    std::function<void()> after_write;
    bool corrupt_read = false;
    std::vector<packs::ArchiveEntry> inspect(const fs::path& path, const packs::ArchiveLimits& limits, std::stop_token stop) override {
        return backend->inspect(path, limits, stop);
    }
    std::string read(const fs::path& path, std::string_view name, std::uint64_t maximum, std::stop_token stop) override {
        auto result = backend->read(path, name, maximum, stop);
        if (corrupt_read && !result.empty()) result.front() ^= 1;
        return result;
    }
    void extract_file(const fs::path& path, std::string_view name, const fs::path& destination, std::uint64_t size, std::stop_token stop) override {
        backend->extract_file(path, name, destination, size, stop);
    }
    void write(const fs::path& path, std::span<const packs::ArchiveInput> inputs, const packs::ArchiveLimits& limits, std::stop_token stop) override {
        backend->write(path, inputs, limits, stop);
        if (after_write) after_write();
    }
    void release(const fs::path& path) noexcept override { backend->release(path); }
};
void require_no_staging(const fs::path& root) {
    for (const auto& entry : io::directory_entries(root))
        require(!nickel::instances::path_to_utf8(entry.path().filename()).starts_with(".nickel-export-"), "Report left an owned staging directory");
}
SafeReport synthetic_snapshot() {
    DiagnosticInput context;
    context.exit_code = 1;
    context.minecraft_version = "1.20.1";
    context.loader = "fabric";
    context.stdout_tail = "OutOfMemoryError: Java heap space";
    return {report_to_json(analyze(context)), {{"nickel-launcher.log", "OutOfMemoryError: Java heap space\n", false}}};
}

void report_target() {
    Fixture fixture;
    const ReportTarget target{fixture.root, "Pack", "launch-synthetic_01"};
    const auto encoded = report_target_to_json(target);
    require(encoded.size() == 3 && encoded["root"] == nickel::instances::path_to_utf8(fixture.root)
        && encoded["versionId"] == "Pack" && encoded["launchId"] == "launch-synthetic_01", "Public report selector changed field names");
    const auto decoded = report_target_from_json(encoded);
    require(decoded.root == target.root && decoded.version_id == target.version_id && decoded.launch_id == target.launch_id,
        "Public report selector lost the frozen launch identity");
    auto bad = encoded; bad["resourceRoot"] = nickel::instances::path_to_utf8(fixture.root);
    rejected([&] { (void)report_target_from_json(bad); }, "invalid_schema");
    bad = encoded; bad.erase("launchId");
    rejected([&] { (void)report_target_from_json(bad); }, "invalid_schema");
    bad = encoded; bad["root"] = "relative";
    rejected([&] { (void)report_target_from_json(bad); }, "unsafe_path");
    bad = encoded; bad["versionId"] = "../escape";
    rejected([&] { (void)report_target_from_json(bad); });
    bad = encoded; bad["launchId"] = "launch/private";
    rejected([&] { (void)report_target_from_json(bad); }, "invalid_schema");
    bad = encoded; bad["launchId"] = std::string(129, 'a');
    rejected([&] { (void)report_target_from_json(bad); }, "invalid_schema");
    bad = encoded; bad["launchId"] = 1;
    rejected([&] { (void)report_target_from_json(bad); }, "invalid_schema");

    DiagnosticInput empty; empty.exit_code = 2;
    auto report = analyze(empty);
    const auto brief = diagnostic_brief_to_json(report);
    require(brief.size() == 5 && brief["category"] == "unknown" && brief["fallbackReason"].is_string()
        && !brief.contains("evidence") && !brief.contains("context"), "Brief report leaked detailed diagnostic inputs");
    const auto launch = launch_error_to_json(target, report);
    require(launch["reportTarget"] == encoded && launch["diagnostic"] == brief, "Launch failure changed the public two-action DTO");
    report.summary = "Failure password=synthetic-secret /home/synthetic-user/private";
    const auto redacted = diagnostic_brief_to_json(report)["reason"].get<std::string>();
    require(redacted.find("synthetic-secret") == std::string::npos && redacted.find("synthetic-user") == std::string::npos,
        "Brief report reason leaked a secret or host path");
    report.summary = std::string(5000, 'x');
    require(diagnostic_brief_to_json(report)["reason"].get<std::string>().size() <= 1024, "Brief report reason exceeded its byte limit");
}

void report_zip() {
    Fixture fixture;
    auto backend = packs::make_zip_backend();
    const auto game = fixture.root / "game";
    fs::create_directories(game / "versions/Pack");
    io::write_exclusive(game / "versions/Pack/Pack.json", Json{{"id", "Pack"}, {"mainClass", "net.minecraft.client.main.Main"}}.dump());
    LaunchLogStore store(game);
    DiagnosticInput context; context.exit_code = 1; context.minecraft_version = "1.20.1";
    const ReportTarget target{game, "Pack", "launch-synthetic_01"};
    const auto destination = fixture.root / "Palladium-error.zip";
    {
        OutputSession output(store);
        output.add_secret("synthetic-zip-secret");
        output.bytes(ProcessStream::stdout_stream, "OutOfMemoryError: Java heap space --accessToken synthetic-zip-secret\n");
        output.bytes(ProcessStream::stderr_stream, "password=synthetic-zip-password\n");
        rejected([&] { (void)capture_error_report(target, store, context); }, "log_session_active");
        require(!fs::exists(destination), "Active log session produced a report ZIP");
        output.eof(ProcessStream::stdout_stream); output.eof(ProcessStream::stderr_stream);
        require(output.finish(std::chrono::milliseconds(20)), "Synthetic output did not drain ordered EOF");
    }
    io::atomic_write(store.paths().latest, "Mixin apply failed /home/synthetic-zip-user/game\n");
    io::atomic_write(store.paths().crash_reports / "crash-synthetic-client.txt", "UnsupportedClassVersionError\n");
    const auto captured = capture_error_report(target, store, context);
    require(captured.target().launch_id == target.launch_id && captured.logs_directory() == store.paths().logs,
        "Opaque report snapshot lost its frozen target");
    const auto returned = export_error_report_zip(destination, captured, *backend);
    require(returned == destination && fs::is_regular_file(returned), "Report ZIP did not return the exact native-dialog path");
    const auto entries = backend->inspect(returned, {8, 8 * 1024 * 1024, 8 * 1024 * 1024}, {});
    std::set<std::string> names;
    for (const auto& entry : entries) {
        require(!entry.directory && !entry.link, "Report ZIP contains a directory or link");
        names.insert(entry.name);
        const auto text = backend->read(returned, entry.name, 8 * 1024 * 1024, {});
        require(text.find("synthetic-zip-secret") == std::string::npos && text.find("synthetic-zip-password") == std::string::npos
            && text.find("synthetic-zip-user") == std::string::npos, "Report ZIP leaked synthetic credentials or user paths");
    }
    const std::set<std::string> expected{"report.json", "READ-ME.txt", "failure.txt", "logs/nickel-launcher.log", "logs/latest.log", "crash-reports/crash-synthetic-client.txt"};
    require(names == expected, "Report ZIP omitted legacy guidance or included an unapproved source");
    const auto manifest = io::parse_strict_json(backend->read(returned, "report.json", 256 * 1024, {}), 256 * 1024);
    require(manifest["schemaVersion"] == 1 && manifest["product"] == "Palladium" && manifest["diagnostic"]["category"] == "outOfMemory"
        && manifest["files"].size() == 3 && manifest["complete"] == true, "Full report ZIP manifest does not describe its actual inputs");
    require(backend->read(returned, "READ-ME.txt", 4096, {}).starts_with("Palladium complete error report\n"),
        "New full report guidance lost the Palladium display name");
    backend->release(returned);
    const auto hash = packs::hash_file(returned, "sha512");
    rejected([&] { (void)export_error_report_zip(returned, captured, *backend); }, "conflict");
    require(packs::hash_file(returned, "sha512") == hash, "Report ZIP overwrote an existing destination");

    auto snapshot = synthetic_snapshot(); snapshot.logs.front().truncated = true;
    const auto partial = fixture.root / "partial.zip";
    export_error_report_zip(partial, snapshot, *backend);
    backend->inspect(partial, {8, 8 * 1024 * 1024, 8 * 1024 * 1024}, {});
    const auto truncated = io::parse_strict_json(backend->read(partial, "report.json", 256 * 1024, {}), 256 * 1024);
    require(truncated["schemaVersion"] == 1 && truncated["product"] == "Palladium"
        && truncated["complete"] == false && !truncated["notes"].empty(), "Bounded report falsely claimed full source completeness");
    require(backend->read(partial, "READ-ME.txt", 4096, {}).starts_with("Palladium diagnostic excerpts\n"),
        "New excerpt guidance lost the Palladium display name");
    backend->release(partial);
    const SafeReport no_logs{report_to_json(analyze(DiagnosticInput{})), {}};
    const auto empty = fixture.root / "no-logs.zip";
    export_error_report_zip(empty, no_logs, *backend);
    backend->inspect(empty, {8, 8 * 1024 * 1024, 8 * 1024 * 1024}, {});
    const auto missing = io::parse_strict_json(backend->read(empty, "report.json", 256 * 1024, {}), 256 * 1024);
    require(missing["files"].empty() && missing["complete"] == false && !missing["notes"].empty(), "No-log excerpt report claimed full evidence");
    backend->release(empty);
    require_no_staging(fixture.root);
}
void full_sources() {
    static_assert(!std::is_default_constructible_v<ErrorReportSnapshot>);
    Fixture fixture;
    const auto game = fixture.root / "game";
    fs::create_directories(game / "versions/Pack");
    io::write_exclusive(game / "versions/Pack/Pack.json", Json{{"id", "Pack"}}.dump());
    std::optional<LaunchLogStore> retained_store;
    const ReportTarget target{game, "Pack", "launch-full_01"};
    DiagnosticInput context; context.exit_code = 1; context.minecraft_version = "1.20.1";
    context.java_vendor = "synthetic-full-auth";
    {
        OutputSession output{LaunchLogStore(game)};
        output.add_secret("synthetic-full-auth");
        retained_store = output.log_store();
        output.add_secret("synthetic-full-late-plan");
        output.bytes(ProcessStream::stdout_stream, "unlabelled synthetic-full-");
        output.bytes(ProcessStream::stdout_stream, "auth\n");
        output.eof(ProcessStream::stdout_stream); output.eof(ProcessStream::stderr_stream);
        require(output.finish(std::chrono::milliseconds(20)), "Full-source launch output did not drain");
    }
    const auto& store = *retained_store;
    require(store.redact_report("synthetic-full-late-plan").text == "***",
        "Copied store did not share secrets registered before process spawn");
    const LaunchLogStore unrelated(game);
    require(unrelated.redact_report("synthetic-full-auth").text == "synthetic-full-auth",
        "Launch secrets leaked into unrelated stores constructed from the same path");
    std::string large = "BEGIN-COMPLETE-GAME-LOG\n";
    const auto line = std::string(4095, 'a') + '\n';
    while (large.size() < 9 * 1024 * 1024) large += line;
    large += "unlabelled synthetic-full-auth\nEND-COMPLETE-GAME-LOG\nOutOfMemoryError: Java heap space\n";
    const auto expected = store.redact_report(large);
    require(expected.safe && expected.text.size() > 8 * 1024 * 1024
        && expected.text.find("synthetic-full-auth") == std::string::npos,
        "Retained launch store lost secrets or the full log excerpt boundary");
    io::write_exclusive(store.paths().latest, large);
    io::write_exclusive(store.paths().logs / "debug.log", "-----BEGIN PRIVATE KEY-----\nsynthetic-pem-body\nsecond-pem-line\n-----END PRIVATE KEY-----\n"
        "password: [\n\"synthetic-full-password\",\n{\"apiKey\":\"nested-value\"}\n]\nDEBUG-END\n");
    io::write_exclusive(store.paths().logs / "nickel-launcher.log.1", "COMPLETE-LAUNCH-ARCHIVE\n");
    fs::create_directories(store.paths().crash_reports);
    io::write_exclusive(store.paths().crash_reports / "crash-full-client.txt", "FULL-CRASH-EVIDENCE synthetic-full-auth\n");
    io::write_exclusive(store.paths().logs / "accounts.json", "DO-NOT-COLLECT-ACCOUNT\n");
    const auto latest = store.latest();
    const auto crashes = store.crashes();
    require(latest && latest->text.find("synthetic-full-auth") == std::string::npos
        && !crashes.empty() && crashes.front().text.find("synthetic-full-auth") == std::string::npos,
        "Retained store query exposed raw game or crash secrets after the session ended");
    auto backend = packs::make_zip_backend();
    {
        const auto snapshot = capture_error_report(target, store, context);
        require(snapshot.diagnosis().category == "outOfMemory" && snapshot.diagnosis().input_truncated,
            "Brief diagnosis did not remain bounded separately from full sources");
        const auto copy = snapshot;
        io::atomic_write(store.paths().latest, "NEW-LAUNCH-UNRELATED-CONTENT\n");
        io::atomic_write(store.paths().logs / "debug.log", "NEW-DEBUG-CONTENT\n");
        const auto destination = fixture.root / "complete.zip";
        export_error_report_zip(destination, copy, *backend);
        const packs::ArchiveLimits limits{131, 64 * 1024 * 1024, 256 * 1024 * 1024};
        const auto entries = backend->inspect(destination, limits, {});
        require(entries.size() == 8, "Full source archive omitted logs/debug/archives or included account material");
        require(backend->read(destination, "logs/latest.log", limits.file_bytes, {}) == expected.text,
            "Full ZIP truncated a 9 MiB game log or reread changed current sources");
        const auto debug = backend->read(destination, "logs/debug.log", limits.file_bytes, {});
        require(debug.find("PRIVATE KEY") == std::string::npos && debug.find("synthetic-pem-body") == std::string::npos
            && debug.find("synthetic-full-password") == std::string::npos && debug.find("DEBUG-END") != std::string::npos,
            "Full ZIP leaked multiline private material or dropped safe source content");
        require(backend->read(destination, "logs/nickel-launcher.log.1", limits.file_bytes, {}) == "COMPLETE-LAUNCH-ARCHIVE\n",
            "Full ZIP did not retain launcher archive evidence");
        require(backend->read(destination, "crash-reports/crash-full-client.txt", limits.file_bytes, {}).find("synthetic-full-auth") == std::string::npos,
            "Full ZIP lost the launch-specific raw crash credential redaction");
        const auto manifest = backend->read(destination, "report.json", 256 * 1024, {});
        require(manifest.find("synthetic-full-auth") == std::string::npos, "Full report metadata leaked a known unlabelled secret");
        const auto index = io::parse_strict_json(manifest, 256 * 1024);
        require(index["schemaVersion"] == 1 && index["product"] == "Palladium" && index["complete"] == true,
            "Full ZIP changed its schema or did not describe complete selected sources");
        backend->release(destination);
        rejected([&] { export_error_report_zip(store.paths().logs / "inside.zip", copy, *backend); }, "unsafe_path");
        std::stop_source stopped; stopped.request_stop();
        rejected([&] { export_error_report_zip(fixture.root / "cancel-full.zip", copy, *backend, stopped.get_token()); }, "cancelled");
        require(copy.target().root == target.root && copy.target().version_id == "Pack" && copy.target().launch_id == "launch-full_01",
            "Immutable snapshot lost its frozen launch selector");
    }
    require_no_staging(game);
    require_no_staging(fixture.root);
    std::cout << "full 9 MiB/retained launch secrets/isolated stores/PEM/collection/debug/archive/immutable capture: PASS\n";
}
void sparse_source(const fs::path& path, std::uint64_t bytes) {
    fs::create_directories(path.parent_path());
#ifdef _WIN32
    struct Lease {
        HANDLE handle;
        ~Lease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(file.handle != INVALID_HANDLE_VALUE, "Cannot create sparse quota fixture");
    DWORD returned = 0;
    require(DeviceIoControl(file.handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr),
        "Quota fixture requires native sparse-file support");
    LARGE_INTEGER end{}; end.QuadPart = static_cast<LONGLONG>(bytes);
    require(SetFilePointerEx(file.handle, end, nullptr, FILE_BEGIN) && SetEndOfFile(file.handle), "Cannot size sparse quota fixture");
#else
    std::ofstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "Cannot create sparse quota fixture");
    file.close();
    fs::resize_file(path, bytes);
#endif
}
void full_security() {
    Fixture fixture;
    const auto game = fixture.root / "game";
    fs::create_directories(game / "versions/Pack");
    io::write_exclusive(game / "versions/Pack/Pack.json", Json{{"id", "Pack"}}.dump());
    LaunchLogStore store(game);
    const ReportTarget target{game, "Pack", "launch-security_01"};
    DiagnosticInput context; context.exit_code = 1;
    fs::create_directories(store.paths().logs);
    io::write_exclusive(store.paths().latest, "-----BEGIN RSA PRIVATE KEY-----\nsynthetic-unclosed-key\n");
    rejected([&] { (void)capture_error_report(target, store, context); }, "unsafe_report");
    require_no_staging(game);
    io::atomic_write(store.paths().latest, std::string(1024 * 1024 + 1, 'a'));
    rejected([&] { (void)capture_error_report(target, store, context); }, "limit");
    require_no_staging(game);
    io::atomic_write(store.paths().latest, std::string(1024 * 1024 - 1, 'a') + '\n');
    { const auto boundary = capture_error_report(target, store, context); }
    require_no_staging(game);
    io::atomic_write(store.paths().latest, "source before capture\n");
    const auto original_hash = packs::hash_file(store.paths().latest, "sha512");
    {
        packs::detail::NativeFile pin(store.paths().latest, packs::detail::NativeFile::Mode::read);
        std::ofstream mutation(store.paths().latest, std::ios::binary | std::ios::app);
#ifdef _WIN32
        require(!mutation, "Pinned report source permitted an external writer");
#else
        mutation << "mutation\n";
        mutation.close();
        rejected([&] { pin.unchanged(); }, "source_changed");
#endif
    }
#ifdef _WIN32
    require(packs::hash_file(store.paths().latest, "sha512") == original_hash, "Denied writer changed pinned source content");
#else
    (void)original_hash;
#endif
    fs::remove(store.paths().latest);
    sparse_source(store.paths().latest, 64ULL * 1024 * 1024 + 1);
    rejected([&] { (void)capture_error_report(target, store, context); }, "limit");
    require_no_staging(game);
    fs::remove(store.paths().latest);
    for (const auto name : {"nickel-launcher.log", "latest.log", "debug.log", "nickel-diagnostic.json"})
        sparse_source(store.paths().logs / name, 64ULL * 1024 * 1024);
    io::write_exclusive(store.paths().logs / "nickel-launch-failure.txt", "x");
    rejected([&] { (void)capture_error_report(target, store, context); }, "limit");
    require_no_staging(game);
    for (const auto& entry : fs::directory_iterator(store.paths().logs)) fs::remove(entry.path());
    fs::create_directories(store.paths().crash_reports);
    for (int index = 0; index < 129; ++index)
        io::write_exclusive(store.paths().crash_reports / ("crash-" + std::to_string(index) + ".txt"), "evidence\n");
    rejected([&] { (void)capture_error_report(target, store, context); }, "limit");
    require_no_staging(game);
    fs::remove_all(store.paths().crash_reports);
    io::write_exclusive(store.paths().latest, "ordinary source\n");
    const auto outside = fixture.root / "outside";
    fs::create_directories(outside);
    rejected([&] { (void)capture_error_report(target, LaunchLogStore(outside), context); }, "identity_mismatch");
    std::stop_source stopped; stopped.request_stop();
    rejected([&] { (void)capture_error_report(target, store, context, stopped.get_token()); }, "cancelled");
    constexpr std::uint64_t reserve = 1024 * 1024, required = 2 * 1024 * 1024;
    detail::verify_report_space(required + reserve, required);
    rejected([&] { detail::verify_report_space(required + reserve - 1, required); }, "insufficient_space");
    rejected([&] { detail::verify_report_space(required - 1, required); }, "insufficient_space");
    rejected([&] { detail::verify_report_space(std::numeric_limits<std::uint64_t>::max(), required); }, "insufficient_space");
    {
        auto snapshot = capture_error_report(target, store, context);
        fs::path private_directory;
        for (const auto& entry : fs::directory_iterator(game))
            if (nickel::instances::path_to_utf8(entry.path().filename()).starts_with(".nickel-export-")) private_directory = entry.path();
        require(!private_directory.empty(), "Cannot locate the synthetic owned capture workspace");
        std::error_code denied;
        const auto replaced = game / "moved-snapshot";
        fs::rename(private_directory, replaced, denied);
#ifdef _WIN32
        require(static_cast<bool>(denied), "Immutable snapshot directory identity was replaceable");
#else
        require(!denied, "POSIX source mutation fixture failed");
        auto backend = packs::make_zip_backend();
        rejected([&] { export_error_report_zip(fixture.root / "changed-source.zip", snapshot, *backend); });
        fs::rename(replaced, private_directory);
#endif
    }
    require_no_staging(game);
    {
        OutputSession output(store);
        output.bytes(ProcessStream::stdout_stream, "password: [\n");
        output.producer_disappeared();
        require(!output.finish(std::chrono::milliseconds(0)), "Undrained sensitive producer was accepted");
    }
    rejected([&] { (void)capture_error_report(target, store, context); }, "unsafe_report");
    require_no_staging(game);
    std::cout << "full report line/file/total/count quotas/pinned identity/preflight/cancel/unsafe drain: PASS\n";
}

void report_security() {
    Fixture fixture;
    Interference backend;
    const auto snapshot = synthetic_snapshot();
    auto malformed = snapshot; malformed.logs.front().text = "password: [\nsynthetic-unclosed";
    rejected([&] { export_error_report_zip(fixture.root / "unsafe.zip", malformed, backend); }, "unsafe_report");
    malformed = snapshot; malformed.logs.front().name = "accounts.json";
    rejected([&] { export_error_report_zip(fixture.root / "account.zip", malformed, backend); }, "unsafe_report");
    malformed = snapshot; malformed.logs.front().name = "crash-evil/nested.txt";
    rejected([&] { export_error_report_zip(fixture.root / "nested.zip", malformed, backend); }, "unsafe_report");
    malformed = snapshot; malformed.logs.push_back(malformed.logs.front());
    rejected([&] { export_error_report_zip(fixture.root / "duplicate.zip", malformed, backend); }, "conflict");
    malformed = snapshot; malformed.logs.front().text.assign(4 * 1024 * 1024 + 1, 'x');
    rejected([&] { export_error_report_zip(fixture.root / "limit.zip", malformed, backend); }, "limit");
    rejected([&] { export_error_report_zip(fixture.root / "wrong.txt", snapshot, backend); }, "unsafe_path");
    std::stop_source stopped; stopped.request_stop();
    rejected([&] { export_error_report_zip(fixture.root / "cancelled.zip", snapshot, backend, stopped.get_token()); }, "cancelled");

    backend.corrupt_read = true;
    rejected([&] { export_error_report_zip(fixture.root / "corrupt.zip", snapshot, backend); }, "integrity");
    require(!fs::exists(fixture.root / "corrupt.zip"), "Changed ZIP content was published");
    backend.corrupt_read = false;
    std::stop_source racing_stop;
    backend.after_write = [&] { racing_stop.request_stop(); };
    rejected([&] { export_error_report_zip(fixture.root / "cancel-after-write.zip", snapshot, backend, racing_stop.get_token()); }, "cancelled");
    require(!fs::exists(fixture.root / "cancel-after-write.zip"), "Cancelled ZIP was published");
    const auto conflict = fixture.root / "race.zip";
    backend.after_write = [&] { io::write_exclusive(conflict, "synthetic external report collision"); };
    rejected([&] { export_error_report_zip(conflict, snapshot, backend); }, "conflict");
    require(io::read_bounded(conflict, 128) == "synthetic external report collision", "ZIP publication changed an external race winner");
    require_no_staging(fixture.root);
}

void report_long_paths() {
    Fixture fixture;
    auto base = fixture.root;
    while (nickel::instances::path_to_utf8(base).size() <= 365)
        base /= "original-logical-report-long-component";
    const auto game = base / "game";
    const auto save_parent = base / "original-native-dialog-save-component" / "original-dialog-long-component";
    const auto destination = save_parent / "Palladium-complete-error-report.zip";
    require(nickel::instances::path_to_utf8(game).size() > 365
        && nickel::instances::path_to_utf8(destination).size() > 440
        && !nickel::instances::path_to_utf8(game).starts_with("\\\\?\\"),
        "Deep report fixture shortened or exposed a native-prefix logical path");
    io::create_directories(game / "versions/Pack");
    io::create_directories(save_parent);
    io::write_exclusive(game / "versions/Pack/Pack.json", Json{{"id", "Pack"}}.dump());
    LaunchLogStore store(game);
    const ReportTarget target{game, "Pack", "launch-deep_01"};
    DiagnosticInput context; context.exit_code = 1;
    {
        OutputSession output(store);
        output.add_secret("synthetic-deep-report-secret");
        output.bytes(ProcessStream::stdout_stream, "OutOfMemoryError: Java heap space synthetic-deep-report-secret\n");
        output.eof(ProcessStream::stdout_stream); output.eof(ProcessStream::stderr_stream);
        require(output.finish(std::chrono::milliseconds(20)), "Deep logical output did not drain ordered EOF");
    }
    const auto latest = std::string("COMPLETE-DEEP-GAME-LOG synthetic-deep-report-secret\n");
    const auto crash = std::string("COMPLETE-DEEP-CRASH synthetic-deep-report-secret\n");
    io::write_exclusive(store.paths().latest, latest);
    io::write_exclusive(store.paths().logs / "debug.log", "COMPLETE-DEEP-DEBUG\n");
    io::write_exclusive(store.paths().logs / "accounts.json", "DO-NOT-COLLECT-DEEP-ACCOUNT\n");
    io::create_directories(store.paths().crash_reports);
    io::write_exclusive(store.paths().crash_reports / "crash-deep-client.txt", crash);
    auto backend = packs::make_zip_backend();
    {
        const auto snapshot = capture_error_report(target, store, context);
        require(snapshot.target().root == game && snapshot.logs_directory() == store.paths().logs,
            "Deep report snapshot lost its original logical selector");
        io::atomic_write(store.paths().latest, "NEW-DEEP-LAUNCH-CONTENT\n");
        const auto returned = export_error_report_zip(destination, snapshot, *backend);
        require(returned == destination && fs::is_regular_file(io::native_path(returned)),
            "Deep report did not return the exact original native-dialog logical path");
        const packs::ArchiveLimits limits{8, 8 * 1024 * 1024, 8 * 1024 * 1024};
        const auto entries = backend->inspect(returned, limits, {});
        std::set<std::string> names;
        for (const auto& entry : entries) {
            require(!entry.directory && !entry.link, "Deep report ZIP contains an unapproved object");
            names.insert(entry.name);
            const auto text = backend->read(returned, entry.name, limits.file_bytes, {});
            require(text.find("synthetic-deep-report-secret") == std::string::npos
                && text.find("DO-NOT-COLLECT-DEEP-ACCOUNT") == std::string::npos,
                "Deep report leaked credentials or collected account data");
        }
        const std::set<std::string> expected{"report.json", "READ-ME.txt", "failure.txt", "logs/nickel-launcher.log",
            "logs/latest.log", "logs/debug.log", "crash-reports/crash-deep-client.txt"};
        require(names == expected, "Deep report omitted sources or enumerated a different directory");
        require(backend->read(returned, "logs/latest.log", limits.file_bytes, {}) == store.redact_report(latest).text
            && backend->read(returned, "crash-reports/crash-deep-client.txt", limits.file_bytes, {}) == store.redact_report(crash).text,
            "Deep report lost complete sources or reread current launch content");
        const auto manifest = io::parse_strict_json(backend->read(returned, "report.json", 256 * 1024, {}), 256 * 1024);
        require(manifest["schemaVersion"] == 1 && manifest["product"] == "Palladium" && manifest["complete"] == true,
            "Deep report changed schema or source completeness");
        backend->release(returned);
        const auto digest = packs::hash_file(returned, "sha512");
        rejected([&] { export_error_report_zip(returned, snapshot, *backend); }, "conflict");
        require(packs::hash_file(returned, "sha512") == digest, "Deep report overwrote an existing destination");
        rejected([&] { export_error_report_zip(store.paths().logs / "inside.zip", snapshot, *backend); }, "unsafe_path");
        rejected([&] { export_error_report_zip(store.paths().crash_reports / "inside.zip", snapshot, *backend); }, "unsafe_path");
        std::stop_source stopped; stopped.request_stop();
        const auto cancelled = save_parent / "cancelled.zip";
        rejected([&] { export_error_report_zip(cancelled, snapshot, *backend, stopped.get_token()); }, "cancelled");
        require(!io::path_exists(cancelled), "Deep report published after cancellation");
    }
    require_no_staging(game);
    require_no_staging(save_parent);
    std::cout << "original logical >365 source/>440 dialog/full crash and debug/secret redaction/no overwrite/no staging: PASS\n";
}

} // namespace
int main(int argc, char** argv) {
    try {
        const std::string_view group = argc > 1 ? argv[1] : "all";
        if (group == "target" || group == "all") report_target();
        if (group == "zip" || group == "all") report_zip();
        if (group == "full" || group == "all") { full_sources(); full_security(); }
        if (group == "security" || group == "all") report_security();
        if (group == "longpath" || group == "all") report_long_paths();
        std::cout << "Report export contracts passed: " << group << '\n';
        return 0;
    } catch (const nickel::ErrorException& error) {
        std::cerr << error.error().code << ": " << error.error().message << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
