#include "core/common/json.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/paths.hpp"
#include "core/config/repository.hpp"
#include "core/config/vault.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using nickel::Json;

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class Fn>
void require_failure(Fn&& fn, std::string_view message) {
    bool failed = false;
    try { fn(); } catch (const std::exception&) { failed = true; }
    require(failed, message);
}

std::string read_raw(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "Cannot open synthetic test file");
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_raw(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.is_open(), "Cannot create synthetic test file");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(static_cast<bool>(output), "Cannot finish synthetic test file");
}

Json read_json(const fs::path& path) { return Json::parse(read_raw(path)); }

template<std::size_t N>
std::array<std::uint8_t, N> fixed_hex(std::string_view text) {
    require(text.size() == N * 2, "Bad fixed-size fixture hex");
    std::array<std::uint8_t, N> result{};
    auto nibble = [](char ch) -> unsigned {
        if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<unsigned>(ch - 'a' + 10);
        throw std::runtime_error("Nonhex fixture character");
    };
    for (std::size_t i = 0; i < N; ++i)
        result[i] = static_cast<std::uint8_t>(nibble(text[2 * i]) * 16 + nibble(text[2 * i + 1]));
    return result;
}

class OwnedScratch {
public:
    OwnedScratch() : root_(fs::temp_directory_path() / ("nickel-golden-" + nickel::unique_token())) {
        require(fs::create_directory(root_), "Scratch directory ownership claim failed");
    }
    ~OwnedScratch() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    const fs::path& root() const { return root_; }
private:
    fs::path root_;
};

class Runner {
public:
    void run(std::string name, const std::function<void()>& fn) {
        Json result{{"name", std::move(name)}};
        try {
            fn();
            result["status"] = "passed";
            ++passed_;
        } catch (const std::exception& error) {
            result["status"] = "failed";
            result["error"] = error.what();
            ++failed_;
        }
        results_.push_back(std::move(result));
    }
    int finish() const {
        std::cout << Json{{"schemaVersion", 1}, {"kind", "real-foundation-golden"},
                          {"passed", passed_}, {"failed", failed_}, {"scenarios", results_}}.dump(2) << '\n';
        return failed_ == 0 ? 0 : 1;
    }
private:
    std::size_t passed_ = 0;
    std::size_t failed_ = 0;
    Json results_ = Json::array();
};

std::string expand_root(std::string text, const fs::path& root) {
    const std::string replacement = root.generic_string();
    for (std::size_t at = 0; (at = text.find("$ROOT", at)) != std::string::npos;) {
        text.replace(at, 5, replacement);
        at += replacement.size();
    }
    return text;
}

fs::path below(const fs::path& root, std::string_view relative) {
    const auto path = nickel::path_from_utf8(relative);
    require(!path.is_absolute(), "Fixture path must be relative");
    for (const auto& part : path) require(part != "..", "Fixture path traverses scratch directory");
    return (root / path).lexically_normal();
}

