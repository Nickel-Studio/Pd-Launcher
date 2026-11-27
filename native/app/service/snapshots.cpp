#include "app/service/snapshots.hpp"
#include "app/service/public_dto.hpp"

#include "core/diagnostics/logs.hpp"
#include "core/instances/instances.hpp"

#include <algorithm>
#include <set>

namespace nickel::app {

Json default_app_settings() {
    return {{"themeMode", "dark"}, {"accent", "mono"}, {"titlebarTint", false},
            {"backgroundImage", nullptr}, {"backgroundBlur", 0}, {"backgroundOpacity", 0.5},
            {"privacyMode", false}, {"versionListSource", "official_auto"},
            {"fileDownloadSource", "official_auto"}, {"communityApiSource", "official_auto"},
            {"maxDownloadThreads", std::uint64_t(10)}, {"downloadLimitBps", std::uint64_t(0)},
            {"verifyMicrosoftSsl", true}, {"offlineUsername", "Player"}, {"accountType", "microsoft"},
            {"littleskinServer", "https://littleskin.cn"}, {"memoryMb", std::uint64_t(4096)},
            {"instanceNamingOptimize", false},
            {"modrinthApiMirror", "https://mod.mcimirror.top/modrinth/v2/"},
            {"curseforgeApiMirror", "https://mod.mcimirror.top/curseforge/v1/"},
            {"communityFileMirror", "https://mod.mcimirror.top/"},
            {"customTheme", {{"hue", 174}, {"saturation", 39}, {"lightness", 44},
                             {"titlebarHueShift", 0}, {"decoration", "lattice"}, {"decorationOpacity", 0.35}}}};
}

Json effective_settings_json(const instances::EffectiveSettings& value) {
    return {{"isolation", value.isolation}, {"javaPath", value.java_path},
            {"windowTitle", value.window_title}, {"customInfo", value.custom_info},
            {"visibility", value.visibility}, {"priority", value.priority},
            {"windowMode", value.window_mode}, {"width", value.width}, {"height", value.height},
            {"memoryMode", "manual"}, {"memoryMb", value.memory_mb},
            {"highPerformanceGpu", value.high_performance_gpu}, {"disableWrapper", value.disable_wrapper},
            {"disableLwjglAgent", value.disable_lwjgl_agent}, {"offlineSkin", value.offline_skin},
            {"offlineSkinName", value.offline_skin_name}, {"offlineSkinPath", value.offline_skin_path},
            {"offlineSkinSlim", value.offline_skin_slim}, {"jvmArgs", value.jvm_args},
            {"gameArgs", value.game_args}, {"preLaunchCommand", value.pre_launch_command},
            {"memoryManagement", value.memory_management}, {"disableFileCheck", value.disable_file_check},
            {"disableModUpdates", value.disable_mod_updates}, {"serverLogin", value.server_login},
            {"serverName", value.server_name}, {"authServer", value.auth_server},
            {"serverAddress", value.server_address}, {"serverRegistration", value.server_registration}};
}

Json instance_scope(const resources::InstanceIdentity& selected, const std::filesystem::path& root) {
    return {{"root", instances::path_to_utf8(root)}, {"versionId", selected.instanceId}};
}

Json account_dto(const accounts::PublicAccount& account, bool privacy) {
    Json result{{"id", account.id}, {"type", accounts::account_type_name(account.type)},
                {"name", account.name}, {"uuid", account.uuid}, {"avatarKey", "account:" + account.id}};
    if (account.server) result["server"] = *account.server;
    if (account.email) {
        const auto separator = account.email->find('@');
        result["emailMasked"] = privacy ? "***" : separator == std::string::npos ? "***"
            : account.email->substr(0, std::min<std::size_t>(separator, 1)) + "***" + account.email->substr(separator);
    } else result["emailMasked"] = "";
    return result;
}

Json project_dto(const resources::Project& project) {
    return {{"id", project.id}, {"projectId", project.id}, {"slug", project.slug},
            {"provider", resources::sourceName(project.source)}, {"kind", resources::kindName(project.kind)},
            {"title", project.title}, {"name", project.title}, {"description", project.summary},
            {"author", project.author}, {"downloads", project.downloads},
            {"gameVersions", project.gameVersions}, {"loaders", project.loaders},
            {"modifiedAt", project.modifiedAt}, {"revision", project.revision},
            {"iconKey", "project:" + resources::projectIdentity(project)}};
}

Json version_dto(const resources::ProjectVersion& version) {
    Json files = Json::array();
    for (const auto& file : version.files)
        files.push_back({{"name", file.fileName}, {"fileName", file.fileName}, {"size", file.sizeBytes},
                         {"primary", file.primary}, {"sha1", file.sha1}, {"sha512", file.sha512}});
    const auto type = version.releaseType == resources::ReleaseType::Release ? "release"
        : version.releaseType == resources::ReleaseType::Beta ? "beta"
        : version.releaseType == resources::ReleaseType::Alpha ? "alpha" : "unknown";
    return {{"id", version.id}, {"versionId", version.id}, {"projectId", version.projectId},
            {"provider", resources::sourceName(version.source)}, {"name", version.name},
            {"version", version.versionNumber}, {"versionNumber", version.versionNumber},
            {"gameVersions", version.gameVersions}, {"loaders", version.loaders},
            {"publishedAt", version.publishedAt}, {"type", type}, {"files", files},
            {"revision", version.revision}};
}

std::string safe_public_message(std::string_view code) {
    return public_error(code).message.toUtf8().toStdString();
}

Json task_dto(const tasks::Snapshot& snapshot) {
    Json result = tasks::to_json(snapshot);
    result["id"] = snapshot.id;
    result["taskStatus"] = result["status"];
    switch (snapshot.status) {
    case tasks::TaskStatus::Running: result["status"] = snapshot.stage == "queued" ? "queued" : "running"; break;
    case tasks::TaskStatus::Cancelling: result["status"] = "cancelling"; break;
    case tasks::TaskStatus::Committing: result["status"] = "committing"; break;
    case tasks::TaskStatus::Succeeded: result["status"] = "completed"; break;
    case tasks::TaskStatus::Cancelled: result["status"] = "cancelled"; break;
    case tasks::TaskStatus::Failed: result["status"] = "error"; break;
    }
    result["terminal"] = snapshot.status == tasks::TaskStatus::Succeeded || snapshot.status == tasks::TaskStatus::Cancelled
        || snapshot.status == tasks::TaskStatus::Failed;
    result["canCancel"] = snapshot.status == tasks::TaskStatus::Running && !snapshot.cancel_requested;
    result["label"] = diagnostics::bounded_safe_line(snapshot.label);
    result["title"] = result["label"];
    result["detail"] = diagnostics::bounded_safe_line(snapshot.detail);
    result["stages"] = Json::array();
    if (!snapshot.stage.empty()) result["stages"].push_back({{"name", diagnostics::bounded_safe_line(snapshot.stage)},
        {"detail", result["detail"]}, {"completed", snapshot.status == tasks::TaskStatus::Succeeded}});
    result["speedBps"] = snapshot.speed_bytes_per_second ? Json(*snapshot.speed_bytes_per_second) : Json(nullptr);
    result["message"] = snapshot.error_code.empty() ? "" : safe_public_message(snapshot.error_code);
    result["error"] = snapshot.error_code.empty() ? Json(nullptr)
        : Json{{"code", public_error(snapshot.error_code).code.toStdString()},
               {"message", safe_public_message(snapshot.error_code)}};
    return result;
}

std::uint64_t unix_milliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint32_t bounded_unsigned(const Json& value, std::string_view field, std::uint32_t fallback,
                               std::uint32_t maximum) {
    const auto found = value.find(std::string(field));
    if (found == value.end()) return fallback;
    if (!found->is_number_integer() || (found->is_number_integer() && !found->is_number_unsigned()
                                      && found->get<std::int64_t>() < 0)) fail_static("invalid_argument");
    const auto number = found->get<std::uint64_t>();
    if (number > maximum) fail_static("invalid_argument");
    return static_cast<std::uint32_t>(number);
}

resources::Strings selected_ids(const Json& value, std::string_view field) {
    const auto found = value.find(std::string(field));
    if (found == value.end() || !found->is_array() || found->empty() || found->size() > 1024)
        fail_static("invalid_argument");
    resources::Strings result;
    std::set<std::string> unique;
    for (const auto& entry : *found) {
        if (!entry.is_string()) fail_static("invalid_argument");
        const auto id = entry.get<std::string>();
        if (id.empty() || id.size() > 512 || id.find('\0') != std::string::npos || !unique.insert(id).second)
            fail_static("invalid_argument");
        result.push_back(id);
    }
    return result;
}

resources::Source selected_source(const Json& value) {
    const auto source = bounded_text(value, "provider", 32);
    if (resources::lower(source) == "modrinth") return resources::Source::Modrinth;
    if (resources::lower(source) == "curseforge") return resources::Source::CurseForge;
    fail_static("invalid_argument");
}

resources::ResourceKind selected_kind(const Json& value) {
    return resources::parseKind(bounded_text(value, "kind", 32));
}

} // namespace nickel::app
