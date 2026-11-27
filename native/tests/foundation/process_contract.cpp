#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/path_guard.hpp"
#include "platform/process.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace nickel;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string failure(const auto& callback) {
    try { callback(); } catch (const ErrorException& error) { return error.error().code; }
    throw std::runtime_error("Restricted process operation unexpectedly succeeded");
}

struct Fixture {
    fs::path path = fs::current_path() / path_from_utf8("foundation-process-" + unique_token());
    Fixture() { fs::create_directories(path); }
    ~Fixture() { std::error_code error; fs::remove_all(path, error); }
};

void test_image_spelling(const fs::path& mock) {
#ifdef _WIN32
    const auto logical = foundation_detail::checked_absolute_path(mock);
    require(logical.native().size() < MAX_PATH, "Short image spelling fixture is not a short path");
    for (const auto purpose : {ProcessPurpose::Preparation, ProcessPurpose::Game}) {
        for (const auto& spelling : {logical, foundation_detail::native_filesystem_path(logical)}) {
            ProcessOptions options;
            options.executable = spelling;
            options.purpose = purpose;
            options.arguments = {"--module-image"};
            auto child = OwnedProcess::spawn(options);
            const auto result = child.wait(5s);
            require(result && result->exit_code == 0 && result->stdout_eof && result->stderr_eof,
                    "Actual short image-name probe did not complete with both EOFs");
            const auto image = Json::parse(result->stdout_text);
            require(!image.value("extended", true) && image.at("nativeUnits").get<std::size_t>() < MAX_PATH &&
                    fs::equivalent(path_from_utf8(image.at("image").get<std::string>()), logical),
                    "Short executable leaked an extended prefix into the actual child image name");
        }
    }
    std::cout << "PASS Windows image name: actual short child module is logical for plain/prefixed input and preparation/game purposes\n";
#else
    (void)mock;
#endif
}

void test_owned_stop(const fs::path& mock) {
    ProcessOptions options;
    options.executable = mock;
    options.arguments = {"--sleep", "3000"};
    auto preparation = OwnedProcess::spawn(options);
    require(failure([&] { preparation.stop_owned_game(); }) == "PermissionDenied", "Game stop accepted preparation child");
    require(failure([&] { preparation.detach(); }) == "PermissionDenied", "Running preparation child detached");
    preparation.terminate();
    const auto prepared = preparation.drain(5s);
    require(prepared && prepared->terminated && prepared->stdout_eof && prepared->stderr_eof, "Preparation stop did not drain");

    options.purpose = ProcessPurpose::Game;
    std::array<std::atomic<unsigned>, 2> eof{};
    options.output = [&](ProcessStream stream, std::string_view, bool ended) {
        if (ended) eof[stream == ProcessStream::Stdout ? 0 : 1].fetch_add(1);
    };
    auto game = OwnedProcess::spawn(options);
    require(failure([&] { game.terminate(); }) == "PermissionDenied", "Preparation terminate accepted game");
    Cancellation cancellation;
    cancellation.cancel();
    require(failure([&] { (void)game.wait(20ms, &cancellation); }) == "Cancelled", "Game cancellation did not notify caller");
    require(game.running(), "Game cancellation killed child");
    game.stop_owned_game();
    game.stop_owned_game();
    const auto stopped = game.drain(5s);
    require(stopped && stopped->terminated && stopped->stdout_eof && stopped->stderr_eof, "Explicit game stop lost drain/termination state");
    require(eof[0] == 1 && eof[1] == 1, "Explicit game stop did not deliver exactly two EOF callbacks");
    game.stop_owned_game();
    game.detach();
    require(failure([&] { game.stop_owned_game(); }) == "InvalidArgument", "Detached game could still be stopped");

    options.output = {};
    options.purpose = ProcessPurpose::Service;
    options.arguments = {"--sleep", "80"};
    auto service = OwnedProcess::spawn(options);
    require(failure([&] { service.stop_owned_game(); }) == "PermissionDenied", "Game stop accepted service");
    require(failure([&] { (void)service.wait(20ms, &cancellation); }) == "Cancelled", "Service cancellation did not notify caller");
    const auto survived = service.wait(5s);
    require(survived && survived->exit_code == 0 && !survived->terminated, "Service implicitly killed");
    std::cout << "PASS owned stop: preparation-only terminate, explicit game-only stop, cancellation survival, detached refusal, dual EOF\n";
}

