#include "diagnostics.hpp"
#include "core/packs/internal.hpp"
#include "report_detail.hpp"
#include "core/config/path_guard.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <type_traits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nickel::diagnostics {
namespace {
namespace io = instances::detail;
constexpr std::size_t report_max_bytes = 8 * 1024 * 1024;
constexpr std::size_t diagnosis_max_bytes = 256 * 1024;
constexpr std::size_t full_file_max_bytes = 64 * 1024 * 1024;
constexpr std::size_t full_total_max_bytes = 256 * 1024 * 1024;
constexpr std::size_t full_max_files = 128;
constexpr std::size_t full_line_max_bytes = 1024 * 1024;
constexpr std::size_t full_max_archives = 3;

void validate_target(const ReportTarget& target) {
    if (!target.root.is_absolute() || instances::path_to_utf8(target.root).size() > 32768)
        throw instances::Error("unsafe_path", "Report target requires an absolute registered game root");
    instances::reject_links(target.root);
    instances::validate_name(target.version_id);
    if (target.launch_id.empty() || target.launch_id.size() > 128
        || !std::all_of(target.launch_id.begin(), target.launch_id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        })) throw instances::Error("invalid_schema", "Report target launch identity is invalid");
}

std::string safe_text(std::string_view input, std::size_t maximum, const LaunchLogStore* store = nullptr) {
    if (input.size() > maximum) throw instances::Error("limit", "Diagnostic ZIP content exceeds its bounded quota");
    const auto decoded = utf8_lossy(input);
    const auto redacted = store ? store->redact_report(decoded) : redact_checked(decoded);
    if (!redacted.safe || redacted.text.size() > maximum)
        throw instances::Error("unsafe_report", "Incomplete or unsafe content prevents diagnostic ZIP export");
    return redacted.text;
}
std::string safe_line(std::string_view input) {
    return bounded_safe_line(safe_text(input, 32 * 1024), 1024);
}
std::string log_entry_name(std::string_view name) {
    if (name == log_file_name || name == "latest.log") return "logs/" + std::string(name);
    if (name.size() <= 128 && name.starts_with("crash-") && name.ends_with(".txt")
        && name.find_first_of("/\\") == std::string_view::npos) {
        packs::validate_archive_path(name);
        return "crash-reports/" + std::string(name);
    }
    throw instances::Error("unsafe_report", "Diagnostic ZIP contains an unapproved log source");
}
std::int64_t generated_at() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::vector<packs::ArchiveInput> report_inputs(const SafeReport& snapshot) {
    if (!snapshot.diagnosis.is_object() || snapshot.logs.size() > 5)
        throw instances::Error("limit", "Diagnostic ZIP snapshot exceeds its source quota");
    const auto diagnosis_text = safe_text(snapshot.diagnosis.dump(2), diagnosis_max_bytes);
    const auto diagnosis = io::parse_strict_json(diagnosis_text, diagnosis_max_bytes);
    std::vector<packs::ArchiveInput> inputs;
    std::set<std::string> names;
    Json files = Json::array(), notes = Json::array();
    std::size_t total = diagnosis_text.size();
    for (const auto& log : snapshot.logs) {
        const auto name = log_entry_name(log.name);
        if (!names.insert(io::ascii_lower(name)).second)
            throw instances::Error("conflict", "Diagnostic ZIP contains duplicate log sources");
        const auto maximum = log.name == log_file_name ? capture_max_bytes
            : log.name == "latest.log" ? 256 * 1024 : 512 * 1024;
        auto text = safe_text(log.text, maximum);
        if (text.size() > report_max_bytes - total) throw instances::Error("limit", "Diagnostic ZIP exceeds total byte quota");
        total += text.size();
        files.push_back(name);
        if (log.truncated) notes.push_back(name + " is a bounded excerpt, not a complete source file.");
        inputs.push_back({name, std::nullopt, std::move(text)});
    }
    if (files.empty()) notes.push_back("No game log or crash report was captured for this failed launch.");
    notes.push_back("This ZIP contains diagnostic excerpts, not a complete full-source error report.");
    Json manifest{{"schemaVersion", 1}, {"product", "Palladium"}, {"generatedAt", generated_at()},
        {"complete", false}, {"files", std::move(files)}, {"notes", std::move(notes)}, {"diagnostic", diagnosis}};
    auto index = safe_text(manifest.dump(2), diagnosis_max_bytes);
    const auto reason = diagnosis.contains("summary") && diagnosis["summary"].is_string()
        ? safe_line(diagnosis["summary"].get<std::string>()) : "No diagnostic summary is available.";
    constexpr std::string_view notice = "Palladium diagnostic excerpts\n\nSend this ZIP when requesting help, rather than only a screenshot.\n"
        "Included logs and diagnostic fields are bounded and redacted. report.json lists omitted or truncated inputs honestly.\n"
        "No account configuration, credentials, private keys, saved worlds or resource packs are collected.\n"
        "Review mod-written personal information before sharing.\n";
    if (index.size() + reason.size() + notice.size() > report_max_bytes - total)
        throw instances::Error("limit", "Diagnostic ZIP metadata exceeds total byte quota");
    inputs.push_back({"report.json", std::nullopt, std::move(index)});
    inputs.push_back({"READ-ME.txt", std::nullopt, std::string(notice)});
    inputs.push_back({"failure.txt", std::nullopt, reason});
    return inputs;
}

