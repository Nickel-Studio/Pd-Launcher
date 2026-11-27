#pragma once

#include "core/instances/instances.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace nickel::diagnostics {
using Json = nickel::Json;
namespace fs = std::filesystem;
inline constexpr std::string_view log_file_name = "nickel-launcher.log";
inline constexpr std::size_t capture_max_lines = 4000;
inline constexpr std::size_t capture_max_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t stream_line_max_bytes = 128 * 1024;
inline constexpr std::size_t display_line_max_bytes = 8 * 1024;
inline constexpr std::string_view redaction_failure = "[nickel: unsafe log redaction rejected]";

struct RedactionResult {
    std::string text;
    bool safe = true;
    std::optional<std::size_t> pending_start;
};
RedactionResult redact_checked(std::string_view text, bool final_input = true,
                                std::span<const std::string> secrets = {});
// Full error reports retain one bounded 64 MiB document until sensitive field boundaries are known.
RedactionResult redact_report_checked(std::string_view text, std::span<const std::string> secrets = {});
std::string redact(std::string_view text);
std::string bounded_safe_line(std::string_view text, std::size_t max_bytes = display_line_max_bytes);
std::string utf8_lossy(std::string_view bytes);

class StreamRedactor {
public:
    void add_secret(std::string_view secret);
    std::string push_line(std::string_view line, bool discarded = false);
    std::string finish(bool failed = false);
    std::string expire(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    bool failed() const noexcept;
private:
    std::string reject();
    std::string pending_;
    std::vector<std::string> secrets_;
    std::optional<std::chrono::steady_clock::time_point> since_;
    std::size_t pending_lines_ = 0;
    bool started_ = false;
    bool failed_ = false;
    bool closed_ = false;
};
class OutputCapture {
public:
    void push_safe(std::string_view line);
    std::string text() const;
    std::size_t size() const noexcept;
    std::size_t bytes() const noexcept;
private:
    std::deque<std::string> lines_;
    std::size_t bytes_ = 0;
};
struct LogPaths {
    fs::path root;
    fs::path logs;
    fs::path launcher;
    fs::path latest;
    fs::path crash_reports;
};
LogPaths resolve_log_paths(const fs::path& root);
struct StoredLog {
    std::string name;
    std::string text;
    bool truncated = false;
};
struct LogSecretState;
class LaunchLogStore {
public:
    explicit LaunchLogStore(fs::path root, std::size_t max_bytes = capture_max_bytes,
                            std::size_t max_archives = 3);
    void start_session();
    void append(std::string_view raw);
    void append_safe(std::string_view safe);
    RedactionResult redact_report(std::string_view raw) const;
    std::optional<StoredLog> tail(std::size_t lines = 2000, std::size_t bytes = 256 * 1024) const;
    std::optional<StoredLog> latest(std::size_t bytes = 256 * 1024) const;
    std::vector<StoredLog> crashes(std::size_t count = 3, std::size_t bytes = 512 * 1024) const;
    const LogPaths& paths() const noexcept;
private:
    void add_secret(std::string_view secret);
    std::vector<std::string> secrets() const;
    void rotate() const;
    LogPaths paths_;
    std::size_t max_bytes_;
    std::size_t archives_;
    std::shared_ptr<LogSecretState> secrets_;
    friend class OutputSession;
};
enum class ProcessStream { stdout_stream, stderr_stream };
struct SafeOutputEvent {
    ProcessStream stream;
    std::string line;
};
using SafeOutputSink = std::function<void(const SafeOutputEvent&)>;

// Only this session receives its child pipes. A stream must report ordered EOF.
class OutputSession {
public:
    OutputSession(LaunchLogStore store, SafeOutputSink sink = {}, bool start_new_log = true);
    ~OutputSession();
    OutputSession(const OutputSession&) = delete;
    OutputSession& operator=(const OutputSession&) = delete;
    // Register launch-plan secrets before any bytes, lines or EOF arrive.
    void add_secret(std::string_view secret);
    void bytes(ProcessStream stream, std::string_view bytes);
    void line(ProcessStream stream, std::string_view line, bool discarded = false);
    void eof(ProcessStream stream, bool failed = false);
    void producer_disappeared();
    bool finish(std::chrono::milliseconds timeout = std::chrono::seconds(5));
    bool finished() const;
    bool safe() const;
    std::string capture_text() const;
    // Copies retain this launch's private secret state; constructing a new store from its path does not.
    LaunchLogStore log_store() const;
private:
    void publish_locked(ProcessStream stream, std::string_view text);
    void close_locked(std::size_t stream, bool failed);
    LaunchLogStore store_;
    SafeOutputSink sink_;
    std::string lease_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::array<StreamRedactor, 2> redactors_;
    std::array<std::string, 2> line_buffers_;
    std::array<bool, 2> discarded_{};
    std::array<bool, 2> observed_{};
    std::array<bool, 2> closed_{};
    OutputCapture capture_;
    bool complete_ = false;
};

// Process owner supplies a callback that joins/drains only the child it owns.
bool drain_owned_installer(OutputSession& session, const std::function<void()>& join_owned_child,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5));
class FinishedLogLease {
public:
    explicit FinishedLogLease(const fs::path& log_path);
    ~FinishedLogLease();
    FinishedLogLease(const FinishedLogLease&) = delete;
    FinishedLogLease& operator=(const FinishedLogLease&) = delete;
private:
    std::string identity_;
};
} // namespace nickel::diagnostics
