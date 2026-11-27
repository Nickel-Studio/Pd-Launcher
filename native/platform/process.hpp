#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nickel {

class Cancellation;
struct Error;

enum class ProcessPurpose { Preparation, Game, Service };
enum class ProcessStream { Stdout, Stderr };
enum class ProcessPriority { Low, Normal, High };
using ProcessOutput = std::function<void(ProcessStream, std::string_view, bool eof)>;

struct ProcessOptions {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    // Windows cwd must be <259 logical UTF-16 units; longer paths throw Unsupported.
    std::filesystem::path cwd;
    std::map<std::string, std::optional<std::string>> environment;
    bool inherit_environment = true;
    bool hide_console = true;
    ProcessPurpose purpose = ProcessPurpose::Preparation;
    std::size_t capture_limit = 1024 * 1024;
    ProcessOutput output;
    // Trusted local cmd.exe command body; mutually exclusive with arguments.
    // Wrapped with /D /S /C and outer quotes, without CRT-escaping shell syntax.
    // Requires the system cmd.exe on Windows; rejected on other platforms.
    std::optional<std::string> windows_shell_command;
    // Game-only, best effort. Failure warns and does not prevent game spawn.
    bool high_performance_gpu = false;
    // Selected Java, not the wrapper. Empty uses executable; ignored when disabled.
    std::filesystem::path gpu_preference_executable;
    // Borrowed error; serialized with output, synchronous at acquire or on the root reaper.
    // Capture owned state; callbacks must not block/re-enter this process session.
    // Detach/destruction suppress later callbacks, not restoration. Exceptions are ignored.
    std::function<void(const Error&)> gpu_preference_warning;
};

struct ProcessResult {
    int exit_code = 0;
    bool signalled = false;
    bool terminated = false;
    std::string stdout_text;
    std::string stderr_text;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    bool stdout_eof = false;
    bool stderr_eof = false;
};

// Raw chunks are borrowed during callbacks. Callbacks on the same process are
// serialized, not UI-thread-bound; byte capture is bounded, pipe drains are not.
class OwnedProcess {
public:
    static OwnedProcess spawn(const ProcessOptions& options);
    ~OwnedProcess();
    OwnedProcess(OwnedProcess&&) noexcept;
    OwnedProcess& operator=(OwnedProcess&&) noexcept;
    OwnedProcess(const OwnedProcess&) = delete;
    OwnedProcess& operator=(const OwnedProcess&) = delete;

    std::uint32_t pid() const noexcept;
    ProcessPurpose purpose() const noexcept;
    // Queries the retained root identity, not a freshly opened numeric PID.
    bool running() const;
    std::optional<ProcessResult> poll();
    // Timeout leaves the child owned/running. Cancellation terminates only
    // Preparation children. Game/Service children are never killed implicitly.
    std::optional<ProcessResult> wait(std::chrono::milliseconds timeout,
                                      const Cancellation* cancellation = nullptr);
    ProcessResult wait(const Cancellation* cancellation = nullptr);
    void terminate();
    // Explicit user game-stop action only; never a preparation cancel/destructor.
    void stop_owned_game();
    void set_priority(ProcessPriority priority);
    std::optional<ProcessResult> drain(std::chrono::milliseconds timeout);
    void detach();

private:
    struct Impl;
    explicit OwnedProcess(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

std::string quote_windows_argument(std::string_view argument);
std::filesystem::path current_executable();
std::map<std::string, std::string> current_environment();

} // namespace nickel
