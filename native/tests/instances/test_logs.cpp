#include "core/diagnostics/diagnostics.hpp"
#include "core/instances/domain_io.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>

using namespace nickel::diagnostics;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejected(F&& fn, std::string_view expected) {
    try { fn(); } catch (const nickel::instances::Error& error) {
        require(error.code() == expected, error.what()); return;
    }
    throw std::runtime_error("Expected rejection");
}
struct Fixture {
    fs::path path = fs::current_path() / ("fixture-logs-" + nickel::instances::detail::random_id());
    Fixture() { fs::create_directory(path); }
    ~Fixture() { std::error_code ec; fs::remove_all(path, ec); }
};
void redaction() {
    const auto safe = redact(R"(Launching --accessToken syntheticabcdefgh1234 --password synthetic-password Authorization: Bearer synthetic-bearer /home/synthetic-user/game C:\Users\synthetic-user\game https://name:pass@example.test/invite/synthetic-invite)");
    require(safe.find("syntheticabcdefgh1234") == std::string::npos && safe.find("synthetic-password") == std::string::npos && safe.find("synthetic-user") == std::string::npos && safe.find("synthetic-invite") == std::string::npos, "Sensitive string leaked");
    const auto json = redact_checked(R"({"accessToken":["synthetic-secret",{"token":"nested-secret"}],"ok":true})");
    require(json.safe && json.text.find("synthetic-secret") == std::string::npos && json.text.find("nested-secret") == std::string::npos, "Collection secret leaked");
    const auto nested = redact_checked(R"({"message":"Authorization: Bearer synthetic-encoded-secret"})");
    require(nested.safe && nested.text.find("synthetic-encoded-secret") == std::string::npos, "Encoded message leaked");
    const auto escaped = redact_checked(R"({"\u0074oken":"synthetic-unicode-secret"})");
    require(escaped.safe && escaped.text.find("synthetic-unicode-secret") == std::string::npos, "Escaped key leaked");
    const auto incomplete = redact_checked(R"({"password":"unclosed synthetic-secret)");
    require(!incomplete.safe && incomplete.text == redaction_failure, "Malformed sensitive quote released");
    StreamRedactor stream;
    require(stream.push_line("safe first").find("safe first") != std::string::npos, "Ordinary log missing");
    require(stream.push_line("password: |").empty(), "YAML header emitted before boundary");
    require(stream.push_line("  synthetic-block-secret").empty(), "YAML continuation leaked");
    const auto yaml = stream.push_line("done: okay");
    require(yaml.find("synthetic-block-secret") == std::string::npos && yaml.find("done: okay") != std::string::npos, "YAML boundary lost");
    require(stream.finish().empty() && !stream.failed(), "Healthy stream rejected");
    StreamRedactor broken;
    broken.push_line("token: [");
    broken.push_line("synthetic-deep-secret");
    require(broken.finish().find(redaction_failure) != std::string::npos && broken.failed(), "Unclosed stream did not fail closed");
    StreamRedactor timed;
    timed.push_line("password: |");
    require(timed.expire(std::chrono::steady_clock::now() + std::chrono::seconds(31)).find(redaction_failure) != std::string::npos, "Pending timeout leaked");
    require(bounded_safe_line(std::string(100000, 'a')).size() <= display_line_max_bytes, "Line byte limit exceeded");
    require(utf8_lossy(std::string("a\xff")) == "a\xef\xbf\xbd", "Lossy UTF-8 invalid");
    OutputCapture capture;
    for (unsigned i = 0; i < 5000; ++i) capture.push_safe(std::string(2000, 'x'));
    require(capture.size() <= capture_max_lines && capture.bytes() <= capture_max_bytes, "Capture exceeded limits");
    std::cout << "redaction CLI/JSON/encoded/YAML/unclosed/timeout/UTF8/4000lines/4MiB/line bytes: PASS\n";
}
void known_secrets() {
    Fixture fixture;
    LaunchLogStore store(fixture.path);
    std::string emitted;
    OutputSession output(store, [&](const auto& event) { emitted += event.line; });
    output.add_secret("synthetic-exact-key");
    output.add_secret("synthetic-auth/+?");
    output.add_secret("short");
    output.bytes(ProcessStream::stdout_stream, "unlabelled synthetic-ex");
    require(emitted.empty(), "Partial authentication secret was emitted before a line boundary");
    rejected([&] { output.add_secret("late-secret"); }, "log_session_started");
    output.bytes(ProcessStream::stdout_stream, "act-key suffix\nencoded synthetic-auth%2F%2B%3F short\n");
    output.bytes(ProcessStream::stderr_stream, "password: [\n\"synthetic-exact-key\"");
    require(emitted.find("synthetic-exact-key") == std::string::npos, "Multiline known credential escaped the shared redactor");
    output.bytes(ProcessStream::stderr_stream, "\n]\nfinal synthetic-auth/+?");
    output.eof(ProcessStream::stdout_stream);
    rejected([&] { FinishedLogLease active(store.paths().launcher); }, "log_session_active");
    output.eof(ProcessStream::stderr_stream);
    require(output.finish(std::chrono::milliseconds(1)), "Known-secret output did not drain");
    const auto text = emitted + output.capture_text() + store.tail()->text;
    for (auto secret : {"synthetic-exact-key", "synthetic-auth/+?", "synthetic-auth%2F%2B%3F", "short"})
        require(text.find(secret) == std::string::npos, "Known authentication secret leaked in UI, capture or stored log");
    nickel::instances::detail::atomic_write(store.paths().latest, "game wrote synthetic-exact-key\n");
    require(store.latest()->text.find("synthetic-exact-key") == std::string::npos, "Finished store forgot its exact game-log secrets");
    require(store.redact_report("synthetic-auth%2f%2b%3f").text == "***", "Lower-case URL-encoded credential leaked");
    OutputSession no_eof(LaunchLogStore(fixture.path / "missing"));
    no_eof.producer_disappeared();
    require(!no_eof.finish(std::chrono::milliseconds(0)) && !no_eof.safe(), "A producer with no observed EOF was declared safe");
    StreamRedactor invalid;
    rejected([&] { invalid.add_secret("unsafe\nsecret"); }, "invalid_secret");
    rejected([&] { invalid.add_secret(std::string(4097, 'x')); }, "invalid_secret");
    std::cout << "known secrets/raw fragments/URL variants/late registration/dual EOF/no producer: PASS\n";
}
void output() {
    Fixture fixture;
    LaunchLogStore store(fixture.path, 1024, 3);
    std::vector<std::string> emitted;
    OutputSession session(store, [&](const auto& event) { emitted.push_back(event.line); });
    rejected([&] { FinishedLogLease lease(store.paths().launcher); }, "log_session_active");
    session.bytes(ProcessStream::stdout_stream, "ordinary\npassword: |");
    session.bytes(ProcessStream::stdout_stream, "\n  synthetic-output-secret\ndone: okay\nEOF last without newline");
    session.bytes(ProcessStream::stderr_stream, "error details\n");
    session.eof(ProcessStream::stdout_stream);
    session.eof(ProcessStream::stderr_stream);
    require(session.finish(std::chrono::milliseconds(1)), "Ordered EOF not drained");
    require(session.capture_text().find("synthetic-output-secret") == std::string::npos && session.capture_text().find("EOF last without newline") != std::string::npos, "Output redaction/last line failed");
    require(std::none_of(emitted.begin(), emitted.end(), [](const auto& line) { return line.find("synthetic-output-secret") != std::string::npos; }), "Frontend emitted secret");
    {
        FinishedLogLease report(store.paths().launcher);
        rejected([&] { OutputSession conflict(store); }, "log_session_active");
    }
    OutputSession missing(store);
    missing.line(ProcessStream::stdout_stream, "password: [");
    missing.line(ProcessStream::stdout_stream, "synthetic-missing-eof");
    missing.producer_disappeared();
    require(!missing.finish(std::chrono::milliseconds(1)) && !missing.safe(), "Lost EOF not failed");
    rejected([&] { safe_report(store, DiagnosticInput{}); }, "unsafe_report");
    OutputSession installer(store);
    bool joined = false;
    require(drain_owned_installer(installer, [&] {
        joined = true;
        installer.line(ProcessStream::stdout_stream, "installer final line");
        installer.eof(ProcessStream::stdout_stream);
        installer.eof(ProcessStream::stderr_stream);
    }, std::chrono::milliseconds(1)) && joined, "Owned installer callback not drained");
    for (unsigned i = 0; i < 10; ++i) store.append(std::string(700, 'a'));
    require(fs::file_size(store.paths().launcher) <= 1024 && fs::is_regular_file(store.paths().logs / "nickel-launcher.log.1"), "Rotating log limits incorrect");
    std::cout << "session lease/ordered EOF/lost EOF/redacted storage and emission/owned installer drain/rotation: PASS\n";
}
void diagnostic() {
    Fixture fixture;
    LaunchLogStore store(fixture.path);
    store.start_session();
    store.append("java.lang.OutOfMemoryError: Java heap space --accessToken synthetic-report-secret1234");
    fs::create_directories(store.paths().crash_reports);
    nickel::instances::detail::atomic_write(store.paths().latest, "Mixin apply failed password=synthetic-report-password\n");
    nickel::instances::detail::atomic_write(store.paths().crash_reports / "crash-synthetic-client.txt", "UnsupportedClassVersionError\n/home/synthetic-user/crash\n");
    DiagnosticInput context;
    context.exit_code = 1;
    context.minecraft_version = "1.20.1";
    context.loader = "forge";
    context.java_major = 17;
    auto report = safe_report(store, context);
    require(report.diagnosis["category"] == "outOfMemory" && report.diagnosis["additional"].size() <= 3, "Diagnostic priority incorrect");
    const auto text = safe_report_text(report);
    require(text.starts_with("Palladium safe diagnostic report\n"), "New safe report lost the Palladium display name");
    require(text.find("synthetic-report-secret1234") == std::string::npos && text.find("synthetic-report-password") == std::string::npos && text.find("synthetic-user") == std::string::npos, "Safe report leaked secret/path");
    write_safe_report(fixture.path / "diagnostic.txt", report);
    rejected([&] { write_safe_report(fixture.path / "diagnostic.txt", report); }, "conflict");
    DiagnosticInput unknown;
    unknown.exit_code = 2;
    auto fallback = analyze(unknown);
    require(fallback.category == "unknown" && fallback.fallback_reason && fallback.severity == "error", "Unknown fallback not explained");
    std::cout << "diagnostic priority/evidence/additional/source bounds/unknown fallback/safe report/no overwrite: PASS\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        const std::string group = argc > 1 ? argv[1] : "all";
        if (group == "redaction" || group == "all") redaction();
        if (group == "secrets" || group == "all") known_secrets();
        if (group == "output" || group == "all") output();
        if (group == "diagnostic" || group == "all") diagnostic();
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}