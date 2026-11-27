#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"
#include "core/config/paths.hpp"
#include "core/config/repository.hpp"
#include "core/config/vault.hpp"
#include "platform/java.hpp"
#include "platform/memory.hpp"
#include "platform/process.hpp"
#include "platform/services.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {
using namespace nickel;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

std::string utf8(std::u8string_view value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

template<class Function>
void rejects(std::string_view expected, Function function) {
    try {
        function();
    } catch (const ErrorException& exception) {
        require(exception.error().code == expected,
                "unexpected error code: " + exception.error().code + "; wanted " + std::string(expected));
        return;
    }
    throw std::runtime_error("operation did not reject with " + std::string(expected));
}

#ifndef _WIN32
Json symlink_rejection(const fs::path& directory, bool ancestor, bool write) {
    const auto real = directory / "real";
    fs::create_directory(real);
    const auto target = real / "data";
    atomic_write_text(target, "original");
    const auto link = directory / "link";
    if (ancestor) fs::create_directory_symlink(real, link);
    else fs::create_symlink(target, link);
    const auto attempted = ancestor ? link / "data" : link;
    bool rejected = false;
    std::string actual_code;
    Json native_details = Json::object();
    try {
        if (write) atomic_write_text(attempted, "changed");
        else (void)read_text_bounded(attempted, 64);
    } catch (const ErrorException& exception) {
        rejected = true;
        actual_code = exception.error().code;
        native_details = exception.error().details;
    } catch (const std::exception& exception) {
        rejected = true;
        actual_code = "UnexpectedStdException";
        native_details = Json{{"message", exception.what()}};
    }
    // Preserve safety observations even when the error-contract assertion fails.
    const bool target_unchanged = read_text_bounded(target, 64) == "original";
    const bool link_retained = fs::is_symlink(fs::symlink_status(link));
    const Json details{{"rejected", rejected}, {"expectedCode", "UnsafePath"},
                       {"actualCode", actual_code}, {"nativeDetails", native_details},
                       {"targetUnchanged", target_unchanged}, {"linkRetained", link_retained},
                       {"ancestor", ancestor}, {"operation", write ? "write" : "read"}};
    if (!rejected || actual_code != "UnsafePath" || !target_unchanged || !link_retained) {
        throw ErrorException(Error{"PortabilityContractFailure", "Symlink rejection contract mismatch", details});
    }
    return details;
}
#endif

Json environment_item(std::string_view key) {
    const auto environment = current_environment();
    const auto found = environment.find(std::string(key));
    return found == environment.end() ? Json(nullptr) : Json(found->second);
}

int child(const std::vector<std::string>& arguments) {
    if (arguments.at(1) == "--child-argv") {
        std::cout << Json{{"arguments", std::vector<std::string>(arguments.begin() + 2, arguments.end())},
                          {"cwd", path_to_utf8(fs::current_path())},
                          {"keep", environment_item("NICKEL_PORTABILITY_KEEP")},
                          {"removed", environment_item("NICKEL_PORTABILITY_REMOVE")},
                          {"unicode", environment_item("NICKEL_PORTABILITY_UNICODE")}}.dump();
    } else if (arguments.at(1) == "--child-sleep") {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoul(arguments.at(2))));
        std::cout << "owned-child-completed\n";
    } else if (arguments.at(1) == "--child-flood") {
        const std::string block(4096, 'f');
        for (int index = 0; index < 32; ++index) {
            std::cout << block;
            std::cerr << block;
        }
    } else {
        throw std::runtime_error("unknown child mode");
    }
    return 0;
}

struct Cases {
    Json results = Json::array();
    fs::path root;
    fs::path executable;
    int failed = 0;

    fs::path directory(std::string_view name) const {
        const auto path = root / path_from_utf8(name);
        fs::create_directories(path);
        return path;
    }

    void run(std::string_view name, const std::function<Json()>& function) {
        try {
            results.push_back(Json{{"name", name}, {"status", "PASS"}, {"details", function()}});
        } catch (const ErrorException& exception) {
            ++failed;
            results.push_back(Json{{"name", name}, {"status", "FAIL"},
                                   {"error", exception.error().message},
                                   {"errorCode", exception.error().code},
                                   {"details", exception.error().details}});
        } catch (const std::exception& exception) {
            ++failed;
            results.push_back(Json{{"name", name}, {"status", "FAIL"}, {"error", exception.what()}});
        }
    }

