#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;

void ensure_capacity(const fs::path& target) {
    if (fs::space(target).available < 256ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error("helper owner-local disk reserve exhausted");
    }
}

void write_new(const fs::path& path, const std::string& text) {
    ensure_capacity(path.parent_path());
    if (fs::exists(path)) {
        throw std::runtime_error("helper evidence file already exists");
    }
    std::ofstream stream(path, std::ios::binary | std::ios::out);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
}

std::string read_small(const fs::path& path) {
    if (fs::file_size(path) > 4096) {
        throw std::runtime_error("helper control exceeds quota");
    }
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

int run(const std::vector<std::string>& args) {
    for (const auto& arg : args) {
        if (arg == "-version" || arg == "--version") {
            std::cerr << "openjdk version \"17.0.14\" 2025-01-21\n"
                      << "OpenJDK Runtime Environment (Synthetic acceptance helper)\n"
                      << "OpenJDK 64-Bit Server VM (Synthetic, mixed mode)\n"
                      << "    java.version = 17.0.14\n    os.arch = amd64\n" << std::flush;
            return 0;
        }
    }
    const char* value = std::getenv("NICKEL_SMOKE_CONTROL_DIRECTORY");
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error("helper requires explicit fixture control directory");
    }
    const std::string control_utf8(value);
    const fs::path control(std::u8string(control_utf8.begin(), control_utf8.end()));
    if (!control.is_absolute() || read_small(control / "helper-control.owner") != "production-smoke-helper-v1\n") {
        throw std::runtime_error("helper ownership marker is missing");
    }
    std::string mode = "exit-failure";
    if (fs::is_regular_file(control / "mode.txt")) {
        mode = read_small(control / "mode.txt");
        if (!mode.empty() && mode.back() == '\n') {
            mode.pop_back();
        }
    }
    std::string argv = "cwd=" + utf8(fs::current_path()) + "\n";
    for (std::size_t index = 0; index < args.size(); ++index) {
        // Length prefixes preserve spaces and Unicode without shell interpretation.
        argv += std::to_string(index) + ":" + std::to_string(args[index].size()) + ":" + args[index] + "\n";
    }
    write_new(control / "argv.txt", argv);
    write_new(control / "running.marker", "owned-java-helper\n");
    std::cout << "[Synthetic/INFO] running owned helper\n" << std::flush;
    std::cout << "accessToken=SMOKE_ACCESS_0123" << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    std::cout << "456789_PUBLIC_TEST\n" << std::flush;
    std::cerr << "[Synthetic/ERROR] java.lang.IllegalStateException: synthetic failure\n" << std::flush;
    if (mode == "hold-running" || mode == "stdout-first") {
        if (mode == "stdout-first") {
            std::fflush(stdout);
#ifdef _WIN32
            _close(_fileno(stdout));
#else
            close(STDOUT_FILENO);
#endif
            write_new(control / "stdout-closed.marker", "stdout-only-not-authoritative\n");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!fs::is_regular_file(control / "release.marker")) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::cerr << "helper control release timeout\n" << std::flush;
                return 124;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::cerr << "[Synthetic/ERROR] stderr-tail-after-stdout: SMOKE_REFRESH_0123456789_PUBLIC_TEST\n" << std::flush;
    write_new(control / "exit.marker", "helper reached exit; GameSession wait still authoritative\n");
    return mode == "exit-success" ? 0 : 23;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> args;
        for (int index = 0; index < argc; ++index) {
            const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1,
                                                nullptr, 0, nullptr, nullptr);
            if (count <= 0) {
                throw std::runtime_error("invalid UTF-16 helper argv");
            }
            std::string utf8_arg(static_cast<std::size_t>(count), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1,
                                    utf8_arg.data(), count, nullptr, nullptr) != count) {
                throw std::runtime_error("UTF-16 helper conversion failed");
            }
            utf8_arg.pop_back();
            args.push_back(std::move(utf8_arg));
        }
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "owned helper error: " << error.what() << '\n';
        return 125;
    }
}
#else
int main(int argc, char** argv) {
    try {
        std::vector<std::string> args;
        for (int index = 0; index < argc; ++index) {
            args.emplace_back(argv[index]);
        }
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "owned helper error: " << error.what() << '\n';
        return 125;
    }
}
#endif
