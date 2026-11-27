#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "filesystem.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

namespace {
DWORD spawn_pipe_holder(unsigned long duration) {
    BOOL owned = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &owned) || !owned) return 0;
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return 0;
    executable.resize(length);
    auto command = L"\"" + executable + L"\" --hold-inherited-pipes " + std::to_wstring(duration);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (!size) return 0;
    std::vector<std::uintptr_t> storage((size + sizeof(std::uintptr_t) - 1) / sizeof(std::uintptr_t));
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) return 0;
    struct Attributes {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~Attributes() { DeleteProcThreadAttributeList(value); }
    } attributes{startup.lpAttributeList};
    std::array<HANDLE, 3> inherited{startup.StartupInfo.hStdInput, startup.StartupInfo.hStdOutput, startup.StartupInfo.hStdError};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited.data(), sizeof(inherited), nullptr, nullptr)) return 0;
    PROCESS_INFORMATION child{};
    // The fixture inherits only stdio and the root's owned job; no breakaway or shell is used.
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup.StartupInfo, &child)) return 0;
    const auto child_owned = IsProcessInJob(child.hProcess, nullptr, &owned) && owned;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return child_owned ? child.dwProcessId : 0;
}
}
#endif

int main(int argc, char** argv) {
    using nickel::minecraft::filesystem_path;
    std::vector<std::string> arguments(argv + 1, argv + argc);
    if (std::find(arguments.begin(), arguments.end(), "-version") != arguments.end()) {
        const auto value = std::getenv("NICKEL_TEST_JAVA_MAJOR");
        const auto major = value ? value : "17";
        std::fprintf(stderr, "java.version = %s.0.1\njava.vendor = Nickel controlled mock\nos.arch = amd64\nsun.arch.data.model = 64\n", major);
        return 0;
    }
    const auto value = [&](const std::string& key, std::string fallback = {}) {
        const auto found = std::find(arguments.begin(), arguments.end(), key);
        return found != arguments.end() && found + 1 != arguments.end() ? *(found + 1) : fallback;
    };
#ifdef _WIN32
    if (const auto delay = value("--hold-inherited-pipes"); !delay.empty()) {
        const auto duration = std::stoul(delay);
        BOOL owned = FALSE;
        if (duration < 1000 || duration > 30000 || !IsProcessInJob(GetCurrentProcess(), nullptr, &owned) || !owned ||
            GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_PIPE ||
            GetFileType(GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_PIPE) return 74;
        std::fprintf(stdout, "held-pipe-child-ready %lu\n", GetCurrentProcessId());
        std::fprintf(stderr, "held-pipe-child-ready %lu\n", GetCurrentProcessId());
        std::fflush(stdout); std::fflush(stderr);
        std::this_thread::sleep_for(std::chrono::milliseconds(duration));
        std::fputs("held-pipe-child-tail\n", stdout); std::fputs("held-pipe-child-tail\n", stderr);
        return 0;
    }
    if (const auto delay = value("--spawn-pipe-holder"); !delay.empty()) {
        const auto duration = std::stoul(delay);
        if (duration < 1000 || duration > 30000) return 75;
        const auto child = spawn_pipe_holder(duration);
        if (!child) {
            std::fprintf(stderr, "controlled pipe-holder spawn failed: %lu\n", GetLastError());
            return 76;
        }
        std::fprintf(stdout, "held-pipe-parent-child %lu\n", child);
        std::fflush(stdout);
        return 0;
    }
#endif
    if (const auto output = value("--output"); !output.empty()) {
        const auto path = std::filesystem::u8path(output);
        std::filesystem::create_directories(filesystem_path(path.parent_path()));
        if (const auto source = value("--copy"); !source.empty())
            std::filesystem::copy_file(filesystem_path(std::filesystem::u8path(source)), filesystem_path(path), std::filesystem::copy_options::overwrite_existing);
        else {
            std::ofstream file(filesystem_path(path), std::ios::binary);
            file << value("--content", "processor-result");
        }
    }
    if (std::find(arguments.begin(), arguments.end(), "optifine.Installer") != arguments.end()) {
        const auto home = std::find_if(arguments.begin(), arguments.end(), [](const auto& argument) { return argument.starts_with("-Duser.home="); });
        if (home == arguments.end()) return 71;
        const auto minecraft = std::filesystem::u8path(home->substr(12)) / ".minecraft";
        std::string version;
        for (const auto& entry : std::filesystem::directory_iterator(filesystem_path(minecraft / "versions"))) {
            if (!entry.is_directory() || !version.empty()) return 72;
            version = entry.path().filename().string();
        }
        if (version.empty() || value("-cp").empty()) return 73;
        const auto id = version + "-OptiFine_Mock";
        const auto root = minecraft / "versions" / id;
        std::filesystem::create_directories(filesystem_path(root));
        const auto library = minecraft / "libraries/optifine/OptiFine/Mock/OptiFine-Mock.jar";
        std::filesystem::create_directories(filesystem_path(library.parent_path()));
        // Preserve a real fixture ZIP so native archive validation is never bypassed.
        std::filesystem::copy_file(filesystem_path(std::filesystem::u8path(value("-cp"))), filesystem_path(library));
        std::ofstream(filesystem_path(root / (id + ".json"))) << "{\"id\":\"" << id << "\",\"inheritsFrom\":\"" << version << "\",\"mainClass\":\"net.minecraft.launchwrapper.Launch\",\"libraries\":[{\"name\":\"optifine:OptiFine:Mock\"}]}";
    }
    if (value("--flood") == "yes") {
        std::array<char, 4096> block;
        block.fill('x');
        for (unsigned n = 0; n < 128; ++n) {
            std::fwrite(block.data(), 1, block.size(), stdout);
            std::fwrite(block.data(), 1, block.size(), stderr);
        }
        std::fputs("\n", stdout); std::fputs("\n", stderr);
    }
    if (const auto secret = value("--accessToken"); !secret.empty()) {
        const auto middle = secret.size() / 2;
        std::fputs("token=", stdout);
        std::fwrite(secret.data(), 1, middle, stdout); std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        std::fwrite(secret.data() + middle, 1, secret.size() - middle, stdout);
        std::fputs("\n", stdout);
    }
    std::fputs("controlled stdout\n", stdout); std::fputs("controlled stderr\n", stderr);
    std::fflush(stdout); std::fflush(stderr);
    std::this_thread::sleep_for(std::chrono::milliseconds(std::stoul(value("--sleep", "0"))));
    std::fputs("stdout-tail", stdout); std::fputs("stderr-tail", stderr);
    return std::stoi(value("--exit", "0"));
}
