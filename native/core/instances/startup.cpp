#include "startup.hpp"
#include "domain_io.hpp"

#include <algorithm>
#include <cctype>

namespace nickel::instances {
namespace {
constexpr std::uint64_t safe_timestamp_max = 9007199254740991ULL;
bool accepted(const Json& record) {
    if (!record.is_object()) return false;
    const auto revision = detail::string_at(record, "eulaVersion");
    const auto it = record.find("eulaAcceptedAt");
    if (!revision || *revision != eula_version || it == record.end()) return false;
    const auto timestamp = detail::unsigned_value(*it);
    return timestamp && *timestamp >= 1 && *timestamp <= safe_timestamp_max;
}
std::string script_argument(std::string_view input, bool windows) {
    if (std::any_of(input.begin(), input.end(), [](unsigned char c) { return c < 32 || c == 127; })
        || (windows && input.find('"') != std::string_view::npos))
        throw Error("unsafe_script", "Path contains unsupported script characters");
    std::string result;
    result += windows ? '"' : '\'';
    for (char c : input) {
        if (windows && c == '%') result += "%%";
        else if (!windows && c == '\'') result += "'\"'\"'";
        else result += c;
    }
    result += windows ? '"' : '\'';
    return result;
}
} // namespace

std::optional<StartupLaunchRequest> parse_startup_arguments(std::span<const std::string> arguments) {
    if (std::none_of(arguments.begin(), arguments.end(), [](const auto& argument) {
        return argument == "--launch-instance" || argument == "--game-root";
    })) return {};
    std::optional<std::string> instance;
    std::optional<std::string> root;
    for (std::size_t i = 0; i < arguments.size(); i += 2) {
        const auto& key = arguments[i];
        if (i + 1 >= arguments.size()) throw Error("invalid_cli", "Startup argument requires a value");
        if (key == "--launch-instance") {
            if (instance) throw Error("invalid_cli", "Duplicate --launch-instance");
            instance = arguments[i + 1];
        } else if (key == "--game-root") {
            if (root) throw Error("invalid_cli", "Duplicate --game-root");
            root = arguments[i + 1];
        } else throw Error("invalid_cli", "Unsupported startup argument");
    }
    if (!instance || !root) throw Error("invalid_cli", "Startup requires both instance identity and game root");
    validate_name(*instance);
    const auto path = path_from_utf8(*root);
    if (!path.is_absolute() || root->empty()) throw Error("invalid_cli", "Startup game root must be absolute");
    return StartupLaunchRequest{path, *instance};
}
std::string launch_script(const fs::path& executable, const StartupLaunchRequest& request, bool windows) {
    validate_name(request.instance_id);
    if (!executable.is_absolute() || !request.game_root.is_absolute()) throw Error("unsafe_script", "Launch script paths must be absolute");
    const auto command = script_argument(path_to_utf8(executable), windows)
        + " --launch-instance " + script_argument(request.instance_id, windows)
        + " --game-root " + script_argument(path_to_utf8(request.game_root), windows);
    return windows ? "@echo off\r\nsetlocal DisableDelayedExpansion\r\nchcp 65001 >nul\r\n" + command + "\r\nexit /b %errorlevel%\r\n"
                   : "#!/bin/sh\nexec " + command + "\n";
}
OnboardingGate::OnboardingGate(fs::path config, std::string version)
    : record_(std::move(config) / "onboarding.json"), version_(std::move(version)) {
    if (!record_.is_absolute() || version_.empty()) throw Error("invalid_onboarding", "Onboarding requires a version and absolute configuration root");
}
Json OnboardingGate::read_record() const {
    try {
        const auto record = detail::read_optional_json(record_, 16 * 1024);
        if (record && record->is_object()) {
            for (auto field : {"eulaVersion", "changelogSeenVersion"}) {
                if (record->contains(field) && !(*record)[field].is_null() && !(*record)[field].is_string()) return Json::object();
            }
            if (record->contains("eulaAcceptedAt") && !(*record)["eulaAcceptedAt"].is_null() && !detail::unsigned_value((*record)["eulaAcceptedAt"])) return Json::object();
            return *record;
        }
        return Json::object();
    } catch (const Error& error) {
        if (error.code() == "unsafe_path" || error.code() == "io") throw;
        return Json::object();
    }
}
OnboardingStatus OnboardingGate::status() const {
    std::lock_guard lock(mutex_);
    const auto record = read_record();
    const bool consent = accepted(record);
    const auto timestamp = consent ? detail::unsigned_value(record["eulaAcceptedAt"]) : std::nullopt;
    return {!consent, detail::string_at(record, "changelogSeenVersion") != version_, exiting_, timestamp};
}
void OnboardingGate::require_accepted_record(const Json& record) const {
    if (exiting_) throw Error("exiting", "Cannot start work while exiting");
    if (!accepted(record)) throw Error("eula_required", "Minecraft EULA must be explicitly accepted");
}
void OnboardingGate::accept_eula(bool checked, std::string_view revision, std::uint64_t timestamp) {
    std::lock_guard lock(mutex_);
    if (!checked || revision != eula_version || exiting_) throw Error("eula_required", "Current EULA checkbox confirmation is required");
    detail::DirectoryLock file_lock(record_.parent_path());
    auto record = read_record();
    if (accepted(record)) return;
    if (timestamp == 0 || timestamp > safe_timestamp_max) throw Error("invalid_timestamp", "EULA signature time is invalid");
    record["eulaVersion"] = eula_version;
    record["eulaAcceptedAt"] = timestamp;
    detail::atomic_write(record_, record.dump());
}
void OnboardingGate::acknowledge_changelog(std::string_view version) {
    std::lock_guard lock(mutex_);
    detail::DirectoryLock file_lock(record_.parent_path());
    auto record = read_record();
    require_accepted_record(record);
    if (version != version_) throw Error("changelog_required", "Changelog revision has changed");
    if (detail::string_at(record, "changelogSeenVersion") == version_) return;
    record["changelogSeenVersion"] = version_;
    detail::atomic_write(record_, record.dump());
}
void OnboardingGate::require_accepted() const {
    std::lock_guard lock(mutex_);
    require_accepted_record(read_record());
}
void OnboardingGate::require_finished() const {
    std::lock_guard lock(mutex_);
    const auto record = read_record();
    require_accepted_record(record);
    if (detail::string_at(record, "changelogSeenVersion") != version_) throw Error("changelog_required", "Current changelog must be acknowledged first");
}
bool OnboardingGate::finish(const std::function<void()>& dispatch) {
    {
        std::lock_guard lock(mutex_);
        const auto record = read_record();
        require_accepted_record(record);
        if (detail::string_at(record, "changelogSeenVersion") != version_) throw Error("changelog_required", "Current changelog must be acknowledged first");
        if (dispatched_) return false;
        dispatched_ = true;
    }
    // The dispatch may read this gate again; it must not run under the consent mutex.
    dispatch();
    return true;
}
std::chrono::steady_clock::time_point OnboardingGate::reject() {
    std::lock_guard lock(mutex_);
    exiting_ = true;
    if (!rejection_deadline_) rejection_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    return *rejection_deadline_;
}
ResolvedLaunchTarget resolve_launch_target(const fs::path& registered_root, std::string_view outer_directory_id,
                                           const Json& global, const MemoryContext& memory) {
    const StartupLaunchRequest request{registered_root, std::string(outer_directory_id)};
    const auto directory = checked_instance_directory(request.game_root, request.instance_id);
    if (is_pending(directory)) throw Error("pending_instance", "Pending installation cannot provide a launch target");
    auto entry = instance_detail(request.game_root, request.instance_id);
    if (!entry.has_json || !entry.ready || !entry.minecraft_version)
        throw Error("incomplete_instance", "Instance must be completed before launch");
    auto profile = load_profile(request.game_root, request.instance_id);
    if (profile && !profile->ready) throw Error("incomplete_instance", "Named profile is not ready");
    fs::path install_root = request.game_root;
    std::string installed_id = request.instance_id;
    auto descriptor_path = directory / path_from_utf8(request.instance_id + ".json");
    if (profile) {
        const auto wrapper = detail::read_optional_json(descriptor_path, descriptor_max_bytes, true);
        if (!wrapper || !wrapper->is_object() || detail::string_at(*wrapper, "id") != request.instance_id
            || detail::string_at(*wrapper, "inheritsFrom") != profile->minecraft_version)
            throw Error("identity_mismatch", "Named instance wrapper differs from its profile identity");
        const auto marker = wrapper->find("nickelProfile");
        if (marker == wrapper->end() || !marker->is_object()
            || detail::string_at(*marker, "minecraftVersion") != profile->minecraft_version)
            throw Error("identity_mismatch", "Named instance wrapper lacks the reserved Minecraft identity");
        installed_id = profile->minecraft_version;
        const auto legacy_version = directory / "versions" / path_from_utf8(installed_id);
        const auto native_root = directory / "instances" / path_from_utf8(request.instance_id);
        const auto native_version = native_root / "versions" / path_from_utf8(installed_id);
        reject_links(legacy_version);
        reject_links(native_version);
        const bool legacy_present = detail::path_exists(legacy_version);
        const bool native_present = detail::path_exists(native_root);
        if (legacy_present && native_present)
            throw Error("conflict", "Named instance contains both legacy and native installation layouts");
        if (!legacy_present && !native_present)
            throw Error("incomplete_instance", "Named instance has no exact Minecraft installation descriptor");
        install_root = legacy_present ? directory : native_root;
        descriptor_path = (legacy_present ? legacy_version : native_version) / path_from_utf8(installed_id + ".json");
    }
    reject_links(descriptor_path);
    if (!fs::is_regular_file(detail::native_path(descriptor_path)))
        throw Error("incomplete_instance", "Exact launch descriptor is unavailable");
    auto descriptor = parse_legacy_json(detail::read_bounded(descriptor_path, descriptor_max_bytes));
    if (!descriptor.is_object() || detail::string_at(descriptor, "id") != installed_id)
        throw Error("identity_mismatch", "Exact launch descriptor differs from its installed version identity");
    if (const auto main = detail::string_at(descriptor, "mainClass"); !main || main->empty())
        throw Error("incomplete_instance", "Exact launch descriptor has no executable main class");
    const auto local = read_local_settings(request.game_root, request.instance_id);
    auto context = memory;
    // Resource isolation belongs to the outer instance; the native inner install root never changes it.
    const auto resource = resolve_isolation(global, local.value) ? directory : request.game_root;
    const auto mods = mods_directory(resource, entry.loader, *entry.minecraft_version);
    context.mod_count = count_mods(mods);
    if (entry.loader == "forge" || entry.loader == "neoforge" || entry.loader == "fabric" || entry.loader == "quilt" || entry.loader == "liteloader")
        context.kind = VersionKind::moddable;
    else if (entry.loader == "optifine") context.kind = VersionKind::optifine;
    ResolvedLaunchTarget result;
    result.request = request;
    result.profile = std::move(profile);
    result.settings = resolve_settings(global, local.value, context);
    result.install_root = std::move(install_root);
    result.resource_root = resource;
    result.mods_root = mods;
    result.minecraft_version = *entry.minecraft_version;
    result.installed_version_id = std::move(installed_id);
    result.descriptor_path = std::move(descriptor_path);
    result.descriptor = std::move(descriptor);
    result.entry = std::move(entry);
    return result;
}
ResolvedStartupLaunch resolve_startup_launch(const StartupLaunchRequest& request, const Json& global,
                                            const OnboardingGate& gate, const MemoryContext& memory) {
    gate.require_finished();
    return resolve_launch_target(request.game_root, request.instance_id, global, memory);
}
} // namespace nickel::instances
