#include "internal.hpp"
#include "core/config/paths.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>

namespace nickel::packs {
namespace {
using namespace detail;
std::mutex bootstrap_mutex;
std::set<std::string> active_bootstraps;
constexpr std::string_view backgrounds[]{"png", "jpg", "jpeg", "webp", "gif", "bmp", "avif"};
constexpr std::string_view theme_modes[]{"system", "light", "dark"};
constexpr std::string_view accents[]{"mono", "emerald", "blue", "violet", "amber", "rose", "nickel", "copper", "abyss", "paper", "fortyTwo", "custom"};
constexpr std::string_view decorations[]{"none", "lattice", "ribbons", "orbits", "folds"};
bool background_path(std::string_view path) {
    for (auto extension : backgrounds) if (path == "Nickel/background." + std::string(extension)) return true;
    return false;
}
void bounded_number(const Json& value, std::string_view field, double minimum, double maximum) {
    const auto it = value.find(std::string(field));
    if (it == value.end()) return;
    if (!it->is_number()) throw Error("invalid_schema", "Portable appearance requires a finite number");
    const auto number = it->get<double>();
    if (!std::isfinite(number) || number < minimum || number > maximum) throw Error("invalid_schema", "Portable appearance number is outside its range");
}
template<std::size_t N> void enum_value(const Json& value, std::string_view field, const std::string_view (&allowed)[N]) {
    if (!value.contains(std::string(field))) return;
    const auto text = required_text(value, field);
    if (std::find(std::begin(allowed), std::end(allowed), text) == std::end(allowed)) throw Error("invalid_schema", "Invalid portable appearance enum");
}
Json select_personalization(const Json& input) {
    Json result = Json::object();
    if (!input.is_object()) return result;
    for (const auto key : {"themeMode", "accent", "titlebarTint", "backgroundBlur", "backgroundOpacity"}) if (input.contains(key)) result[key] = input[key];
    if (input.contains("customTheme") && input["customTheme"].is_object()) {
        const auto& incoming = input["customTheme"];
        Json theme{{"hue", 174.0}, {"saturation", 39.0}, {"lightness", 44.0}, {"titlebarHueShift", 0.0}, {"decoration", "lattice"}, {"decorationOpacity", 0.35}};
        for (const auto* key : {"hue", "saturation", "lightness", "titlebarHueShift", "decoration", "decorationOpacity"}) if (incoming.contains(key)) theme[key] = incoming[key];
        result["customTheme"] = std::move(theme);
    } else if (io::string_at(input, "accent") == "custom") result["customTheme"] = {{"hue", 174.0}, {"saturation", 39.0}, {"lightness", 44.0}, {"titlebarHueShift", 0.0}, {"decoration", "lattice"}, {"decorationOpacity", 0.35}};
    return result;
}
fs::path canonical_portable_root(const fs::path& root) {
    if (!root.is_absolute()) throw Error("unsafe_path", "Portable root must be absolute");
    reject_links(root);
    if (!fs::is_directory(io::native_path(root))) throw Error("unsafe_path", "Portable root is not a directory");
    return io::canonical_path(root);
}
std::optional<std::string> committed_version(const fs::path& root, const Json& manifest) {
    const auto game = root / ".minecraft";
    const auto versions = game / "versions";
    reject_links(versions);
    if (!fs::is_directory(io::native_path(versions))) return {};
    for (const auto& entry : instances::scan_instances(game)) {
        if (!entry.ready || !entry.has_json) continue;
        const auto path = instances::pick_version_json(entry.directory, entry.id);
        if (!path) continue;
        try {
            const auto value = io::parse_strict_json(io::read_bounded(*path, instances::descriptor_max_bytes), instances::descriptor_max_bytes);
            if (value.contains("nickelPack") && value["nickelPack"].is_object() && io::string_at(value["nickelPack"], "portablePackageId") == io::string_at(manifest, "packageId")) return entry.id;
        } catch (const Error&) { continue; }
    }
    return {};
}
void save_completed(const fs::path& root, const Json& manifest, std::string_view id) {
    instances::validate_name(id);
    io::atomic_write(root / "Nickel/portable-import.json", Json{{"packageId", manifest["packageId"]}, {"versionId", id}}.dump(2));
}
struct BootstrapGuard {
    std::string identity;
    explicit BootstrapGuard(const fs::path& root) : identity(instances::root_identity(path_to_utf8(root))) {
        std::lock_guard lock(bootstrap_mutex);
        if (!active_bootstraps.insert(identity).second) throw Error("conflict", "Portable bootstrap is already running");
    }
    ~BootstrapGuard() { std::lock_guard lock(bootstrap_mutex); active_bootstraps.erase(identity); }
};
std::optional<Json> read_instance_settings(const fs::path& root) {
    const auto value = io::read_optional_json(root / "Nickel/instance-settings.json", instances::settings_max_bytes);
    return value ? std::optional<Json>(instances::validate_portable_settings(*value)) : std::nullopt;
}
}
Json validate_portable_manifest(const Json& manifest) {
    if (manifest.dump().size() > portable_json_max_bytes) throw Error("invalid_schema", "Portable marker exceeds size limit");
    try {
        const auto canonical = nickel::parse_portable_manifest(manifest.dump());
        instances::validate_name(canonical.instance_name);
    } catch (const instances::Error&) { throw; }
    catch (const nickel::ErrorException&) { throw Error("invalid_schema", "Invalid or unsupported portable schema"); }
    return manifest;
}
std::optional<Json> read_portable_manifest(const fs::path& root) {
    if (!root.is_absolute()) throw Error("unsafe_path", "Portable marker root must be absolute");
    reject_links(root / portable_manifest_file);
    const auto value = io::read_optional_json(root / portable_manifest_file, portable_json_max_bytes);
    return value ? std::optional<Json>(validate_portable_manifest(*value)) : std::nullopt;
}
Json validate_personalization(const Json& input, const Json& manifest) {
    validate_portable_manifest(manifest);
    constexpr std::string_view keys[]{"themeMode", "accent", "customTheme", "titlebarTint", "backgroundBlur", "backgroundOpacity", "backgroundImage"};
    io::strict_keys(input, keys);
    if (input.dump().size() > portable_json_max_bytes) throw Error("limit", "Portable appearance exceeds size quota");
    enum_value(input, "themeMode", theme_modes);
    enum_value(input, "accent", accents);
    if (input.contains("titlebarTint") && !input["titlebarTint"].is_null() && !input["titlebarTint"].is_boolean()) throw Error("invalid_schema", "Portable titlebar tint must be boolean");
    bounded_number(input, "backgroundBlur", 0, 40);
    bounded_number(input, "backgroundOpacity", 0, 1);
    if (input.contains("customTheme") && !input["customTheme"].is_null()) {
        const auto& theme = input["customTheme"];
        constexpr std::string_view fields[]{"hue", "saturation", "lightness", "titlebarHueShift", "decoration", "decorationOpacity"};
        io::strict_keys(theme, fields, fields);
        bounded_number(theme, "hue", 0, 360);
        bounded_number(theme, "saturation", 0, 100);
        bounded_number(theme, "lightness", 0, 100);
        bounded_number(theme, "titlebarHueShift", -180, 180);
        bounded_number(theme, "decorationOpacity", 0, 1);
        enum_value(theme, "decoration", decorations);
    }
    if (input.contains("backgroundImage") && !input["backgroundImage"].is_null() && !background_path(required_text(input, "backgroundImage"))) throw Error("unsafe_path", "Portable background must use the fixed relative filename");
    return input;
}
std::optional<PortableBootstrap> load_portable_bootstrap(const fs::path& directory, ArchiveBackend& backend, std::stop_token cancel) {
    io::check_cancelled(cancel);
    const auto manifest = read_portable_manifest(directory);
    if (!manifest) return {};
    PortableBootstrap bootstrap;
    bootstrap.root = canonical_portable_root(directory);
    bootstrap.manifest = *manifest;
    bootstrap.payload = bootstrap.root / "Nickel/pack.mrpack";
    bootstrap.game_root = bootstrap.root / ".minecraft";
    bootstrap.data_root = bootstrap.root / "Nickel";
    reject_links(bootstrap.game_root);
    reject_links(bootstrap.data_root);
    const auto plan = inspect_pack(bootstrap.payload, required_text(*manifest, "instanceName"), backend, cancel);
    if (plan.manifest.name != required_text(*manifest, "name") || plan.manifest.version != required_text(*manifest, "version")) throw Error("integrity", "Portable marker differs from its mrpack payload");
    if (manifest->contains("personalization") && !(*manifest)["personalization"].is_null()) {
        const auto path = bootstrap.root / "Nickel/personalization.json";
        bootstrap.personalization = validate_personalization(io::parse_strict_json(io::read_bounded(path, portable_json_max_bytes), portable_json_max_bytes), *manifest);
        if (const auto background = io::string_at(*bootstrap.personalization, "backgroundImage")) validate_background(bootstrap.root / path_from_utf8(*background));
    }
    bootstrap.instance_settings = read_instance_settings(bootstrap.root);
    if (read_portable_manifest(bootstrap.root) != manifest) throw Error("source_changed", "Portable marker changed during validation");
    return bootstrap;
}
std::optional<BootstrapStatus> portable_bootstrap_status(const fs::path& root) {
    const auto manifest = read_portable_manifest(root);
    if (!manifest) return {};
    const auto canonical = canonical_portable_root(root);
    const auto id = committed_version(canonical, *manifest);
    const auto identity = instances::root_identity(path_to_utf8(canonical));
    bool active = false;
    { std::lock_guard lock(bootstrap_mutex); active = active_bootstraps.contains(identity); }
    BootstrapStatus status{required_text(*manifest, "packageId"), required_text(*manifest, "name"), canonical / ".minecraft", id ? "installed" : active ? "running" : "pending", id};
    return status;
}
std::optional<ImportResult> start_portable_bootstrap(const fs::path& root, const InstallServices& services,
                                                    const instances::OnboardingGate& gate, std::stop_token cancel) {
    gate.require_finished();
    io::check_cancelled(cancel);
    const auto manifest = read_portable_manifest(root);
    if (!manifest) return {};
    if (!services.archive) throw Error("missing_service", "Portable bootstrap requires the shared archive service");
    const auto canonical = canonical_portable_root(root);
    BootstrapGuard guard(canonical);
    const auto data = canonical / "Nickel";
    reject_links(data);
    io::create_directories(data);
    io::DirectoryLock cross_process(data);
    if (const auto existing = committed_version(canonical, *manifest)) {
        save_completed(canonical, *manifest, *existing);
        return ImportResult{*existing, canonical / ".minecraft/versions" / path_from_utf8(*existing)};
    }
    auto bootstrap = load_portable_bootstrap(canonical, *services.archive, cancel);
    if (!bootstrap || bootstrap->manifest != *manifest) throw Error("source_changed", "Portable marker changed before bootstrap");
    seed_personalization(*bootstrap);
    auto plan = inspect_pack(bootstrap->payload, required_text(*manifest, "instanceName"), *services.archive, cancel);
    plan.source["portablePackageId"] = (*manifest)["packageId"];
    plan.instance_settings = bootstrap->instance_settings;
    gate.require_finished();
    if (read_portable_manifest(canonical) != manifest) throw Error("source_changed", "Portable marker changed before import");
    const auto result = import_pack(std::move(plan), bootstrap->game_root, services, gate, cancel);
    save_completed(canonical, *manifest, result.instance_id);
    return result;
}
void seed_personalization(const PortableBootstrap& bootstrap) {
    validate_portable_manifest(bootstrap.manifest);
    const auto settings = bootstrap.data_root / "settings.json";
    reject_links(settings);
    if (io::path_exists(settings) || !bootstrap.personalization) return;
    if (read_portable_manifest(bootstrap.root) != std::optional<Json>(bootstrap.manifest)) throw Error("source_changed", "Portable marker changed before appearance seed");
    const auto value = validate_personalization(*bootstrap.personalization, bootstrap.manifest);
    if (const auto background = io::string_at(value, "backgroundImage")) validate_background(bootstrap.root / path_from_utf8(*background));
    try { io::atomic_write(settings, value.dump(2), false); }
    catch (const Error& failure) { if (failure.code() != "conflict" || !io::path_exists(settings)) throw; }
}
Json personalization_for_display(const PortableBootstrap& bootstrap, const Json& input) {
    auto value = validate_personalization(input, bootstrap.manifest);
    if (const auto relative = io::string_at(value, "backgroundImage")) {
        const auto path = bootstrap.root / path_from_utf8(*relative);
        validate_background(path);
        value["backgroundImage"] = path_to_utf8(path);
    }
    return value;
}
Json personalization_for_storage(const PortableBootstrap& bootstrap, const Json& input) {
    auto value = input;
    if (const auto text = io::string_at(value, "backgroundImage")) {
        const auto path = path_from_utf8(*text);
        if (path.is_absolute()) {
            reject_links(path);
            const auto relative = path.lexically_relative(bootstrap.root);
            if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") throw Error("unsafe_path", "Portable appearance cannot store a host path");
            value["backgroundImage"] = path_to_utf8(relative);
        }
    }
    return validate_personalization(value, bootstrap.manifest);
}
} // namespace nickel::packs

