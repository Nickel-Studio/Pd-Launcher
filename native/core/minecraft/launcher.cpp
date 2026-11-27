#include "launcher.hpp"
#include "maven.hpp"
#include "runtime.hpp"
#include "filesystem.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <utility>

namespace nickel::minecraft {
namespace {

void emit(const LaunchEventCallback& events, const LaunchEvent& event) noexcept {
    if (events) { try { events(event); } catch (...) {} }
}

std::string text(const Json& object, std::string_view name, std::string fallback = {}) {
    if (!object.is_object()) return fallback;
    const auto found = object.find(std::string(name));
    if (found == object.end() || found->is_null()) return fallback;
    if (!found->is_string()) throw ErrorException(Error{"minecraft.metadata", "Invalid launch metadata string"});
    return found->get<std::string>();
}

std::string local_text(const std::filesystem::path& path, std::uint64_t limit, const CancelCheck& check) {
    reject_links(path); check();
    const auto native = filesystem_path(path);
    if (!std::filesystem::is_regular_file(native) || std::filesystem::file_size(native) > limit)
        throw ErrorException(Error{"minecraft.file", "Launch metadata file is missing or exceeds its quota"});
    std::ifstream input(native, std::ios::binary);
    std::string result;
    std::array<char, 65536> buffer{};
    while (input) {
        check(); input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        result.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
        if (result.size() > limit) throw ErrorException(Error{"minecraft.limit", "Launch metadata byte quota exceeded"});
    }
    if (!input.eof()) throw ErrorException(Error{"minecraft.io", "Cannot read launch metadata"});
    check(); return result;
}

void validate_launchable(const InstalledVersion& version, const Services& services, const CancelCheck& check) {
    check();
    for (const auto& path : {version.root, version.gameDirectory,
                            path_inside(version.root, "versions/" + version.versionId)}) {
        reject_links(path);
        if (std::filesystem::exists(filesystem_path(path / ".nickel-pack.pending")))
            throw ErrorException(Error{"minecraft.pending", "Unpublished or incomplete instances cannot be launched"});
        if (services.validateLaunchable) services.validateLaunchable(path, check);
    }
}

void class_header(const Services& services, const AgentArtifact& agent, unsigned javaMajor, const CancelCheck& check) {
    if (!services.archiveEntries || !services.archiveRead) throw ErrorException(Error{"minecraft.services", "Agent archive verifier is unavailable"});
    const auto entries = services.archiveEntries(agent.file.destination, check);
    if (entries.size() > 100000) throw ErrorException(Error{"minecraft.limit", "Agent archive entry quota exceeded"});
    const auto manifest = services.archiveRead(agent.file.destination, "META-INF/MANIFEST.MF", 64 * 1024, check);
    if (manifest_attribute(manifest, agent.manifestAttribute) != agent.entryClass)
        throw ErrorException(Error{"minecraft.agent", "Fixed runtime agent entry point does not match"});
    auto name = agent.entryClass; std::replace(name.begin(), name.end(), '.', '/'); name += ".class";
    const auto bytes = services.archiveRead(agent.file.destination, name, 1024 * 1024, check);
    if (bytes.size() < 8 || static_cast<unsigned char>(bytes[0]) != 0xca || static_cast<unsigned char>(bytes[1]) != 0xfe ||
        static_cast<unsigned char>(bytes[2]) != 0xba || static_cast<unsigned char>(bytes[3]) != 0xbe)
        throw ErrorException(Error{"minecraft.agent", "Fixed runtime agent class is corrupt"});
    const auto major = (static_cast<unsigned>(static_cast<unsigned char>(bytes[6])) << 8) | static_cast<unsigned char>(bytes[7]);
    if (major < 45 || major > javaMajor + 44) throw ErrorException(Error{"minecraft.agent", "Runtime agent class is incompatible with selected Java"});
}

struct OutputRelay {
    struct Stream { std::string line; bool discarded = false; };
    std::mutex mutex;
    std::array<Stream, 2> streams;
    std::string launchId;
    std::vector<std::string> secrets;
    LaunchEventCallback events;
    bool complete = false;