struct ArchiveLease {
    packs::ArchiveBackend& backend;
    fs::path path;
    ~ArchiveLease() { backend.release(path); }
};
struct ReportSource {
    std::string name;
    fs::path file;
    std::uint64_t size = 0;
    std::string sha512;
};
std::string report_excerpt(std::string_view text, std::size_t maximum) {
    if (text.size() <= maximum) return std::string(text);
    auto start = text.size() - maximum;
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) ++start;
    return std::string(text.substr(start));
}
void require_report_space(const fs::path& parent, std::uint64_t needed) {
    foundation_detail::DirectoryGuard pinned(parent);
#ifdef _WIN32
    // MinGW's filesystem::space rejects extended paths even when the directory is valid.
    auto native = io::native_path(parent).native();
    if (!native.empty() && native.back() != L'\\') native += L'\\';
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(native.c_str(), &available, nullptr, nullptr))
        throw instances::Error("insufficient_space", "Cannot verify space for a safe error-report snapshot");
    pinned.verify();
    detail::verify_report_space(available.QuadPart, needed);
#else
    std::error_code error;
    const auto available = fs::space(parent, error);
    pinned.verify();
    if (error) throw instances::Error("insufficient_space", "Cannot verify space for a safe error-report snapshot");
    detail::verify_report_space(available.available, needed);
