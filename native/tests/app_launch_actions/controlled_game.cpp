#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

void write(FILE* stream, std::string_view text) {
    (void)std::fwrite(text.data(), 1, text.size(), stream);
    (void)std::fflush(stream);
}

bool requested(int argc, char** argv, std::string_view option) {
    for (int index = 1; index < argc; ++index)
        if (std::string_view(argv[index]) == option) return true;
    return false;
}

std::string value(int argc, char** argv, std::string_view option, std::string fallback) {
    for (int index = 1; index + 1 < argc; ++index)
        if (std::string_view(argv[index]) == option) return argv[index + 1];
    return fallback;
}

std::filesystem::path utf8_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool classpath_contains(std::string_view classpath, std::string_view expected) {
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    while (!classpath.empty()) {
        const auto end = classpath.find(separator);
        const auto entry = classpath.substr(0, end);
        if (utf8_path(entry) == utf8_path(expected)) return true;
        if (end == std::string_view::npos) break;
        classpath.remove_prefix(end + 1);
    }
    return false;
}

bool hold_descendant_pipes(std::chrono::milliseconds delay) {
#ifdef _WIN32
    std::wstring executable(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!size || size == executable.size()) return false;
    executable.resize(size);
    auto command = L"\"" + executable + L"\" --fixture-mode pipe-holder --fixture-delay " + std::to_wstring(delay.count());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!SetHandleInformation(startup.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)
        || !SetHandleInformation(startup.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) return false;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return false;
    write(stdout, "descendant-pid=" + std::to_string(process.dwProcessId) + '\n');
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    const auto child = fork();
    if (child < 0) return false;
    if (child == 0) {
        std::this_thread::sleep_for(delay);
        write(stdout, "descendant-stdout-tail");
        write(stderr, "descendant-stderr-tail");
        _exit(0);
    }
    write(stdout, "descendant-pid=" + std::to_string(child) + '\n');
#endif
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (requested(argc, argv, "-version")) {
        write(stderr, "openjdk version \"21.0.5\"\n"
                      "OpenJDK Runtime Environment ControlledFixture\n"
                      "OpenJDK 64-Bit Server VM\n"
                      "    java.version = 21.0.5\n"
                      "    java.vendor = ControlledFixture\n"
                      "    os.arch = amd64\n");
        return 0;
    }
    const auto mode = value(argc, argv, "--fixture-mode", "dual-eof");
    const auto delay = std::chrono::milliseconds(std::stoul(value(argc, argv, "--fixture-delay", "40")));
    if (mode == "preparation") {
        const auto record = value(argc, argv, "--fixture-ready", "");
        if (record.empty()) return 93;
#ifdef _WIN32
        const auto pid = GetCurrentProcessId();
#else
        const auto pid = getpid();
#endif
        std::ofstream ready(utf8_path(record), std::ios::binary | std::ios::trunc);
        ready << pid << '\n';
        ready.close();
        if (!ready) return 94;
        std::this_thread::sleep_for(delay);
        return 0;
    }
    const auto expected_client = value(argc, argv, "--fixture-expect-client", "");
    const auto expected_directory = value(argc, argv, "--fixture-expect-directory", "");
    const auto expected_version = value(argc, argv, "--fixture-expect-version", "");
    if ((!expected_client.empty() && !classpath_contains(value(argc, argv, "-cp", ""), expected_client))
        || (!expected_directory.empty() && utf8_path(value(argc, argv, "--gameDir", ""))
            != utf8_path(expected_directory))
        || (!expected_version.empty() && value(argc, argv, "--version", "") != expected_version)) {
        write(stderr, "fixture-launch-plan-mismatch\n");
        return 91;
    }
    const auto record = value(argc, argv, "--fixture-record", "");
    if (!record.empty()) std::ofstream(utf8_path(record), std::ios::binary | std::ios::app) << "game-start\n";
    if (mode == "pipe-holder") {
        std::this_thread::sleep_for(delay);
        write(stdout, "descendant-stdout-tail");
        write(stderr, "descendant-stderr-tail");
        return 0;
    }
    if (mode == "descendant-pipes") return hold_descendant_pipes(delay) ? 17 : 92;
    if (mode == "hold") {
        write(stdout, "owned-game-running\n");
        std::this_thread::sleep_for(delay);
        write(stderr, "owned-game-tail");
        return 0;
    }
    if (mode == "registered-secret") {
        write(stdout, "literal-prefix regist");
        std::this_thread::sleep_for(delay);
        write(stdout, "ered-fixture-secret literal-suffix\n");
        write(stderr, "fixture-line-one\nfixture-line-two\n");
        return 0;
    }
    if (mode == "unsafe") {
        write(stdout, "password=[\nfixture-private-value\n");
        write(stderr, "independent-stderr-visible\n");
        return 7;
    }
    if (mode == "oversized") {
        write(stdout, "password=[\n");
        write(stdout, std::string(128 * 1024 + 64, 'x'));
        write(stdout, "\nfixture-private-value\n]\n");
        write(stderr, "independent-stderr-visible\n");
        return 9;
    }
    if (mode == "burst") {
        for (int index = 0; index != 6000; ++index)
            write(stdout, "bounded-output-" + std::to_string(index) + '\n');
        write(stderr, "burst-stderr-final");
        return 0;
    }
    if (mode == "crash") {
        std::filesystem::create_directories("logs");
        std::ofstream latest("logs/latest.log", std::ios::binary);
        latest << "crash-source-begin\njava.lang.OutOfMemoryError: Java heap space\npassword=fixture-private-value\n"
               << "literal-prefix registered-fixture-secret literal-suffix\n";
        for (int index = 0; index != 20000; ++index) latest << "complete-source-evidence-" << index << '\n';
        latest << "crash-source-end\n";
        latest.close();
        write(stderr, "java.lang.OutOfMemoryError: Java heap space\n");
        return 17;
    }
    write(stdout, "stdout-visible\npassword=[\n");
    std::this_thread::sleep_for(delay);
    write(stderr, "stderr-visible\n");
    write(stdout, "fixture-private-value\n]\nstdout-final-without-newline");
    std::this_thread::sleep_for(delay);
    write(stderr, "stderr-final-without-newline");
    return 17;
}
