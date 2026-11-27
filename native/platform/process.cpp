#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "platform/process.hpp"
#include "platform/gpu_preferences.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"
#include "core/common/error.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace nickel {
namespace {
using namespace foundation_detail;
#ifdef _WIN32
class Handle {
public:
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { reset(); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value = std::exchange(other.value, INVALID_HANDLE_VALUE); }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
};
std::wstring wide(std::string_view text) {
    if (!valid_utf8(text) || text.find('\0') != std::string_view::npos || text.size() > static_cast<std::size_t>(INT_MAX)) {
        fail("InvalidArgument", "Process text must be valid UTF-8 without NUL");
    }
    if (text.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!length) fail("InvalidArgument", "Process text cannot be converted to UTF-16");
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), length)) {
        fail("InvalidArgument", "Process text cannot be converted to UTF-16");
    }
    return output;
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!length) fail("InvalidArgument", "System text is not valid Unicode");
    std::string output(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), length, nullptr, nullptr);
    return output;
}
struct CaseLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
#else
class Handle {
public:
    int value = -1;
    explicit Handle(int h = -1) : value(h) {}
    ~Handle() { reset(); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value = std::exchange(other.value, -1); }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset() { if (value >= 0) ::close(value); value = -1; }
};
#endif

void validate_options(const ProcessOptions& options) {
    if (options.executable.empty()) fail("InvalidArgument", "Process executable must not be empty");
    if (options.capture_limit > 16 * 1024 * 1024) fail("LimitExceeded", "Process output capture limit is too large");
    if (options.windows_shell_command) {
        const auto& command = *options.windows_shell_command;
        if (!options.arguments.empty()) fail("InvalidArgument", "Windows shell command cannot be combined with argument vectors");
        if (command.empty() || !valid_utf8(command) || command.find('\0') != std::string::npos) {
            fail("InvalidArgument", "Windows shell command must be nonempty UTF-8 without NUL");
        }
#ifndef _WIN32
        fail("Unsupported", "Windows shell commands are only supported on Windows");
#endif
    }
    for (const auto& argument : options.arguments) {
        if (!valid_utf8(argument) || argument.find('\0') != std::string::npos) fail("InvalidArgument", "Process argument is not valid UTF-8");
    }
    for (const auto& [key, value] : options.environment) {
        if (key.empty() || key.find('=') != std::string::npos || key.find('\0') != std::string::npos ||
            !valid_utf8(key) || (value && (!valid_utf8(*value) || value->find('\0') != std::string::npos))) {
            fail("InvalidArgument", "Process environment entry is invalid");
        }
    }
    if (!options.cwd.empty()) {
        std::error_code error;
#ifdef _WIN32
        const auto cwd = checked_absolute_path(options.cwd);
        // CreateProcessW needs room for a separator; extended prefixes do not lift this limit.
        if (cwd.native().size() >= MAX_PATH - 1) fail("Unsupported", "Windows process working directory must be shorter than 259 UTF-16 units");
        if (!std::filesystem::is_directory(native_filesystem_path(cwd), error) || error)
#else
        if (!std::filesystem::is_directory(options.cwd, error) || error)
#endif
            fail("InvalidArgument", "Process working directory does not exist");
    }
}

#ifdef _WIN32
std::string windows_command_line(const ProcessOptions& options, const std::filesystem::path& executable) {
    auto preferred = executable;
    preferred.make_preferred();
    std::string command = quote_windows_argument(utf8(preferred.native()));
    if (options.windows_shell_command) {
        const auto length = GetSystemDirectoryW(nullptr, 0);
        if (!length) fail("IoError", "Cannot resolve the system command interpreter");
        std::vector<wchar_t> directory(length);
        const auto written = GetSystemDirectoryW(directory.data(), length);
        if (!written || written >= length) fail("IoError", "Cannot resolve the system command interpreter");
        const auto interpreter = std::filesystem::path(std::wstring(directory.data(), written)) / L"cmd.exe";
        std::error_code error;
        if (!std::filesystem::equivalent(native_filesystem_path(executable), native_filesystem_path(interpreter), error) || error) {
            fail("InvalidArgument", "Windows shell command requires the system cmd.exe executable");
        }
        command += " /D /S /C \"" + *options.windows_shell_command + "\"";
    } else {
        for (const auto& argument : options.arguments) command += " " + quote_windows_argument(argument);
    }
    return command;
}
#endif

} // namespace

