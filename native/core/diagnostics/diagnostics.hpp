#pragma once

#include "logs.hpp"

namespace nickel::packs { class ArchiveBackend; }

namespace nickel::diagnostics {
struct DiagnosticInput {
    std::optional<int> exit_code;
    std::string minecraft_version;
    std::string loader;
    std::optional<std::string> optifine;
    std::optional<std::uint32_t> java_major;
    std::optional<std::string> java_vendor;
    std::string stdout_tail;
    std::string stderr_tail;
    std::optional<std::string> latest_log;
    std::optional<std::string> crash_report;
    bool input_truncated = false;
};
struct DiagnosticEvidence {
    std::string source;
    std::size_t line = 0;
    std::string excerpt;
    std::string summary;
};
struct DiagnosticFinding {
    std::string category = "unknown";
    std::string severity = "info";
    std::string summary;
    std::string advice;
    std::vector<DiagnosticEvidence> evidence;
};
struct DiagnosticReport : DiagnosticFinding {
    std::vector<DiagnosticFinding> additional;
    std::optional<std::string> fallback_reason;
    bool input_truncated = false;
    Json context = Json::object();
};
DiagnosticReport analyze(const DiagnosticInput& input);
Json report_to_json(const DiagnosticReport& report);
DiagnosticInput collect_diagnostic_input(const LaunchLogStore& store, DiagnosticInput context);
struct SafeReport {
    Json diagnosis;
    std::vector<StoredLog> logs;
};
SafeReport safe_report(const LaunchLogStore& store, const DiagnosticInput& context);
std::string safe_report_text(const SafeReport& report);
void write_safe_report(const fs::path& destination, const SafeReport& report, bool replace = false);

// App owns this selector and the immutable failed-launch snapshot; Minecraft events do not carry it.
struct ReportTarget {
    fs::path root;
    std::string version_id;
    std::string launch_id;
};
Json report_target_to_json(const ReportTarget& target);
ReportTarget report_target_from_json(const Json& value);
Json diagnostic_brief_to_json(const DiagnosticReport& report);
Json launch_error_to_json(const ReportTarget& target, const DiagnosticReport& report);

// The opaque snapshot pins redacted source files, not live logs or account data.
class ErrorReportSnapshot {
public:
    ErrorReportSnapshot(const ErrorReportSnapshot&) noexcept = default;
    ErrorReportSnapshot& operator=(const ErrorReportSnapshot&) noexcept = default;
    const ReportTarget& target() const noexcept;
    const fs::path& logs_directory() const noexcept;
    const DiagnosticReport& diagnosis() const noexcept;
private:
    struct State;
    explicit ErrorReportSnapshot(std::shared_ptr<const State> state);
    std::shared_ptr<const State> state_;
    friend ErrorReportSnapshot capture_error_report(const ReportTarget&, const LaunchLogStore&,
                                                    const DiagnosticInput&, std::stop_token);
    friend fs::path export_error_report_zip(const fs::path&, const ErrorReportSnapshot&,
                                            packs::ArchiveBackend&, std::stop_token);
};
ErrorReportSnapshot capture_error_report(const ReportTarget& target, const LaunchLogStore& store,
                                         const DiagnosticInput& context, std::stop_token cancel = {});

// Return the exact native-dialog destination only after a verified ZIP is published without overwrite.
fs::path export_error_report_zip(const fs::path& destination, const ErrorReportSnapshot& snapshot,
                                 packs::ArchiveBackend& archive, std::stop_token cancel = {});
// Bounded excerpts remain available for diagnostics; they are not full-source report snapshots.
fs::path export_error_report_zip(const fs::path& destination, const SafeReport& snapshot,
                                 packs::ArchiveBackend& archive, std::stop_token cancel = {});
fs::path export_error_report_zip(const fs::path& destination, const LaunchLogStore& store,
                                 const DiagnosticInput& context, packs::ArchiveBackend& archive,
                                 std::stop_token cancel = {});
} // namespace nickel::diagnostics
