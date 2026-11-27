#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"
#include "core/config/paths.hpp"
#include "core/config/repository.hpp"
#include "core/config/vault.hpp"
#include "platform/java.hpp"
#include "platform/process.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace nickel;
using namespace nickel::foundation_detail;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}
std::string failure(const auto& callback) {
    try { callback(); } catch (const ErrorException& error) { return error.error().code; }
    throw std::runtime_error("Restricted long-path operation unexpectedly succeeded");
}

// Independent fixture I/O does not rely on the production path converter.
fs::path raw_native(fs::path path) {
#ifdef _WIN32
    path.make_preferred();
    const auto text = path.native();
    if (text.starts_with(L"\\\\?\\")) return path;
    if (text.starts_with(L"\\\\")) return fs::path(L"\\\\?\\UNC\\" + text.substr(2));
    return fs::path(L"\\\\?\\" + text);
#else
    return path;
#endif
}
void raw_write(const fs::path& path, std::string_view text) {
    fs::create_directories(raw_native(path.parent_path()));
    std::ofstream file(raw_native(path), std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(file), "Independent long-path fixture write failed");
}
std::string raw_read(const fs::path& path) {
    std::ifstream file(raw_native(path), std::ios::binary);
    require(static_cast<bool>(file), "Independent long-path fixture read failed");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
bool raw_exists(const fs::path& path) { return fs::exists(raw_native(path)); }
struct Root {
    fs::path top = fs::current_path() / ("foundation-longpaths-" + unique_token());
    fs::path deep = top;
    Root() {
        unsigned index = 0;
        while (deep.native().size() < 520) {
            deep /= path_from_utf8("segment-" + std::to_string(index++) + "-\xE6\xB5\x8B\xE8\xAF\x95 space-" + std::string(32, 'a'));
        }
        fs::create_directories(raw_native(deep));
    }
    ~Root() { std::error_code error; fs::remove_all(raw_native(top), error); }
};

void test_storage(const fs::path& root) {
    const auto file = root / "atomic text.txt";
    atomic_write_text(file, "first-synthetic");
    require(read_text_bounded(file, 64) == "first-synthetic", "Long-path write/read failed");
    atomic_write_text(file, "second-synthetic");
    require(read_text_bounded(raw_native(file), 64) == "second-synthetic", "Extended input did not read the same file");
    require(!path_missing(file), "Long existing file was reported missing");
    const std::array<std::uint8_t, 3> bytes{1, 2, 3};
    const auto reserve = root / "exclusive.data";
    require(atomic_create_file(reserve, bytes, true), "Long-path no-replace creation failed");
    require(!atomic_create_file(reserve, bytes, true), "Long-path no-replace creation replaced an existing file");
    require(read_file_bounded(reserve, 3) == std::vector<std::uint8_t>(bytes.begin(), bytes.end()), "Reservation bytes changed");
    const auto exclusive = root / "direct.data";
    create_file_exclusive(exclusive, bytes, true);
    require(failure([&] { create_file_exclusive(exclusive, bytes, true); }) == "AlreadyExists", "Long exclusive create ignored winner");
    const auto moved = root / "moved.data";
    require(rename_no_replace(exclusive, moved), "Long-path handle rename failed");
    require(!rename_no_replace(reserve, moved) && raw_exists(reserve), "Long-path rename consumed contender");
    const auto stage = root / "source.stage";
    raw_write(stage, "owned");
    const auto hash = sha256_text("owned");
    require(move_file_if_matches(stage, root / "matching.stage", hash), "Long-path conditional move failed");
    require(!remove_file_if_matches(root / "matching.stage", sha256_text("other")), "Long-path cleanup ignored hash");
    require(remove_file_if_matches(root / "matching.stage", hash), "Long-path handle cleanup failed");
    require(!raw_exists(root / "matching.stage"), "Long-path cleanup left owned content");
    {
        FileLock lock(root / ".lock");
        require(failure([&] { FileLock contender(root / ".lock", nullptr, 20ms); }) == "Busy", "Long-path lock did not exclude contender");
    }
    Cancellation cancelled;
    cancelled.cancel();
    require(failure([&] { atomic_write_text(root / "cancelled.txt", "never", {.cancellation = &cancelled}); }) == "Cancelled", "Long-path cancellation ignored");
    require(!raw_exists(root / "cancelled.txt"), "Cancelled long-path write published");
    std::cout << "PASS long storage: logical/extended read, atomic/create/reserve, same-parent NT rename, hash cleanup, lock/cancel\n";
}

void test_vault_repository(const fs::path& root) {
    Vault missing(root / "missing-vault");
    require(failure([&] { (void)missing.open("v1:AAAA"); }) == "NotFound", "Missing long-path vault opened ciphertext");
    require(!raw_exists(root / "missing-vault"), "Missing long-path open created key/root");
    Vault vault(root / "vault");
    const auto sealed = vault.seal("synthetic password");
    const auto key = raw_read(vault.key_path());
    require(key.size() == 32 && vault.open(sealed) == "synthetic password", "Long vault round trip failed");
    require(vault.load_or_create_key() == vault.load_key() && raw_read(vault.key_path()) == key, "Long-path vault replaced key");
    JsonRepository repository(root / "repository");
    const Json original{{"u64", std::uint64_t(18446744073709551615ULL)}, {"unknown", Json{{"nested", "preserve"}}}};
    repository.write("settings.json", original);
    require(repository.read("settings.json") == original, "Long-path repository changed u64/unknown fields");
    repository.transaction([&] {
        repository.write("accounts.json", Json::array({Json{{"id", "synthetic"}}}), accounts_max_bytes);
        repository.write("active_account.json", Json{{"id", "synthetic"}});
    });
    require(repository.read("accounts.json", Json::array(), accounts_max_bytes)[0]["id"] == "synthetic" &&
        repository.read("active_account.json")["id"] == "synthetic", "Long multi-save incomplete");
    require(failure([&] {
        repository.update("settings.json", Json::object(), [&](Json& value) {
            raw_write(repository.root() / "settings.json", "{\"external\":true}");
            value["local"] = true;
        });
    }) == "Conflict", "Long-path transaction lost external edit");
    require(raw_read(repository.root() / "settings.json") == "{\"external\":true}", "Conflict replaced external long-path config");
    std::cout << "PASS long vault/repository: no-key side effects, existing key, u64/unknown fields, accounts+active, snapshot conflict\n";
}

void test_recovery(const fs::path& root) {
    constexpr std::string_view token = "0123456789abcdef0123456789abcdef";
    const std::string before = "{\"before\":true}";
    const std::string after = "{\"after\":true}";
    for (unsigned boundary = 0; boundary < 4; ++boundary) {
        const auto directory = root / ("recovery-" + std::to_string(boundary));
        const auto target = directory / "settings.json";
        const auto stage = directory / (".nickel-stage-" + std::string(token) + "-0");
        const auto backup = directory / (".nickel-backup-" + std::string(token) + "-0");
        const auto journal = directory / (".nickel-transaction-" + std::string(token) + ".json");
        raw_write(target, before);
        raw_write(stage, after);
        Json value{{"schemaVersion", 1}, {"transactionId", token}, {"entries", Json::array({Json{
            {"name", "settings.json"}, {"stage", path_to_utf8(stage.filename())},
            {"originalHash", sha256_text(before)}, {"newHash", sha256_text(after)},
            {"originalSize", static_cast<std::uint64_t>(before.size())}, {"newSize", static_cast<std::uint64_t>(after.size())},
            {"limit", settings_max_bytes}}})}};
        raw_write(journal, value.dump());
        if (boundary == 1 || boundary == 2) fs::rename(raw_native(target), raw_native(backup));
        if (boundary == 2) raw_write(target, after);
        if (boundary == 3) raw_write(target, "{\"contender\":true}");
        JsonRepository repository(directory);
        if (boundary == 3) {
            require(failure([&] { (void)repository.read("settings.json"); }) == "Conflict", "Deep recovery overwrote external contender");
            require(raw_read(target) == "{\"contender\":true}" && raw_exists(stage) && raw_exists(journal), "Deep conflict consumed evidence");
        } else {
            require(repository.read("settings.json") == Json{{"after", true}}, "Deep read did not recover journal");
            require(!raw_exists(stage) && !raw_exists(backup) && !raw_exists(journal), "Deep recovery left owned artifacts");
        }
    }
    std::cout << "PASS deep journals: original/backup-gap/published recovery gate, scan/cleanup and preserved external conflict\n";
}

void test_roots_and_guards(const fs::path& root) {
    const PathContext context{root / "launcher" / "Nickel.exe", root / "roaming", root / "app-data", false};
    PathResolver resolver(context);
    const auto system = context.system_config_base / path_from_utf8(application_identifier);
    raw_write(system / "cache" / "synthetic.txt", "marker");
    require(resolver.config_root(false) == system, "Long system marker was not selected");
    require(resolver.config_root(true) == system, "Long root creation leaked native prefix");
    raw_write(system / "game.json", "{\"gameDir\":\"synthetic-game-directory\"}");
    require(resolver.default_game_directory() == path_from_utf8("synthetic-game-directory"), "Long-path config read fell back to another root");
    raw_write(root / "launcher" / path_from_utf8(portable_manifest_name),
        Json{{"schemaVersion", 1}, {"format", "nickel-portable"}, {"packageId", "0123456789abcdef0123456789abcdef"},
            {"name", "synthetic"}, {"version", "1"}, {"instanceName", "safe"}, {"payload", "Nickel/pack.mrpack"},
            {"gameDirectory", ".minecraft"}, {"dataDirectory", "Nickel"}}.dump());
    require(resolver.portable_manifest().has_value() && resolver.config_root() == root / "launcher" / "Nickel", "Long portable marker not respected");
    for (const auto suffix : {"../escape", "value.", "value ", "CON.txt", "file:ads"}) {
#ifdef _WIN32
        require(failure([&] { atomic_write_text(root / suffix, "never"); }) == "UnsafePath", "Extended path bypassed Windows component policy");
#else
        if (std::string_view(suffix) == "../escape")
            require(failure([&] { atomic_write_text(root / suffix, "never"); }) == "UnsafePath", "Long path bypassed traversal policy");
#endif
    }
#ifdef _WIN32
    for (const auto suffix : {"../escape", "value.", "value ", "CON.txt", "file:ads"}) {
        require(failure([&] { atomic_write_text(raw_native(root) / suffix, "never"); }) == "UnsafePath", "Caller extended prefix bypassed component policy");
    }
    const auto unc = fs::path(L"\\\\server\\share\\folder\\file");
    require(native_filesystem_path(unc) == fs::path(L"\\\\?\\UNC\\server\\share\\folder\\file"), "UNC conversion did not preserve server/share");
    require(checked_absolute_path(fs::path(L"\\\\?\\UNC\\server\\share\\folder\\file")) == unc, "Extended UNC leaked into logical path");
    for (const auto* unsafe : {L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\file", L"\\\\.\\C:\\file", L"\\??\\C:\\file", L"\\\\?\\Volume{synthetic}\\file", L"\\\\?\\UNC\\server", L"\\\\server\\share.\\file"}) {
        require(failure([&] { (void)checked_absolute_path(fs::path(unsafe)); }) == "UnsafePath", "Device or invalid UNC namespace accepted");
    }
    DirectoryGuard guard(root);
    const auto handle = CreateFileW(raw_native(root).c_str(), GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    require(handle == INVALID_HANDLE_VALUE, "Long-path directory guard allowed mutator");
    guard.verify();
#endif
    std::cout << "PASS long roots/guards: cache marker, game config, portable, unprefixed DTO roots, traversal/device/ADS/UNC and ancestor pinning\n";
}

void test_long_executable(const fs::path& mock, const fs::path& root) {
    const auto executable = root / "copied mock.exe";
    fs::copy_file(raw_native(mock), raw_native(executable));
#ifndef _WIN32
    fs::permissions(executable, fs::status(mock).permissions());
#endif
    ProcessOptions options;
    options.executable = executable;
#ifdef _WIN32
    options.arguments = {"--module-image"};
    auto image_child = OwnedProcess::spawn(options);
    const auto image_result = image_child.wait(5s);
    require(image_result && image_result->exit_code == 0 && image_result->stdout_eof && image_result->stderr_eof,
            "Long image-name probe did not drain correctly");
    const auto image = Json::parse(image_result->stdout_text);
    require(image.value("extended", false) && image.at("nativeUnits").get<std::size_t>() > 500 &&
            fs::equivalent(raw_native(path_from_utf8(image.at("image").get<std::string>())), raw_native(executable)),
            "Genuinely long executable did not retain required native image-path support");
    std::cout << "PASS long executable image: actual GetModuleFileNameW retains necessary extended prefix at "
              << image.at("nativeUnits") << " native units\n";
#endif
    options.arguments = {"--echo", "synthetic long executable"};
    options.environment["NICKEL_SYNTHETIC"] = "long-path-value";
    auto child = OwnedProcess::spawn(options);
    const auto result = child.wait(5s);
    require(result && result->exit_code == 23 && result->stdout_eof && result->stderr_eof &&
        Json::parse(result->stdout_text)["env"] == "long-path-value", "Long executable did not drain/run correctly");
    const auto runtime = probe_java(executable, nullptr, 5s);
    require(runtime.major == 8 && runtime.is_64_bit, "Long executable Java probe failed");
#ifdef _WIN32
    const auto java = root / "jdk" / "bin" / "java.exe";
#else
    const auto java = root / "jdk" / "bin" / "java";
#endif
    fs::create_directories(raw_native(java.parent_path()));
    fs::copy_file(raw_native(mock), raw_native(java));
#ifndef _WIN32
    fs::permissions(java, fs::status(mock).permissions());
#endif
    JavaDiscoveryOptions discovery;
    discovery.include_environment = false;
    discovery.include_system_locations = false;
    discovery.executables = {java, raw_native(java)};
    discovery.roots = {root};
    const auto found = discover_java(discovery);
    require(found.size() == 1 && found.front().major == 8 && found.front().executable == java,
        "Long Java discovery lost, duplicated or prefixed the runtime");
    const auto parent_cwd = fs::current_path();
    options.cwd = root;
#ifdef _WIN32
    require(failure([&] { (void)OwnedProcess::spawn(options); }) == "Unsupported", "Long cwd support was falsely advertised");
    options.cwd = raw_native(root);
    require(failure([&] { (void)OwnedProcess::spawn(options); }) == "Unsupported", "Extended cwd bypassed the documented limit");
#else
    auto cwd_child = OwnedProcess::spawn(options);
    const auto cwd_result = cwd_child.wait(5s);
    require(cwd_result && Json::parse(cwd_result->stdout_text)["cwd"] == path_to_utf8(root), "Long POSIX child cwd was not preserved");
#endif
    require(fs::current_path() == parent_cwd, "Child cwd operation changed process-global cwd");
    std::cout << "PASS long executable: explicit application path, UTF8 argv/environment, both EOFs, Java probe/discovery and honest cwd boundary\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Mock executable path required");
        std::cout << std::unitbuf;
        Root root;
        std::cout << "ACTUAL long root native units=" << root.deep.native().size() << '\n';
        require(root.deep.native().size() > 500, "Long path fixture was shortened");
        test_storage(root.deep);
        test_vault_repository(root.deep);
        test_recovery(root.deep);
        test_roots_and_guards(root.deep);
        test_long_executable(fs::absolute(path_from_utf8(argv[1])), root.deep);
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL " << error.error().code << ": " << error.what() << " details=" << error.error().details.dump() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
    }
    return 1;
}