    ProcessOptions sleeper(std::chrono::milliseconds duration) const {
        ProcessOptions options;
        options.executable = executable;
        options.arguments = {"--child-sleep", std::to_string(duration.count())};
        options.cwd = root;
        options.inherit_environment = false;
        return options;
    }
};

int probe(const std::vector<std::string>& arguments) {
    require(arguments.size() == 3 && arguments[1] == "--fixture-root", "supply an owned fixture root");
    const auto root = fs::absolute(path_from_utf8(arguments[2])).lexically_normal();
    require(root.has_relative_path() && root != root.root_path(), "refusing a filesystem root fixture");
    fs::create_directories(root);
    Cases cases{Json::array(), root, current_executable(), 0};

    cases.run("current-executable", [&] {
        require(fs::is_regular_file(cases.executable), "platform current_executable is not a real file");
        return Json{{"path", path_to_utf8(cases.executable)}};
    });
    cases.run("utf8-path-roundtrip", [&] {
        const auto text = utf8(u8"\u542f\u52a8\u5668/\u7528\u6237 \U0001f680/e\u0301.json");
        require(valid_utf8(text), "valid Unicode text rejected");
        require(path_to_utf8(path_from_utf8(text)) == text, "UTF-8 path changes code points");
        const auto path = cases.directory("utf8") / path_from_utf8(text);
        atomic_write_text(path, utf8(u8"\u4e2d\u6587 \U0001f680"));
        require(read_text_bounded(path, 128) == utf8(u8"\u4e2d\u6587 \U0001f680"), "UTF-8 write/read mismatch");
        return Json{{"path", path_to_utf8(path)}, {"bytes", fs::file_size(path)}};
    });
    cases.run("utf8-invalid-and-nul", [&] {
        for (const std::string& invalid : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
                                           std::string("\xf4\x90\x80\x80", 4), std::string("a\0b", 3)}) {
            rejects("InvalidArgument", [&] { (void)path_from_utf8(invalid); });
        }
        return Json{{"rejections", 4}};
    });
    cases.run("long-path-atomic-write", [&] {
        auto path = cases.directory("longpath");
        for (int index = 0; index < 6; ++index) path /= std::string(65, 'p') + std::to_string(index);
        path /= "settings.json";
        atomic_write_text(path, "{\"portable\":true}");
        require(path_to_utf8(path).size() > 400, "fixture is not long enough");
        require(read_text_bounded(path, 64) == "{\"portable\":true}", "long path data mismatch");
        return Json{{"pathLengthUtf8", path_to_utf8(path).size()}, {"path", path_to_utf8(path)},
                    {"scope", "Fixture manifest only; not evidence of final product manifest"}};
    });
    cases.run("repository-uint64-unknown-fields", [&] {
        JsonRepository repository(cases.directory("repository"));
        const Json source{{"unknown", Json{{"huge", std::numeric_limits<std::uint64_t>::max()},
                                            {"unicode", utf8(u8"\u7528\u6237")}}}, {"count", 0}};
        repository.write("settings.json", source);
        repository.update("settings.json", Json::object(), [](Json& value) { value["count"] = 1; });
        const auto actual = repository.read("settings.json");
        require(actual["unknown"] == source["unknown"] && actual["count"] == 1, "canonical JSON loses fields");
        return Json{{"uint64", actual["unknown"]["huge"]}};
    });
    cases.run("atomic-cancel-preserves-existing", [&] {
        const auto path = cases.directory("cancel-write") / "settings.json";
        atomic_write_text(path, "before");
        Cancellation cancellation;
        cancellation.cancel();
        AtomicWriteOptions options;
        options.cancellation = &cancellation;
        rejects("Cancelled", [&] { atomic_write_text(path, "after", options); });
        require(read_text_bounded(path, 64) == "before", "cancelled save changed disk");
        return Json::object();
    });
    cases.run("rename-no-replace-file", [&] {
        const auto directory = cases.directory("rename-file");
        const auto source = directory / "source";
        const auto target = directory / "target";
        atomic_write_text(source, "source");
        atomic_write_text(target, "target");
        require(!rename_no_replace(source, target), "existing target was overwritten");
        require(read_text_bounded(source, 64) == "source" && read_text_bounded(target, 64) == "target", "failed reserve changes bytes");
        const auto unique = directory / "unique";
        require(rename_no_replace(source, unique) && !fs::exists(source), "reserve to unused path failed");
        return Json::object();
    });
    cases.run("rename-no-replace-directory", [&] {
        const auto directory = cases.directory("rename-directory");
        const auto source = directory / "source";
        const auto target = directory / "target";
        fs::create_directory(source);
        fs::create_directory(target);
        atomic_write_text(source / "owned", "source");
        atomic_write_text(target / "owned", "target");
        require(!rename_no_replace(source, target), "existing directory was replaced");
        require(rename_no_replace(source, directory / "unique"), "directory publication failed");
        require(read_text_bounded(target / "owned", 64) == "target", "existing directory content changed");
        return Json::object();
    });
    cases.run("vault-openssl-gcm-interoperability", [&] {
        VaultKey key{};
        VaultNonce nonce{};
        // AES-256-GCM zero-key/nonce empty-message tag: 530f8afbc74536b9a963b4f1c4cb738b.
        const std::string empty = "v1:AAAAAAAAAAAAAAAAUw+K+8dFNrmpY7TxxMtziw==";
        const auto sealed = Vault::seal_with_nonce(key, nonce, "");
        require(sealed == empty, "AES-256-GCM known tag mismatch");
        require(Vault::open_with_key(key, empty).empty(), "known vector decrypt failed");
        const auto unicode = utf8(u8"synthetic-only:\u7528\u6237\U0001f680");
        const auto text = Vault::seal_with_nonce(key, nonce, unicode);
        require(Vault::open_with_key(key, text) == unicode, "GCM Unicode roundtrip failed");
        auto changed = text;
        changed[24] = changed[24] == 'A' ? 'B' : 'A';
        rejects("AuthenticationFailed", [&] { (void)Vault::open_with_key(key, changed); });
        return Json{{"knownVector", empty}, {"syntheticOnly", true}};
    });
    cases.run("vault-private-key-permissions", [&] {
        const auto directory = cases.directory("vault-private");
        Vault vault(directory);
        const auto key = vault.load_or_create_key();
        require(key == vault.load_or_create_key() && fs::file_size(vault.key_path()) == 32, "vault key is unstable");
#ifndef _WIN32
        struct stat status{};
        require(::stat(vault.key_path().c_str(), &status) == 0, "cannot stat owned key");
        require((status.st_mode & 0777) == 0600, "POSIX key is not mode0600");
        return Json{{"mode", "0600"}, {"path", path_to_utf8(vault.key_path())}};
#else
        return Json{{"path", path_to_utf8(vault.key_path())},
                    {"scope", "ACL requires separate actual SID audit; key length/identity only"}};
#endif
    });
