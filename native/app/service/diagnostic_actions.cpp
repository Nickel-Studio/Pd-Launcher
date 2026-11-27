#include "app/application_state.hpp"

#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"
#include "core/instances/domain_io.hpp"
#include "core/config/path_guard.hpp"
#include "core/network/hash.hpp"
#include "core/tasks/secure_fs.hpp"
#include "platform/services.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace nickel::app {
namespace {

std::filesystem::path selected_output(const Json& args, std::string_view extension,
                                      std::uint64_t required_space) {
    const auto path = instances::path_from_utf8(bounded_text(args, "path", 32768));
    if (!path.is_absolute() || resources::lower(instances::path_to_utf8(path.extension())) != extension)
        fail_static("invalid_argument");
    instances::reject_links(path);
    instances::reject_links(path.parent_path());
    if (!std::filesystem::is_directory(path.parent_path()) || std::filesystem::exists(path))
        fail_static("storage");
    if (std::filesystem::space(path.parent_path()).available < required_space) fail_static("storage");
    return path;
}

std::string authorized_external(ApplicationState& state, const Json& args) {
    if (args.contains("documentKey")) {
        const auto document = state.description(bounded_text(args, "documentKey", 128));
        const auto url = document->body.externalLink(bounded_text(args, "opaqueKey", 256));
        if (document->released.load() || url.empty()) fail_static("unauthorized");
        return resources::checkedUrl(url, resources::externalLinkRules());
    }
    if (args.value("projectHomepage", false)) {
        const auto source = selected_source(args);
        const auto id = bounded_text(args, "projectId", 128);
        resources::Project project;
        {
            std::lock_guard lock(state.state_mutex);
            const auto found = state.projects.find(resources::sourceName(source) + ':' + id);
            if (found == state.projects.end() || found->second.source != source) fail_static("unauthorized");
            project = found->second;
        }
        if (args.contains("kind") && selected_kind(args) != project.kind) fail_static("unauthorized");
        return resources::checkedUrl(project.pageUrl, resources::externalLinkRules());
    }
    const auto requested = bounded_text(args, "url", 4096);
    constexpr std::array allowed{
        std::string_view("https://github.com/Nickel-Studio/Ni-Launcher"),
        std::string_view("https://github.com/Nickel-Studio/Ni-Launcher/blob/main/LICENSE"),
        std::string_view("https://github.com/Nickel-Studio/Pd-Launcher/blob/main/LICENSE-ASSETS"),
        std::string_view("https://github.com/ItzArona"),
        std::string_view("https://github.com/ItzPlana"),
        std::string_view("https://github.com/ShittimChests"),
        instances::eula_url
    };
    if (std::find(allowed.begin(), allowed.end(), requested) != allowed.end()) return requested;
    bool verified = false;
    {
        std::lock_guard lock(state.state_mutex);
        verified = std::any_of(state.auth_sessions.begin(), state.auth_sessions.end(), [&](const auto& item) {
            return !item.second->cancellation.cancelled() && item.second->verified_url == requested;
        });
    }
    if (!verified) fail_static("unauthorized");
    return resources::checkedUrl(requested, resources::externalLinkRules());
}

std::filesystem::path authorized_location(ApplicationState& state, const Json& args) {
    if (args.contains("path")) return state.registered_root(bounded_text(args, "path", 32768));
    const auto location = bounded_text(args, "location", 32);
    if (args.contains("launchId")) {
        if (location != "logs") fail_static("unauthorized");
        return diagnostics::resolve_log_paths(state.owned_launch(args)->resource_root).logs;
    }
    const auto selected = state.identity(args);
    const auto root = state.registered_root(selected.rootId);
    const auto current = state.context(selected);
    if (location == "instance") return instances::checked_instance_directory(root, selected.instanceId);
    if (location == "mods") return current.modsRoot;
    if (location == "resourcepacks") return current.resourcepacksRoot;
    if (location == "shaderpacks") return current.shaderpacksRoot;
    if (location == "saves") return current.resourceRoot / "saves";
    if (location == "logs") return diagnostics::resolve_log_paths(current.resourceRoot).logs;
    fail_static("invalid_argument");
}

} // namespace