struct OwnedProcess::Impl {
    std::uint32_t process_id = 0;
    ProcessPurpose process_purpose = ProcessPurpose::Preparation;
    std::size_t capture_limit = 0;
    ProcessOutput output;
    std::function<void(const Error&)> gpu_warning;
    std::optional<GpuPreferenceGuard> gpu_guard;
    std::mutex callback_mutex;
    std::mutex mutex;
    std::condition_variable changed;
    ProcessResult result;
    bool exited = false;
    bool reaped = false;
    bool stdout_done = false;
    bool stderr_done = false;
    bool reader_failed = false;
    bool callback_failed = false;
#ifdef _WIN32
    Handle process;
    FILETIME creation_time{};
    Handle job;
    Handle stdout_pipe;
    Handle stderr_pipe;
#else
    pid_t native_pid = -1;
    Handle stdout_pipe;
    Handle stderr_pipe;
    bool process_group = false;
#endif

    void emit(ProcessStream stream, std::string_view bytes, bool eof) {
        std::scoped_lock callback_lock(callback_mutex);
        if (output) {
            try { output(stream, bytes, eof); }
            catch (...) { std::scoped_lock lock(mutex); callback_failed = true; }
        }
    }
    void warn_gpu(const Error& error) noexcept {
        try {
            std::scoped_lock callback_lock(callback_mutex);
            if (gpu_warning) { try { gpu_warning(error); } catch (...) {} }
        } catch (...) {}
    }
    void prepare_gpu(const ProcessOptions& options, const std::filesystem::path& executable) noexcept {
        if (!options.high_performance_gpu) return;
        try {
            if (process_purpose != ProcessPurpose::Game) {
                warn_gpu(Error{"InvalidArgument", "Temporary GPU preferences require a Game-purpose process", Json::object()});
                return;
            }
            gpu_guard.emplace(GpuPreferenceGuard::acquire(options.gpu_preference_executable.empty() ?
                executable : options.gpu_preference_executable));
        } catch (const ErrorException& error) {
            warn_gpu(error.error());
        } catch (...) {
            try { warn_gpu(Error{"IoError", "Cannot acquire temporary GPU preference", Json::object()}); } catch (...) {}
        }
    }
    void restore_gpu() noexcept {
        if (!gpu_guard) return;
        try { (void)gpu_guard->restore(); }
        catch (const ErrorException& error) { warn_gpu(error.error()); }
        catch (...) { try { warn_gpu(Error{"IoError", "Cannot restore temporary GPU preference", Json::object()}); } catch (...) {} }
        gpu_guard.reset();
    }
    void stop_output() {
        std::scoped_lock lock(callback_mutex);
        output = {};
        gpu_warning = {};
    }
    void read_stream(ProcessStream stream) {
        const bool stdout_stream = stream == ProcessStream::Stdout;
        auto& pipe = stdout_stream ? stdout_pipe : stderr_pipe;
        std::array<char, 64 * 1024> bytes{};
        bool reached_eof = false;
        for (;;) {
#ifdef _WIN32
            DWORD count = 0;
            if (!ReadFile(pipe.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) reached_eof = true;
                else { std::scoped_lock lock(mutex); reader_failed = true; }
                break;
            }
#else
            const auto count = ::read(pipe.value, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { std::scoped_lock lock(mutex); reader_failed = true; break; }
#endif
            if (count == 0) { reached_eof = true; break; }
            const auto length = static_cast<std::size_t>(count);
            {
                std::scoped_lock lock(mutex);
                auto& text = stdout_stream ? result.stdout_text : result.stderr_text;
                auto& truncated = stdout_stream ? result.stdout_truncated : result.stderr_truncated;
                const auto keep = std::min(capture_limit - text.size(), length);
                text.append(bytes.data(), keep);
                if (keep < length) truncated = true;
            }
            emit(stream, {bytes.data(), length}, false);
        }
        pipe.reset();
        if (reached_eof) emit(stream, {}, true);
        {
            std::scoped_lock lock(mutex);
            if (stdout_stream) { stdout_done = true; result.stdout_eof = reached_eof; }
            else { stderr_done = true; result.stderr_eof = reached_eof; }
        }
        changed.notify_all();
    }
    void reap() {
#ifdef _WIN32
        // Only a signalled retained root handle licenses GPU restoration. A wait
        // error must not destroy the guard while a detached game may still run.
        while (WaitForSingleObject(process.value, INFINITE) != WAIT_OBJECT_0) {
            { std::scoped_lock lock(mutex); reader_failed = true; }
            changed.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        DWORD code = 1;
        const bool ok = GetExitCodeProcess(process.value, &code) != 0;
        restore_gpu();
        {
            std::scoped_lock lock(mutex);
            result.exit_code = ok ? static_cast<int>(code) : -1;
            if (!ok) reader_failed = true;
            exited = true;
            reaped = true;
        }
        changed.notify_all();
#else
        siginfo_t state{};
        int observed;
        do { observed = ::waitid(P_PID, static_cast<id_t>(native_pid), &state, WEXITED | WNOWAIT); }
        while (observed < 0 && errno == EINTR);
        std::unique_lock lock(mutex);
        result.signalled = observed == 0 && state.si_code != CLD_EXITED;
        result.exit_code = observed == 0 ? (result.signalled ? 128 + state.si_status : state.si_status) : -1;
        if (observed != 0) reader_failed = true;
        exited = true;
        changed.notify_all();
        // Reserve the root PID/group while descendants still own output pipes.
        // Explicit stop can then signal only this still-owned process group.
        changed.wait(lock, [&] { return stdout_done && stderr_done; });
        int status = 0;
        pid_t waited = -1;
        if (observed == 0) {
            do { waited = ::waitpid(native_pid, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
        }
        if (observed == 0 && waited != native_pid) reader_failed = true;
        reaped = true;
        changed.notify_all();
#endif
    }
    bool complete() const { return exited && reaped && stdout_done && stderr_done; }
    void terminate_owned(ProcessPurpose permitted_purpose) {
        if (process_purpose != permitted_purpose) fail("PermissionDenied", "Owned process purpose does not permit this stop operation");
        std::scoped_lock lock(mutex);
        if (result.terminated) return;
#ifdef _WIN32
        if (!process_id || process_id == GetCurrentProcessId()) fail("PermissionDenied", "Refusing to terminate the launcher process");
        const bool has_job = job.value != INVALID_HANDLE_VALUE && job.value != nullptr;
        if (has_job) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
                fail("IoError", "Cannot inspect the owned process job before stopping it");
            }
            if (!accounting.ActiveProcesses) return;
        } else if (exited || WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) {
            return;
        }
        const auto ok = has_job ? TerminateJobObject(job.value, 125) : TerminateProcess(process.value, 125);
        if (!ok) {
            throw ErrorException(Error{"IoError", "Cannot stop the owned process",
                Json{{"nativeError", GetLastError()}, {"pid", process_id}, {"ownedJob", has_job}}});
        }
#else
        if (native_pid <= 0 || native_pid == ::getpid()) fail("PermissionDenied", "Refusing to terminate the launcher process");
        // The reaper leaves the root unreaped until both real pipe EOFs, keeping
        // this PID/group reserved even if the root exits before its descendants.
        siginfo_t state{};
        if (::waitid(P_PID, static_cast<id_t>(native_pid), &state, WEXITED | WNOHANG | WNOWAIT) != 0) {
            if (errno == ECHILD) return;
            fail("IoError", "Cannot inspect the owned process before stopping it");
        }
        if (state.si_pid != 0 && !process_group) return;
        if (::kill(process_group ? -native_pid : native_pid, SIGKILL) != 0) {
            if (errno == ESRCH) return;
            fail("IoError", "Cannot stop the owned process");
        }
#endif
        result.terminated = true;
    }
    void terminate_preparation() { terminate_owned(ProcessPurpose::Preparation); }
    void stop_game() { terminate_owned(ProcessPurpose::Game); }
};

OwnedProcess::OwnedProcess(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

OwnedProcess OwnedProcess::spawn(const ProcessOptions& options) {
    validate_options(options);
    auto impl = std::make_shared<Impl>();
    impl->process_purpose = options.purpose;
    impl->capture_limit = options.capture_limit;
    impl->output = options.output;
    impl->gpu_warning = options.gpu_preference_warning;
    bool reaper_started = false;
    std::error_code error;
#ifdef _WIN32
    const auto executable = checked_absolute_path(options.executable);
    if (!std::filesystem::is_regular_file(native_filesystem_path(executable), error) || error) fail("NotFound", "Process executable does not exist");
    const auto cwd = options.cwd.empty() ? std::filesystem::path{} : checked_absolute_path(options.cwd);
#else
    const auto executable = std::filesystem::absolute(options.executable, error);
    if (error || !std::filesystem::is_regular_file(executable, error)) fail("NotFound", "Process executable does not exist");
    const auto cwd = options.cwd.empty() ? std::filesystem::path{} : std::filesystem::absolute(options.cwd, error);
    if (error) fail("InvalidArgument", "Process working directory cannot be resolved");
#endif
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle stdout_write, stderr_write;
    if (!CreatePipe(&impl->stdout_pipe.value, &stdout_write.value, &security, 0) ||
        !CreatePipe(&impl->stderr_pipe.value, &stderr_write.value, &security, 0) ||
        !SetHandleInformation(impl->stdout_pipe.value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(impl->stderr_pipe.value, HANDLE_FLAG_INHERIT, 0)) {
        fail("IoError", "Cannot create owned process output pipes");
    }
    Handle stdin_null(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (stdin_null.value == INVALID_HANDLE_VALUE) fail("IoError", "Cannot create owned process standard input");
    const auto command = windows_command_line(options, executable);
    auto command_wide = wide(command);
    if (command_wide.size() >= 32767) fail("LimitExceeded", "Process command line exceeds the Windows limit");
    command_wide.push_back(L'\0');
    std::map<std::wstring, std::wstring, CaseLess> environment;
    if (options.inherit_environment) for (const auto& [key, value] : current_environment()) environment[wide(key)] = wide(value);
    for (const auto& [key, value] : options.environment) {
        if (value) environment[wide(key)] = wide(*value);
        else environment.erase(wide(key));
    }
    std::vector<wchar_t> environment_block;
    for (const auto& [key, value] : environment) {
        const auto entry = key + L"=" + value;
        environment_block.insert(environment_block.end(), entry.begin(), entry.end());
        environment_block.push_back(L'\0');
    }
    environment_block.push_back(L'\0');
    if (environment.empty()) environment_block.push_back(L'\0');
    if (environment_block.size() > 1024 * 1024) fail("LimitExceeded", "Process environment exceeds its size limit");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_null.value;
    startup.StartupInfo.hStdOutput = stdout_write.value;
    startup.StartupInfo.hStdError = stderr_write.value;
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<std::uint8_t> attributes(attribute_size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) fail("IoError", "Cannot restrict process handle inheritance");
    struct AttributeCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST attributes;
        ~AttributeCleanup() { DeleteProcThreadAttributeList(attributes); }
    } cleanup_attributes{startup.lpAttributeList};
    std::array<HANDLE, 3> inherited{stdin_null.value, stdout_write.value, stderr_write.value};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    inherited.data(), sizeof(inherited), nullptr, nullptr)) {
        fail("IoError", "Cannot restrict process handle inheritance");
    }
    PROCESS_INFORMATION process{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
    if (options.hide_console) flags |= CREATE_NO_WINDOW;
    auto application = executable;
    application.make_preferred();
    // The image spelling is visible to the child; short Java images cannot bootstrap with a device prefix.
    if (application.native().size() >= MAX_PATH) application = native_filesystem_path(executable);
    impl->prepare_gpu(options, executable);
    if (!CreateProcessW(application.c_str(), command_wide.data(), nullptr, nullptr, TRUE, flags,
                         environment_block.data(), cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &process)) {
        io_fail("start the owned process", executable, static_cast<int>(GetLastError()));
    }
    impl->process = Handle(process.hProcess);
    impl->process_id = process.dwProcessId;
    Handle main_thread(process.hThread);
    FILETIME exit_time{}, kernel_time{}, user_time{};
    if (!GetProcessTimes(impl->process.value, &impl->creation_time, &exit_time, &kernel_time, &user_time)) {
        TerminateProcess(impl->process.value, 125);
        WaitForSingleObject(impl->process.value, INFINITE);
        fail("IoError", "Cannot retain the owned process birth identity");
    }
    if (options.purpose != ProcessPurpose::Service) {
        impl->job = Handle(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        // Games stay alive on destruction/detach; only explicit game-stop kills
        // their job. Preparation retains the historical kill-on-close policy.
        if (options.purpose == ProcessPurpose::Preparation) limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (impl->job.value == INVALID_HANDLE_VALUE || impl->job.value == nullptr ||
            !SetInformationJobObject(impl->job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(impl->job.value, impl->process.value)) {
            TerminateProcess(impl->process.value, 125);
            WaitForSingleObject(impl->process.value, INFINITE);
            fail("IoError", "Cannot establish owned process job");
        }
    }
    if (impl->gpu_guard) {
        // Install the root lifetime owner before the game can run. Monitor
        // creation failure cannot drop a preference belonging to a live game.
        std::thread root_reaper;
        try {
            root_reaper = std::thread([impl] { impl->reap(); });
            root_reaper.detach();
            reaper_started = true;
        } catch (...) {
            TerminateProcess(impl->process.value, 125);
            WaitForSingleObject(impl->process.value, INFINITE);
            if (root_reaper.joinable()) root_reaper.join();
            fail("IoError", "Cannot retain the owned GPU game root monitor");
        }
    }
    if (ResumeThread(main_thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(impl->process.value, 125);
        WaitForSingleObject(impl->process.value, INFINITE);
        fail("IoError", "Cannot resume the owned process");
    }
    stdout_write.reset(); stderr_write.reset(); stdin_null.reset();
#else
    int stdout_fds[2]{-1, -1}, stderr_fds[2]{-1, -1};
#if defined(__linux__)
    if (::pipe2(stdout_fds, O_CLOEXEC) != 0) fail("IoError", "Cannot create process output pipes");
#else
    if (::pipe(stdout_fds) != 0) fail("IoError", "Cannot create process output pipes");
    ::fcntl(stdout_fds[0], F_SETFD, FD_CLOEXEC); ::fcntl(stdout_fds[1], F_SETFD, FD_CLOEXEC);
#endif
    impl->stdout_pipe = Handle(stdout_fds[0]); Handle stdout_write(stdout_fds[1]);
#if defined(__linux__)
    if (::pipe2(stderr_fds, O_CLOEXEC) != 0) fail("IoError", "Cannot create process output pipes");
#else
    if (::pipe(stderr_fds) != 0) fail("IoError", "Cannot create process output pipes");
    ::fcntl(stderr_fds[0], F_SETFD, FD_CLOEXEC); ::fcntl(stderr_fds[1], F_SETFD, FD_CLOEXEC);
#endif
    impl->stderr_pipe = Handle(stderr_fds[0]); Handle stderr_write(stderr_fds[1]);
    Handle stdin_null(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (stdin_null.value < 0) fail("IoError", "Cannot create process standard input");
    auto environment = options.inherit_environment ? current_environment() : std::map<std::string, std::string>{};
    for (const auto& [key, value] : options.environment) { if (value) environment[key] = *value; else environment.erase(key); }
    std::vector<std::string> env_storage;
    for (const auto& [key, value] : environment) env_storage.push_back(key + "=" + value);
    std::vector<char*> env;
    for (auto& item : env_storage) env.push_back(item.data());
    env.push_back(nullptr);
    std::vector<std::string> argv_storage{path_to_utf8(executable)};
    argv_storage.insert(argv_storage.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argv;
    for (auto& item : argv_storage) argv.push_back(item.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions) != 0) fail("IoError", "Cannot initialize owned process actions");
    struct ActionsCleanup { posix_spawn_file_actions_t* value; ~ActionsCleanup() { posix_spawn_file_actions_destroy(value); } } cleanup_actions{&actions};
    if (posix_spawnattr_init(&attributes) != 0) fail("IoError", "Cannot initialize owned process attributes");
    struct AttributesCleanup { posix_spawnattr_t* value; ~AttributesCleanup() { posix_spawnattr_destroy(value); } } cleanup_attributes{&attributes};
    int status = posix_spawn_file_actions_adddup2(&actions, stdin_null.value, STDIN_FILENO);
    status |= posix_spawn_file_actions_adddup2(&actions, stdout_write.value, STDOUT_FILENO);
    status |= posix_spawn_file_actions_adddup2(&actions, stderr_write.value, STDERR_FILENO);
    status |= posix_spawn_file_actions_addclose(&actions, impl->stdout_pipe.value);
    status |= posix_spawn_file_actions_addclose(&actions, impl->stderr_pipe.value);
#if defined(__linux__) || defined(__APPLE__)
    if (!cwd.empty()) status |= posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
#else
    if (!cwd.empty()) fail("Unsupported", "Owned process cwd is unavailable on this system");
#endif
    sigset_t mask, defaults;
    sigemptyset(&mask); sigemptyset(&defaults); sigaddset(&defaults, SIGPIPE);
    status |= posix_spawnattr_setsigmask(&attributes, &mask);
    status |= posix_spawnattr_setsigdefault(&attributes, &defaults);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (options.purpose != ProcessPurpose::Service) { flags |= POSIX_SPAWN_SETPGROUP; status |= posix_spawnattr_setpgroup(&attributes, 0); impl->process_group = true; }
    status |= posix_spawnattr_setflags(&attributes, flags);
    if (status) fail("IoError", "Cannot prepare owned process actions");
    impl->prepare_gpu(options, executable);
    const auto spawn_error = posix_spawn(&impl->native_pid, executable.c_str(), &actions, &attributes, argv.data(), env.data());
    if (spawn_error != 0) fail("IoError", "Cannot start the owned process");
    impl->process_id = static_cast<std::uint32_t>(impl->native_pid);
    stdout_write.reset(); stderr_write.reset(); stdin_null.reset();
#endif
    try {
        std::thread([impl] { impl->read_stream(ProcessStream::Stdout); }).detach();
        std::thread([impl] { impl->read_stream(ProcessStream::Stderr); }).detach();
        if (!reaper_started) std::thread([impl] { impl->reap(); }).detach();
    } catch (...) {
        impl->stop_output();
        if (impl->process_purpose == ProcessPurpose::Preparation) { try { impl->terminate_preparation(); } catch (...) {} }
        fail("IoError", "Cannot create owned process output monitors");
    }
    return OwnedProcess(std::move(impl));
}

OwnedProcess::~OwnedProcess() {
    if (!impl_) return;
    impl_->stop_output();
    if (impl_->process_purpose == ProcessPurpose::Preparation) { try { impl_->terminate_preparation(); } catch (...) {} }
}
OwnedProcess::OwnedProcess(OwnedProcess&&) noexcept = default;
OwnedProcess& OwnedProcess::operator=(OwnedProcess&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            impl_->stop_output();
            if (impl_->process_purpose == ProcessPurpose::Preparation) { try { impl_->terminate_preparation(); } catch (...) {} }
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}
std::uint32_t OwnedProcess::pid() const noexcept { return impl_ ? impl_->process_id : 0; }
ProcessPurpose OwnedProcess::purpose() const noexcept { return impl_ ? impl_->process_purpose : ProcessPurpose::Preparation; }
bool OwnedProcess::running() const {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->exited) return false;
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    return WaitForSingleObject(impl_->process.value, 0) == WAIT_TIMEOUT &&
        GetProcessId(impl_->process.value) == impl_->process_id &&
        GetProcessTimes(impl_->process.value, &creation, &exit, &kernel, &user) &&
        creation.dwLowDateTime == impl_->creation_time.dwLowDateTime &&
        creation.dwHighDateTime == impl_->creation_time.dwHighDateTime;
#else
    siginfo_t state{};
    int observed;
    do { observed = ::waitid(P_PID, static_cast<id_t>(impl_->native_pid), &state, WEXITED | WNOHANG | WNOWAIT); }
    while (observed < 0 && errno == EINTR);
    return observed == 0 && state.si_pid == 0;
#endif
}
std::optional<ProcessResult> OwnedProcess::poll() { return wait(std::chrono::milliseconds(0)); }
std::optional<ProcessResult> OwnedProcess::wait(std::chrono::milliseconds timeout, const Cancellation* cancellation) {
    if (!impl_) fail("InvalidArgument", "Process ownership was detached");
    if (timeout.count() < 0) fail("InvalidArgument", "Process timeout must not be negative");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (cancelled(cancellation)) {
            if (impl_->process_purpose == ProcessPurpose::Preparation) impl_->terminate_preparation();
            check_cancel(cancellation);
        }
        std::unique_lock lock(impl_->mutex);
        if (impl_->complete()) {
            if (impl_->reader_failed) fail("IoError", "Owned process output could not be completely drained");
            if (impl_->callback_failed) fail("IoError", "Owned process output callback failed");
            return impl_->result;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return std::nullopt;
        impl_->changed.wait_for(lock, std::min(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), std::chrono::milliseconds(10)));
    }
}
ProcessResult OwnedProcess::wait(const Cancellation* cancellation) {
    for (;;) { if (auto result = wait(std::chrono::seconds(30), cancellation)) return *result; }
}
void OwnedProcess::terminate() { if (!impl_) fail("InvalidArgument", "Process ownership was detached"); impl_->terminate_preparation(); }
void OwnedProcess::stop_owned_game() { if (!impl_) fail("InvalidArgument", "Process ownership was detached"); impl_->stop_game(); }
void OwnedProcess::set_priority(ProcessPriority priority) {
    if (!impl_) fail("InvalidArgument", "Process ownership was detached");
    std::scoped_lock lock(impl_->mutex);
    if (impl_->exited) fail("NotFound", "Owned process already exited");
#ifdef _WIN32
    const auto value = priority == ProcessPriority::Low ? BELOW_NORMAL_PRIORITY_CLASS :
        priority == ProcessPriority::High ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
    if (!SetPriorityClass(impl_->process.value, value)) fail("PermissionDenied", "Cannot change owned process priority");
#else
    const auto value = priority == ProcessPriority::Low ? 5 : priority == ProcessPriority::High ? -5 : 0;
    if (::setpriority(PRIO_PROCESS, static_cast<id_t>(impl_->native_pid), value) != 0) fail("PermissionDenied", "Cannot change owned process priority");
#endif
}
std::optional<ProcessResult> OwnedProcess::drain(std::chrono::milliseconds timeout) { return wait(timeout); }
void OwnedProcess::detach() {
    if (!impl_) return;
    if (impl_->process_purpose == ProcessPurpose::Preparation && running()) fail("PermissionDenied", "A running preparation process cannot be detached");
    impl_->stop_output();
    impl_.reset();
}

std::string quote_windows_argument(std::string_view argument) {
    if (argument.find('\0') != std::string_view::npos || !valid_utf8(argument)) fail("InvalidArgument", "Process argument is invalid");
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string_view::npos) return std::string(argument);
    std::string output = "\"";
    std::size_t slashes = 0;
    for (const auto c : argument) {
        if (c == '\\') { ++slashes; continue; }
        if (c == '"') { output.append(slashes * 2 + 1, '\\'); output.push_back('"'); }
        else { output.append(slashes, '\\'); output.push_back(c); }
        slashes = 0;
    }
    output.append(slashes * 2, '\\');
    output.push_back('"');
    return output;
}

std::filesystem::path current_executable() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!size) fail("IoError", "Cannot locate the launcher executable");
        if (size < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), size));
        if (buffer.size() >= 32768) fail("LimitExceeded", "Launcher executable path exceeds its size limit");
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 512;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) { buffer.resize(size); if (_NSGetExecutablePath(buffer.data(), &size) != 0) fail("IoError", "Cannot locate the launcher executable"); }
    return std::filesystem::weakly_canonical(path_from_utf8(buffer.data()));