#endif
}
bool report_path_inside(const fs::path& path, const fs::path& directory) {
    const auto target = instances::root_identity(instances::path_to_utf8(io::canonical_path(path)));
    const auto parent = instances::root_identity(instances::path_to_utf8(io::canonical_path(directory)));
    return packs::detail::within(target, parent);
}
std::string sanitized_source(packs::detail::NativeFile& input, std::uint64_t expected,
                              const LaunchLogStore& store, std::stop_token cancel) {
    if (input.size() != expected) throw instances::Error("source_changed", "Full report source changed after selection");
    if (expected > full_file_max_bytes) throw instances::Error("limit", "Full report source exceeds its file quota");
    std::string raw;
    raw.reserve(static_cast<std::size_t>(expected));
    struct Erase { std::string& text; ~Erase() { std::fill(text.begin(), text.end(), '\0'); } } erase{raw};
    std::array<char, packs::detail::copy_buffer_bytes> buffer{};
    std::uint64_t offset = 0;
    std::size_t line_bytes = 0;
    while (offset < expected) {
        io::check_cancelled(cancel);
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), expected - offset));
        if (input.read_at(offset, buffer.data(), count) != count)
            throw instances::Error("source_changed", "Full report source ended before its selected size");
        for (std::size_t index = 0; index < count; ++index) {
            if (++line_bytes > full_line_max_bytes) throw instances::Error("limit", "Full report source has an overlong line");
            if (buffer[index] == '\n') line_bytes = 0;
        }
        raw.append(buffer.data(), count);
        offset += count;
    }
    std::fill(buffer.begin(), buffer.end(), '\0');
    input.unchanged();
    io::check_cancelled(cancel);
    auto decoded = utf8_lossy(raw);
    Erase erase_decoded{decoded};
    if (decoded.size() > full_file_max_bytes) throw instances::Error("limit", "Full report UTF-8 source exceeds its file quota");
    auto safe = store.redact_report(decoded);
    if (!safe.safe) throw instances::Error("unsafe_report", "Incomplete sensitive fields prevent a full error report");
    if (safe.text.size() > full_file_max_bytes) throw instances::Error("limit", "Redacted full report source exceeds its file quota");
    input.unchanged();
    io::check_cancelled(cancel);
    return std::move(safe.text);
}
fs::path write_report_zip(const fs::path& destination, std::vector<packs::ArchiveInput> inputs,
                          const packs::ArchiveLimits& limits, packs::ArchiveBackend& backend, std::stop_token cancel,
                          const std::function<void()>& verify_sources = {}) {
    io::check_cancelled(cancel);
    if (!destination.is_absolute() || io::ascii_lower(instances::path_to_utf8(destination.extension())) != ".zip")
        throw instances::Error("unsafe_path", "Diagnostic ZIP destination must be an absolute .zip file");
    instances::reject_links(destination);
    packs::validate_archive_path(instances::path_to_utf8(destination.filename()));
    if (io::path_exists(destination)) throw instances::Error("conflict", "Diagnostic ZIP refuses to overwrite an existing file");
    if (!fs::is_directory(io::native_path(destination.parent_path())))
        throw instances::Error("unsafe_path", "Diagnostic ZIP destination requires an existing native-dialog parent");
    foundation_detail::DirectoryGuard destination_pin(destination.parent_path());
    static_assert(std::is_nothrow_move_constructible_v<fs::path>);
    // Allocate the logical result before publication; returning it must not fail after the ZIP is visible.
    fs::path result = destination;
    std::uint64_t total = 0;
    for (const auto& input : inputs) {
        const auto bytes = input.file ? fs::file_size(io::native_path(*input.file)) : input.bytes.size();
        if (bytes > limits.file_bytes || bytes > limits.total_bytes - total)
            throw instances::Error("limit", "Diagnostic ZIP sources exceed their file or total quota");
        total += bytes;
    }
    if (inputs.size() > limits.entries) throw instances::Error("limit", "Diagnostic ZIP exceeds its entry quota");
    const auto overhead = inputs.size() * 8192ULL + 1024 * 1024;
    require_report_space(destination.parent_path(), 2 * (total + overhead));
    packs::detail::Workspace workspace(destination.parent_path());
    const auto staged = workspace.directory() / "report.zip";
    ArchiveLease release{backend, staged};
    if (verify_sources) verify_sources();
    backend.write(staged, inputs, limits, cancel);
    io::check_cancelled(cancel);
    if (verify_sources) verify_sources();
    const auto entries = backend.inspect(staged, limits, cancel);
    packs::validate_archive_entries(entries, limits);
    if (entries.size() != inputs.size()) throw instances::Error("integrity", "Diagnostic ZIP verification has an unexpected entry count");
    std::map<std::string, const packs::ArchiveInput*> expected;
    for (const auto& input : inputs) {
        if (!expected.emplace(input.name, &input).second) throw instances::Error("conflict", "Diagnostic ZIP contains duplicate sources");
    }
    for (const auto& entry : entries) {
        io::check_cancelled(cancel);
        const auto found = expected.find(entry.name);
        if (found == expected.end() || entry.directory || entry.link)
            throw instances::Error("integrity", "Diagnostic ZIP verification found a different source identity");
        const auto& input = *found->second;
        const auto bytes = input.file ? io::read_bounded(*input.file, static_cast<std::size_t>(limits.file_bytes)) : input.bytes;
        if (entry.size != bytes.size() || backend.read(staged, entry.name, limits.file_bytes, cancel) != bytes)
            throw instances::Error("integrity", "Diagnostic ZIP verification found changed content");
        expected.erase(found);
    }
    if (!expected.empty()) throw instances::Error("integrity", "Diagnostic ZIP verification omitted a source");
    backend.release(staged);
    if (verify_sources) verify_sources();
    const auto digest = packs::hash_file(staged, "sha256", cancel);
    destination_pin.verify();
    io::copy_no_replace(staged, destination, limits.total_bytes + overhead, cancel, digest);
    return result;
}
} // namespace