    void publish(bool error, Stream& stream) {
        auto content = stream.discarded ? std::string("[discarded oversized process output]") : redact_output(stream.line, secrets);
        if (!content.empty() && content.back() == '\r') content.pop_back();
        constexpr std::size_t chunk = 8192;
        if (content.empty()) emit(events, {launchId, error ? "stderr" : "stdout", {}});
        for (std::size_t offset = 0; offset < content.size(); offset += chunk)
            emit(events, {launchId, error ? "stderr" : "stdout", content.substr(offset, chunk)});
        stream.line.clear(); stream.discarded = false;
    }
    void append(bool error, std::string_view bytes) noexcept {
        try {
            std::lock_guard lock(mutex);
            if (complete) return;
            auto& stream = streams[error ? 1 : 0];
            for (const auto c : bytes) {
                if (c == '\n') publish(error, stream);
                else if (!stream.discarded) {
                    if (stream.line.size() == 128 * 1024) { stream.line.clear(); stream.discarded = true; }
                    else stream.line.push_back(c);
                }
            }
        } catch (...) {}
    }
    void drain() noexcept {
        try {
            std::lock_guard lock(mutex);
            if (complete) return;
            for (std::size_t index = 0; index < streams.size(); ++index) {
                auto& stream = streams[index];
                if (!stream.line.empty() || stream.discarded) publish(index == 1, stream);
                emit(events, {launchId, index == 1 ? "stderr-eof" : "stdout-eof", {}});
            }
            complete = true;
        } catch (...) {}
    }
};

void before_launch(const LaunchRequest& request, const Services& services, const CancelCheck& check) {
    if (request.settings.preLaunchCommand.empty()) return;
    if (request.settings.preLaunchCommand.size() > 64 * 1024 || request.settings.preLaunchCommand.find('\0') != std::string::npos)
        throw ErrorException(Error{"minecraft.command", "User pre-launch command exceeds its limit or contains NUL"});
    if (!services.spawn) throw ErrorException(Error{"minecraft.services", "Pre-launch process adapter is unavailable"});
    ProcessSpec spec;
    spec.workingDirectory = request.version.gameDirectory;
    spec.terminateOnDrop = true; spec.purpose = ProcessKind::Preparation;
    if (request.platform.os == "windows") {
        const auto root = std::getenv("SystemRoot");
        if (!root) throw ErrorException(Error{"minecraft.command", "Windows system shell is unavailable"});
        spec.executable = std::filesystem::u8path(root) / "System32/cmd.exe";
        spec.windowsShellCommand = request.settings.preLaunchCommand;
    } else { spec.executable = "/bin/sh"; spec.arguments = {"-c", request.settings.preLaunchCommand}; }
    check();
    auto child = services.spawn(spec, {});
    if (!child || !child->pid()) throw ErrorException(Error{"minecraft.command", "Pre-launch command did not create an owned process"});
    try {
        const auto code = child->wait(check, true, request.settings.preLaunchTimeout);
        if (!child->drain_owned(std::chrono::seconds(10))) throw ErrorException(Error{"minecraft.process.drain", "Pre-launch process streams did not drain"});
        if (code != 0) throw ErrorException(Error{"minecraft.command.exit", "User pre-launch command failed", {{"exitCode", code}}});
        check();
    } catch (...) {
        const auto error = std::current_exception();
        try { child->terminate_owned(); (void)child->drain_owned(std::chrono::seconds(10)); } catch (...) {}
        std::rethrow_exception(error);
    }
}

}

struct Launcher::Preparation {
    Cancellation cancellation;
    bool handedToGame = false;
};

GameSession::GameSession(std::string id, std::shared_ptr<OwnedProcess> process, LaunchEventCallback events,
                         std::function<void()> outputDrained)
    : GameSession(std::move(id), std::move(events), std::move(outputDrained)) {
    if (!process || !process->pid()) throw ErrorException(Error{"minecraft.session", "Game session has no owned process"});
    handoff(std::move(process));
}

GameSession::GameSession(std::string id, LaunchEventCallback events, std::function<void()> outputDrained)
    : launchId_(std::move(id)), events_(std::move(events)), outputDrained_(std::move(outputDrained)) {
    validate_launch_id(launchId_);
}

void GameSession::handoff(std::shared_ptr<OwnedProcess> process) noexcept { process_ = std::move(process); }
const std::string& GameSession::launch_id() const noexcept { return launchId_; }
std::uint64_t GameSession::pid() const noexcept { return process_ ? process_->pid() : 0; }
const nickel::OwnedProcess* GameSession::native_process() const noexcept { return process_ ? process_->native_process() : nullptr; }
bool GameSession::running() const { return process_ && process_->running(); }

int GameSession::wait() {
    for (;;) {
        if (!running()) return wait(std::chrono::seconds(10));
        try { return wait(std::chrono::seconds(1)); }
        catch (const ErrorException& error) {
            if (error.error().code != "minecraft.process.timeout") throw;
        }
    }
}

int GameSession::wait(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0 || timeout == std::chrono::milliseconds::max())
        throw ErrorException(Error{"minecraft.process.timeout", "Game wait timeout must be finite and nonnegative"});
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(waitMutex_, std::defer_lock);
    if (!lock.try_lock_for(timeout))
        throw ErrorException(Error{"minecraft.process.timeout", "Game wait timed out acquiring session ownership"});
    if (exitCode_) return *exitCode_;
    const auto remaining = [&] {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        return std::max(std::chrono::milliseconds(0), timeout - elapsed);
    };
    const auto code = process_->wait({}, false, remaining());
    if (!process_->drain_owned(remaining()))
        throw ErrorException(Error{"minecraft.process.drain", "Game exited without stdout/stderr EOF"});
    if (outputDrained_) outputDrained_();
    exitCode_ = code;
    emit(events_, {launchId_, "exited", {}, pid(), code});
    return code;
}