#ifndef _WIN32
    cases.run("symlink-leaf-read-rejection", [&] {
        return symlink_rejection(cases.directory("symlink-leaf-read"), false, false);
    });
    cases.run("symlink-leaf-write-rejection", [&] {
        return symlink_rejection(cases.directory("symlink-leaf-write"), false, true);
    });
    cases.run("symlink-ancestor-read-rejection", [&] {
        return symlink_rejection(cases.directory("symlink-ancestor-read"), true, false);
    });
    cases.run("symlink-ancestor-write-rejection", [&] {
        return symlink_rejection(cases.directory("symlink-ancestor-write"), true, true);
    });
    cases.run("posix-case-distinct-files", [&] {
        const auto directory = cases.directory("case-sensitive");
        atomic_write_text(directory / "Case", "one");
        atomic_write_text(directory / "case", "two");
        require(read_text_bounded(directory / "Case", 64) == "one", "filesystem folds case");
        require(read_text_bounded(directory / "case", 64) == "two", "filesystem case fixture mismatch");
        return Json::object();
    });
    cases.run("xdg-owned-system-context", [&] {
        const auto environment = current_environment();
        require(environment.at("HOME") == path_to_utf8(root / "home"), "runner did not isolate HOME");
        const auto context = PathContext::system();
#ifdef __APPLE__
        require(context.macos && context.system_config_base == root / "home/Library/Application Support", "macOS support directory mismatch");
#else
        require(context.system_config_base == root / "xdg-config", "XDG config not honored");
        require(context.app_data_base == root / "xdg-data" / path_from_utf8(application_identifier), "XDG data not honored");
#endif
        return Json{{"config", path_to_utf8(context.system_config_base)}, {"data", path_to_utf8(context.app_data_base)}};
    });
    cases.run("posix-dialog-adapter-boundary", [&] {
        rejects("Unsupported", [&] { (void)show_native_dialog(DialogOptions{}); });
        return Json{{"scope", "Core intentionally requires Qt GUI adapter; complete UI not validated"}};
    });