OperationResult ApplicationState::action_diagnostics(ui::UiAction action, const Json& args,
                                                    const Cancellation& cancellation) {
    cancellation.check();
    OperationResult result;
    switch (action) {
    case ui::UiAction::OpenExternalUrl: {
        const auto url = authorized_external(*this, args);
        cancellation.check();
        nickel::open_url(url);
        result.data = {{"opened", true}};
        return result;
    }
    case ui::UiAction::OpenPath: {
        const auto path = authorized_location(*this, args);
        instances::reject_links(path);
        if (!std::filesystem::is_directory(path)) fail_static("not_found");
        cancellation.check();
        nickel::open_path(path);
        result.data = {{"opened", true}};
        return result;
    }
    case ui::UiAction::ClearLaunchError: {
        const auto launch = owned_launch(args);
        std::optional<diagnostics::ErrorReportSnapshot> retired_snapshot;
        std::optional<diagnostics::SafeReport> retired_report;
        {
            std::lock_guard lock(state_mutex);
            if (launch->preparing.load() || launch->draining.load()
                || launch->public_state.value("phase", std::string{}) != "finished"
                || launch->public_state.value("state", std::string{}) != "failed") fail_static("not_found");
            retired_snapshot.swap(launch->failure_snapshot);
            retired_report.swap(launch->failure_report);
            launch->public_state.erase("reportTarget");
            launch->public_state.erase("diagnostic");
            launch->public_state.erase("error");
            launch->public_state.update({{"state", "idle"}, {"reportReady", false}, {"canCancel", false}});
            result.data = launch->public_state;
        }
        result.changes.emplace_back(ui::UiTopic::Launch, result.data);
        return result;
    }
    case ui::UiAction::ExportLogs: {
        const auto launch = owned_launch(args);
        diagnostics::SafeReport snapshot;
        {
            std::lock_guard lock(state_mutex);
            if (!launch->failure_report) fail_static("not_found");
            snapshot = *launch->failure_report;
        }
        const auto destination = selected_output(args, ".txt", 12 * 1024 * 1024);
        cancellation.check();
        diagnostics::write_safe_report(destination, snapshot, false);
        result.data = {{"path", instances::path_to_utf8(destination)}};
        return result;
    }
    case ui::UiAction::ExportErrorReport: {
        const auto launch = owned_launch(args);
        std::optional<diagnostics::ErrorReportSnapshot> snapshot;
        {
            std::lock_guard lock(state_mutex);
            snapshot = launch->failure_snapshot;
        }
        if (!snapshot) fail_static("not_found");
        const auto& target = snapshot->target();
        if (target.launch_id != launch->launch_id || target.version_id != launch->identity.instanceId
            || instances::root_identity(instances::path_to_utf8(target.root)) != launch->identity.rootId)
            fail_static("unauthorized");
        const auto destination = selected_output(args, ".zip", 32 * 1024 * 1024);
        const auto published = diagnostics::export_error_report_zip(destination, *snapshot, *archive,
                                                                    cancellation.stop_token());
        result.data = {{"path", instances::path_to_utf8(published)}};
        return result;
    }
    case ui::UiAction::ScanJunk:
    case ui::UiAction::CleanJunk:
        return action_cleanup(action, args, cancellation);
    default:
        fail_static("invalid_argument");
    }
}