void GameSession::stop_owned() { process_->terminate_owned(); }

Launcher::Launcher(Services services) : services_(std::move(services)) {
    if (!services_.spawn || !services_.verify) throw ErrorException(Error{"minecraft.services", "Launcher process/hash adapters are unavailable"});
}

std::shared_ptr<Launcher::Preparation> Launcher::begin(std::string_view id) {
    validate_launch_id(id);
    std::lock_guard lock(mutex_);
    if (preparations_.contains(id) || sessions_.contains(id)) throw ErrorException(Error{"minecraft.launchId", "launchId was already used by this launcher"});
    auto preparation = std::make_shared<Preparation>();
    preparations_.emplace(std::string(id), preparation);
    return preparation;
}

void Launcher::finish(std::string_view id, const std::shared_ptr<Preparation>& preparation) {
    std::lock_guard lock(mutex_);
    const auto found = preparations_.find(id);
    if (found != preparations_.end() && found->second == preparation) preparations_.erase(found);
}

LaunchPlan Launcher::prepare_checked(LaunchRequest request, const Services& services, const CancelCheck& check,
                                     const LaunchEventCallback& events) {
    check(); validate_version_id(request.version.versionId);
    validate_launchable(request.version, services, check);
    emit(events, {request.launchId, "preparing", "Preparing Minecraft files and Java"});
    if (request.version.profile.contains("inheritsFrom")) {
        const auto resolved = resolve_version(request.version.versionId, request.version.root, [&](std::string_view id) {
            if (id == request.version.versionId) return request.version.profile;
            return parse_metadata(local_text(path_inside(request.version.root, "versions/" + std::string(id) + '/' + std::string(id) + ".json"), 16 * 1024 * 1024, check));
        });
        request.version.profile = resolved.metadata;
        request.version.minecraftVersion = resolved.baseVersionId;
    }
    const auto mc = request.version.minecraftVersion.empty() ? request.version.versionId : request.version.minecraftVersion;
    std::optional<unsigned> metadataMajor;
    if (request.version.profile.contains("javaVersion") && request.version.profile["javaVersion"].contains("majorVersion")) metadataMajor = request.version.profile["javaVersion"]["majorVersion"].get<unsigned>();
    const auto requirement = loader_java_requirement(mc, request.version.loaders, text(request.version.profile, "releaseTime"), metadataMajor);
    auto candidates = request.javaCandidates;
    std::optional<std::filesystem::path> selected;
    if (request.java) selected = request.java->path;
    else if (request.version.java.path) selected = request.version.java.path;
    else {
        if (candidates.empty() && services.discoverJava) candidates = services.discoverJava(check);
        selected = select_java(requirement, candidates).path;
    }
    if (!selected) throw ErrorException(Error{"minecraft.java", "No compatible Java runtime is installed"});
    request.java = probe_java(services, *selected, check);
    request.version.java = validate_manual_java(requirement, *request.java);
    if (!request.assetIndex.is_object() && request.version.profile.contains("assetIndex")) {
        const auto& descriptor = request.version.profile["assetIndex"];
        const auto id = text(descriptor, "id"); validate_version_id(id);
        auto file = artifact_download(descriptor, path_inside(request.version.root, "assets/indexes/" + id + ".json"), FileKind::Metadata);
        file.maximumBytes = 64 * 1024 * 1024;
        ensure_file(services, file, check);
        request.assetIndex = parse_metadata(local_text(file.destination, file.maximumBytes, check), 64 * 1024 * 1024);
    }
    auto plan = build_launch_plan(request);
    auto platform = request.platform; platform.architecture = request.java->architecture;
    const auto nativeBase = path_inside(request.version.root, "versions/" + request.version.versionId + "/natives");
    const auto libraries = plan_libraries(request.version.profile, request.version.root, platform, nativeBase);
    const auto ensure = [&](const DownloadFile& file) {
        check(); reject_links(file.destination);
        const auto native = filesystem_path(file.destination);
        if (!request.settings.verifyFiles && std::filesystem::is_regular_file(native) && std::filesystem::file_size(native) <= file.maximumBytes) return;
        ensure_file(services, file, check);
    };
    for (const auto& file : request.version.files) ensure(file);
    for (const auto& file : libraries.downloads) ensure(file);
    const auto clientId = text(request.version.profile, "jar", mc);
    if (request.version.profile.contains("downloads") && request.version.profile["downloads"].contains("client"))
        ensure(artifact_download(request.version.profile["downloads"]["client"], path_inside(request.version.root, "versions/" + clientId + '/' + clientId + ".jar"), FileKind::Client));
    if (request.assetIndex.is_object()) {
        const auto assets = plan_assets(request.version.profile, request.assetIndex, request.version.root, request.version.gameDirectory);
        for (const auto& file : assets.downloads) ensure(file);
    }
    if (request.version.profile.contains("logging") && request.version.profile["logging"].contains("client")) {
        const auto& logging = request.version.profile["logging"]["client"];
        if (logging.contains("file")) {
            const auto id = text(logging["file"], "id"); validate_version_id(id);
            ensure(artifact_download(logging["file"], path_inside(request.version.root, "assets/log_configs/" + id), FileKind::Logging));
        }
    }
    for (const auto& native : libraries.natives) {
        check();
        if (!services.extractNatives) throw ErrorException(Error{"minecraft.services", "Native extractor is unavailable"});
        services.extractNatives(native, check);
    }
    for (const auto& copy : plan.assetCopies) copy_managed(services, copy, check);
    for (const auto& agent : plan.agents) { ensure_file(services, agent.file, check); class_header(services, agent, plan.java.major, check); }
    before_launch(request, services, check);
    check(); validate_launchable(request.version, services, check);
    emit(events, {request.launchId, "prepared", "Minecraft preparation completed"});
    return plan;
}

