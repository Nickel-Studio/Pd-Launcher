#include "internal.hpp"
#include <algorithm>
#include <set>
#include <unordered_map>

namespace nickel::packs {
namespace {
using namespace detail;
constexpr std::string_view directories[]{"mods", "config", "defaultconfigs", "kubejs", "scripts", "patchouli_books", "datapacks", "openloader", "global_packs", "resources", "structures", "packmenu", "paxi", "tacz", "immersive_paintings", "schematics", "replay_recordings", "replay_videos", "resourcepacks", "shaderpacks", "screenshots", "saves", "Xaero", "xaero", "XaeroWaypoints", "XaeroWorldMap", "journeymap", "voxelmap"};
constexpr std::string_view files[]{"options.txt", "optionsof.txt", "optionsshaders.txt", "servers.dat", "servers.dat_old", "hotbar.nbt", "command_history.txt", "emi.json", "patchouli_data.json"};
constexpr std::pair<std::string_view, std::string_view> config_scopes[]{
    {"config/jei/world", "jeiPersonal"}, {"config/tacz/custom", "tacz"}, {"config/openloader", "packData"},
    {"config/global_packs", "packData"}, {"config/paxi/datapacks", "packData"}, {"config/paxi/resourcepacks", "packData"},
    {"config/packmenu", "packData"}, {"config/patchouli_books", "packData"}};
std::string folded_path(std::string_view text) {
    auto key = io::ascii_lower(text);
#ifdef _WIN32
    key = instances::root_identity(key);
#endif
    return key;
}
bool directory_allowed(std::string_view group, std::string_view path) {
    const auto lower = io::ascii_lower(path);
    for (const auto& [scope, category] : config_scopes) if (scope == lower && group == category) return true;
    if (lower == "journeymap") return group == "config" || group == "maps";
    if (lower == "shaderpacks") return group == "shaderOptions";
    if (lower.starts_with("mods/")) return group == "mods" || group == "disabledMods";
    const auto slash = lower.find('/');
    const auto inferred = content_group(lower);
    if (!inferred || inferred->first != group) return false;
    if (slash != std::string::npos) {
        const auto root = lower.substr(0, slash);
        return (root == "resourcepacks" || root == "shaderpacks" || root == "saves") && lower.find('/', slash + 1) == std::string::npos;
    }
    return std::find(std::begin(directories), std::end(directories), path) != std::end(directories)
        && lower != "mods" && lower != "resourcepacks" && lower != "shaderpacks" && lower != "saves";
}
struct Selection { std::string path; std::string group; bool directory = false; };
Selection parse_selection(std::string_view text) {
    if (text.empty() || text.size() > 4096 || text.ends_with('/')) throw Error("unsafe_path", "Invalid export selection");
    if (text.starts_with("@dir/")) {
        const auto rest = text.substr(5);
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos) throw Error("unsafe_path", "Invalid export directory token");
        const auto group = rest.substr(0, slash), path = rest.substr(slash + 1);
        validate_pack_path(path);
        if (!directory_allowed(group, path)) throw Error("unsafe_content", "Export directory or group is not allowed");
        return {std::string(path), std::string(group), true};
    }
    validate_pack_path(text);
    const auto group = content_group(text);
    if (!group) throw Error("unsafe_content", "Export file is not allowed");
    return {std::string(text), group->first, false};
}
void validate_request(const ExportRequest& request) {
    if (!request.game_root.is_absolute()) throw Error("unsafe_path", "Export root must be absolute");
    instances::validate_name(request.instance_id);
    required_text(Json{{"name", request.name}}, "name");
    required_text(Json{{"version", request.version}}, "version");
    if (request.format == ExportFormat::mrpack && (request.include_launcher || request.include_personalization)) throw Error("invalid_schema", "Launcher or personalization requires portable ZIP");
    if (request.format != ExportFormat::mrpack && request.format != ExportFormat::portable) throw Error("invalid_schema", "Unknown export format");
    if (request.selections.size() > ArchiveLimits{}.entries) throw Error("limit", "Export selection quota exceeded");
    std::set<std::string> unique;
    for (const auto& selection : request.selections) {
        parse_selection(selection);
        if (!unique.insert(folded_path(selection)).second) throw Error("conflict", "Duplicate export selection");
    }
}
void push_preview(std::vector<ExportFile>& entries, const fs::path& source, std::string_view relative,
                  std::string_view group, bool selected, bool directory) {
    if (entries.size() >= ArchiveLimits{}.entries) throw Error("limit", "Export preview quota exceeded");
    validate_pack_path(relative);
    const auto token = directory ? "@dir/" + std::string(group) + "/" + std::string(relative) : std::string(relative);
    parse_selection(token);
    ExportFile entry;
    entry.path = token;
    entry.source_path = source;
    entry.group = group;
    entry.selected = selected;
    entry.directory = directory;
    entry.display_name = path_to_utf8(source.filename());
    if (!directory) {
        NativeFile input(source, NativeFile::Mode::read);
        if (input.size() > max_pack_file_bytes) throw Error("limit", "Export preview file exceeds quota");
        entry.size = input.size();
    }
    entries.push_back(std::move(entry));
}
void collect_preview(const fs::path& resource, const fs::path& source, std::string_view relative,
                     std::vector<ExportFile>& entries, std::stop_token cancel) {
    io::check_cancelled(cancel);
    const auto group = content_group(relative);
    if (!group) return;
    reject_links(source);
    if (!io::path_exists(source)) return;
    const auto native_source = io::native_path(source);
    if (fs::is_regular_file(native_source)) { push_preview(entries, source, relative, group->first, group->second, false); return; }
    if (!fs::is_directory(native_source)) throw Error("unsafe_path", "Special content file in export source");
    const auto lower = io::ascii_lower(relative);
    if (lower == "mods" || lower == "resourcepacks" || lower == "shaderpacks" || lower == "saves") {
        bool shader_options = false;
        for (const auto& child : io::directory_entries(source, false, cancel)) {
            io::check_cancelled(cancel);
            const auto child_name = std::string(relative) + "/" + path_to_utf8(child.path().filename());
            const auto inferred = content_group(child_name);
            if (!inferred) continue;
            reject_links(child.path());
            const bool directory = child.is_directory();
            if (!directory && !child.is_regular_file()) throw Error("unsafe_path", "Special content file in export source");
            if (lower == "shaderpacks" && inferred->first == "shaderOptions") { shader_options = true; continue; }
            if (directory && lower == "mods") {
                push_preview(entries, child.path(), child_name, "mods", true, true);
                push_preview(entries, child.path(), child_name, "disabledMods", false, true);
            } else push_preview(entries, child.path(), child_name, inferred->first, inferred->second, directory);
        }
        if (shader_options) push_preview(entries, source, relative, "shaderOptions", true, true);
        return;
    }
    if (lower == "journeymap") {
        push_preview(entries, source, relative, "config", true, true);
        push_preview(entries, source, relative, "maps", false, true);
        return;
    }
    push_preview(entries, source, relative, group->first, group->second, true);
    if (lower == "config") {
        for (const auto& [scope, category] : config_scopes) {
            const auto path = resource / path_from_utf8(scope);
            reject_links(path);
            if (fs::is_directory(io::native_path(path))) push_preview(entries, path, scope, category, category != "jeiPersonal", true);
        }
        for (const auto* file : {"config/emi.json", "config/patchouli_data.json", "config/iris.properties", "config/oculus.properties"}) collect_preview(resource, resource / file, file, entries, cancel);
    }
}
bool can_descend(std::string_view path, std::string_view group) {
    const auto inferred = content_group(path);
    return (inferred && inferred->first == group) || (path == "journeymap" && group == "config")
        || (path == "shaderpacks" && group == "shaderOptions") || (path.starts_with("mods/") && group == "disabledMods");
}
struct ChosenFile { std::string path; fs::path source; std::uint64_t size; };
void discover(const fs::path& path, std::string_view relative, std::string_view group, std::size_t depth,
              std::vector<ChosenFile>& chosen, std::set<std::string>& unique, std::uint64_t& total,
              std::stop_token cancel) {
    io::check_cancelled(cancel);
    if (depth > 64) throw Error("limit", "Export recursion exceeds quota");
    if (sensitive_content_path(relative)) return;
    const auto inferred = content_group(relative);
    if (!inferred && !can_descend(relative, group)) return;
    validate_pack_path(relative);
    reject_links(path);
    if (!io::path_exists(path)) throw Error("source_changed", "Selected export source disappeared");
    const auto native = io::native_path(path);
    if (fs::is_directory(native)) {
        if (!can_descend(relative, group)) return;
        for (const auto& child : io::directory_entries(path, false, cancel)) {
            const auto name = std::string(relative) + "/" + path_to_utf8(child.path().filename());
            discover(child.path(), name, group, depth + 1, chosen, unique, total, cancel);
        }
        return;
    }
    if (!fs::is_regular_file(native)) throw Error("unsafe_path", "Special selected export source");
    if (!inferred || inferred->first != group) return;
    if (!unique.insert(folded_path(relative)).second) return;
    NativeFile input(path, NativeFile::Mode::read);
    const auto size = input.size();
    if (chosen.size() >= ArchiveLimits{}.entries || size > max_pack_file_bytes || size > max_pack_bytes - total) throw Error("limit", "Selected export content exceeds quota");
    total += size;
    chosen.push_back({std::string(relative), path, size});
}
bool download_resource(std::string_view path) {
    const auto lower = io::ascii_lower(path);
    const auto group = content_group(lower);
    if (!group) return false;
    return (group->first == "mods" && (lower.ends_with(".jar") || lower.ends_with(".zip") || lower.ends_with(".litemod")))
        || ((group->first == "resourcepacks" || group->first == "shaderpacks") && lower.ends_with(".zip"));
}
}
ExportRequest export_request_from_json(const Json& value) {
    constexpr std::string_view keys[]{"dir", "versionId", "name", "version", "files", "bundleResources", "modrinthOnly", "format", "includeLauncher", "includePersonalization"};
    io::strict_keys(value, keys, keys);
    ExportRequest request;
    request.game_root = path_from_utf8(required_text(value, "dir", 4096));
    request.instance_id = required_text(value, "versionId", 160);
    request.name = required_text(value, "name");
    request.version = required_text(value, "version");
    if (!value["files"].is_array()) throw Error("invalid_schema", "Export selections must be an array");
    for (const auto& file : value["files"]) { if (!file.is_string()) throw Error("invalid_schema", "Export selection must be text"); request.selections.push_back(file.get<std::string>()); }
    for (const auto* key : {"bundleResources", "modrinthOnly", "includeLauncher", "includePersonalization"}) if (!value[key].is_boolean()) throw Error("invalid_schema", "Export flags must be booleans");
    request.bundle_resources = value["bundleResources"].get<bool>();
    request.modrinth_only = value["modrinthOnly"].get<bool>();
    request.include_launcher = value["includeLauncher"].get<bool>();
    request.include_personalization = value["includePersonalization"].get<bool>();
    const auto format = required_text(value, "format");
    if (format != "mrpack" && format != "portable") throw Error("invalid_schema", "Unknown export format");
    request.format = format == "mrpack" ? ExportFormat::mrpack : ExportFormat::portable;
    validate_request(request);
    return request;
}
ExportPreview preview_export(const fs::path& root, std::string_view id, const Json& global, std::stop_token cancel) {
    io::check_cancelled(cancel);
    const auto instance = instances::instance_detail(root, id);
    if (!instance.has_json || !instance.ready || !instance.minecraft_version) throw Error("incomplete_instance", "Instance must be ready before export");
    const auto profile = instances::load_profile(root, id);
    ExportPreview preview;
    preview.name = instance.display_name;
    preview.minecraft_version = profile ? profile->minecraft_version : *instance.minecraft_version;
    const auto kind = profile && profile->loader ? *profile->loader : instance.loader;
    if (kind != "vanilla") {
        if (kind != "forge" && kind != "neoforge" && kind != "fabric" && kind != "quilt") throw Error("unsupported_loader", "Instance loader cannot be exported as mrpack");
        const auto build = profile ? profile->loader_version : instance.loader_version.value_or("");
        if (build.empty()) throw Error("incomplete_instance", "Exact loader build is required for export");
        preview.loader = PackLoader{kind, build};
    }
    try {
        const auto settings = instances::read_local_settings(root, id);
        preview.instance_settings = instances::project_portable_settings(settings.value);
    } catch (const nickel::ErrorException& failure) { preview.instance_settings_error = failure.error().code; }
    const auto resource = instances::resource_directory(root, id, global);
    std::set<std::string> visited;
    for (const auto directory : directories) {
        const auto path = resource / path_from_utf8(directory);
        if (!visited.insert(folded_path(path_to_utf8(path))).second) continue;
        collect_preview(resource, path, directory, preview.files, cancel);
    }
    for (const auto file : files) collect_preview(resource, resource / path_from_utf8(file), file, preview.files, cancel);
    std::sort(preview.files.begin(), preview.files.end(), [](const auto& left, const auto& right) { return std::tie(left.group, left.display_name, left.path) < std::tie(right.group, right.display_name, right.path); });
    return preview;
}
void export_pack(const ExportRequest& request, const fs::path& destination, const Json& global,
                  const ExportServices& services, const instances::OnboardingGate& gate, std::stop_token cancel) {
    gate.require_finished();
    io::check_cancelled(cancel);
    validate_request(request);
    if (!services.archive) throw Error("missing_service", "Export requires the shared archive service");
    if (!destination.is_absolute()) throw Error("unsafe_path", "Export destination must be absolute");
    reject_links(destination);
    if (io::path_exists(destination)) throw Error("conflict", "Export destination already exists");
    const auto preview = preview_export(request.game_root, request.instance_id, global, cancel);
    if (request.format == ExportFormat::portable && preview.instance_settings_error) throw Error("unsafe_settings", "Instance settings could not be safely read for portable export");
    const auto resource = instances::resource_directory(request.game_root, request.instance_id, global);
    std::vector<ChosenFile> chosen;
    std::set<std::string> unique;
    std::uint64_t total = 0;
    for (const auto& text : request.selections) {
        io::check_cancelled(cancel);
        const auto selection = parse_selection(text);
        const auto allowed = std::find_if(preview.files.begin(), preview.files.end(), [&](const auto& entry) { return entry.path == text; });
        fs::path source;
        if (allowed != preview.files.end()) source = allowed->source_path;
        else if (!selection.directory) {
            const auto parent = std::find_if(preview.files.begin(), preview.files.end(), [&](const auto& entry) {
                if (!entry.directory || entry.group != selection.group) return false;
                return within(selection.path, parse_selection(entry.path).path);
            });
            if (parent == preview.files.end()) throw Error("source_changed", "Export selection is absent from current preview");
            source = resource / path_from_utf8(selection.path);
        } else throw Error("source_changed", "Export directory selection is absent from current preview");
        if (selection.directory != fs::is_directory(io::native_path(source))) throw Error("source_changed", "Export selection type changed");
        discover(source, selection.path, selection.group, 0, chosen, unique, total, cancel);
    }
    std::sort(chosen.begin(), chosen.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    Workspace workspace(destination.parent_path());
    std::vector<ArchiveInput> payload_inputs;
    PackManifest manifest;
    manifest.name = request.name;
    manifest.version = request.version;
    manifest.minecraft_version = preview.minecraft_version;
    manifest.loader = preview.loader;
    for (const auto& file : chosen) {
        io::check_cancelled(cancel);
        const auto copied = workspace.directory() / "snapshot" / path_from_utf8(file.path);
        if (copy_file(file.source, copied, max_pack_file_bytes, cancel) != file.size) throw Error("source_changed", "Selected file changed after preview");
        validate_content_file(copied, file.path, request.format == ExportFormat::portable);
        bool external = false;
        if (!request.bundle_resources && download_resource(file.path)) {
            if (!services.resolve_resource) throw Error("missing_service", "External resource export requires the community resolver");
            auto resolved = services.resolve_resource(copied, file.path, cancel);
            io::check_cancelled(cancel);
            if (resolved) {
                if (resolved->path != file.path) throw Error("integrity", "Resolved resource target differs from selected file");
                verify_pack_file(*resolved, copied, cancel);
                if (resolved->downloads.empty() || resolved->downloads.size() > 16)
                    throw Error("integrity", "Resolved resource requires bounded provider download URLs");
                for (const auto& url : resolved->downloads) {
                    validate_download_url(url);
                    const bool modrinth = url.starts_with("https://cdn.modrinth.com/");
                    if (request.modrinth_only && !modrinth)
                        throw Error("unsafe_url", "Modrinth-only export resolved a different download provider");
                    if (services.validate_resource_download_url) services.validate_resource_download_url(url);
                    else if (!modrinth) throw Error("unsafe_url", "Resource download requires explicit provider approval");
                }
                manifest.files.push_back(std::move(*resolved));
                external = true;
            } else if (request.modrinth_only) throw Error("resource_not_found", "Selected resource is not available from Modrinth; bundle it or deselect it");
        }
        if (!external) payload_inputs.push_back({"overrides/" + file.path, copied, {}});
    }
    payload_inputs.push_back({std::string(mrpack_index), {}, mrpack_manifest_to_json(manifest).dump(2)});
    const auto payload = workspace.directory() / "pack.mrpack";
    services.archive->write(payload, payload_inputs, {}, cancel);
    io::check_cancelled(cancel);
    const auto verification = inspect_pack(payload, request.instance_id, *services.archive, cancel);
    if (verification.manifest.name != request.name || verification.manifest.minecraft_version != preview.minecraft_version) throw Error("integrity", "Exported mrpack did not verify");
    const auto staged = request.format == ExportFormat::portable ? workspace.directory() / "portable.zip" : payload;
    if (request.format == ExportFormat::portable) write_portable(request, payload, preview.instance_settings, staged, workspace.directory(), services, cancel);
    services.archive->release(staged);
    gate.require_finished();
    io::check_cancelled(cancel);
    const auto digest = hash_file(staged, "sha256", cancel);
    io::DirectoryLock publication(destination.parent_path());
    io::copy_no_replace(staged, destination, max_pack_bytes, cancel, digest);
}
} // namespace nickel::packs