namespace {

struct CleanupStamp {
    std::filesystem::path path;
    bool directory = false;
    bool watch_entries = false;
    bool present = false;
    tasks::detail::FileIdentity identity;
    tasks::detail::FileRevision revision;
    std::uint64_t size = 0;
    std::filesystem::file_time_type modified;
    bool operator==(const CleanupStamp&) const = default;
};

CleanupStamp cleanup_stamp(const std::filesystem::path& path, bool directory, bool watch_entries = false) {
    CleanupStamp stamp;
    stamp.path = tasks::detail::logical_path(path);
    stamp.directory = directory;
    stamp.watch_entries = watch_entries;
    instances::reject_links(stamp.path);
    const auto native = foundation_detail::native_filesystem_path(stamp.path);
    std::error_code error;
    const auto status = std::filesystem::symlink_status(native, error);
    if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
        return stamp;
    if (error) fail_static("storage");
    stamp.present = true;
    if (directory) {
        if (!std::filesystem::is_directory(status)) fail_static("unauthorized");
        stamp.identity = tasks::detail::directory_identity(stamp.path);
        if (watch_entries) stamp.modified = std::filesystem::last_write_time(native);
    } else {
        if (!std::filesystem::is_regular_file(status)) fail_static("unauthorized");
        auto file = tasks::detail::SecureFile::open_read(stamp.path);
        stamp.identity = file.identity();
        stamp.revision = file.revision();
        stamp.size = file.size();
    }
    return stamp;
}

Json cleanup_locations_json(const diagnostics::JunkLocations& locations) {
    Json roots = Json::array();
    for (const auto& root : locations.gameRoots) {
        Json entries = Json::array();
        for (const auto& instance : root.instances)
            entries.push_back({{"versionId", instance.versionId},
                {"effectiveDirectory", instances::path_to_utf8(instance.effectiveDirectory)}});
        roots.push_back({{"root", instances::path_to_utf8(root.root)}, {"instances", entries}});
    }
    return {{"gameRoots", roots}, {"configRoot", locations.configRoot
        ? Json(instances::path_to_utf8(*locations.configRoot)) : Json(nullptr)}};
}

struct CleanupAuthorization {
    diagnostics::JunkLocations locations;
    std::vector<CleanupStamp> metadata;
    std::vector<std::filesystem::path> checked_roots;
    std::string fingerprint;

