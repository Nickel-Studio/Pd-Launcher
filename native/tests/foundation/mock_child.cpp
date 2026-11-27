#include "core/common/json.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/repository.hpp"
#include "core/config/vault.hpp"
#include "platform/process.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
#ifdef _WIN32
int spawn_pipe_holder(const std::vector<std::string>& arguments) {
    using namespace nickel;
    if (arguments.size() != 3) return 2;
    auto command = path_from_utf8(quote_windows_argument(path_to_utf8(current_executable())) +
        " --hold-pipes " + quote_windows_argument(arguments[1]) + " " + quote_windows_argument(arguments[2])).native();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process)) return 6;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::cout << "root-final" << std::flush;
    return 29;
}
#endif
int run(const std::vector<std::string>& arguments) {
    using namespace nickel;
    if (arguments.empty()) return 2;
    const auto& mode = arguments[0];
#ifdef _WIN32
    if (mode == "--module-image") {
        std::vector<wchar_t> image(32768);
        const auto length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
        if (!length || length >= image.size()) return 4;
        const std::wstring_view raw(image.data(), length);
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, raw.data(),
            static_cast<int>(raw.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 4;
        std::string utf8(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()),
            utf8.data(), size, nullptr, nullptr) != size) return 4;
        std::cout << Json{{"image", utf8}, {"nativeUnits", length},
            {"extended", raw.starts_with(L"\\\\?\\")}}.dump() << std::flush;
        return 0;
    }
    if (mode == "--spawn-pipe-holder") return spawn_pipe_holder(arguments);
    if (mode == "--hold-pipes" && arguments.size() == 3) {
        std::cout << "descendant-ready" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(arguments[2])));
        atomic_write_text(path_from_utf8(arguments[1]), "descendant-survived");
        std::cout << "descendant-final" << std::flush;
        std::cerr << "descendant-stderr-final" << std::flush;
        return 0;
    }
#endif
    if (mode == "--java" || mode == "-XshowSettings:properties") {
        std::cerr << "Property settings:\n    java.version = 1.8.0_402\n    java.vendor = Synthetic Vendor\n    os.arch = amd64\n    sun.arch.data.model = 64\n";
        return 0;
    }
    if (mode == "--echo") {
        Json value;
        value["arguments"] = arguments;
        value["cwd"] = path_to_utf8(std::filesystem::current_path());
        const auto environment = current_environment();
        const auto item = environment.find("NICKEL_SYNTHETIC");
        value["env"] = item == environment.end() ? Json(nullptr) : Json(item->second);
        std::cout << value.dump() << std::flush;
        std::cerr << "synthetic-stderr-final" << std::flush;
        return 23;
    }
    if (mode == "--flood") {
        std::string stdout_bytes(32768, 'o'), stderr_bytes(32768, 'e');
        for (unsigned i = 0; i < 48; ++i) {
            std::cout.write(stdout_bytes.data(), static_cast<std::streamsize>(stdout_bytes.size()));
            std::cerr.write(stderr_bytes.data(), static_cast<std::streamsize>(stderr_bytes.size()));
        }
        std::cout << "stdout-tail" << std::flush;
        std::cerr << "stderr-tail" << std::flush;
        return 0;
    }
    if (mode == "--sleep") {
        const auto duration = arguments.size() > 1 ? std::stoi(arguments[1]) : 1000;
        std::this_thread::sleep_for(std::chrono::milliseconds(duration));
        std::cout << "completed" << std::flush;
        return 0;
    }
    if (mode == "--vault-create" && arguments.size() == 3) {
        Vault vault(path_from_utf8(arguments[1]));
        const auto sealed = vault.seal(arguments[2]);
        std::cout << sealed << std::flush;
        return 0;
    }
    if (mode == "--increment" && arguments.size() == 3) {
        JsonRepository repository(path_from_utf8(arguments[1]));
        const auto count = std::stoi(arguments[2]);
        for (int i = 0; i < count; ++i) {
            repository.update("settings.json", Json{{"count", 0}}, [](Json& value) {
                value["count"] = value.value("count", 0) + 1;
            });
        }
        return 0;
    }
    if (mode == "--game-marker" && arguments.size() == 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(arguments[2])));
        atomic_write_text(path_from_utf8(arguments[1]), "survived");
        return 0;
    }
    return 3;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        const auto text = std::wstring_view(argv[i]);
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (size < 0) return 4;
        std::string utf8(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
        arguments.push_back(std::move(utf8));
    }
    try { return run(arguments); } catch (const std::exception& error) { std::cerr << error.what(); return 5; }
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    try { return run(arguments); } catch (const std::exception& error) { std::cerr << error.what(); return 5; }
}
#endif
