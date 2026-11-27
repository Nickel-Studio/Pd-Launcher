#pragma once

#include "settings.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>

namespace nickel::instances {
inline constexpr std::string_view eula_version = "minecraft-eula-v1";
inline constexpr std::string_view eula_url = "https://www.minecraft.net/zh-hans/eula";
struct StartupLaunchRequest {
    fs::path game_root;
    std::string instance_id;
};
std::optional<StartupLaunchRequest> parse_startup_arguments(std::span<const std::string> arguments);
std::string launch_script(const fs::path& executable, const StartupLaunchRequest& request,
                          bool windows);
struct OnboardingStatus {
    bool needs_eula = true;
    bool needs_changelog = true;
    bool exiting = false;
    std::optional<std::uint64_t> accepted_at;
};
class OnboardingGate {
public:
    OnboardingGate(fs::path config_directory, std::string current_version);
    OnboardingStatus status() const;
    void accept_eula(bool checked, std::string_view revision, std::uint64_t timestamp_seconds);
    void acknowledge_changelog(std::string_view version);
    void require_accepted() const;
    void require_finished() const;
    bool finish(const std::function<void()>& startup_dispatch);
    std::chrono::steady_clock::time_point reject();
private:
    Json read_record() const;
    void require_accepted_record(const Json& record) const;
    fs::path record_;
    std::string version_;
    mutable std::mutex mutex_;
    bool exiting_ = false;
    bool dispatched_ = false;
    std::optional<std::chrono::steady_clock::time_point> rejection_deadline_;
};
struct ResolvedLaunchTarget {
    StartupLaunchRequest request;
    InstanceEntry entry;
    std::optional<NamedProfile> profile;
    EffectiveSettings settings;
    fs::path install_root;
    fs::path resource_root;
    fs::path mods_root;
    std::string minecraft_version;
    std::string installed_version_id;
    fs::path descriptor_path;
    Json descriptor;
};
// Resolve data only; App enforces onboarding and owns the child process before launching.
ResolvedLaunchTarget resolve_launch_target(const fs::path& registered_root,
                                           std::string_view outer_directory_id,
                                           const Json& global_settings,
                                           const MemoryContext& memory = {});
using ResolvedStartupLaunch = ResolvedLaunchTarget;
ResolvedStartupLaunch resolve_startup_launch(const StartupLaunchRequest& request,
                                            const Json& global_settings,
                                            const OnboardingGate& gate,
                                            const MemoryContext& memory = {});
} // namespace nickel::instances
