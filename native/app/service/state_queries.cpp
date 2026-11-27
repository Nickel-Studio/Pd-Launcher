#include "app/application_state.hpp"

#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"
#include "core/instances/domain_io.hpp"
#include "core/instances/icons.hpp"
#include "core/minecraft/maven.hpp"
#include "core/minecraft/services_native.hpp"
#include "core/resources/utility.hpp"
#include "platform/memory.hpp"

#include <QCryptographicHash>

#include <algorithm>
#include <array>

namespace nickel::app {
namespace {

Json runtime_information(ApplicationState& state, const Json& scope) {
    std::optional<std::string> nonce;
    if (scope.contains("requestNonce")) {
        nonce = bounded_text(scope, "requestNonce", 128);
        if (!std::all_of(nonce->begin(), nonce->end(), [](unsigned char value) {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9') || value == '_' || value == '-';
        })) fail_static("invalid_argument");
    }
    const auto physical = physical_memory();
    instances::MemoryContext memory;
    memory.total_mb = physical.total_mb;
    memory.available_mb = physical.available_mb;
    if (scope.contains("root") || scope.contains("versionId")) {
        const auto root = bounded_text(scope, "root", 32768, false);
        const auto version = bounded_text(scope, "versionId", 512, false);
        if (root.empty() != version.empty()) fail_static("invalid_argument");
        if (!root.empty()) {
            const auto selected = state.identity(scope);
            const auto context = state.context(selected);
            memory.mod_count = instances::count_mods(context.modsRoot);
            if (context.loader == "optifine") memory.kind = instances::VersionKind::optifine;
            else if (context.loader != "vanilla" && context.loader != "unknown") memory.kind = instances::VersionKind::moddable;
        }
    }
    Json result{{"version", build_version()}, {"platform", minecraft::Platform::current().os},
                {"architecture", minecraft::Platform::current().architecture},
                {"physicalMemoryMb", physical.total_mb ? Json(*physical.total_mb) : Json(nullptr)},
                {"totalMemoryMb", physical.total_mb ? Json(*physical.total_mb) : Json(nullptr)},
                {"availableMemoryMb", physical.available_mb ? Json(*physical.available_mb) : Json(nullptr)},
                {"suggestedMemoryMb", instances::automatic_memory_mb(memory)},
                {"modCount", memory.mod_count}, {"capabilities", {{"highPerformanceGpu", false}}}};
    if (nonce) result["requestNonce"] = *nonce;
    return result;
}

Json installed_resources(const resources::InstanceContext& context, const Cancellation& cancellation) {
    Json rows = Json::array();
    for (const auto& [kind, directory] : std::array<std::pair<const char*, std::filesystem::path>, 3>{{
             {"resourcepack", context.resourcepacksRoot}, {"shader", context.shaderpacksRoot},
             {"datapack", context.datapacksRoot}}}) {
        cancellation.check();
        instances::reject_links(directory);
        if (!std::filesystem::exists(directory)) continue;
        resources::detail::checkedDirectory(directory);
        for (const auto& item : std::filesystem::directory_iterator(directory)) {
            cancellation.check();
            if (rows.size() >= 10000) fail_static("busy");
            instances::reject_links(item.path(), false);
            if (!item.is_regular_file()) continue;
            const auto name = instances::path_to_utf8(item.path().filename());
            if (!resources::validResourceFileName(name)) continue;
            rows.push_back({{"id", std::string(kind) + ':' + name}, {"kind", kind},
                            {"name", name}, {"fileName", name}, {"size", item.file_size()},
                            {"revision", resources::detail::fileRevision(item.path())}});
        }
    }
    return rows;
}

void attach_instance_icon(ApplicationState& state, const std::filesystem::path& root,
                          const std::string& id, Json& data, OperationResult& result,
                          const Cancellation& cancellation) {
    auto icon = instances::read_instance_icon(root, id, cancellation.stop_token());
    if (!icon) return;
    const auto identity = Json::array({instances::root_identity(instances::path_to_utf8(root)), id, icon->reference}).dump();
    const auto key = "instance:" + QCryptographicHash::hash(QByteArray::fromStdString(identity),
        QCryptographicHash::Sha256).toHex().toStdString();
    media::EncodedImage encoded{std::move(icon->png), "image/png", media::ImageKind::avatar};
    auto image = ui::imageFromRgba(state.images.decode_bounded(encoded, cancellation));
    data["icon"] = key;
    data["iconKey"] = key;
    data["iconSource"] = "managed";
    data["mediaScope"] = instance_scope({instances::root_identity(instances::path_to_utf8(root)), id}, root);
    result.media.emplace_back(key, std::move(image));
}

} // namespace

OperationResult ApplicationState::query(ui::UiQuery query, const Json& scope, const Cancellation& cancellation) {
    cancellation.check();
    if (stopping.load()) fail_static("cancelled");
    OperationResult result;
    switch (query) {
    case ui::UiQuery::Bootstrap: {
        const auto status = onboarding.status();
        if (!startup_requires_explicit_finish_ && !status.needs_eula && !status.needs_changelog && !status.exiting)
            if (!onboarding.finish([&] { dispatch_startup(cancellation); })) dispatch_startup(cancellation);
        const auto gate = onboarding_snapshot();
        result.data = {{"settings", settings()}, {"accounts", accounts_snapshot()},
                       {"roots", root_snapshot()}, {"selectedInstance", selected_instance()},
                       {"downloads", downloads_snapshot()}, {"launch", launch_snapshot()},
                       {"onboarding", gate}, {"startupReady", gate["startupReady"]}};
        return result;
    }
    case ui::UiQuery::Accounts: {
        result.data = accounts_snapshot();
        const auto active = accounts.active();
        if (active) {
            media::SkinRequest request;
            request.account_id = active->id;
            request.source = active->type;
            request.name = active->name;
            if (!active->uuid.empty()) request.uuid = active->uuid;
            request.server = active->server;
            result.media.emplace_back("account:" + active->id, ui::imageFromRgba(images.head(request, cancellation).head));
        }
        return result;
    }
    case ui::UiQuery::Characters: {
        std::lock_guard lock(state_mutex);
        if (scope.contains("sessionId")) {
            const auto key = bounded_text(scope, "sessionId", 128);
            const auto found = auth_sessions.find(key);
            if (found == auth_sessions.end()) fail_static("not_found");
            result.data = found->second->public_state;
        } else result.data = login;
        return result;
    }
    case ui::UiQuery::InstanceRoots:
        result.data = root_snapshot();
        return result;
    case ui::UiQuery::Instances: {
        const auto root = registered_root(bounded_text(scope, "root", 32768));
        Json rows = Json::array();
        for (const auto& entry : instances::scan_instances(root)) {
            cancellation.check();
            if (rows.size() >= 10000) fail_static("busy");
            auto row = instances::entry_to_json(entry);
            row["root"] = instances::path_to_utf8(root);
            row["versionId"] = entry.id;
            if (entry.ready) attach_instance_icon(*this, root, entry.id, row, result, cancellation);
            rows.push_back(std::move(row));
        }
        result.data = {{"root", instances::path_to_utf8(root)}, {"items", rows},
                       {"selectedInstance", selected_instance()}};
        return result;
    }
    case ui::UiQuery::VersionDetail: {
        const auto selected = identity(scope);
        const auto root = registered_root(selected.rootId);
        const auto entry = instances::instance_detail(root, selected.instanceId);
        result.data = instances::entry_to_json(entry);
        result.data.update(instance_scope(selected, root));
        result.data["name"] = entry.display_name;
        const auto descriptor = instances::pick_version_json(entry.directory, selected.instanceId);
        if (descriptor) {
            const auto metadata = instances::parse_legacy_json(instances::detail::read_bounded(*descriptor, instances::descriptor_max_bytes));
            const auto custom = metadata.find("nickelCustom");
            if (custom != metadata.end() && custom->is_object())
                result.data["description"] = custom->value("description", std::string{});
        }
        if (entry.ready) attach_instance_icon(*this, root, selected.instanceId, result.data, result, cancellation);
        return result;
    }
    case ui::UiQuery::VersionSettings: {
        const auto selected = identity(scope);
        const auto root = registered_root(selected.rootId);
        const auto local = instances::read_local_settings(root, selected.instanceId);
        const auto physical = physical_memory();
        instances::MemoryContext memory;
        memory.total_mb = physical.total_mb;
        memory.available_mb = physical.available_mb;
        const auto effective = instances::resolve_settings(settings("game"), local.value, memory);
        result.data = instance_scope(selected, root);
        result.data.update({{"values", local.value.is_object() ? local.value : instances::default_version_settings()},
                            {"effective", effective_settings_json(effective)}, {"source", local.source},
                            {"unmappedLegacyKeys", local.unmapped_legacy_keys}});
        return result;
    }
    case ui::UiQuery::JavaInstallations: {
        Json rows = Json::array();
        for (const auto& candidate : discover_java(cancellation))
            rows.push_back({{"path", instances::path_to_utf8(candidate.path)}, {"version", candidate.version},
                            {"vendor", candidate.vendor}, {"architecture", candidate.architecture},
                            {"major", candidate.major}, {"bits", candidate.bits}, {"executable", candidate.executable}});
        result.data = {{"items", rows}};
        return result;
    }
    case ui::UiQuery::VanillaCatalog: {
        const auto services = minecraft::make_native_services(http);
        const auto check = minecraft::cancellation_check(services, cancellation);
        const auto manifest = minecraft::parse_metadata(services.fetch(minecraft::InstallRequest{}.manifestUrl, 8 * 1024 * 1024, check));
        if (!manifest.contains("versions") || !manifest["versions"].is_array()
            || manifest["versions"].size() > 20000) fail_static("integrity");
        Json rows = Json::array();
        for (const auto& entry : manifest["versions"]) {
            cancellation.check();
            if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string()) fail_static("integrity");
            const auto id = entry["id"].get<std::string>();
            minecraft::validate_version_id(id);
            rows.push_back({{"id", id}, {"name", id}, {"type", entry.value("type", std::string("unknown"))},
                            {"releaseTime", entry.value("releaseTime", std::string{})}});
        }
        const auto latest = manifest.value("latest", Json::object());
        result.data = {{"items", rows}, {"latestRelease", latest.value("release", std::string{})},
                       {"latestSnapshot", latest.value("snapshot", std::string{})}};
        return result;
    }
    case ui::UiQuery::LoaderCatalog: {
        const auto minecraft_version = bounded_text(scope, "minecraftVersion", 128);
        const auto services = minecraft::make_native_services(http);
        const auto check = minecraft::cancellation_check(services, cancellation);
        minecraft::Catalog catalog(services);
        Json rows = Json::array();
        Json issues = Json::array();
        for (const auto kind : {minecraft::LoaderKind::Fabric, minecraft::LoaderKind::LegacyFabric,
                               minecraft::LoaderKind::Quilt, minecraft::LoaderKind::Forge,
                               minecraft::LoaderKind::NeoForge, minecraft::LoaderKind::OptiFine,
                               minecraft::LoaderKind::LiteLoader}) {
            cancellation.check();
            try {
                const auto builds = catalog.builds(kind, minecraft_version, check);
                for (const auto& build : builds) {
                    if (rows.size() >= 10000) fail_static("busy");
                    rows.push_back({{"id", std::string(minecraft::loader_name(kind)) + ':' + build.version},
                                    {"kind", minecraft::loader_name(kind)}, {"name", minecraft::loader_name(kind)},
                                    {"version", build.version}, {"minecraftVersion", build.minecraftVersion},
                                    {"stable", build.stable}, {"recommended", build.recommended},
                                    {"compatible", build.installable}});
                }
            } catch (const ErrorException& error) {
                cancellation.check();
                issues.push_back(std::string(minecraft::loader_name(kind)) + ": " + safe_public_message(error.error().code));
            }
        }
        result.data = {{"items", rows}, {"issues", issues}};
        return result;
    }
    case ui::UiQuery::CommunitySearch:
    case ui::UiQuery::ProjectDetail:
    case ui::UiQuery::ProjectVersions:
    case ui::UiQuery::RequiredDependencies:
        return query_community(query, scope, cancellation);
    case ui::UiQuery::Mods: {
        const auto selected = identity(scope);
        const auto current = context(selected);
        result.data = instance_scope(selected, registered_root(selected.rootId));
        std::vector<resources::ModUpdate> updates;
        {
            std::lock_guard lock(state_mutex);
            const auto cached = mod_updates.find(Json::array({selected.rootId, selected.instanceId}).dump());
            if (cached != mod_updates.end()) updates = cached->second;
        }
        Json rows = Json::array();
        for (const auto& mod : mods->scan(selected, cancellation)) {
            Json row{{"id", mod.fileName}, {"fileName", mod.fileName}, {"name", mod.name},
                     {"description", mod.description}, {"version", mod.version}, {"enabled", mod.enabled},
                     {"tags", mod.tags}, {"modIds", mod.modIds}, {"duplicateGroup", mod.duplicateGroup},
                     {"updateAvailable", mod.updateAvailable}, {"revision", mod.revision}};
            const auto cached = std::find_if(updates.begin(), updates.end(), [&](const auto& update) {
                return update.fileName == mod.fileName && update.instance == selected
                    && update.instanceRevision == current.revision && !update.originalSha256.empty();
            });
            if (cached != updates.end()) {
                cancellation.check();
                const auto file = resources::detail::checkedLeaf(current.modsRoot, mod.fileName);
                if (resources::hashResourceFile(file, cancellation).sha256 == cached->originalSha256) {
                    row["identified"] = cached->identified;
                    row["updateAvailable"] = cached->available.has_value();
                }
            }
            rows.push_back(std::move(row));
        }
        result.data["items"] = std::move(rows);
        result.data["revision"] = current.revision;
        return result;
    }
    case ui::UiQuery::InstalledResources: {
        const auto selected = identity(scope);
        result.data = instance_scope(selected, registered_root(selected.rootId));
        const auto current = context(selected);
        result.data["items"] = installed_resources(current, cancellation);
        result.data["revision"] = current.revision;
        return result;
    }
    case ui::UiQuery::Settings: {
        auto section = bounded_text(scope, "section", 16, false);
        if (section.empty()) section = "app";
        result.data = {{"section", section}, {"values", settings(section)}};
        return result;
    }
    case ui::UiQuery::Downloads:
        result.data = downloads_snapshot();
        return result;
    case ui::UiQuery::Logs: {
        std::shared_ptr<diagnostics::LaunchLogStore> logs;
        if (scope.contains("launchId")) {
            const auto launch = owned_launch(scope);
            std::lock_guard lock(state_mutex);
            logs = launch->log_store;
            if (!logs) fail_static("not_found");
        } else logs = log_store_for_root(context(identity(scope)).resourceRoot);
        cancellation.check();
        Json rows = Json::array();
        if (const auto launcher = logs->tail()) rows.push_back({{"name", launcher->name}, {"text", launcher->text}, {"truncated", launcher->truncated}});
        if (const auto latest = logs->latest()) rows.push_back({{"name", latest->name}, {"text", latest->text}, {"truncated", latest->truncated}});
        for (const auto& crash : logs->crashes()) rows.push_back({{"name", crash.name}, {"text", crash.text}, {"truncated", crash.truncated}});
        cancellation.check();
        result.data = {{"items", rows}};
        return result;
    }
    case ui::UiQuery::ExportTree: {
        const auto selected = identity(scope);
        const auto root = registered_root(selected.rootId);
        const auto preview = packs::preview_export(root, selected.instanceId, settings("game"), cancellation.stop_token());
        Json rows = Json::array();
        for (const auto& file : preview.files)
            rows.push_back({{"id", file.path}, {"path", file.path}, {"name", file.display_name},
                            {"group", file.group}, {"directory", file.directory}, {"selected", file.selected},
                            {"size", file.size ? Json(*file.size) : Json(nullptr)}});
        result.data = instance_scope(selected, root);
        result.data.update({{"name", preview.name}, {"minecraftVersion", preview.minecraft_version},
                            {"items", rows}, {"includedSettings", preview.instance_settings.included},
                            {"omittedSettings", preview.instance_settings.omitted}});
        return result;
    }
    case ui::UiQuery::JunkPreview:
        return action_diagnostics(ui::UiAction::ScanJunk, scope, cancellation);
    case ui::UiQuery::RuntimeInformation:
        result.data = runtime_information(*this, scope);
        return result;
    case ui::UiQuery::Onboarding:
        result.data = onboarding_snapshot();
        return result;
    }
    fail_static("invalid_argument");
}

} // namespace nickel::app