void test_output_failure(const fs::path& mock) {
    ProcessOptions options;
    options.executable = mock;
    options.arguments = {"--echo", "synthetic"};
    std::array<std::atomic<unsigned>, 2> eof{};
    options.output = [&](ProcessStream stream, std::string_view, bool ended) {
        if (ended) eof[stream == ProcessStream::Stdout ? 0 : 1].fetch_add(1);
        throw std::runtime_error("Synthetic callback failure");
    };
    auto child = OwnedProcess::spawn(options);
    require(failure([&] { (void)child.wait(5s); }) == "IoError", "Failed callback reported process success");
    require(eof[0] == 1 && eof[1] == 1, "Callback failure stopped EOF drain");
    std::cout << "PASS output failure: callback error fails closed after both streams drain\n";
}

void test_detach_survival(const fs::path& mock, const fs::path& root) {
    ProcessOptions options;
    options.executable = mock;
    options.purpose = ProcessPurpose::Game;
    const auto marker = root / "explicit-detach-survived.txt";
    options.arguments = {"--game-marker", path_to_utf8(marker), "100"};
    auto game = OwnedProcess::spawn(options);
    game.detach();
    for (unsigned i = 0; i < 500 && !fs::exists(marker); ++i) std::this_thread::sleep_for(10ms);
    require(fs::exists(marker), "Explicit detach killed owned game");
    std::cout << "PASS explicit detach: game continues without process control ownership\n";
}

void test_shell(const fs::path& mock, const fs::path& root) {
    ProcessOptions options;
    options.executable = mock;
    options.arguments = {"--echo"};
    options.windows_shell_command = "echo synthetic";
    require(failure([&] { (void)OwnedProcess::spawn(options); }) == "InvalidArgument", "Shell command and argument vector were combined");
    options.arguments.clear();
    options.windows_shell_command = std::string("echo\0unsafe", 11);
    require(failure([&] { (void)OwnedProcess::spawn(options); }) == "InvalidArgument", "NUL shell command accepted");
    options.windows_shell_command = std::string("\xFF", 1);
    require(failure([&] { (void)OwnedProcess::spawn(options); }) == "InvalidArgument", "Invalid UTF8 shell command accepted");
    options.windows_shell_command = "";
    require(failure([&] { (void)OwnedProcess::spawn(options); }) == "InvalidArgument", "Empty shell command accepted");
    options.windows_shell_command = "echo synthetic";
#ifdef _WIN32
    require(failure([&] { (void)OwnedProcess::spawn(options); }) == "InvalidArgument", "Shell command accepted non-interpreter executable");
    const auto length = GetSystemDirectoryW(nullptr, 0);
    require(length != 0, "System interpreter directory unavailable");
    std::wstring directory(length, L'\0');
    const auto written = GetSystemDirectoryW(directory.data(), length);
    require(written != 0 && written < length, "System interpreter path overflow");
    directory.resize(written);
    options.executable = fs::path(directory) / L"cmd.exe";
    options.cwd = root;
    options.inherit_environment = false;
    options.environment["NICKEL_SYNTHETIC"] = "expanded-value";
    options.windows_shell_command = "echo \"space & quoted\"&echo caret^&literal&echo %NICKEL_SYNTHETIC%&echo stderr-final 1>&2";
    auto shell = OwnedProcess::spawn(options);
    const auto result = shell.wait(5s);
    if (result && result->exit_code != 0) {
        std::cerr << "SHELL exit=" << result->exit_code << " stdout=" << result->stdout_text << " stderr=" << result->stderr_text << '\n';
    }
    require(result && result->exit_code == 0 && result->stdout_eof && result->stderr_eof, "Shell process did not drain successfully");
    require(result->stdout_text.find("\"space & quoted\"") != std::string::npos &&
        result->stdout_text.find("caret&literal") != std::string::npos &&
        result->stdout_text.find("expanded-value") != std::string::npos &&
        result->stderr_text.find("stderr-final") != std::string::npos, "cmd shell quoting/expansion was CRT-escaped");
    const auto script = root / "quoted command space.cmd";
    // Quote the expanded echo too: %~1 removes quotes before cmd parses '&'.
    const std::string script_text = "@echo off\r\necho \"arg=[%~1]\"\r\nexit /b 0\r\n";
    atomic_write_text(script, script_text);
    options.windows_shell_command = "\"" + path_to_utf8(script) + "\" \"argument & space\"";
    auto quoted = OwnedProcess::spawn(options);
    const auto quoted_result = quoted.wait(5s);
    if (quoted_result && quoted_result->exit_code != 0) {
        std::cerr << "QUOTED SHELL exit=" << quoted_result->exit_code << " stdout=" << quoted_result->stdout_text << " stderr=" << quoted_result->stderr_text << '\n';
    }
    require(quoted_result && quoted_result->exit_code == 0 && quoted_result->stdout_eof && quoted_result->stderr_eof &&
        quoted_result->stdout_text == "\"arg=[argument & space]\"\r\n" && quoted_result->stderr_text.empty(),
        "cmd outer quotes broke quoted script path/argument");
    options.windows_shell_command = std::string(32767, 'a');
    require(failure([&] { (void)OwnedProcess::spawn(options); }) == "LimitExceeded", "Oversized shell command accepted");
    std::cout << "PASS Windows shell: trusted command body, /D /S /C, quoted scripts, &, ^, expansion, stderr, length and exclusivity\n";
#else
    (void)root;
    require(failure([&] { (void)OwnedProcess::spawn(options); }) == "Unsupported", "Windows shell command accepted on POSIX");
    std::cout << "PASS Windows shell: rejected on POSIX\n";
#endif
}

