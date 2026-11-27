#pragma once

#include "installer.hpp"
#include "memory.hpp"

#include <mutex>

namespace nickel::minecraft {

struct LaunchIdentity {
    std::string type = "offline";
    std::string name;
    std::string uuid;
    std::string accessToken;
    std::string userType = "legacy";
    std::string xuid;
    std::string clientId;
    std::string userProperties = "{}";
    std::string yggdrasilApi;
    std::string metadataUrl;
};

struct LaunchSettings {
    std::uint64_t memoryMb = 4096;
    std::optional<std::uint64_t> initialMemoryMb;
    std::optional<std::uint64_t> physicalMemoryMb;
    std::string memoryManagement = "g1-tuned";
    std::vector<std::string> jvmArguments;
    std::vector<std::string> gameArguments;
    std::string serverAddress;
    std::string customInfo;
    std::string preLaunchCommand;
    std::chrono::milliseconds preLaunchTimeout = std::chrono::seconds(60);
    std::optional<unsigned> width;
    std::optional<unsigned> height;
    std::optional<unsigned> windowsBuild;
    bool fullscreen = false;
    bool demo = false;
    bool disableLwjglAgent = false;
    bool disableWrapper = false;
    bool windowsUtf8 = false;
    bool bypassOffline = false;
    bool verifyFiles = true;
    bool launchable = true;
    bool highPerformanceGpu = false;
};

struct LaunchRequest {
    std::string launchId;
    InstalledVersion version;
    LaunchIdentity identity;
    LaunchSettings settings;
    Platform platform = Platform::current();
    std::optional<JavaCandidate> java;
    std::vector<JavaCandidate> javaCandidates;
    Json assetIndex;
};

struct AgentArtifact {
    DownloadFile file;
    std::string manifestAttribute;
    std::string entryClass;
};

struct LaunchPlan {
    std::string launchId;
    std::string versionId;
    std::string mainClass;
    ProcessSpec process;
    std::vector<std::filesystem::path> classpath;
    std::filesystem::path nativesDirectory;
    std::vector<AgentArtifact> agents;
    std::vector<CopyFile> assetCopies;
    std::vector<std::string> secrets;
    JavaSelection java;
    std::vector<std::string> redacted_arguments() const;
};

struct LaunchEvent {
    std::string launchId;
    std::string kind;
    std::string message;
    std::uint64_t pid = 0;
    std::optional<int> exitCode;
};

using LaunchEventCallback = std::function<void(const LaunchEvent&)>;

class GameSession {
public:
    GameSession(std::string launchId, std::shared_ptr<OwnedProcess> process,
                LaunchEventCallback events = {}, std::function<void()> outputDrained = {});
    const std::string& launch_id() const noexcept;
    std::uint64_t pid() const noexcept;
    // Borrowed from this session; callers must retain its shared lifetime and verify live ownership.
    const nickel::OwnedProcess* native_process() const noexcept;
    bool running() const;
    int wait();
    // One finite budget for exit and both output streams; timeout never stops the game.
    int wait(std::chrono::milliseconds timeout);
    void stop_owned();
private:
    friend class Launcher;
    GameSession(std::string launchId, LaunchEventCallback events, std::function<void()> outputDrained);
    void handoff(std::shared_ptr<OwnedProcess> process) noexcept;
    std::string launchId_;
    std::shared_ptr<OwnedProcess> process_;
    LaunchEventCallback events_;
    std::function<void()> outputDrained_;
    std::timed_mutex waitMutex_;
    std::optional<int> exitCode_;
};

LaunchPlan build_launch_plan(const LaunchRequest& request);
std::vector<std::string> split_arguments(std::string_view input);
std::string manifest_attribute(std::string_view manifest, std::string_view key);
void validate_launch_id(std::string_view launchId);
std::string redact_output(std::string_view output, std::span<const std::string> secrets);
Json launch_plan_json(const LaunchPlan& plan);
LaunchSettings launch_settings_from_json(const Json& effectiveSettings);

class Launcher {
public:
    explicit Launcher(Services services = native_services());
    LaunchPlan prepare(const LaunchRequest& request, const Cancellation& cancellation,
                       const LaunchEventCallback& events = {});
    std::shared_ptr<GameSession> launch(const LaunchRequest& request, const Cancellation& cancellation,
                                       const LaunchEventCallback& events = {});
    LaunchPlan prepare(const LaunchRequest& request, std::stop_token cancellation,
                       const LaunchEventCallback& events = {});
    std::shared_ptr<GameSession> launch(const LaunchRequest& request, std::stop_token cancellation,
                                       const LaunchEventCallback& events = {});
    bool cancel_prepare(std::string_view launchId);
    std::shared_ptr<GameSession> session(std::string_view launchId) const;
private:
    struct Preparation;
    Services services_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Preparation>, std::less<>> preparations_;
    std::map<std::string, std::shared_ptr<GameSession>, std::less<>> sessions_;
    LaunchPlan prepare_checked(LaunchRequest request, const Services& services,
                               const CancelCheck& check, const LaunchEventCallback& events);
    std::shared_ptr<Preparation> begin(std::string_view launchId);
    void finish(std::string_view launchId, const std::shared_ptr<Preparation>& preparation);
};

}