namespace nickel::packs::detail {
std::optional<Json> settings_for_payload(const fs::path& file) {
    const auto data = file.parent_path();
    const auto root = data.parent_path();
    if (file.filename() != "pack.mrpack" || data.filename() != "Nickel" || root.empty()) return {};
    const auto manifest = read_portable_manifest(root);
    if (!manifest) return {};
    if (file.lexically_normal() != (root / "Nickel/pack.mrpack").lexically_normal()) return {};
    return read_instance_settings(root);
}
void write_portable(const ExportRequest& request, const fs::path& payload, const instances::SettingsProjection& settings,
                    const fs::path& target, const fs::path& workspace, const ExportServices& services, std::stop_token cancel) {
    io::check_cancelled(cancel);
    Json manifest{{"schemaVersion", 1}, {"format", portable_format}, {"packageId", io::random_id()}, {"name", request.name},
                  {"version", request.version}, {"instanceName", request.instance_id}, {"payload", "Nickel/pack.mrpack"},
                  {"gameDirectory", ".minecraft"}, {"dataDirectory", "Nickel"}};
    std::vector<ArchiveInput> files{{"Nickel/pack.mrpack", payload, {}}};
    if (!settings.settings.empty()) files.push_back({"Nickel/instance-settings.json", {}, instances::validate_portable_settings(settings.settings).dump(2)});
    if (request.include_personalization) {
        manifest["personalization"] = "Nickel/personalization.json";
        auto appearance = select_personalization(services.personalization);
        if (services.background) {
            const auto extension = io::ascii_lower(path_to_utf8(services.background->extension()));
            if (extension.empty()) throw Error("unsafe_content", "Portable background requires an image extension");
            const auto relative = "Nickel/background" + extension;
            if (!background_path(relative)) throw Error("unsafe_content", "Unsupported portable background extension");
            const auto copied = workspace / "appearance" / path_from_utf8(relative);
            copy_file(*services.background, copied, background_max_bytes, cancel);
            validate_background(copied);
            appearance["backgroundImage"] = relative;
            files.push_back({relative, copied, {}});
        }
        files.push_back({"Nickel/personalization.json", {}, validate_personalization(appearance, manifest).dump(2)});
    }
    if (request.include_launcher) {
        if (!services.launcher || !services.validate_launcher_artifact || services.launcher_notices.empty()) throw Error("missing_service", "Portable launcher requires a verified release artifact and license/source inputs");
        const auto os = services.launcher_os;
        if (os != "windows" && os != "linux") throw Error("unsupported_platform", "Portable schema 1 cannot copy a macOS app as one file");
        const auto name = os == "windows" ? "Nickel.exe" : "Nickel-launcher";
        manifest["launcher"] = {{"path", name}, {"os", os}, {"arch", services.launcher_arch}};
        validate_portable_manifest(manifest);
        services.validate_launcher_artifact(*services.launcher, cancel);
        const auto copied = workspace / "launcher" / name;
        copy_file(*services.launcher, copied, max_pack_file_bytes, cancel);
        services.validate_launcher_artifact(copied, cancel);
        files.push_back({name, copied, {}});
        constexpr std::string_view notices[]{"LICENSE", "LICENSE-ASSETS", "NOTICE.md", "SOURCE.txt"};
        std::set<std::string> provided;
        for (const auto& notice : services.launcher_notices) {
            validate_archive_path(notice.name);
            if (notice.file || (std::find(std::begin(notices), std::end(notices), notice.name) == std::end(notices) && !notice.name.starts_with("LICENSES/"))) throw Error("unsafe_content", "Portable release notices must be trusted embedded white-list inputs");
            if (notice.bytes.size() > 4 * 1024 * 1024 || !provided.insert(notice.name).second) throw Error("limit", "Invalid portable notice input");
            files.push_back(notice);
        }
        for (auto required : notices) if (!provided.contains(std::string(required))) throw Error("missing_service", "Portable launcher is missing a required license or source notice");
    }
    validate_portable_manifest(manifest);
    files.push_back({std::string(portable_manifest_file), {}, manifest.dump(2)});
    files.push_back({"README-portable.txt", {}, "Palladium portable pack (schema 1)\nExtract the complete ZIP. Open the verified launcher, accept the Minecraft EULA and changelog, then import once. Keep Nickel and .minecraft beside the executable; these legacy directory names are required for schema 1 compatibility. Accounts, keys, Java paths and pre-launch commands are not included. macOS requires the complete signed app from the release.\n"});
    services.archive->write(target, files, {}, cancel);
    const auto entries = services.archive->inspect(target, {}, cancel);
    validate_archive_entries(entries, {});
    const auto read_manifest = io::parse_strict_json(services.archive->read(target, portable_manifest_file, portable_json_max_bytes, cancel), portable_json_max_bytes);
    if (validate_portable_manifest(read_manifest) != manifest) throw Error("integrity", "Portable envelope did not round-trip its exact manifest");
    services.archive->release(target);
}
} // namespace nickel::packs::detail