struct ErrorReportSnapshot::State {
    ReportTarget target;
    fs::path logs;
    DiagnosticReport diagnosis;
    std::unique_ptr<packs::detail::Workspace> workspace;
    std::unique_ptr<foundation_detail::DirectoryGuard> directory_pin;
    std::vector<ReportSource> sources;
    std::vector<packs::ArchiveInput> metadata;
    std::vector<std::unique_ptr<packs::detail::NativeFile>> pins;
};
ErrorReportSnapshot::ErrorReportSnapshot(std::shared_ptr<const State> state) : state_(std::move(state)) {}
const ReportTarget& ErrorReportSnapshot::target() const noexcept { return state_->target; }
const fs::path& ErrorReportSnapshot::logs_directory() const noexcept { return state_->logs; }
const DiagnosticReport& ErrorReportSnapshot::diagnosis() const noexcept { return state_->diagnosis; }

Json report_target_to_json(const ReportTarget& target) {
    validate_target(target);
    return {{"root", instances::path_to_utf8(target.root)}, {"versionId", target.version_id}, {"launchId", target.launch_id}};
}
ReportTarget report_target_from_json(const Json& value) {
    constexpr std::string_view keys[]{"root", "versionId", "launchId"};
    io::strict_keys(value, keys, keys);
    const auto root = io::string_at(value, "root"), version = io::string_at(value, "versionId"), launch = io::string_at(value, "launchId");
    if (!root || !version || !launch) throw instances::Error("invalid_schema", "Report target fields must be strings");
    ReportTarget target{instances::path_from_utf8(*root), *version, *launch};
    validate_target(target);
    return target;
}
Json diagnostic_brief_to_json(const DiagnosticReport& report) {
    return {{"category", safe_line(report.category)}, {"severity", safe_line(report.severity)},
        {"reason", safe_line(report.summary)},
        {"fallbackReason", report.fallback_reason ? Json(safe_line(*report.fallback_reason)) : Json(nullptr)},
        {"inputTruncated", report.input_truncated}};
}
Json launch_error_to_json(const ReportTarget& target, const DiagnosticReport& report) {
    return {{"reportTarget", report_target_to_json(target)}, {"diagnostic", diagnostic_brief_to_json(report)}};
}