    bool unchanged() const {
        try {
            for (const auto& stamp : metadata)
                if (cleanup_stamp(stamp.path, stamp.directory, stamp.watch_entries) != stamp) return false;
            return true;
        } catch (...) { return false; }
    }
};

bool cleanup_idle(ApplicationState& state, const CleanupAuthorization& authorization,
                  const Cancellation* excluding = nullptr) {
    if (state.stopping.load() || state.pending_operations(excluding)) return false;
    struct Snapshot {
        bool busy;
        std::filesystem::path resource_root;
        std::shared_ptr<minecraft::GameSession> game;
    };
    std::vector<Snapshot> launches;
    {
        std::lock_guard lock(state.state_mutex);
        for (const auto& [id, launch] : state.launches) {
            const auto status = launch->public_state.value("state", std::string{});
            const auto phase = launch->public_state.value("phase", std::string{});
            launches.push_back({launch->preparing.load() || launch->draining.load()
                || status == "running" || status == "launching" || phase == "draining",
                launch->resource_root, launch->game});
        }
    }
    for (const auto& root : authorization.checked_roots) {
        if (state.tasks->conflicts(root)) return false;
        for (const auto& launch : launches) {
            if (tasks::detail::paths_overlap(root, launch.resource_root)
                && (launch.busy || (launch.game && launch.game->running()))) return false;
        }
    }
    return true;
}

std::shared_ptr<CleanupAuthorization> cleanup_authorization(ApplicationState& state,
                                                           const Cancellation& cancellation) {
    auto authority = std::make_shared<CleanupAuthorization>();
    std::set<std::string> metadata_paths;
    const auto remember = [&](const std::filesystem::path& path, bool directory = false) {
        const auto key = instances::root_identity(instances::path_to_utf8(path));
        if (!metadata_paths.insert(key).second) return;
        if (authority->metadata.size() >= 20000) fail_static("busy");
        authority->metadata.push_back(cleanup_stamp(path, directory));
    };
    remember(state.repository.root(), true);
    for (const auto document : {"instances.json", "settings.json", "game.json"})
        remember(state.repository.document_path(document));
    remember(state.paths.executable_directory() / instances::path_from_utf8(portable_manifest_name));
    const auto registered = state.roots.get();
    const auto global = state.settings("game");
    const auto app = state.settings();
    std::map<std::string, std::filesystem::path> roots;
    const auto add_root = [&](const std::filesystem::path& raw) {
        if (!raw.is_absolute()) return;
        roots.emplace(instances::root_identity(instances::path_to_utf8(raw)), raw);
    };
    for (const auto& root : registered.roots) add_root(instances::path_from_utf8(root));
    add_root(state.paths.default_game_directory());
    add_root(state.paths.bundled_game_directory());
    if (roots.size() > 256) fail_static("busy");
    std::size_t instance_count = 0;
    for (const auto& [key, root] : roots) {
        cancellation.check();
        diagnostics::JunkGameRoot location{root, {}};
        try {
            remember(root, true);
            remember(root / "versions", true);
            remember(root / "gameData.json");
            if (std::filesystem::is_directory(foundation_detail::native_filesystem_path(root))) {
                authority->checked_roots.push_back(root);
                for (const auto& entry : instances::scan_instances(root)) {
                    cancellation.check();
                    if (++instance_count > 4096) fail_static("busy");
                    remember(entry.directory, true);
                    remember(entry.directory / instances::path_from_utf8(instances::settings_file));
                    remember(entry.directory / instances::path_from_utf8(instances::profile_file));
                    remember(entry.directory / instances::path_from_utf8(instances::pending_file));
                    if (const auto descriptor = instances::pick_version_json(entry.directory, entry.id)) remember(*descriptor);
                    if (!entry.ready || !entry.minecraft_version || instances::is_pending(entry.directory)) continue;
                    const auto effective = instances::resource_directory(root, entry.id, global);
                    const auto effective_key = instances::root_identity(instances::path_to_utf8(effective));
                    if (effective_key != key && effective_key != instances::root_identity(instances::path_to_utf8(entry.directory)))
                        fail_static("unauthorized");
                    location.instances.push_back({entry.id, effective});
                }
            }
        } catch (const ErrorException& error) {
            if (cancellation.cancelled() || error.error().code == "busy") throw;
            // JunkService reports unsupported or linked locations without expanding authority.
        }
        authority->locations.gameRoots.push_back(std::move(location));
    }
    authority->locations.configRoot = state.repository.root();
    if (!authority->unchanged()) fail_static("conflict");
    Json stamps = Json::array();
    for (const auto& stamp : authority->metadata)
        stamps.push_back({instances::path_to_utf8(stamp.path), stamp.directory, stamp.present,
            stamp.identity.volume, stamp.identity.index, stamp.revision.modified, stamp.revision.changed,
            stamp.size, stamp.modified.time_since_epoch().count()});
    const auto encoded = Json{instances::registry_to_json(registered), app, global,
        cleanup_locations_json(authority->locations), stamps}.dump();
    transport::detail::IncrementalHash hash(transport::HashAlgorithm::Sha256);
    hash.update(std::as_bytes(std::span(encoded)));
    authority->fingerprint = hash.finish();
    return authority;
}

std::string_view junk_kind(diagnostics::JunkKind kind) {
    switch (kind) {
    case diagnostics::JunkKind::GameLog: return "gameLog";
    case diagnostics::JunkKind::CrashReport: return "crashReport";
    case diagnostics::JunkKind::JvmLog: return "jvmLog";
    case diagnostics::JunkKind::AuthlibLog: return "authlibLog";
    case diagnostics::JunkKind::ConfigLog: return "configLog";
    case diagnostics::JunkKind::ConfigTemporary: return "configTemporary";
    }
    fail_static("integrity");
}

} // namespace

struct CleanupCoordinator {
    explicit CleanupCoordinator(ApplicationState& state)
        : owner(state), junk({}, [this](const diagnostics::JunkLocations& locations) {
            std::shared_ptr<const CleanupAuthorization> checked;
            std::optional<Cancellation> operation;
            {
                std::lock_guard lock(mutex);
                checked = authority;
                operation = active_cancellation;
            }
            return checked && cleanup_locations_json(locations) == cleanup_locations_json(checked->locations)
                && checked->unchanged() && cleanup_idle(owner, *checked, operation ? &*operation : nullptr);
        }) {}

    std::shared_ptr<const CleanupAuthorization> current() const {
        std::lock_guard lock(mutex);
        return authority;
    }

    ApplicationState& owner;
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    mutable std::mutex mutex;
    std::optional<Cancellation> active_cancellation;
    std::shared_ptr<const CleanupAuthorization> authority;
    std::map<std::string, std::shared_ptr<const CleanupAuthorization>> previews;
    diagnostics::JunkRegistration registration;
    diagnostics::JunkService junk;
};

void ApplicationState::initialize_cleanup() {
    cleanup_ = std::make_shared<CleanupCoordinator>(*this);
}