void vault_checks(Runner& runner, const fs::path& fixtures) {
    const Json vectors = read_json(fixtures / "vault.json");
    for (const auto& item : vectors.at("valid")) {
        runner.run("vault/" + item.at("name").get<std::string>(), [item] {
            const auto key = fixed_hex<32>(item.at("keyHex").get<std::string>());
            const auto nonce = fixed_hex<12>(item.at("nonceHex").get<std::string>());
            const std::string plaintext = item.at("plaintext").get<std::string>();
            const std::string sealed = item.at("sealed").get<std::string>();
            require(nickel::Vault::seal_with_nonce(key, nonce, plaintext) == sealed,
                    "Product AES-GCM encoding differs from independent fixture");
            require(nickel::Vault::open_with_key(key, sealed) == plaintext,
                    "Product cannot decrypt independent legacy-format fixture");
        });
    }
    for (const auto& item : vectors.at("invalid")) {
        runner.run("vault/" + item.at("name").get<std::string>(), [item] {
            const auto key = fixed_hex<32>(item.at("keyHex").get<std::string>());
            require_failure([&] { (void)nickel::Vault::open_with_key(key, item.at("sealed").get<std::string>()); },
                            "Product accepted invalid or unauthenticated vault input");
        });
    }
    runner.run("vault/outer-whitespace-legacy", [vectors] {
        const auto& item = vectors.at("valid").at(1);
        const auto key = fixed_hex<32>(item.at("keyHex").get<std::string>());
        require(nickel::Vault::open_with_key(key, " \t" + item.at("sealed").get<std::string>() + "\r\n") ==
                item.at("plaintext").get<std::string>(), "Legacy outer whitespace changed");
    });
    runner.run("vault/missing-key-never-created", [vectors] {
        OwnedScratch scratch;
        const auto root = scratch.root() / "config";
        fs::create_directory(root);
        nickel::Vault vault(root);
        require_failure([&] { (void)vault.open(vectors.at("valid").at(1).at("sealed").get<std::string>()); },
                        "Decrypt unexpectedly succeeded with no key");
        require(fs::is_empty(root), "Failed decryption created or changed key material");
    });
    runner.run("vault/corrupt-key-never-replaced", [vectors] {
        OwnedScratch scratch;
        const auto root = scratch.root() / "config";
        write_raw(root / "credentials.key", "broken-key");
        nickel::Vault vault(root);
        require_failure([&] { (void)vault.open(vectors.at("valid").at(1).at("sealed").get<std::string>()); },
                        "Decrypt unexpectedly succeeded with corrupt key");
        require_failure([&] { (void)vault.seal("synthetic"); }, "Seal replaced an existing corrupt key");
        require(read_raw(root / "credentials.key") == "broken-key", "Corrupt original key was overwritten");
    });
    runner.run("vault/seal-random-nonce-and-raw-key", [] {
        OwnedScratch scratch;
        const auto root = scratch.root() / "config";
        fs::create_directory(root);
        nickel::Vault vault(root);
        const auto first = vault.seal("synthetic repeated value");
        const auto key = read_raw(root / "credentials.key");
        const auto second = vault.seal("synthetic repeated value");
        require(key.size() == 32, "Key file is not raw 32 bytes");
        require(key == read_raw(root / "credentials.key"), "Seal replaced claimed key");
        require(first != second, "Repeated encryption reused its nonce");
        require(vault.open(first) == "synthetic repeated value" && vault.open(second) == "synthetic repeated value",
                "Randomly encrypted values cannot be reopened");
    });
}

void path_checks(Runner& runner, const fs::path& fixtures) {
    const Json vectors = read_json(fixtures / "paths.json");
    for (const auto& item : vectors.at("cases")) {
        runner.run("paths/" + item.at("name").get<std::string>(), [item] {
            OwnedScratch scratch;
            const auto root = scratch.root();
            std::vector<std::pair<fs::path, std::string>> originals;
            for (const auto& dir : item.at("directories")) fs::create_directories(below(root, dir.get<std::string>()));
            for (const auto& [name, value] : item.at("files").items()) {
                const auto path = below(root, name);
                const auto text = expand_root(value.get<std::string>(), root);
                write_raw(path, text);
                originals.emplace_back(path, text);
            }
            const auto executable = below(root, item.at("executable").get<std::string>());
            fs::create_directories(executable.parent_path());
            const nickel::PathContext context{executable, root / "system-config", root / "old-app-data",
                                             item.at("macos").get<bool>()};
            const nickel::PathResolver paths(context);
            if (item.value("expectError", false)) {
                require_failure([&] { (void)paths.config_root(false); }, "Invalid portable marker silently fell back");
                require_failure([&] { (void)paths.default_game_directory(); }, "Invalid marker allowed game-root fallback");
            } else {
                require(paths.config_root(false).lexically_normal() == below(root, item.at("expectedConfig").get<std::string>()),
                        "Configuration root violates whole-root precedence");
                require(paths.default_game_directory().lexically_normal() == below(root, item.at("expectedGame").get<std::string>()),
                        "Game root violates legacy precedence");
                require(paths.bundled_game_directory().lexically_normal() == below(root, item.at("expectedBundledGame").get<std::string>()),
                        "Bundled game root incorrectly follows user configuration");
            }
            for (const auto& [path, original] : originals)
                require(read_raw(path) == original, "Path resolution modified an existing synthetic file");
        });
    }
}

