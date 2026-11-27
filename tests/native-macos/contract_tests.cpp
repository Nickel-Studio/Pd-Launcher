#if !defined(__APPLE__)
#error "This probe must compile and execute on a real Apple host."
#endif

#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/paths.hpp"
#include "core/config/repository.hpp"
#include "core/config/vault.hpp"
#include "platform/process.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace nickel;
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<typename Function>
void must_fail(Function&& function, std::string_view expected = {}) {
    try {
        function();
    } catch (const ErrorException& error) {
        require(expected.empty() || error.error().code == expected, "Unexpected failure classification");
        return;
    }
    throw std::runtime_error("A restricted operation unexpectedly succeeded");
}

struct Fixture {
    fs::path root = fs::current_path() / ("macos-delivery-owned-" + unique_token());
    Fixture() {
        fs::create_directories(root);
        require(::chmod(root.c_str(), 0700) == 0, "Private fixture root chmod failed");
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

void write_fixture(const fs::path& path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(bool(output), "Owned synthetic fixture write failed");
}

Json paths_and_legacy(const fs::path& root) {
    const auto parent = root / path_from_utf8("unicode-\xe9\x95\x8d application space");
    const auto executable = parent / "Nickel.app/Contents/MacOS/Nickel";
    write_fixture(executable, "Synthetic path test; not an executable artifact");
    const auto system = root / "synthetic-home/Library/Application Support";
    const auto old_root = system / path_from_utf8(application_identifier);
    PathResolver resolver(PathContext{executable, system, old_root, true});
    require(resolver.executable_directory() == parent, "Data root did not land outside the .app signature boundary");
    require(resolver.config_root(false) == parent / "Nickel", "Empty root selection changed legacy adjacent policy");
    require(!fs::exists(parent / "Nickel"), "Read-only root selection created user data");
    require(directory_for_executable(parent / "faux.app/not-Contents/MacOS/Nickel", true) ==
                parent / "faux.app/not-Contents/MacOS", "Faux bundle path matched a substring");
    require(directory_for_executable(parent / "faux-dir/Contents/MacOS/Nickel", true) ==
                parent / "faux-dir/Contents/MacOS", "Non-app ancestor treated as a bundle");
    write_fixture(old_root / "settings.json", "{corrupt synthetic legacy settings");
    require(resolver.config_root(false) == old_root, "Corrupt prior data silently switched or merged roots");
    require(!fs::exists(parent / "Nickel"), "Prior root was copied into another namespace");
    JsonRepository repository(old_root);
    require(repository.read("settings.json", Json::object()).is_object(), "Legacy corrupt settings fallback differs");
    const Json state{{"theme", "custom"}, {"unknownFuture", {{"counter", 9007199254740993ULL}}},
                     {"gameDir", path_to_utf8(root / "old-game")}};
    repository.write("settings.json", state);
    const auto reread = repository.read("settings.json");
    require(reread == state && reread["unknownFuture"]["counter"].get<std::uint64_t>() == 9007199254740993ULL,
            "Legacy state or integer precision lost on canonical repository roundtrip");
    write_fixture(old_root / "accounts.json", "[]");
    write_fixture(old_root / "onboarding.json", "{\"accepted\":false,\"version\":\"0.1.0\"}");
    const auto before = read_text_bounded(old_root / "onboarding.json", 1024);
    (void)resolver.config_root(false);
    require(read_text_bounded(old_root / "onboarding.json", 1024) == before, "Path inspection consumed the first-run/version record");
    write_fixture(parent / "nickel.portable.json", "{invalid marker");
    must_fail([&] { (void)resolver.config_root(false); });
    require(!fs::exists(executable.parent_path() / "Nickel"), "Data was written inside Contents/MacOS");
    return Json{{"status", "executed"}, {"outsideRealBundleStructure", true},
                {"wholeLegacyRootAndUnknownFields", true}, {"legacySchemaRoundtrip", true},
                {"fullVersionMigrationExecuted", false}, {"firstRunDispatchExecuted", false}};
}

Json vault_permissions(const fs::path& root, const fs::path& self) {
    VaultKey zero_key{};
    VaultNonce zero_nonce{};
    const std::string nist_empty = "v1:AAAAAAAAAAAAAAAAUw+K+8dFNrmpY7TxxMtziw==";
    require(Vault::seal_with_nonce(zero_key, zero_nonce, "") == nist_empty, "AES-256-GCM NIST empty vector differs");
    require(Vault::open_with_key(zero_key, nist_empty).empty(), "Compatible v1 empty envelope cannot be opened");
    Vault missing(root / "missing-key");
    must_fail([&] { (void)missing.open(nist_empty); });
    require(!fs::exists(root / "missing-key"), "Decryption generated a missing key or root");
    Vault vault(root / "vault");
    const auto sealed = vault.seal("synthetic-only fixture secret");
    require(vault.open(sealed) == "synthetic-only fixture secret", "Canonical Vault roundtrip failed");
    const auto original = read_file_bounded(vault.key_path(), 32);
    require(original.size() == 32, "Key byte length is not 32");
    require(vault.seal("synthetic-only fixture secret") != sealed, "Vault nonce was reused");
    require(read_file_bounded(vault.key_path(), 32) == original, "Existing key was changed");
    struct stat metadata{};
    require(::lstat(vault.key_path().c_str(), &metadata) == 0, "Synthetic key metadata unavailable");
    require(S_ISREG(metadata.st_mode) && (metadata.st_mode & 0777) == 0600 && metadata.st_uid == ::geteuid(),
            "macOS Vault key is linked/non-private or not owned by the fixture uid");
    auto wrong = vault.load_key();
    wrong[0] ^= 1;
    must_fail([&] { (void)Vault::open_with_key(wrong, sealed); });
    must_fail([&] { (void)vault.open("v2:AAAA"); });
    auto damaged = sealed;
    damaged[damaged.size() - 4] = damaged[damaged.size() - 4] == 'A' ? 'B' : 'A';
    must_fail([&] { (void)vault.open(damaged); });
    const auto concurrent_root = root / "vault-eight-process-race";
    std::vector<OwnedProcess> children;
    for (int index = 0; index < 8; ++index) {
        ProcessOptions options;
        options.executable = self;
        options.arguments = {"--child-vault", path_to_utf8(concurrent_root)};
        options.capture_limit = 4096;
        children.push_back(OwnedProcess::spawn(options));
    }
    for (auto& child : children) {
        const auto result = child.wait(10s);
        require(result && result->exit_code == 0 && result->stdout_eof && result->stderr_eof,
                "Owned synthetic Vault creator did not exit/drain");
        require(Vault(concurrent_root).open(result->stdout_text) == "synthetic-race",
                "Concurrent key creators published incompatible keys");
    }
    return Json{{"status", "executed"}, {"keyMode", "0600"}, {"keyUid", metadata.st_uid},
                {"regularNonSymlink", true}, {"eightProcessExclusiveCreation", true},
                {"nistV1Compatibility", true}, {"noKeyOnDecrypt", true},
                {"extendedAclAndUserMigrationExecuted", false}};
}

Json process_lifetime(const fs::path& root, const fs::path& self) {
    ProcessOptions options;
    options.executable = self;
    options.purpose = ProcessPurpose::Game;
    options.arguments = {"--child-marker", path_to_utf8(root / "game-destructor-survived"), "150"};
    {
        auto game = OwnedProcess::spawn(options);
        require(game.purpose() == ProcessPurpose::Game, "Owned game purpose lost");
    }
    for (int index = 0; index < 300 && !fs::exists(root / "game-destructor-survived"); ++index) {
        std::this_thread::sleep_for(10ms);
    }
    require(fs::exists(root / "game-destructor-survived"), "Launcher handle destruction killed the owned game");
    options.arguments = {"--child-marker", path_to_utf8(root / "game-cancel-survived"), "150"};
    auto game = OwnedProcess::spawn(options);
    Cancellation cancellation;
    cancellation.cancel();
    must_fail([&] { (void)game.wait(10ms, &cancellation); }, "Cancelled");
    require(game.running(), "Preparation cancellation killed an owned game");
    must_fail([&] { game.terminate(); }, "PermissionDenied");
    const auto result = game.wait(5s);
    require(result && result->exit_code == 0 && result->stdout_eof && result->stderr_eof,
            "Surviving game did not complete with dual EOF");
    options.arguments = {"--child-sleep", "5000"};
    std::array<std::atomic<int>, 2> eof{};
    options.output = [&](ProcessStream stream, std::string_view, bool ended) {
        if (ended) ++eof[stream == ProcessStream::Stdout ? 0 : 1];
    };
    auto stopped_game = OwnedProcess::spawn(options);
    stopped_game.stop_owned_game();
    const auto drained = stopped_game.drain(5s);
    require(drained && drained->terminated && drained->stdout_eof && drained->stderr_eof && eof[0] == 1 && eof[1] == 1,
            "Explicit game-only stop failed dual EOF or one-time callbacks");
    options.output = {};
    options.purpose = ProcessPurpose::Service;
    options.arguments = {"--child-marker", path_to_utf8(root / "unrelated-owned-sentinel-survived"), "180"};
    auto sentinel = OwnedProcess::spawn(options);
    options.purpose = ProcessPurpose::Preparation;
    options.arguments = {"--child-sleep", "5000"};
    auto preparation = OwnedProcess::spawn(options);
    must_fail([&] { (void)preparation.wait(10ms, &cancellation); }, "Cancelled");
    const auto cancelled = preparation.drain(5s);
    require(cancelled && cancelled->terminated && cancelled->stdout_eof && cancelled->stderr_eof,
            "Preparation cancellation failed owned-group termination/drain");
    const auto survivor = sentinel.wait(5s);
    require(survivor && survivor->exit_code == 0 && fs::exists(root / "unrelated-owned-sentinel-survived"),
            "Preparation cancel reached another owned purpose/process");
    return Json{{"status", "executed"}, {"gameSurvivesDestructorAndPrepareCancel", true},
                {"explicitOwnedGameStopAndDualEof", true}, {"otherProcessSurvives", true},
                {"actualMinecraftOrFirstRunExecuted", false}};
}

int child(int argc, char** argv) {
    const std::string_view flag(argv[1]);
    if (flag == "--child-vault" && argc == 3) {
        std::cout << Vault(path_from_utf8(argv[2])).seal("synthetic-race");
        return 0;
    }
    if (flag == "--child-sleep" && argc == 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(argv[2])));
        return 0;
    }
    if (flag == "--child-marker" && argc == 4) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(argv[3])));
        write_fixture(path_from_utf8(argv[2]), "owned child completed");
        std::cout << "synthetic stdout final\n";
        std::cerr << "synthetic stderr final\n";
        return 0;
    }
    throw std::runtime_error("Unknown owned child fixture command");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1) return child(argc, argv);
        Fixture fixture;
        const auto self = current_executable();
        Json evidence{{"schema", 1}, {"kind", "real-apple-synthetic-production-foundation-probe"},
                      {"executedOnApple", true}, {"productDelivered", false},
                      {"pathsAndLegacy", paths_and_legacy(fixture.root)},
                      {"vault", vault_permissions(fixture.root, self)},
                      {"gameProcess", process_lifetime(fixture.root, self)}};
        std::cout << evidence.dump(2) << '\n';
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL macOS synthetic contract probe: " << error.error().code << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL macOS synthetic contract probe: " << error.what() << '\n';
    }
    return 1;
}
