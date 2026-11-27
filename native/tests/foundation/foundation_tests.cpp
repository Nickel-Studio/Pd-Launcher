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

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

namespace {
using namespace nickel;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
void require(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
template<class F> void fails(F&& callback, std::string_view message) {
    bool failed = false;
    try { callback(); } catch (const ErrorException&) { failed = true; }
    require(failed, message);
}
struct FixtureRoot {
    fs::path path = fs::current_path() / path_from_utf8("foundation-synthetic-" + unique_token());
    FixtureRoot() { fs::create_directories(path); }
    ~FixtureRoot() { std::error_code error; fs::remove_all(path, error); }
};
void raw_write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) throw std::runtime_error("Synthetic raw fixture write failed");
}
template<std::size_t N> std::array<std::uint8_t, N> from_hex(std::string_view text) {
    require(text.size() == N * 2, "hex fixture size");
    auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::array<std::uint8_t, N> result{};
    for (std::size_t i = 0; i < N; ++i) result[i] = static_cast<std::uint8_t>(digit(text[i * 2]) * 16 + digit(text[i * 2 + 1]));
    return result;
}
Json good_marker() {
    return Json{{"schemaVersion", 1}, {"format", "nickel-portable"}, {"packageId", "0123456789abcdef0123456789abcdef"},
        {"name", "Synthetic Pack"}, {"version", "1.0"}, {"instanceName", "Synthetic instance"},
        {"payload", "Nickel/pack.mrpack"}, {"gameDirectory", ".minecraft"}, {"dataDirectory", "Nickel"}};
}