ErrorReportSnapshot capture_error_report(const ReportTarget& target, const LaunchLogStore& store,
                                         const DiagnosticInput& context, std::stop_token cancel) {
    io::check_cancelled(cancel);
    validate_target(target);
    const auto instance = instances::checked_instance_directory(target.root, target.version_id);
    if (instances::is_pending(instance)) throw instances::Error("pending_instance", "Pending installation cannot provide a failed-launch report");
    const auto resource = io::logical_path(store.paths().root);
    const auto identity = instances::root_identity(instances::path_to_utf8(io::canonical_path(resource)));
    const auto root_identity = instances::root_identity(instances::path_to_utf8(io::canonical_path(target.root)));
    const auto instance_identity = instances::root_identity(instances::path_to_utf8(io::canonical_path(instance)));
    if (identity != root_identity && identity != instance_identity)
        throw instances::Error("identity_mismatch", "Failed-launch logs do not belong to the frozen root and instance");
    const auto paths = resolve_log_paths(resource);
    if (io::logical_path(store.paths().logs) != paths.logs.lexically_normal())
        throw instances::Error("identity_mismatch", "Failed-launch log paths differ from the frozen resource root");
    FinishedLogLease lease(paths.launcher);
    DiagnosticInput input;
    input.exit_code = context.exit_code;
    input.minecraft_version = safe_text(context.minecraft_version, 512, &store);
    input.loader = safe_text(context.loader, 512, &store);
    input.java_major = context.java_major;
    input.input_truncated = context.input_truncated;
    if (context.optifine) input.optifine = safe_text(*context.optifine, 512, &store);
    if (context.java_vendor) input.java_vendor = safe_text(*context.java_vendor, 4096, &store);
    input.stdout_tail = safe_text(context.stdout_tail, capture_max_bytes, &store);
    input.stderr_tail = safe_text(context.stderr_tail, capture_max_bytes, &store);
    if (context.latest_log) input.latest_log = safe_text(*context.latest_log, capture_max_bytes, &store);
    if (context.crash_report) input.crash_report = safe_text(*context.crash_report, capture_max_bytes, &store);
    struct SelectedSource {
        std::string name;
        std::unique_ptr<packs::detail::NativeFile> file;
        std::uint64_t size;
    };
    std::vector<SelectedSource> selected;
    std::set<std::string> names;
    std::uint64_t source_total = 0, possible_total = 0;
    Json files = Json::array(), notes = Json::array();
    const auto select = [&](std::string name, const fs::path& file) {
        io::check_cancelled(cancel);
        instances::reject_links(file);
        std::error_code ec;
        const auto status = fs::symlink_status(io::native_path(file), ec);
        if (status.type() == fs::file_type::not_found || ec == std::errc::no_such_file_or_directory) return;
        if (ec || !fs::is_regular_file(status)) throw instances::Error("unsafe_report", "Full report source is not a readable regular file");
        packs::validate_archive_path(name);
        if (!names.insert(io::ascii_lower(name)).second) throw instances::Error("conflict", "Full report source names have a case conflict");
        if (selected.size() >= full_max_files) throw instances::Error("limit", "Full error report exceeds its file quota");
        auto pin = std::make_unique<packs::detail::NativeFile>(file, packs::detail::NativeFile::Mode::read);
        const auto size = pin->size();
        if (size > full_file_max_bytes || size > full_total_max_bytes - source_total)
            throw instances::Error("limit", "Full error report exceeds its file or total byte quota");
        source_total += size;
        possible_total += std::min<std::uint64_t>(full_file_max_bytes, 3 * size);
        selected.push_back({std::move(name), std::move(pin), size});
    };
    for (const auto name : {log_file_name, std::string_view("latest.log"), std::string_view("debug.log"),
                           std::string_view("nickel-diagnostic.json"), std::string_view("nickel-launch-failure.txt")})
        select("logs/" + std::string(name), paths.logs / instances::path_from_utf8(name));
    for (std::size_t index = 1; index <= full_max_archives; ++index) {
        const auto name = std::string(log_file_name) + '.' + std::to_string(index);
        select("logs/" + name, paths.logs / name);
    }
    instances::reject_links(paths.crash_reports);
    if (io::path_exists(paths.crash_reports)) {
        if (!fs::is_directory(io::native_path(paths.crash_reports))) throw instances::Error("unsafe_report", "Crash report source is not a plain directory");
        std::vector<fs::path> crashes;
        std::size_t inspected = 0;
        for (const auto& entry : io::directory_entries(paths.crash_reports, false, cancel)) {
            io::check_cancelled(cancel);
            if (++inspected > 100000) throw instances::Error("limit", "Crash report directory exceeds the bounded scan quota");
            if (io::ascii_lower(instances::path_to_utf8(entry.path().extension())) != ".txt") continue;
            if (crashes.size() >= full_max_files) throw instances::Error("limit", "Too many crash reports for a full error report");
            crashes.push_back(entry.path());
        }
        std::sort(crashes.begin(), crashes.end());
        for (const auto& crash : crashes) select("crash-reports/" + instances::path_to_utf8(crash.filename()), crash);
    } else notes.push_back("The game did not generate a standalone crash report; existing logs are included.");
    // Reject quota/capacity failures before creating any snapshot files; retain source handles through capture.
    require_report_space(resource, std::min<std::uint64_t>(full_total_max_bytes, possible_total + diagnosis_max_bytes + 64 * 1024));
    auto state = std::make_shared<ErrorReportSnapshot::State>();
    state->target = target;
    state->logs = paths.logs;
    state->workspace = std::make_unique<packs::detail::Workspace>(resource);
    state->directory_pin = std::make_unique<foundation_detail::DirectoryGuard>(state->workspace->directory());
    std::uint64_t sanitized_total = 0;
    for (auto& source : selected) {
        auto safe = sanitized_source(*source.file, source.size, store, cancel);
        if (safe.size() > full_total_max_bytes - sanitized_total) throw instances::Error("limit", "Redacted full error report exceeds its total quota");
        sanitized_total += safe.size();
        if (source.name == "logs/nickel-launcher.log" && input.stdout_tail.empty()) {
            input.stdout_tail = report_excerpt(safe, 128 * 1024);
            input.input_truncated = input.input_truncated || safe.size() > 128 * 1024;
        } else if (source.name == "logs/latest.log") {
            input.latest_log = report_excerpt(safe, 256 * 1024);
            input.input_truncated = input.input_truncated || safe.size() > 256 * 1024;
        } else if (source.name.starts_with("crash-reports/")) {
            input.crash_report = report_excerpt(safe, 512 * 1024);
            input.input_truncated = input.input_truncated || safe.size() > 512 * 1024;
        }
        const auto copied = state->workspace->directory() / std::to_string(state->sources.size());
        io::write_exclusive(copied, safe);
        auto pin = std::make_unique<packs::detail::NativeFile>(copied, packs::detail::NativeFile::Mode::read);
        const auto digest = packs::hash_file(copied, "sha512", cancel);
        pin->unchanged();
        files.push_back(source.name);
        state->sources.push_back({std::move(source.name), copied, pin->size(), digest});
        state->pins.push_back(std::move(pin));
    }
    for (const auto& source : selected) source.file->unchanged();
    if (files.empty()) notes.push_back("No game logs were generated; this report includes the failure and diagnostic context only.");
    state->diagnosis = analyze(input);
    const auto diagnosis = io::parse_strict_json(safe_text(report_to_json(state->diagnosis).dump(2), diagnosis_max_bytes), diagnosis_max_bytes);
    Json manifest{{"schemaVersion", 1}, {"product", "Palladium"}, {"generatedAt", generated_at()},
        {"complete", true}, {"files", std::move(files)}, {"notes", std::move(notes)}, {"diagnostic", diagnosis}};
    constexpr std::string_view notice = "Palladium complete error report\n\nSend this ZIP when requesting help; do not send only a crash screenshot or select one log.\n"
        "All selected launcher/game logs and crash reports are complete redacted source files, not tails.\n"
        "No account configuration, credentials, private keys, saved worlds or resource packs are collected.\n"
        "Review mod-written personal information before sharing.\n";
    state->metadata = {{"report.json", std::nullopt, safe_text(manifest.dump(2), diagnosis_max_bytes)},
                       {"READ-ME.txt", std::nullopt, std::string(notice)},
                       {"failure.txt", std::nullopt, safe_line(state->diagnosis.summary)}};
    for (const auto& metadata : state->metadata) {
        if (metadata.bytes.size() > full_total_max_bytes - sanitized_total) throw instances::Error("limit", "Full report metadata exceeds its total quota");
        sanitized_total += metadata.bytes.size();
    }
    state->directory_pin->verify();
    io::check_cancelled(cancel);
    return ErrorReportSnapshot(std::move(state));
}

