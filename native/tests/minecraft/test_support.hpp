#pragma once
#include "launcher.hpp"
#include "runtime.hpp"
#include "maven.hpp"
#include "filesystem.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

namespace test {
using namespace nickel::minecraft;
using nickel::Cancellation;
using nickel::Error;
using nickel::ErrorException;
using nickel::Json;
namespace fs = std::filesystem;

inline void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
template<class Function>
inline void rejects(Function function, std::string_view code = {}) {
    try { function(); }
    catch (const ErrorException& error) {
        if (!code.empty() && error.error().code != code)
            throw std::runtime_error("Unexpected typed error code: expected " + std::string(code) + ", got " + error.error().code);
        return;
    }
    catch (const std::exception&) { if (code.empty()) return; throw; }
    throw std::runtime_error("Operation should have failed");
}
inline void put(const fs::path& path, std::string_view value) {
    fs::create_directories(filesystem_path(path.parent_path()));
    std::ofstream file(filesystem_path(path), std::ios::binary | std::ios::trunc);
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    require(static_cast<bool>(file), "Test fixture write failed");
}
inline std::string get(const fs::path& path) {
    std::ifstream file(filesystem_path(path), std::ios::binary);
    require(static_cast<bool>(file), "Test fixture read failed");
    return {std::istreambuf_iterator<char>(file), {}};
}
struct Tree {
    fs::path root;
    explicit Tree(std::string_view name) {
        static std::atomic<unsigned> sequence{0};
        require(fs::space(filesystem_path(fs::current_path())).available >= 64ULL * 1024 * 1024,
                "Insufficient controlled fixture space; no fixture write was attempted");
        root = fs::absolute(fs::path(".fixtures-lifecycle") / (std::string(name) + '-' + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + '-' + std::to_string(sequence++)));
        fs::create_directories(filesystem_path(root));
    }
    ~Tree() { try { std::error_code error; fs::remove_all(filesystem_path(root), error); } catch (...) {} }
};
inline JavaCandidate java(const fs::path& path, unsigned major = 17) { return {path, std::to_string(major) + ".0.1", "controlled mock", "amd64", major, 64, true}; }
inline Json profile(std::string id = "1.20.1") {
    return Json{{"id", id}, {"mainClass", "net.minecraft.client.main.Main"}, {"libraries", Json::array()},
                {"downloads", {{"client", {{"url", "http://127.0.0.1/client.jar"}}}}},
                {"javaVersion", {{"majorVersion", 17}}},
                {"arguments", {{"game", Json::array({"--username", "${auth_player_name}", "--uuid", "${auth_uuid}", "--accessToken", "${auth_access_token}"})}}}};
}
inline InstallRequest install_request(const Tree& tree) {
    InstallRequest request;
    request.root = tree.root / "managed"; request.gameDirectory = tree.root / "game";
    request.versionId = "1.20.1"; request.instanceName = "test-pack";
    request.profile = profile(); request.javaCandidates = {java(tree.root / "mock-java.exe")};
    request.platform = {"windows", "amd64", "10.0.19045", {}}; request.installJava = false;
    return request;
}
inline LaunchRequest launch_request(const Tree& tree) {
    LaunchRequest request;
    request.launchId = "test-launch";
    request.version.root = tree.root / "managed"; request.version.gameDirectory = tree.root / "game";
    request.version.versionId = "1.20.1"; request.version.minecraftVersion = "1.20.1";
    request.version.profile = profile(); request.java = java(tree.root / "mock-java.exe");
    request.identity = {"offline", "Controlled Player", "01234567890123456789012345678901", "CONTROLLED_SECRET"};
    request.platform = {"windows", "amd64", "10.0.19045", {}};
    put(request.java->path, "mock executable");
    fs::create_directories(filesystem_path(request.version.gameDirectory));
    return request;
}
struct Child final : OwnedProcess {
    static inline std::atomic<unsigned> next{100};
    const unsigned id = next++;
    std::atomic<bool> alive{true}, terminated{false}, drained{false};
    int code = 0;
    OutputCallback output;
    std::function<void(const CancelCheck&)> onWait;
    std::uint64_t pid() const noexcept override { return id; }
    bool running() const override { return alive; }
    int wait(const CancelCheck& check, bool kill, std::chrono::milliseconds) override {
        try { if (check) check(); if (onWait) onWait(check); }
        catch (...) { if (kill) terminate_owned(); drained = true; throw; }
        if (output) { output(false, "stdout-tail"); output(true, "stderr-tail"); }
        alive = false; drained = true; return code;
    }
    void terminate_owned() override { terminated = true; alive = false; }
    bool drain_owned(std::chrono::milliseconds) override { drained = !alive; return drained; }
};
inline Services synthetic_services() {
    Services services;
    services.cancellationCheck = [](const Cancellation& cancel) { cancel.check(); };
    services.verify = [](const fs::path& path, const FileHashes& hashes, std::optional<std::uint64_t> size, const CancelCheck& check) {
        check();
        const auto native = filesystem_path(path);
        if (!fs::is_regular_file(native) || (size && fs::file_size(native) != *size)) return false;
        // This tier tests orchestration only; production SHA verification has separate tests.
        if (hashes.sha1 == std::string(40, 'a')) return get(path) == "processor-result";
        if (hashes.sha1 == std::string(40, 'b')) return get(path) == "wrong-result";
        return true;
    };
    services.atomicWrite = [](const fs::path& path, std::string_view bytes, const CancelCheck& check) { check(); put(path, bytes); check(); };
    services.download = [](const DownloadFile& file, const CancelCheck& check, const ProgressCallback&) { check(); put(file.destination, "downloaded-controlled-bytes"); };
    services.spawn = [](const ProcessSpec&, const OutputCallback& output) {
        auto child = std::make_shared<Child>(); child->output = output;
        if (output) output(true, "java.version = 17.0.1\njava.vendor = controlled mock\nos.arch = amd64\nsun.arch.data.model = 64\n");
        return child;
    };
    services.archiveRead = [](const fs::path&, std::string_view name, std::uint64_t, const CancelCheck& check) {
        check();
        if (name == "META-INF/MANIFEST.MF") return std::string("Manifest-Version: 1.0\nMain-Class: example.Processor\n\n");
        return std::string("\xCA\xFE\xBA\xBE\0\0\0\x34", 8);
    };
    return services;
}
}