void test_vault(const fs::path& mock) {
    FixtureRoot root;
    const auto fixtures = Json::parse(read_text_bounded(path_from_utf8(NICKEL_FOUNDATION_FIXTURES), 64 * 1024));
    for (const auto& item : fixtures["cases"]) {
        const auto key = from_hex<32>(item["key"].get<std::string>());
        const auto nonce = from_hex<12>(item["nonce"].get<std::string>());
        const auto plaintext = item["plaintext"].get<std::string>();
        const auto sealed = item["sealed"].get<std::string>();
        require(Vault::seal_with_nonce(key, nonce, plaintext) == sealed, "CNG differs from independent AES256GCM fixture");
        require(Vault::open_with_key(key, sealed) == plaintext, "Python AES256GCM fixture cannot be opened");
        auto wrong = key; wrong[0] ^= 1;
        fails([&] { (void)Vault::open_with_key(wrong, sealed); }, "wrong key accepted");
        auto corrupt = sealed; corrupt.back() = corrupt.back() == '=' ? 'A' : 'B';
        fails([&] { (void)Vault::open_with_key(key, corrupt); }, "tampered envelope accepted");
    }
    const auto key = from_hex<32>(fixtures["cases"][0]["key"].get<std::string>());
    fails([&] { (void)Vault::open_with_key(key, fixtures["invalidUtf8"].get<std::string>()); }, "invalid UTF8 accepted");
    for (const auto bad : {"v2:AAAA", "v1:AAAA", "v1:!!!!", "v1:AAAA===", "plain", "v1:AA==AA=="}) {
        fails([&] { (void)Vault::open_with_key(key, bad); }, "invalid envelope accepted");
    }
    Vault missing(root.path / "missing");
    fails([&] { (void)missing.open(fixtures["cases"][0]["sealed"].get<std::string>()); }, "missing key did not fail");
    require(!fs::exists(root.path / "missing"), "open created missing key/root");
    Vault vault(root.path / "vault");
    const auto first = vault.seal("synthetic password");
    const auto original = read_file_bounded(vault.key_path(), 32);
    require(vault.open(first) == "synthetic password", "vault normal round trip");
    require(vault.seal("synthetic password") != first, "random nonce reused");
    require(read_file_bounded(vault.key_path(), 32) == original, "existing key replaced");
    std::cout << "PASS vault vectors and no-key side effect\n";
    std::vector<OwnedProcess> children;
    const auto concurrent = root.path / "race";
    for (unsigned i = 0; i < 8; ++i) {
        ProcessOptions options;
        options.executable = mock;
        options.arguments = {"--vault-create", path_to_utf8(concurrent), "parallel-synthetic"};
        children.push_back(OwnedProcess::spawn(options));
    }
    Vault concurrent_vault(concurrent);
    for (auto& child : children) {
        const auto result = child.wait(15s);
        require(result && result->exit_code == 0, "concurrent key child failed");
        require(concurrent_vault.open(result->stdout_text) == "parallel-synthetic", "concurrent key writers diverged");
    }
    std::cout << "PASS vault eight-process creation\n";
    for (const auto size : {0, 16, 31, 33, 8192}) {
        const auto directory = root.path / ("bad-" + std::to_string(size));
        fs::create_directories(directory);
        std::string bytes(static_cast<std::size_t>(size), 'x');
        raw_write(directory / "credentials.key", bytes);
        Vault broken(directory);
        fails([&] { (void)broken.load_or_create_key(); }, "damaged key recreated");
        require(read_text_bounded(directory / "credentials.key", 8192) == bytes, "damaged key changed");
    }
    const auto half = root.path / "half";
    fs::create_directories(half);
    raw_write(half / "credentials.key", std::string(16, 'z'));
#ifdef _WIN32
    struct CompletingWriter {
        HANDLE handle = INVALID_HANDLE_VALUE;
        void close() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); handle = INVALID_HANDLE_VALUE; }
        ~CompletingWriter() { close(); }
    } writer{CreateFileW((half / "credentials.key").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(writer.handle != INVALID_HANDLE_VALUE, "Synthetic completing key writer unavailable");
#endif
    std::exception_ptr writer_failure;
    std::jthread complete([&] {
        try {
            std::this_thread::sleep_for(10ms);
#ifdef _WIN32
            const std::string bytes(16, 'z');
            LARGE_INTEGER offset{}; offset.QuadPart = 16;
            DWORD written = 0;
            require(SetFilePointerEx(writer.handle, offset, nullptr, FILE_BEGIN) &&
                WriteFile(writer.handle, bytes.data(), 16, &written, nullptr) && written == 16 &&
                FlushFileBuffers(writer.handle), "Synthetic half-key append failed");
#else
            raw_write(half / "credentials.key", std::string(32, 'z'));
#endif
        } catch (...) { writer_failure = std::current_exception(); }
#ifdef _WIN32
        writer.close();
#endif
    });
    std::exception_ptr reader_failure;
    std::optional<VaultKey> completed_key;
    try { completed_key = Vault(half).load_key(); } catch (...) { reader_failure = std::current_exception(); }
    complete.join();
    if (writer_failure) std::rethrow_exception(writer_failure);
    if (reader_failure) std::rethrow_exception(reader_failure);
    require(completed_key && (*completed_key)[31] == 'z', "half-write retry did not recover");
#ifdef _WIN32
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto status = GetNamedSecurityInfoW(const_cast<LPWSTR>(vault.key_path().c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor);
    require(status == ERROR_SUCCESS && acl && descriptor, "private key ACL unavailable");
    require(acl->AceCount == 2, "key ACL inherited extra principals");
    LocalFree(descriptor);
#else
    struct stat info{};
    require(::stat(vault.key_path().c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "key permissions are not 0600");
#endif
    for (const auto& entry : fs::directory_iterator(concurrent)) require(!entry.path().filename().string().starts_with(".credentials.key.tmp-"), "key temp leftover");
    std::cout << "PASS vault: independent vectors, nonce/tag, no-key side effect, key races, retries, permissions\n";
}

void test_paths() {
    FixtureRoot root;
    const auto exe = root.path / "exe";
    const auto config = root.path / "system";
    const auto appdata = root.path / "app-data";
    fs::create_directories(exe);
    PathResolver resolver(PathContext{exe / "Nickel.exe", config, appdata, false});
    require(resolver.config_root(false) == exe / "Nickel", "empty config should be adjacent");
    require(!fs::exists(exe / "Nickel"), "read-only config resolution created directory");
    const auto system_root = config / "studio.nickel.launcher";
    for (const auto entry : {"settings.json", "accounts.json", "active_account.json", "game.json", "instances.json", "secrets.local.json", "credentials.key", "offline-skins", "agents", "cache"}) {
        raw_write(system_root / entry, "corrupt or cached");
        require(resolver.config_root(false) == system_root, "whole-root precedence lost for isolated entry");
        require(!fs::exists(exe / "Nickel"), "root selection blended/copied roots");
        fs::remove(system_root / entry);
    }
    fs::create_directories(system_root / "cache/downloads");
    raw_write(exe / "Nickel/settings.json", "{}");
    require(resolver.config_root(false) == system_root, "system cache must beat adjacent settings");
    raw_write(system_root / "game.json", Json{{"gameDir", "synthetic-game"}}.dump());
    raw_write(system_root / "instances.json", Json{{"roots", Json::array({"root-one", "root-two"})}, {"active", "chosen-root"}}.dump());
    require(resolver.default_game_directory() == "synthetic-game", "gameDir precedence failed");
    raw_write(system_root / "game.json", "{broken");
    require(resolver.default_game_directory() == "chosen-root", "active registry precedence failed");
    raw_write(system_root / "instances.json", Json{{"roots", Json::array({"root-one", "root-two"})}, {"active", "   "}}.dump());
    require(resolver.default_game_directory() == "root-one", "first registry root precedence failed");
    raw_write(system_root / "instances.json", Json{{"roots", 42}, {"active", "invalid-root"}}.dump());
    fs::create_directories(appdata / "minecraft/versions");
    require(resolver.default_game_directory() == appdata / "minecraft", "legacy app-data game fallback failed");
    fs::remove_all(appdata);
    require(resolver.default_game_directory() == exe / ".minecraft", "adjacent game fallback failed");
    require(resolver.bundled_game_directory() == exe / ".minecraft", "bundled game must ignore overrides");
    const auto bundle = exe / "Nickel.app/Contents/MacOS/Nickel";
    require(directory_for_executable(bundle, true) == exe, "exact macOS bundle normalization failed");
    require(directory_for_executable(bundle, false) == bundle.parent_path(), "macOS normalization leaked to Windows");
    for (const auto wrong : {"Nickel/Contents/MacOS/Nickel", "Nickel.app/Resources/MacOS/Nickel", "Nickel.app/Contents/MacOS/helpers/Nickel"}) {
        const auto path = exe / wrong;
        require(directory_for_executable(path, true) == path.parent_path(), "incomplete bundle shape normalized");
    }
    const auto marker = good_marker();
    raw_write(exe / "nickel.portable.json", marker.dump());
    require(resolver.config_root(false) == exe / "Nickel", "portable root did not beat ordinary config");
    require(resolver.default_game_directory() == exe / ".minecraft", "portable game layout did not win");
    require(read_portable_manifest(exe).has_value(), "portable marker unreadable");
    PathResolver nested(PathContext{exe / "nested/Nickel.exe", config, appdata, false});
    require(!nested.portable_manifest(), "ancestor marker leaked into nested launcher");
    for (const auto name : {"CON", "NUL.file", "CLOCK$", "COM1", "LPT9", "COM\xC2\xB9", "nickel.profile", "gameData", "nickel.settings", "a..b", "tail.", "dir/name", " leading"}) {
        auto bad = marker; bad["instanceName"] = name;
        fails([&] { (void)parse_portable_manifest(bad.dump()); }, "reserved/path instanceName accepted");
    }
    for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{{"schemaVersion", 2}, {"schemaVersion", 1.0},
        {"format", "other"}, {"packageId", "short"}, {"payload", "../pack.mrpack"}, {"gameDirectory", "../game"},
        {"dataDirectory", "Other"}, {"unknown", true}, {"name", "bad\nname"}, {"version", ""},
        {"launcher", Json{{"path", "Nickel.exe"}, {"os", "windows"}, {"arch", "x64"}, {"unknown", 1}}}}) {
        auto bad = marker; bad[key] = value;
        fails([&] { (void)parse_portable_manifest(bad.dump()); }, "invalid portable marker accepted");
    }
    auto portable = parse_portable_manifest(marker.dump());
    validate_portable_manifest(portable);
    require(portable_manifest_json(portable) == marker, "portable serialization changed schema");
    fails([&] { (void)parse_portable_manifest(std::string(65537, 'x')); }, "oversized marker accepted");
    raw_write(exe / "nickel.portable.json", "{broken");
    fails([&] { (void)resolver.config_root(false); }, "corrupt marker silently changed root");
    std::cout << "PASS paths: whole config root, precedence, exact macOS shape, strict portable layout\n";
}

void test_repository(const fs::path& mock) {
    FixtureRoot root;
    JsonRepository repository(root.path / "repo");
    require(repository.read("settings.json").empty(), "missing settings fallback");
    require(!fs::exists(repository.root()), "missing read created root");
    raw_write(repository.root() / "settings.json", "{broken");
    require(repository.inspect("settings.json", Json::object()).state == JsonReadResult::State::Corrupt, "corrupt settings state");
    require(read_text_bounded(repository.root() / "settings.json", 64) == "{broken", "corrupt read destroyed evidence");
    raw_write(repository.root() / "settings.json", "[]");
    require(repository.read("settings.json").is_object(), "settings wrong shape fallback");
    raw_write(repository.root() / "settings.json", std::string(settings_max_bytes + 1, 'x'));
    require(repository.inspect("settings.json", Json::object()).state == JsonReadResult::State::Oversized, "oversized read fallback");
    raw_write(repository.root() / "settings.json", "{}");
    const auto max = std::numeric_limits<std::uint64_t>::max();
    Json settings{{"largeInteger", max}, {"unknown", Json{{"nested", "preserved"}}}};
    repository.write("settings.json", settings);
    require(repository.read("settings.json") == settings, "unknown fields/uint64 lost");
    fails([&] { repository.write_text("settings.json", "[]"); }, "settings wrong shape write allowed");
    fails([&] { repository.write_text("settings.json", "{broken"); }, "corrupt write allowed");
    fails([&] { repository.write_text("../settings.json", "{}"); }, "path traversal document name allowed");
    require(repository.read("settings.json") == settings, "failed writes destroyed old settings");
    repository.transaction([&] {
        repository.write_text("accounts.json", "[{\"id\":\"x\",\"accessToken\":\"legacy-token\",\"password\":\"v1:synthetic\"}]", accounts_max_bytes);
        repository.write_text("active_account.json", "{\"id\":\"x\"}");
        require(repository.read("accounts.json", Json::array(), accounts_max_bytes).size() == 1, "read your transactional write");
    });
    require(repository.read("active_account.json")["id"] == "x", "two document transaction active not committed");
    require(repository.read("accounts.json", Json::array(), accounts_max_bytes)[0]["accessToken"] == "legacy-token", "legacy tokens transformed");
    fails([&] { repository.transaction([&] { repository.write("settings.json", Json{{"abort", true}}); throw ErrorException(Error{"SyntheticAbort", "abort"}); }); }, "transaction callback abort accepted");
    require(repository.read("settings.json") == settings, "callback abort committed settings");
    Cancellation cancellation;
    fails([&] { repository.transaction([&] { repository.write("settings.json", Json{{"cancel", true}}); cancellation.cancel(); }, &cancellation); }, "cancelled transaction committed");
    require(repository.read("settings.json") == settings, "cancelled transaction changed settings");
    fails([&] { repository.transaction([&] {
        auto value = repository.read("settings.json");
        raw_write(repository.root() / "settings.json", "{\"external\":true}");
        value["ourChange"] = true;
        repository.write("settings.json", value);
    }); }, "external update not detected");
    require(repository.read("settings.json")["external"] == true, "external changes overwritten");
    repository.write("settings.json", Json{{"count", 0}});
    std::vector<OwnedProcess> children;
    for (unsigned i = 0; i < 4; ++i) {
        ProcessOptions options; options.executable = mock;
        options.arguments = {"--increment", path_to_utf8(repository.root()), "12"};
        children.push_back(OwnedProcess::spawn(options));
    }
    for (auto& child : children) { const auto result = child.wait(30s); require(result && result->exit_code == 0, "concurrent repository child failed"); }
    require(repository.read("settings.json")["count"] == 48, "transaction read/modify/write lost concurrent updates");
    require(repository.recover().restored == 0, "ordinary save left pending transaction");
    for (const auto& entry : fs::directory_iterator(repository.root())) {
        const auto name = path_to_utf8(entry.path().filename());
        require(!name.starts_with(".nickel-stage-") && !name.starts_with(".nickel-transaction-") && name.find(".tmp-") == std::string::npos, "successful save has leftovers");
    }
    std::cout << "PASS repository: unknown/u64, corrupt/oversized, atomic multi-save, cancel, external conflict, cross-process locks\n";
}

void test_recovery() {
    FixtureRoot root;
    auto make = [&](const fs::path& directory, std::string_view original, std::string_view next, bool active) {
        fs::create_directories(directory);
        raw_write(directory / "settings.json", original);
        const std::string token(32, 'a');
        const std::string stage = ".nickel-stage-" + token + "-0";
        raw_write(directory / stage, next);
        Json entry{{"name", "settings.json"}, {"stage", stage}, {"originalHash", foundation_detail::sha256_text(original)},
            {"newHash", foundation_detail::sha256_text(next)}, {"originalSize", static_cast<std::uint64_t>(original.size())},
            {"newSize", static_cast<std::uint64_t>(next.size())}, {"limit", static_cast<std::uint64_t>(settings_max_bytes)}};
        Json entries = Json::array({entry});
        if (active) {
            const std::string text = "{\"id\":\"synthetic\"}";
            raw_write(directory / (".nickel-stage-" + token + "-1"), text);
            entries.push_back(Json{{"name", "active_account.json"}, {"stage", ".nickel-stage-" + token + "-1"},
                {"originalHash", nullptr}, {"newHash", foundation_detail::sha256_text(text)}, {"originalSize", std::uint64_t(0)},
                {"newSize", static_cast<std::uint64_t>(text.size())}, {"limit", static_cast<std::uint64_t>(settings_max_bytes)}});
        }
        raw_write(directory / (".nickel-transaction-" + token + ".json"), Json{{"schemaVersion", 1}, {"transactionId", token}, {"entries", entries}}.dump());
    };
    const auto success = root.path / "success";
    make(success, "{\"before\":true}", "{\"after\":true}", true);
    JsonRepository repository(success);
    const auto recovered = repository.recover();
    require(recovered.restored == 2 && !recovered.conflicts && !recovered.rejected, "durable crash journal did not roll forward");
    require(repository.read("settings.json")["after"] == true && repository.read("active_account.json")["id"] == "synthetic", "recovered values wrong");
    const auto conflict = root.path / "conflict";
    make(conflict, "{}", "{\"next\":true}", false);
    raw_write(conflict / "settings.json", "{\"external\":true}");
    const auto report = JsonRepository(conflict).recover();
    require(report.conflicts == 1 && read_text_bounded(conflict / "settings.json", 64) == "{\"external\":true}", "recovery overwrote external conflict");
    fails([&] { JsonRepository(conflict).write("settings.json", Json::object()); }, "pending external conflict silently discarded");
    const auto damaged = root.path / "damaged";
    make(damaged, "{}", "{\"next\":true}", false);
    raw_write(damaged / (".nickel-stage-" + std::string(32, 'a') + "-0"), "corrupt");
    require(JsonRepository(damaged).recover().rejected != 0, "tampered stage recovered");
    require(read_text_bounded(damaged / "settings.json", 64) == "{}", "corrupt stage touched original");
    std::cout << "PASS recovery: synthetic crash, verified rollforward, external conflict, damaged stage\n";
}

void test_process(const fs::path& mock) {
    FixtureRoot root;
    const auto cwd = root.path / path_from_utf8("unicode-\xE9\x9B\xAA space");
    fs::create_directories(cwd);
    std::mutex mutex;
    std::array<bool, 2> eof{};
    std::array<std::size_t, 2> count{};
    ProcessOptions options;
    options.executable = mock; options.cwd = cwd;
    options.arguments = {"--echo", "", "space argument", "quote\"slash\\", "trailing\\", "\xE9\x9B\xAA"};
    options.inherit_environment = false; options.environment["NICKEL_SYNTHETIC"] = "\xE9\x9B\xAA env";
    options.output = [&](ProcessStream stream, std::string_view bytes, bool ended) {
        std::scoped_lock lock(mutex);
        const auto index = stream == ProcessStream::Stdout ? 0U : 1U;
        require(!eof[index], "output delivered after EOF");
        count[index] += bytes.size();
        eof[index] = ended;
    };
    auto process = OwnedProcess::spawn(options);
    const auto result = process.wait(10s);
    require(result && result->exit_code == 23 && result->stdout_eof && result->stderr_eof, "process result/EOF incorrect");
    const auto value = Json::parse(result->stdout_text);
    require(value["arguments"] == options.arguments, "Windows argv quoting changed arguments");
    require(value["cwd"] == path_to_utf8(cwd) && value["env"] == "\xE9\x9B\xAA env", "cwd/environment UTF8 lost");
    require(result->stderr_text == "synthetic-stderr-final" && eof[0] && eof[1], "stderr final line/EOF lost");
    eof = {}; count = {};
    options.arguments = {"--flood"}; options.capture_limit = 128;
    auto flood = OwnedProcess::spawn(options);
    const auto flooded = flood.wait(20s);
    require(flooded && flooded->exit_code == 0 && flooded->stdout_truncated && flooded->stderr_truncated,
        "flood output did not drain with bounded capture");
    require(flooded->stdout_text.size() == 128 && flooded->stderr_text.size() == 128, "capture limit exceeded");
    require(eof[0] && eof[1] && count[0] > 1024 * 1024 && count[1] > 1024 * 1024, "flood EOF or uncaptured pipe bytes lost");
    options.output = {}; options.arguments = {"--sleep", "3000"};
    auto sleeper = OwnedProcess::spawn(options);
    require(!sleeper.wait(5ms), "process timeout killed or finished sleeper");
    require(sleeper.running(), "timed-out child no longer owned/running");
    Cancellation cancellation;
    cancellation.cancel();
    fails([&] { (void)sleeper.wait(10s, &cancellation); }, "preparation cancellation did not fail");
    const auto stopped = sleeper.drain(5s);
    require(stopped && stopped->terminated && stopped->stdout_eof && stopped->stderr_eof, "cancelled child not fully reaped/drained");
    options.arguments = {"--sleep", "120"}; options.purpose = ProcessPurpose::Game;
    auto game = OwnedProcess::spawn(options);
    fails([&] { game.terminate(); }, "game externally terminable");
    fails([&] { (void)game.wait(10s, &cancellation); }, "game cancellation did not notify caller");
    require(game.wait(5s)->exit_code == 0, "game killed by cancellation");
    const auto marker = root.path / "game-survived.txt";
    {
        options.arguments = {"--game-marker", path_to_utf8(marker), "120"};
        auto detached_game = OwnedProcess::spawn(options);
    }
    for (unsigned i = 0; i < 100 && !fs::exists(marker); ++i) std::this_thread::sleep_for(10ms);
    require(fs::exists(marker), "game destructor killed running game");
    JavaDiscoveryOptions discovery;
    discovery.executables = {mock}; discovery.include_environment = false; discovery.include_system_locations = false;
    const auto java = discover_java(discovery);
    require(java.size() == 1 && java[0].major == 8 && java[0].is_64_bit && java[0].vendor == "Synthetic Vendor", "owned Java probe failed");
    require(parse_java_output(mock, "openjdk version \"21.0.5\"")->major == 21, "modern Java output parse failed");
    require(!parse_java_output(mock, "java.version = unknown"), "invalid Java version parsed");
    std::cout << "PASS process: argv UTF8, cwd/env, dual EOF, bounded flood, timeout/cancel ownership, game survival, Java mock\n";
}

void test_atomic_links() {
    FixtureRoot root;
    const auto target = root.path / "settings.json";
    atomic_write_text(target, "old");
    atomic_write_text(target, "new");
    require(read_text_bounded(target, 16) == "new", "atomic overwrite failed");
    fails([&] { create_file_exclusive(target, {}); }, "exclusive create overwrote existing");
    require(read_text_bounded(target, 16) == "new", "exclusive create changed existing");
    Cancellation cancellation; cancellation.cancel();
    fails([&] { atomic_write_text(target, "cancelled", {.cancellation = &cancellation}); }, "cancelled atomic write committed");
    require(read_text_bounded(target, 16) == "new", "cancelled atomic replaced existing");
    fs::create_directories(root.path / "occupied");
    fails([&] { atomic_write_text(root.path / "occupied", "data"); }, "atomic rename over directory succeeded");
    fs::create_directories(root.path / "source"); fs::create_directories(root.path / "destination");
    require(!rename_no_replace(root.path / "source", root.path / "destination"), "directory reservation overwrote existing");
    require(fs::exists(root.path / "source"), "reservation conflict consumed source");
    require(rename_no_replace(root.path / "source", root.path / "fresh"), "directory reservation failed");
    fs::create_directories(root.path / "outside");
    raw_write(root.path / "outside/value", "outside");
#ifdef _WIN32
    // Junctions exercise reparse rejection without requiring symlink privileges.
    const auto junction = root.path / "junction";
    fs::create_directories(junction);
    HANDLE handle = CreateFileW(junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "synthetic junction open failed");
    const auto substitute = L"\\??\\" + (root.path / "outside").native();
    const auto printable = (root.path / "outside").native();
    const std::size_t buffer_bytes = 16 + (substitute.size() + printable.size() + 2) * sizeof(wchar_t);
    std::vector<std::uint8_t> buffer(buffer_bytes);
    auto* tag = reinterpret_cast<DWORD*>(buffer.data()); *tag = IO_REPARSE_TAG_MOUNT_POINT;
    auto* fields = reinterpret_cast<WORD*>(buffer.data() + 4);
    fields[0] = static_cast<WORD>(buffer_bytes - 8); fields[1] = 0;
    fields[2] = 0; fields[3] = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    fields[4] = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t)); fields[5] = static_cast<WORD>(printable.size() * sizeof(wchar_t));
    auto* names = reinterpret_cast<wchar_t*>(buffer.data() + 16);
    std::copy(substitute.begin(), substitute.end(), names);
    std::copy(printable.begin(), printable.end(), names + substitute.size() + 1);
    DWORD returned = 0;
    const auto status = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    require(status != 0, "synthetic junction creation failed");
#else
    const auto junction = root.path / "junction";
    fs::create_directory_symlink(root.path / "outside", junction);
#endif
    fails([&] { reject_path_links(junction / "value"); }, "linked ancestor accepted");
    fails([&] { (void)read_text_bounded(junction / "value", 16); }, "linked file read accepted");
    fails([&] { atomic_write_text(junction / "value", "overwritten"); }, "linked path atomic write accepted");
    require(read_text_bounded(root.path / "outside/value", 16) == "outside", "link check touched external fixture");
    fs::remove(junction);
    for (const auto& entry : fs::directory_iterator(root.path)) require(path_to_utf8(entry.path().filename()).find(".tmp-") == std::string::npos, "atomic temp leftover");
    std::cout << "PASS atomic: durable replacement, exclusive reserve, cancel, failure cleanup, reparse ancestor rejection\n";
}