#endif
    cases.run("macos-bundle-path-policy-only", [&] {
        const auto bundle = root / "Nickel.app/Contents/MacOS/Nickel";
        require(directory_for_executable(bundle, true) == root, "bundle-adjacent portable root policy mismatch");
        require(directory_for_executable(bundle, false) == bundle.parent_path(), "non-Mac path changed");
        return Json{{"scope", "Pure path policy on current host; NOT a macOS host run"}};
    });
    cases.run("owned-process-argv-cwd-environment", [&] {
        ProcessOptions options;
        options.executable = cases.executable;
        options.cwd = cases.directory("process-cwd") / path_from_utf8(utf8(u8"\u76ee\u5f55 space"));
        fs::create_directories(options.cwd);
        options.inherit_environment = false;
        options.environment["NICKEL_PORTABILITY_KEEP"] = "owned";
        options.environment["NICKEL_PORTABILITY_REMOVE"] = std::nullopt;
        options.environment["NICKEL_PORTABILITY_UNICODE"] = utf8(u8"\u4e2d\u6587\U0001f680");
        const std::vector<std::string> expected{"", "one argument", "quote\"inside", "apostrophe'", "tail\\",
                                                "space tail\\", utf8(u8"\u7528\u6237\U0001f680"),
                                                "a;b|$(not-a-command)", "C:\\Game Path\\"};
        options.arguments = {"--child-argv"};
        options.arguments.insert(options.arguments.end(), expected.begin(), expected.end());
        auto process = OwnedProcess::spawn(options);
        const auto result = process.wait(10s);
        require(result && result->exit_code == 0 && result->stdout_eof && result->stderr_eof, "argv child did not drain successfully");
        const auto actual = Json::parse(result->stdout_text);
        require(actual["arguments"] == expected, "argv boundaries or Unicode changed");
        require(actual["cwd"] == path_to_utf8(options.cwd), "child cwd mismatch");
        require(actual["keep"] == "owned" && actual["removed"].is_null(), "environment override/removal mismatch");
        require(actual["unicode"] == *options.environment["NICKEL_PORTABILITY_UNICODE"], "Unicode environment mismatch");
        return Json{{"argumentCount", expected.size()}, {"pid", process.pid()}};
    });
    cases.run("owned-process-rejects-invalid-input", [&] {
        auto options = cases.sleeper(10ms);
        options.arguments = {std::string("bad\0arg", 7)};
        rejects("InvalidArgument", [&] { (void)OwnedProcess::spawn(options); });
        options.arguments = {std::string("\xc0\xaf", 2)};
        rejects("InvalidArgument", [&] { (void)OwnedProcess::spawn(options); });
        options.arguments = {"--child-sleep", "10"};
        options.environment["bad=key"] = "value";
        rejects("InvalidArgument", [&] { (void)OwnedProcess::spawn(options); });
        return Json{{"rejections", 3}};
    });
    cases.run("owned-process-timeout-retains-child", [&] {
        auto process = OwnedProcess::spawn(cases.sleeper(400ms));
        require(!process.wait(5ms) && process.running(), "timeout lost or terminated child");
        const auto result = process.wait(5s);
        require(result && result->exit_code == 0 && !result->terminated, "timeout child did not finish naturally");
        return Json{{"pid", process.pid()}};
    });
    cases.run("owned-preparation-cancel-and-drain", [&] {
        auto process = OwnedProcess::spawn(cases.sleeper(5s));
        Cancellation cancellation;
        cancellation.cancel();
        rejects("Cancelled", [&] { (void)process.wait(5s, &cancellation); });
        const auto result = process.wait(5s);
        require(result && result->terminated && result->stdout_eof && result->stderr_eof, "cancelled preparation not fully drained");
        return Json{{"pid", process.pid()}, {"exitCode", result->exit_code}, {"signalled", result->signalled}};
    });
    cases.run("owned-game-cancel-does-not-kill", [&] {
        auto options = cases.sleeper(700ms);
        options.purpose = ProcessPurpose::Game;
        auto process = OwnedProcess::spawn(options);
        Cancellation cancellation;
        cancellation.cancel();
        rejects("Cancelled", [&] { (void)process.wait(5s, &cancellation); });
        require(process.running(), "game cancellation killed owned game");
        rejects("PermissionDenied", [&] { process.terminate(); });
        const auto result = process.wait(5s);
        require(result && result->exit_code == 0 && !result->terminated, "game did not exit naturally");
        return Json{{"pid", process.pid()}};
    });
    cases.run("bounded-capture-unbounded-drain", [&] {
        auto options = cases.sleeper(0ms);
        options.arguments = {"--child-flood"};
        options.capture_limit = 1024;
        std::atomic<std::size_t> bytes{0};
        std::atomic<int> eof{0};
        options.output = [&](ProcessStream, std::string_view chunk, bool done) {
            bytes += chunk.size();
            if (done) ++eof;
        };
        auto process = OwnedProcess::spawn(options);
        const auto result = process.wait(10s);
        require(result && result->exit_code == 0 && result->stdout_eof && result->stderr_eof, "flood child did not drain");
        require(result->stdout_text.size() == 1024 && result->stderr_text.size() == 1024 &&
                result->stdout_truncated && result->stderr_truncated, "capture bound mismatch");
        require(bytes == 262144 && eof == 2, "callbacks lost data/EOF");
        return Json{{"callbackBytes", bytes.load()}, {"stdoutCapture", result->stdout_text.size()},
                    {"stderrCapture", result->stderr_text.size()}, {"eofCount", eof.load()}};
    });
    cases.run("physical-memory-native-adapter", [&] {
        const auto memory = physical_memory();
        require(memory.total_mb && *memory.total_mb > 0, "physical total unavailable");
        require(!memory.available_mb || *memory.available_mb <= *memory.total_mb, "available exceeds total");
        return Json{{"totalMiB", *memory.total_mb},
                    {"availableMiB", memory.available_mb ? Json(*memory.available_mb) : Json(nullptr)},
                    {"scope", "OS adapter fixture; not launcher memory benchmark"}};
    });
    cases.run("java-probe-parser-platform-architecture", [&] {
        for (const auto& architecture : std::array<std::string, 3>{"amd64", "aarch64", "x86"}) {
            const auto parsed = parse_java_output(cases.executable,
                "java.version = 21.0.5\njava.vendor = Synthetic\nos.arch = " + architecture +
                "\nsun.arch.data.model = " + (architecture == "x86" ? "32\n" : "64\n"));
            require(parsed && parsed->major == 21 && parsed->architecture == architecture &&
                    parsed->is_64_bit == (architecture != "x86"), "Java architecture parser mismatch");
        }
        return Json{{"scope", "Parser fixtures only; no user Java/game scanned or launched"}};
    });

#ifdef _WIN32
    constexpr const char* host = "windows";
#elif defined(__APPLE__)
    constexpr const char* host = "macos";
#else
    constexpr const char* host = "linux";
#endif
    const Json report{{"schema", 1}, {"scope", "Independent fixtures linked to actual foundation/platform implementation; NOT full launcher"},
                      {"host", host}, {"fixtureRoot", path_to_utf8(root)}, {"caseCount", cases.results.size()},
                      {"failed", cases.failed}, {"cases", cases.results}};
    std::cout << report.dump(2) << '\n';
    return cases.failed == 0 ? 0 : 1;
}

int entry(const std::vector<std::string>& arguments) {
    try {
        if (arguments.size() > 1 && arguments[1].starts_with("--child-")) return child(arguments);
        return probe(arguments);
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 2;
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) {
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0) return 2;
        std::string value(static_cast<std::size_t>(length), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, value.data(), length, nullptr, nullptr)) return 2;
        value.pop_back();
        arguments.push_back(std::move(value));
    }
    return entry(arguments);
}
#else
int main(int argc, char** argv) {
    return entry(std::vector<std::string>(argv, argv + argc));
}
#endif