OperationResult ApplicationState::action_cleanup(ui::UiAction action, const Json& args,
                                                 const Cancellation& cancellation) {
    if (!cleanup_ || stopping.load()) fail_static("cancelled");
    if (cleanup_->busy.test_and_set()) fail_static("busy");
    struct Release {
        CleanupCoordinator& coordinator;
        ~Release() {
            { std::lock_guard lock(coordinator.mutex); coordinator.active_cancellation.reset(); }
            coordinator.busy.clear();
        }
    } release{*cleanup_};
    {
        std::lock_guard lock(cleanup_->mutex);
        cleanup_->active_cancellation = cancellation;
    }
    OperationResult result;
    if (action == ui::UiAction::ScanJunk) {
        if (!args.empty()) fail_static("invalid_argument");
        cancellation.check();
        const auto next = cleanup_authorization(*this, cancellation);
        const auto previous = cleanup_->current();
        if (!previous || previous->fingerprint != next->fingerprint) {
            cleanup_->registration = cleanup_->junk.registerLocations(next->locations);
            std::lock_guard lock(cleanup_->mutex);
            cleanup_->authority = next;
            cleanup_->previews.clear();
        }
        if (!cleanup_idle(*this, *next, &cancellation)) fail_static("busy");
        const auto preview = cleanup_->junk.preview(cancellation.stop_token());
        {
            std::lock_guard lock(cleanup_->mutex);
            if (cleanup_->previews.size() >= 4) cleanup_->previews.erase(cleanup_->previews.begin());
            cleanup_->previews.emplace(preview.previewId, cleanup_->authority);
        }
        Json items = Json::array();
        for (const auto& item : preview.items)
            items.push_back({{"path", instances::path_to_utf8(item.path)}, {"kind", junk_kind(item.kind)},
                {"size", item.size}, {"revision", item.revision}});
        Json issues = Json::array();
        for (const auto& issue : cleanup_->registration.issues)
            issues.push_back({{"path", instances::path_to_utf8(issue.path)}, {"code", issue.code}});
        result.data = {{"previewId", preview.previewId}, {"revision", preview.revision},
            {"totalBytes", preview.totalBytes}, {"fileCount", preview.fileCount}, {"count", preview.fileCount},
            {"skippedCount", preview.skippedCount}, {"items", items}, {"issues", issues}};
        return result;
    }
    if (action != ui::UiAction::CleanJunk || args.size() != 1) fail_static("invalid_argument");
    const auto id = bounded_text(args, "previewId", 128);
    std::shared_ptr<const CleanupAuthorization> authority;
    {
        std::lock_guard lock(cleanup_->mutex);
        const auto found = cleanup_->previews.find(id);
        if (found == cleanup_->previews.end()) fail_static("not_found");
        authority = found->second;
        cleanup_->previews.erase(found);
    }
    if (!authority->unchanged() || !cleanup_idle(*this, *authority, &cancellation)) {
        cleanup_->junk.discardPreview(id);
        result.data = {{"freedBytes", 0}, {"removedCount", 0}, {"failedCount", 0}, {"missingCount", 0},
            {"conflictCount", 0}, {"cancelledCount", 0}, {"cancelled", cancellation.cancelled()}, {"locationConflict", true}};
        result.error_code = "conflict";
        return result;
    }
    std::vector<MutationGate::Lease> leases;
    for (const auto& root : authority->checked_roots)
        leases.push_back(mutations.root(instances::root_identity(instances::path_to_utf8(root)), cancellation));
    const auto cleaned = cleanup_->junk.clean(id, cancellation.stop_token());
    result.data = {{"freedBytes", cleaned.freedBytes}, {"removedCount", cleaned.removedCount},
        {"failedCount", cleaned.failedCount}, {"missingCount", cleaned.missingCount},
        {"conflictCount", cleaned.conflictCount}, {"cancelledCount", cleaned.cancelledCount},
        {"cancelled", cleaned.cancelled}, {"locationConflict", cleaned.locationConflict}};
    if (cleaned.cancelled) result.error_code = "cancelled";
    else if (cleaned.locationConflict || cleaned.conflictCount) result.error_code = "conflict";
    else if (cleaned.failedCount) result.error_code = "storage";
    return result;
}

} // namespace nickel::app