void test_memory_windows() {
    const auto memory = physical_memory();
    require(memory.total_mb && memory.available_mb && *memory.total_mb >= *memory.available_mb, "physical memory result invalid");
    const auto synthetic = parse_linux_meminfo("MemTotal: 8192000 kB\nMemAvailable: 2048000 kB\n");
    require(synthetic.total_mb == 8000 && synthetic.available_mb == 2000, "meminfo parse incorrect");
    require(!parse_linux_meminfo("MemTotal: garbage kB").total_mb, "invalid memory parsed");
    require(is_game_window(3, 3, "GLFW30", "Minecraft") && !is_game_window(3, 3, "GLFW30", "FML loading"), "game/splash window filter");
    require(!is_game_window(3, 4, "GLFW30", "Minecraft") && !is_game_window(3, 3, "Other", "Minecraft"), "foreign window accepted");
    require(replace_title_placeholders("{date} {time}", "2026/1/1", "00:00:00") == "2026/1/1 00:00:00", "title placeholders");
    fails([&] { open_url("file:///sensitive"); }, "unsafe external URL accepted");
    fails([&] { open_url("https://user:secret@host/"); }, "userinfo link accepted");
    std::cout << "PASS memory/window policy: physical query, invalid meminfo, splash/foreign exclusion, unsafe URL refusal\n";
}
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    try {
        require(argc == 2, "mock executable path required");
        const auto mock = fs::absolute(path_from_utf8(argv[1]));
        test_vault(mock);
        test_paths();
        test_repository(mock);
        test_recovery();
        test_process(mock);
        test_atomic_links();
        test_memory_windows();
        std::cout << "Foundation actual C++20 compatibility tests complete\n";
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL " << error.error().code << ": " << error.what() << " details=" << error.error().details.dump() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