fs::path export_error_report_zip(const fs::path& destination, const ErrorReportSnapshot& snapshot,
                                 packs::ArchiveBackend& backend, std::stop_token cancel) {
    io::check_cancelled(cancel);
    const auto& state = *snapshot.state_;
    instances::reject_links(destination);
    if (report_path_inside(destination, state.logs) || report_path_inside(destination, state.logs.parent_path() / "crash-reports")
        || report_path_inside(destination, state.workspace->directory()))
        throw instances::Error("unsafe_path", "Full reports must be saved outside log and private snapshot directories");
    const auto verify = [&] {
        state.directory_pin->verify();
        for (std::size_t index = 0; index < state.sources.size(); ++index) {
            io::check_cancelled(cancel);
            const auto& source = state.sources[index];
            state.pins[index]->unchanged();
            if (state.pins[index]->size() != source.size || packs::hash_file(source.file, "sha512", cancel) != source.sha512)
                throw instances::Error("integrity", "Immutable full report source identity changed");
        }
        state.directory_pin->verify();
    };
    verify();
    std::vector<packs::ArchiveInput> inputs;
    for (const auto& source : state.sources) inputs.push_back({source.name, source.file, {}});
    inputs.insert(inputs.end(), state.metadata.begin(), state.metadata.end());
    return write_report_zip(destination, std::move(inputs),
                            {full_max_files + 3, full_file_max_bytes, full_total_max_bytes}, backend, cancel, verify);
}
fs::path export_error_report_zip(const fs::path& destination, const SafeReport& snapshot,
                                 packs::ArchiveBackend& backend, std::stop_token cancel) {
    io::check_cancelled(cancel);
    auto inputs = report_inputs(snapshot);
    const auto count = inputs.size();
    return write_report_zip(destination, std::move(inputs), {count, report_max_bytes, report_max_bytes}, backend, cancel);
}
fs::path export_error_report_zip(const fs::path& destination, const LaunchLogStore& store,
                                 const DiagnosticInput& context, packs::ArchiveBackend& backend,
                                 std::stop_token cancel) {
    io::check_cancelled(cancel);
    const auto snapshot = safe_report(store, context);
    return export_error_report_zip(destination, snapshot, backend, cancel);
}
} // namespace nickel::diagnostics