void portable_checks(Runner& runner, const fs::path& fixtures) {
    const Json vectors = read_json(fixtures / "portable.json");
    for (const auto& item : vectors.at("valid")) {
        runner.run("portable/" + item.at("name").get<std::string>(), [item] {
            const auto parsed = nickel::parse_portable_manifest(item.at("manifest").dump());
            require(parsed.schema_version == 1 && parsed.format == "nickel-portable", "Schema 1 format changed");
            require(parsed.data_directory == "Nickel" && parsed.game_directory == ".minecraft" &&
                    parsed.payload == "Nickel/pack.mrpack", "Canonical schema 1 layout changed");
        });
    }
    for (const auto& item : vectors.at("invalid")) {
        runner.run("portable/" + item.at("name").get<std::string>(), [item] {
            require_failure([&] { (void)nickel::parse_portable_manifest(item.at("manifest").dump()); },
                            "Strict schema 1 accepted a malformed or unknown-field manifest");
        });
    }
}

void settings_checks(Runner& runner, const fs::path& fixtures) {
    const Json vectors = read_json(fixtures / "settings.json");
    for (const auto* group : {"valid", "invalid"}) {
        for (const auto& item : vectors.at(group)) {
            runner.run("settings/" + item.at("name").get<std::string>(), [item] {
                OwnedScratch scratch;
                const auto root = scratch.root() / "config";
                const auto text = item.at("text").get<std::string>();
                write_raw(root / "settings.json", text);
                nickel::JsonRepository repository(root);
                require(repository.read("settings.json", Json::object(), nickel::settings_max_bytes) == item.at("expected"),
                        "Settings object/size/corrupt fallback differs from legacy contract");
                require(read_raw(root / "settings.json") == text, "Settings read silently repaired an original file");
            });
        }
    }
    runner.run("settings/update-preserves-unknown-and-integers", [vectors] {
        OwnedScratch scratch;
        const auto root = scratch.root() / "config";
        const auto item = vectors.at("valid").at(1);
        write_raw(root / "settings.json", item.at("text").get<std::string>());
        nickel::JsonRepository repository(root);
        repository.update("settings.json", Json::object(), [](Json& value) { value["accent"] = "blue"; });
        auto expected = item.at("expected");
        expected["accent"] = "blue";
        require(read_json(root / "settings.json") == expected, "Update lost unknown fields or 64-bit integer precision");
    });
    runner.run("settings/invalid-write-leaves-original", [] {
        OwnedScratch scratch;
        const auto root = scratch.root() / "config";
        const std::string original = "{\"accent\":\"nickel\",\"unknown\":9007199254740993}";
        write_raw(root / "settings.json", original);
        nickel::JsonRepository repository(root);
        require_failure([&] { repository.write_text("settings.json", "[]"); }, "Settings accepted non-object write");
        require_failure([&] { repository.write_text("settings.json", "{broken"); }, "Settings accepted invalid JSON write");
        require_failure([&] { repository.write_text("settings.json", std::string(256 * 1024 + 1, ' ')); },
                        "Settings accepted an oversized write");
        require(read_raw(root / "settings.json") == original, "Failed settings write changed original bytes");
    });
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Expected exact generated-fixture directory argument");
        const auto fixtures = nickel::path_from_utf8(argv[1]);
        require(fs::is_directory(fixtures), "Fixture directory is absent");
        Runner runner;
        vault_checks(runner, fixtures);
        path_checks(runner, fixtures);
        portable_checks(runner, fixtures);
        settings_checks(runner, fixtures);
        return runner.finish();
    } catch (const std::exception& error) {
        std::cerr << "Golden acceptance could not run: " << error.what() << '\n';
        return 2;
    }
}