void test_exited_root_pipes(const fs::path& mock, const fs::path& root) {
#ifdef _WIN32
    const auto marker = root / "stopped-descendant.txt";
    ProcessOptions options;
    options.executable = mock;
    options.purpose = ProcessPurpose::Game;
    options.arguments = {"--spawn-pipe-holder", path_to_utf8(marker), "8000"};
    std::array<std::atomic<unsigned>, 2> eof{};
    std::atomic<bool> ready{false};
    options.output = [&](ProcessStream stream, std::string_view bytes, bool ended) {
        if (bytes.find("descendant-ready") != std::string_view::npos) ready = true;
        if (ended) eof[stream == ProcessStream::Stdout ? 0 : 1].fetch_add(1);
    };
    auto game = OwnedProcess::spawn(options);
    for (unsigned i = 0; i < 500 && (!ready || game.running()); ++i) std::this_thread::sleep_for(10ms);
    require(ready && !game.running(), "Actual root did not exit while descendant kept pipes open");
    require(!game.wait(30ms) && eof[0] == 0 && eof[1] == 0, "Exited root was falsely complete before held-pipe EOF");
    Cancellation cancellation;
    cancellation.cancel();
    require(failure([&] { (void)game.wait(10ms, &cancellation); }) == "Cancelled" && !fs::exists(marker), "Game cancellation implicitly stopped descendant");
    game.stop_owned_game();
    game.stop_owned_game();
    const auto result = game.wait(5s);
    require(result && result->exit_code == 29 && result->terminated && result->stdout_eof && result->stderr_eof,
        "Explicit stop did not bound owned descendants after root exit with real dual EOF");
    require(eof[0] == 1 && eof[1] == 1 && !fs::exists(marker), "Owned descendant survived explicit stop or EOF was fabricated");
    game.stop_owned_game();
    options.output = {};
    const auto survived_marker = root / "destructor-descendant.txt";
    options.arguments = {"--spawn-pipe-holder", path_to_utf8(survived_marker), "150"};
    { auto survives = OwnedProcess::spawn(options); }
    for (unsigned i = 0; i < 500 && !fs::exists(survived_marker); ++i) std::this_thread::sleep_for(10ms);
    require(fs::exists(survived_marker), "Game destructor implicitly killed owned descendant job");
    std::cout << "PASS exited game root: held descendant pipes timeout without fake EOF, explicit owned-job stop+dualEOF, idempotence, destructor survival\n";
#else
    (void)mock; (void)root;
#endif
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Mock child path is required");
        const auto mock = fs::absolute(path_from_utf8(argv[1]));
        Fixture root;
        test_image_spelling(mock);
        test_owned_stop(mock);
        test_output_failure(mock);
        test_detach_survival(mock, root.path);
        test_shell(mock, root.path);
        test_exited_root_pipes(mock, root.path);
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL " << error.error().code << ": " << error.what() << " details=" << error.error().details.dump() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
    }
    return 1;
}
