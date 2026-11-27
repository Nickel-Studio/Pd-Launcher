#include "diagnostics.hpp"
#include "core/instances/domain_io.hpp"

#include <algorithm>
#include <array>
#include <sstream>

namespace nickel::diagnostics {
namespace {
namespace io = instances::detail;
struct Rule {
    std::string_view category;
    std::string_view severity;
    std::vector<std::string_view> patterns;
    std::string_view summary;
    std::string_view advice;
};
const std::array<Rule, 12> rules{{
    {"outOfMemory", "critical", {"java.lang.outofmemoryerror", "outofmemoryerror", "out of memory error", "the system is out of physical ram or swap space", "could not reserve enough space for object heap", "there is insufficient memory for the java runtime environment to continue", "native memory allocation (malloc) failed", "paging file is too small", "unable to create new native thread", "gc overhead limit exceeded", "java heap space"}, "The game ran out of memory.", "Close memory-heavy applications, increase the per-instance memory limit within physical RAM, or reduce mods and high-resolution packs. Use 64-bit Java."},
    {"jvmArgumentError", "critical", {"unrecognized option:", "unrecognized vm option", "error: could not create the java virtual machine", "error occurred during initialization of vm", "improperly specified vm option", "invalid maximum heap size", "invalid initial heap size", "too small maximum heap", "missing or invalid option", "could not find or load main class net.minecraft"}, "JVM arguments or virtual-machine initialization failed.", "Remove unsupported JVM options and check -Xmx/-Xms values against physical memory."},
    {"javaVersionIncompatible", "critical", {"unsupported class file major version", "unsupported class file version", "unsupportedclassversionerror", "has been compiled by a more recent version of the java runtime", "java.lang.nosuchfieldexception: ucp", "because module java.base does not export", "classnotfoundexception: jdk.nashorn.api.scripting.nashornscriptenginefactory", "classnotfoundexception: java.lang.invoke.lambdametafactory", "the requested compatibility level java_11 could not be set", "no such method: sun.misc.unsafe.defineanonymousclass", "java.lang.noclassdeffounderror: jdk/nashorn"}, "Java is incompatible with the game or installed mods.", "Select Java matching this game and loader: usually Java 8 through 1.16.5, Java 16/17 for 1.17, Java 17 through 1.20.4, and Java 21 from 1.20.5."},
    {"forgeJavaIncompatible", "critical", {"nosuchmethoderror: sun.security.util.manifestentryverifier", "nosuchmethoderror: 'void sun.security.util.manifestentryverifier", "java.lang.classcastexception: java.base/jdk", "java.lang.classcastexception: class jdk."}, "Forge is incompatible with this Java runtime.", "Update Forge to a compatible build or use the Java version this Forge release requires. Prefer Java 8 for historical 1.12.2 Forge."},
    {"mixinFailure", "error", {"mixin prepare failed", "mixin apply failed", "mixinapplyerror", "mixintransformererror", "mixin.injection.throwables.", ".json] failed during", "org.spongepowered.asm.launch.mixintweaker"}, "Mixin preparation, injection or transformation failed.", "Identify the mod id in the evidence and disable recently added or mismatched mods. Install the correct MixinBootstrap when required."},
    {"optiFineForgeIncompatible", "error", {"spritecontents.<init>", "rendersystem.getbackenddescription", "bakedquad.<init>", "forgegui.renderselecteditemname", "font.ellipsize", "shaders mod detected. please remove it, optifine has built-in support for shaders"}, "OptiFine and Forge are incompatible.", "Use an OptiFine build matching Forge or remove OptiFine and test equivalent rendering mods."},
    {"missingModDependency", "error", {"missing or unsupported mandatory dependencies:", "missing dependency", "mod resolution failed", "requires any version of", "requires a version of"}, "A mod dependency is missing or has an incompatible version.", "Install the dependency named in the evidence and ensure its game and loader versions match."},
    {"modConflict", "error", {"incompatible mods found!", "mod resolution encountered an incompatible mod set", "some of your mods are incompatible"}, "Installed mods conflict.", "Disable the conflicting mods one at a time and update them to builds matching this game and loader."},
    {"modInitializationFailure", "error", {"failed to create mod instance", "caught exception from ", "loaderexceptionmodcrash: caught exception from ", "due to errors, provided by "}, "A mod or loader threw an initialization error.", "Disable the named mod and update it to a version compatible with this loader."},
    {"duplicateMod", "error", {"duplicatemodsfoundexception", "found a duplicate mod", "found duplicate mods", "modresolutionexception: duplicate", "multiple entries with same key:"}, "A mod is installed more than once.", "Keep one compatible version of each mod in the effective mods directory."},
    {"forgeInstallIncomplete", "error", {"cannot find launch target fmlclient", "invalid paths argument, contained no existing paths", "found multiple arguments for option fml.forgeversion"}, "Forge files or version metadata are incomplete.", "Complete or reinstall the exact same Forge build and verify its required libraries."},
    {"graphicsDriverIssue", "error", {"the driver does not appear to support opengl", "couldn't set pixel format", "pixel format not accelerated", "1282: invalid operation", "maybe try a lower resolution resourcepack?"}, "The graphics driver or OpenGL capabilities caused failure.", "Update or roll back the graphics driver, try the dedicated GPU, and disable shaders or lower-resolution packs for diagnosis."}
}};
struct Source {
    std::string name;
    std::vector<std::string> lines;
    bool truncated = false;
};
Source source(std::string_view name, std::string_view raw, std::size_t bytes, bool head) {
    Source result{std::string(name), {}, raw.size() > bytes};
    const auto redacted = redact_checked(utf8_lossy(raw));
    const auto& safe = redacted.text;
    std::string bounded = safe;
    if (safe.size() > bytes) {
        auto start = safe.size() - bytes;
        while (start < safe.size() && (static_cast<unsigned char>(safe[start]) & 0xc0) == 0x80) ++start;
        bounded = safe.substr(start);
        if (head) {
            const auto count = bytes / 3;
            bounded = bounded_safe_line(std::string_view(safe).substr(0, count), count) + "\n[nickel: source truncated]\n" + bounded_safe_line(bounded, bytes - count - 40);
        }
    }
    std::size_t start = 0;
    while (start < bounded.size()) {
        auto end = bounded.find_first_of("\r\n", start);
        if (end == std::string::npos) end = bounded.size();
        result.lines.push_back(bounded_safe_line(std::string_view(bounded).substr(start, end - start), stream_line_max_bytes));
        if (end < bounded.size() && bounded[end] == '\r' && end + 1 < bounded.size() && bounded[end + 1] == '\n') ++end;
        start = end + 1;
    }
    if (result.lines.size() > capture_max_lines) {
        result.truncated = true;
        if (head) {
            std::vector<std::string> output(result.lines.begin(), result.lines.begin() + capture_max_lines / 4);
            output.insert(output.end(), result.lines.end() - capture_max_lines * 3 / 4, result.lines.end());
            result.lines = std::move(output);
        } else result.lines.erase(result.lines.begin(), result.lines.end() - capture_max_lines);
    }
    return result;
}
std::vector<Source> sources(const DiagnosticInput& input) {
    std::vector<Source> result;
    if (input.crash_report && !input.crash_report->empty()) result.push_back(source("crashReport", *input.crash_report, 512 * 1024, true));
    if (input.latest_log && !input.latest_log->empty()) result.push_back(source("latestLog", *input.latest_log, 256 * 1024, false));
    if (!input.stderr_tail.empty()) result.push_back(source("stderr", input.stderr_tail, 128 * 1024, false));
    if (!input.stdout_tail.empty()) result.push_back(source("stdout", input.stdout_tail, 128 * 1024, false));
    return result;
}
std::string excerpt(std::string_view line, std::string_view needle) {
    if (line.size() <= 1280) return bounded_safe_line(line, 1280);
    const auto position = io::ascii_lower(line).find(needle);
    auto start = position == std::string::npos || position < 640 ? 0 : position - 640;
    while (start < line.size() && (static_cast<unsigned char>(line[start]) & 0xc0) == 0x80) ++start;
    return (start ? "..." : "") + bounded_safe_line(line.substr(start), 1280);
}
std::vector<DiagnosticEvidence> evidence(const std::vector<Source>& sources, const std::vector<std::string_view>& patterns, std::string_view category) {
    std::vector<DiagnosticEvidence> result;
    for (const auto& source : sources) {
        for (std::size_t i = 0; i < source.lines.size(); ++i) {
            const auto lower = io::ascii_lower(source.lines[i]);
            const auto pattern = std::find_if(patterns.begin(), patterns.end(), [&](const auto& item) { return lower.find(item) != std::string::npos; });
            if (pattern != patterns.end()) result.push_back({source.name, i + 1, excerpt(source.lines[i], *pattern), "Matched " + std::string(category)});
            if (result.size() >= 3) return result;
        }
    }
    return result;
}
Json evidence_json(const std::vector<DiagnosticEvidence>& evidence) {
    Json result = Json::array();
    for (const auto& item : evidence) result.push_back({{"source", item.source}, {"line", item.line}, {"excerpt", redact(item.excerpt)}, {"summary", item.summary}});
    return result;
}
Json finding_json(const DiagnosticFinding& finding) {
    return {{"category", finding.category}, {"severity", finding.severity}, {"summary", finding.summary}, {"advice", finding.advice}, {"evidence", evidence_json(finding.evidence)}};
}
void validate_safe_log(const StoredLog& log) {
    if (!redact_checked(log.text).safe) throw instances::Error("unsafe_report", "Incomplete or unsafe log prevents full report export");
}
} // namespace

DiagnosticReport analyze(const DiagnosticInput& input) {
    const auto bounded = sources(input);
    std::vector<DiagnosticFinding> findings;
    std::string combined;
    for (const auto& source : bounded) for (const auto& line : source.lines) combined += io::ascii_lower(line) + '\n';
    for (const auto& rule : rules) {
        auto matches = evidence(bounded, rule.patterns, rule.category);
        if (rule.category == "optiFineForgeIncompatible" && matches.empty()
            && (input.optifine || combined.find("optifine") != std::string::npos)
            && (io::ascii_lower(input.loader).find("forge") != std::string::npos || combined.find("forge") != std::string::npos)
            && (combined.find("nosuchmethoderror") != std::string::npos || combined.find("nosuchfielderror") != std::string::npos))
            matches = evidence(bounded, {"optifine", "forge", "nosuchmethoderror", "nosuchfielderror"}, rule.category);
        if (rule.category == "graphicsDriverIssue" && matches.empty() && combined.find("exception_access_violation") != std::string::npos
            && (combined.find("nvoglv") != std::string::npos || combined.find("atio") != std::string::npos || combined.find("# c  [ig") != std::string::npos))
            matches = evidence(bounded, {"exception_access_violation", "nvoglv", "atio", "# c  [ig"}, rule.category);
        if (!matches.empty()) findings.push_back({std::string(rule.category), std::string(rule.severity), std::string(rule.summary), std::string(rule.advice), std::move(matches)});
    }
    DiagnosticReport report;
    if (findings.empty()) {
        report.category = "unknown";
        report.severity = input.exit_code && *input.exit_code != 0 ? "error" : bounded.empty() ? "warning" : "info";
        report.summary = bounded.empty() ? "No game output or crash report is available." : "No known crash signature matched.";
        report.advice = "Verify Java and JVM arguments, inspect the game logs, and disable recently installed mods to isolate the failure.";
        report.fallback_reason = bounded.empty() ? "No stdout, stderr, latest.log or crash report was captured."
            : input.exit_code && *input.exit_code != 0 ? "Nonzero exit code without a known log signature." : "No known log signature.";
        for (const auto& source : bounded) {
            for (auto i = source.lines.size(); i > 0; --i) {
                if (!io::trim(source.lines[i - 1]).empty()) {
                    report.evidence.push_back({source.name, i, excerpt(source.lines[i - 1], ""), "Last captured output"});
                    break;
                }
            }
            if (report.evidence.size() >= 3) break;
        }
    } else {
        static_cast<DiagnosticFinding&>(report) = findings.front();
        const auto count = std::min<std::size_t>(findings.size(), 4);
        report.additional.assign(findings.begin() + 1, findings.begin() + count);
    }
    report.input_truncated = input.input_truncated || std::any_of(bounded.begin(), bounded.end(), [](const auto& source) { return source.truncated; });
    report.context = {{"exitCode", input.exit_code ? Json(*input.exit_code) : Json(nullptr)},
                      {"minecraftVersion", bounded_safe_line(redact(input.minecraft_version), 512)},
                      {"loader", bounded_safe_line(redact(input.loader), 512)},
                      {"optifine", input.optifine ? Json(bounded_safe_line(redact(*input.optifine), 512)) : Json(nullptr)},
                      {"javaMajor", input.java_major ? Json(*input.java_major) : Json(nullptr)},
                      {"javaVendor", input.java_vendor ? Json(bounded_safe_line(redact(*input.java_vendor), 512)) : Json(nullptr)}};
    return report;
}
Json report_to_json(const DiagnosticReport& report) {
    auto result = finding_json(report);
    result["additional"] = Json::array();
    for (const auto& finding : report.additional) result["additional"].push_back(finding_json(finding));
    result["fallbackReason"] = report.fallback_reason ? Json(*report.fallback_reason) : Json(nullptr);
    result["inputTruncated"] = report.input_truncated;
    result["context"] = report.context;
    return result;
}
DiagnosticInput collect_diagnostic_input(const LaunchLogStore& store, DiagnosticInput input) {
    if (auto launcher = store.tail(capture_max_lines, 128 * 1024); launcher && input.stdout_tail.empty()) {
        input.stdout_tail = launcher->text;
        input.input_truncated = input.input_truncated || launcher->truncated;
    }
    if (auto latest = store.latest()) { input.latest_log = latest->text; input.input_truncated = input.input_truncated || latest->truncated; }
    const auto crashes = store.crashes();
    if (!crashes.empty()) {
        input.crash_report = crashes.front().text;
        input.input_truncated = input.input_truncated || crashes.front().truncated;
    }
    return input;
}
SafeReport safe_report(const LaunchLogStore& store, const DiagnosticInput& context) {
    FinishedLogLease lease(store.paths().launcher);
    SafeReport result;
    const auto input = collect_diagnostic_input(store, context);
    for (const auto& raw : {input.stdout_tail, input.stderr_tail, input.latest_log.value_or(""), input.crash_report.value_or("")})
        if (!redact_checked(raw).safe) throw instances::Error("unsafe_report", "Incomplete sensitive field prevents diagnostic export");
    if (auto log = store.tail(capture_max_lines, capture_max_bytes)) { validate_safe_log(*log); result.logs.push_back(*log); }
    if (auto log = store.latest(256 * 1024)) { validate_safe_log(*log); result.logs.push_back(*log); }
    for (auto log : store.crashes()) { validate_safe_log(log); result.logs.push_back(std::move(log)); }
    result.diagnosis = report_to_json(analyze(input));
    return result;
}
std::string safe_report_text(const SafeReport& report) {
    auto text = std::string("Palladium safe diagnostic report\n") + report.diagnosis.dump(2) + '\n';
    for (const auto& log : report.logs) {
        validate_safe_log(log);
        text += "\n--- " + bounded_safe_line(redact(log.name), 512) + " ---\n";
        text += log.text;
        text += '\n';
    }
    const auto safe = redact_checked(text);
    if (!safe.safe) throw instances::Error("unsafe_report", "Report cannot be safely redacted");
    return safe.text;
}
void write_safe_report(const fs::path& destination, const SafeReport& report, bool replace) {
    instances::reject_links(destination);
    const auto text = safe_report_text(report);
    if (text.size() > 8 * 1024 * 1024) throw instances::Error("limit", "Diagnostic report exceeds size limit");
    io::atomic_write(destination, text, replace);
}
} // namespace nickel::diagnostics