#else
    std::vector<char> buffer(512);
    for (;;) {
        const auto size = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (size < 0) fail("IoError", "Cannot locate the launcher executable");
        if (static_cast<std::size_t>(size) < buffer.size()) return path_from_utf8(std::string_view(buffer.data(), static_cast<std::size_t>(size)));
        if (buffer.size() >= 1024 * 1024) fail("LimitExceeded", "Launcher executable path exceeds its size limit");
        buffer.resize(buffer.size() * 2);
    }
#endif
}
std::map<std::string, std::string> current_environment() {
    std::map<std::string, std::string> result;
#ifdef _WIN32
    LPWCH raw = GetEnvironmentStringsW();
    if (!raw) fail("IoError", "Cannot read the process environment");
    struct Cleanup { LPWCH raw; ~Cleanup() { FreeEnvironmentStringsW(raw); } } cleanup{raw};
    for (const auto* current = raw; *current;) {
        const std::wstring_view entry(current);
        current += entry.size() + 1;
        const auto equal = entry.find(L'=');
        if (equal == 0 || equal == std::wstring_view::npos) continue;
        result[utf8(entry.substr(0, equal))] = utf8(entry.substr(equal + 1));
    }
#else
    for (char** item = environ; item && *item; ++item) {
        const std::string_view entry(*item);
        const auto equal = entry.find('=');
        if (equal == std::string_view::npos || !valid_utf8(entry)) continue;
        result[std::string(entry.substr(0, equal))] = entry.substr(equal + 1);
    }
#endif
    return result;
}

} // namespace nickel