LaunchPlan Launcher::prepare(const LaunchRequest& request, const Cancellation& cancellation, const LaunchEventCallback& events) {
    auto preparation = begin(request.launchId);
    try {
        std::stop_callback callback(cancellation.stop_token(), [preparation] { preparation->cancellation.cancel(); });
        auto services = services_.forOperation ? services_.forOperation(preparation->cancellation) : services_;
        const auto check = cancellation_check(services, preparation->cancellation);
        auto plan = prepare_checked(request, services, check, events);
        finish(request.launchId, preparation); return plan;
    } catch (const ErrorException& error) {
        finish(request.launchId, preparation); emit(events, {request.launchId, error.error().code == "cancelled" ? "cancelled" : "failed", "Minecraft preparation failed"}); throw;
    } catch (const std::exception& error) {
        finish(request.launchId, preparation); emit(events, {request.launchId, "failed", "Minecraft preparation failed"});
        throw ErrorException(Error{"minecraft.prepare", error.what()});
    }
}

std::shared_ptr<GameSession> Launcher::launch(const LaunchRequest& request, const Cancellation& cancellation,
                                            const LaunchEventCallback& events) {
    auto preparation = begin(request.launchId);
    try {
        std::stop_callback callback(cancellation.stop_token(), [preparation] { preparation->cancellation.cancel(); });
        auto services = services_.forOperation ? services_.forOperation(preparation->cancellation) : services_;
        const auto check = cancellation_check(services, preparation->cancellation);
        auto plan = prepare_checked(request, services, check, events);
        auto relay = std::make_shared<OutputRelay>();
        relay->launchId = request.launchId; relay->secrets = plan.secrets; relay->events = events;
        plan.process.purpose = ProcessKind::Game; plan.process.terminateOnDrop = false;
        if (plan.process.highPerformanceGpu) {
            plan.process.gpuPreferenceWarning = [relay](const Error& error) noexcept {
                try {
                    emit(relay->events, {relay->launchId, "warning", redact_output(error.message, relay->secrets)});
                } catch (...) {}
            };
        }
        OutputCallback output = [relay](bool error, std::string_view bytes) { relay->append(error, bytes); };
        auto session = std::shared_ptr<GameSession>(new GameSession(request.launchId, events, [relay] { relay->drain(); }));
        LaunchEvent started{request.launchId, "started", "Owned Minecraft process started"};
        {
            // Allocate and register ownership before spawn; a returned game needs only a noexcept handoff.
            std::lock_guard lock(mutex_);
            const auto registered = sessions_.emplace(request.launchId, session);
            if (!registered.second) throw ErrorException(Error{"minecraft.launchId", "launchId was already used by this launcher"});
            const auto preparing = preparations_.find(request.launchId);
            try {
                check(); preparation->handedToGame = true;
                auto process = services.spawn(plan.process, output);
                if (!process) throw ErrorException(Error{"minecraft.spawn", "Java game spawn returned no owned process"});
                session->handoff(std::move(process));
                if (!session->pid()) throw ErrorException(Error{"minecraft.spawn", "Java game spawn returned no owned process"});
                preparations_.erase(preparing);
            } catch (...) {
                // A real returned process stays queryable even if a custom adapter violates its PID contract.
                if (!session->process_) sessions_.erase(registered.first);
                throw;
            }
        }
        started.pid = session->pid();
        emit(events, started);
        return session;
    } catch (const ErrorException& error) {
        finish(request.launchId, preparation); emit(events, {request.launchId, error.error().code == "cancelled" ? "cancelled" : "failed", "Minecraft launch failed"}); throw;
    } catch (const std::exception& error) {
        finish(request.launchId, preparation); emit(events, {request.launchId, "failed", "Minecraft launch failed"});
        throw ErrorException(Error{"minecraft.launch", error.what()});
    }
}

LaunchPlan Launcher::prepare(const LaunchRequest& request, std::stop_token stop, const LaunchEventCallback& events) {
    Cancellation cancellation;
    std::stop_callback callback(stop, [&] { cancellation.cancel(); });
    return prepare(request, cancellation, events);
}

std::shared_ptr<GameSession> Launcher::launch(const LaunchRequest& request, std::stop_token stop, const LaunchEventCallback& events) {
    Cancellation cancellation;
    std::stop_callback callback(stop, [&] { cancellation.cancel(); });
    return launch(request, cancellation, events);
}

bool Launcher::cancel_prepare(std::string_view id) {
    validate_launch_id(id);
    std::lock_guard lock(mutex_);
    const auto found = preparations_.find(id);
    if (found == preparations_.end() || found->second->handedToGame) return false;
    found->second->cancellation.cancel(); return true;
}

std::shared_ptr<GameSession> Launcher::session(std::string_view id) const {
    validate_launch_id(id);
    std::lock_guard lock(mutex_);
    const auto found = sessions_.find(id);
    return found == sessions_.end() ? nullptr : found->second;
}

}
