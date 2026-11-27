#include "instances.hpp"
#include "settings.hpp"
#include "domain_io.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <tuple>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nickel::instances {
namespace {
std::recursive_mutex domain_mutex;
std::set<std::string> active_transactions;
std::string folded(std::string_view value) {
#ifdef _WIN32
    const auto wide = path_from_utf8(value).native();
    if (wide.empty()) return {};
    const auto size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
                                    static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
    if (size > 0) {
        std::wstring lower(static_cast<std::size_t>(size), L'\0');
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
                      static_cast<int>(wide.size()), lower.data(), size, nullptr, nullptr, 0);
        return path_to_utf8(fs::path(lower));
    }
#endif
    return detail::ascii_lower(value);
}
bool occupied(const fs::path& parent, std::string_view name) {
    const auto target = folded(name);
    for (const auto& entry : detail::directory_entries(parent)) {
        if (folded(path_to_utf8(entry.path().filename())) == target) return true;
    }
    return false;
}
bool valid_build(std::string_view build) {
    return !build.empty() && build.size() <= 160 && build != "latest" && build != "recommended"
        && std::all_of(build.begin(), build.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '+';
        });
}
bool known_loader(std::string_view value) {
    constexpr std::array values{"forge", "neoforge", "fabric", "legacyfabric", "quilt", "liteloader", "optifine"};
    const auto lower = detail::ascii_lower(value);
    return std::find(values.begin(), values.end(), lower) != values.end();
}
std::string candidate_name(std::string_view input, std::size_t suffix) {
    std::string base = detail::trim(input);
    for (char& c : base) {
        if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) c = '_';
    }
    for (auto p = base.find(".."); p != std::string::npos; p = base.find("..")) base.replace(p, 2, "_");
    while (!base.empty() && (base.back() == '.' || base.back() == ' ')) base.pop_back();
    if (base.empty()) base = "Modpack";
    const std::string ending = suffix == 0 ? "" : "\xef\xbc\x88" + std::to_string(suffix) + "\xef\xbc\x89";
    const auto truncate = [&] {
        while (base.size() + ending.size() > 160) {
            base.pop_back();
            while (!base.empty() && (static_cast<unsigned char>(base.back()) & 0xc0) == 0x80) base.pop_back();
            if (!base.empty() && static_cast<unsigned char>(base.back()) >= 0xc0) base.pop_back();
        }
        while (!base.empty() && (base.back() == '.' || base.back() == ' ')) base.pop_back();
    };
    truncate();
    try { validate_name(base); } catch (...) { base.insert(base.begin(), '_'); truncate(); }
    return base + ending;
}
Json registry_read(const fs::path& file) {
    try {
        const auto value = detail::read_optional_json(file, settings_max_bytes);
        return value.value_or(Json::object());
    } catch (const Error& error) {
        if (error.code() == "unsafe_path") throw;
        return Json::object();
    }
}
Registry read_registry(const fs::path& config) {
    return registry_from_text(registry_read(config / "instances.json").dump());
}
void write_registry(const fs::path& config, const Registry& registry) {
    const auto text = registry_to_json(registry).dump();
    if (text.size() > settings_max_bytes) throw Error("limit", "Instance registry exceeds 256 KiB");
    detail::atomic_write(config / "instances.json", text);
}
bool is_april_fools(std::string_view id) {
    constexpr std::array<std::string_view, 7> versions{"15w14a", "3d shareware v1.34", "1.rv-pre1", "20w14infinite", "22w13oneblockat", "23w13a_or_b", "24w14potato"};
    const auto lower = detail::ascii_lower(detail::trim(id));
    return std::find(versions.begin(), versions.end(), lower) != versions.end();
}
bool metadata_version(std::string_view value) {
    static const std::regex pattern(R"(^(?:[abc]?[0-9]+\.[0-9]+(?:\.[0-9]+)?(?:_[0-9]+)?(?:-(?:pre-?|rc-?|snapshot-)[0-9]+)?|[0-9]{2}w[0-9]{2}[a-z]|(?:rd|inf)-[0-9]+)$)");
    return is_april_fools(value) || value == "2.0" || value == "22w13oneblockatatime"
        || value == "20w14\xe2\x88\x9e" || std::regex_match(value.begin(), value.end(), pattern);
}
std::vector<std::string> libraries(const Json& value) {
    std::vector<std::string> names;
    auto it = value.find("libraries");
    if (it != value.end() && it->is_array()) {
        for (const auto& library : *it) {
            if (auto name = detail::string_at(library, "name")) names.push_back(*name);
            if (names.size() >= 10000) break;
        }
    }
    return names;
}
std::optional<std::string> artifact_version(const std::vector<std::string>& names, std::string_view coordinate) {
    const auto prefix = std::string(coordinate) + ':';
    for (const auto& name : names) {
        if (name.starts_with(prefix)) {
            auto version = name.substr(prefix.size());
            const auto separator = version.find(':');
            return version.substr(0, separator);
        }
    }
    return {};
}
std::optional<std::string> game_argument(const Json& value, std::string_view key) {
    auto arguments = value.find("arguments");
    if (arguments == value.end() || !arguments->is_object()) return {};
    auto game = arguments->find("game");
    if (game == arguments->end() || !game->is_array()) return {};
    for (std::size_t i = 0; i + 1 < game->size(); ++i) {
        const auto& argument = (*game)[i];
        if (argument.is_string() && argument.get_ref<const std::string&>() == key
            && (*game)[i + 1].is_string()) return (*game)[i + 1].get<std::string>();
    }
    return {};
}
std::optional<std::string> minecraft_identity(const fs::path& root, const Json& value,
                                              std::set<std::string>& visited, unsigned depth = 0) {
    if (const auto it = value.find("nickelProfile"); it != value.end() && it->is_object()) {
        if (auto mc = detail::string_at(*it, "minecraftVersion"); mc && valid_minecraft_version(*mc)) return mc;
    }
    if (auto mc = detail::string_at(value, "clientVersion"); mc && metadata_version(*mc)) return mc;
    if (auto parent = detail::string_at(value, "inheritsFrom"); parent && valid_minecraft_version(*parent)) {
        if (metadata_version(*parent)) return parent;
        if (depth < 8 && visited.insert(*parent).second) {
            try {
                const auto dir = checked_instance_directory(root, *parent);
                if (!is_pending(dir)) {
                    const auto descriptor = detail::read_optional_json(dir / path_from_utf8(*parent + ".json"), descriptor_max_bytes, true);
                    if (descriptor) {
                        if (auto mc = minecraft_identity(root, *descriptor, visited, depth + 1)) return mc;
                    }
                }
            } catch (...) {}
        }
    }
    if (auto mc = game_argument(value, "--fml.mcVersion"); mc && metadata_version(*mc)) return mc;
    if (auto mc = detail::string_at(value, "jar"); mc && metadata_version(*mc)) return mc;
    const auto names = libraries(value);
    for (auto coordinate : {"net.fabricmc:intermediary", "org.quiltmc:intermediary"}) {
        if (auto mc = artifact_version(names, coordinate); mc && metadata_version(*mc)) return mc;
    }
    if (auto forge = artifact_version(names, "net.minecraftforge:forge")) {
        const auto separator = forge->find('-');
        const auto mc = forge->substr(0, separator);
        if (metadata_version(mc)) return mc;
    }
    if (auto mc = detail::string_at(value, "id"); mc && metadata_version(*mc)) return mc;
    return {};
}
std::string detect_loader(const Json& descriptor, std::string_view directory_id) {
    if (auto it = descriptor.find("nickelProfile"); it != descriptor.end() && it->is_object())
        return detail::ascii_lower(detail::string_at(*it, "loader").value_or("vanilla"));
    const auto id = detail::string_at(descriptor, "id").value_or(std::string(directory_id));
    const auto hint = detail::ascii_lower(id + ' ' + detail::string_at(descriptor, "mainClass").value_or("")
        + ' ' + detail::string_at(descriptor, "inheritsFrom").value_or(""));
    const auto names = libraries(descriptor);
    const auto has_library = [&](std::string_view prefix) {
        return std::any_of(names.begin(), names.end(), [&](const auto& name) { return name.starts_with(prefix); });
    };
    if (hint.find("neoforge") != std::string::npos || has_library("net.neoforged:") || has_library("net.neoforge:")) return "neoforge";
    if (hint.find("optifabric") != std::string::npos || has_library("net.optifabric:")) return "optifabric";
    // Quilt stays Fabric in the legacy list; an explicit profile loader retains its actual kind.
    if (hint.find("fabric") != std::string::npos || hint.find("quilt") != std::string::npos
        || has_library("net.fabricmc:fabric-loader") || has_library("org.quiltmc:quilt-loader")) return "fabric";
    if (hint.find("forge") != std::string::npos || has_library("net.minecraftforge:")) return "forge";
    if (hint.find("optifine") != std::string::npos || has_library("optifine:")) return "optifine";
    if (hint.find("liteloader") != std::string::npos || has_library("com.mumfrey:")) return "liteloader";
    const auto first = id.find_first_of("0123456789");
    bool numbered = false;
    if (first != std::string::npos) {
        const auto end = id.find_first_not_of("0123456789.", first);
        auto token = id.substr(first, end == std::string::npos ? end : end - first);
        while (!token.empty() && token.back() == '.') token.pop_back();
        static const std::regex numbers(R"(^([0-9]+)\.([0-9]+)(?:\.([0-9]+))?$)");
        std::smatch match;
        if (std::regex_match(token, match, numbers)) {
            try {
                const auto version = std::tuple{std::stoull(match[1].str()), std::stoull(match[2].str()), match[3].matched ? std::stoull(match[3].str()) : 0ULL};
                numbered = true;
                if (version <= std::tuple{1ULL, 5ULL, 2ULL}) return "legacy";
            } catch (const std::exception&) {}
        }
    }
    const auto type = detail::string_at(descriptor, "type").value_or("release");
    return numbered || type == "release" || type == "snapshot" || type == "old_beta" || type == "old_alpha" ? "vanilla" : "unknown";
}
std::string pick_icon(std::string_view loader, std::string_view id, std::string_view version_type, bool has_json) {
    if (loader == "forge") return "anvil";
    if (loader == "neoforge") return "neoforge";
    if (loader == "fabric") return "fabric";
    if (loader == "optifine") return "grass_path";
    if (loader == "liteloader") return "egg";
    if (!has_json) return "redstone_block";
    if (is_april_fools(id)) return "gold_block";
    if (version_type == "release") return "grass";
    if (version_type == "snapshot") return "command_block";
    if (version_type == "old_beta" || version_type == "old_alpha") return "cobblestone";
    return "redstone_block";
}
InstanceEntry classify(const fs::path& root, const fs::path& directory, std::string_view id) {
    InstanceEntry entry;
    entry.id = std::string(id);
    entry.display_name = entry.id;
    entry.directory = directory;
    entry.named_profile = detail::path_exists(directory / profile_file);
    auto descriptor_path = pick_version_json(directory, id);
    if (!descriptor_path) return entry;
    Json descriptor;
    try { descriptor = parse_legacy_json(detail::read_bounded(*descriptor_path, descriptor_max_bytes)); }
    catch (...) { return entry; }
    if (!descriptor.is_object()) return entry;
    entry.has_json = true;
    entry.ready = true;
    entry.version_type = detail::string_at(descriptor, "type").value_or("release");
    entry.release_time = detail::string_at(descriptor, "releaseTime");
    entry.loader = detect_loader(descriptor, id);
    const auto custom = descriptor.find("nickelCustom");
    if (custom != descriptor.end() && custom->is_object()) {
        if (auto name = detail::string_at(*custom, "name"); name && !detail::trim(*name).empty()) entry.display_name = detail::trim(*name);
        entry.category = detail::string_at(*custom, "category").value_or("");
        const auto favorite = custom->find("favorite");
        entry.favorite = favorite != custom->end() && favorite->is_boolean() && favorite->get<bool>();
        const auto favorite_at = custom->find("favoriteAt");
        if (favorite_at != custom->end()) entry.favorite_at = detail::unsigned_value(*favorite_at);
    }
    std::set<std::string> chain;
    entry.minecraft_version = minecraft_identity(root, descriptor, chain);
    if (entry.named_profile) {
        try {
            if (auto profile = load_profile(root, id)) {
                entry.minecraft_version = profile->minecraft_version;
                entry.loader = detail::ascii_lower(profile->loader.value_or("vanilla"));
                if (profile->loader) entry.loader_version = profile->loader_version;
                entry.ready = profile->ready;
            }
        } catch (...) { entry.ready = false; }
    }
    if (!entry.loader_version) {
        const auto names = libraries(descriptor);
        const std::array<std::pair<std::string_view, std::string_view>, 6> coordinates{{
            {"forge", "net.minecraftforge:forge"}, {"neoforge", "net.neoforged:neoforge"},
            {"fabric", "net.fabricmc:fabric-loader"}, {"quilt", "org.quiltmc:quilt-loader"},
            {"liteloader", "com.mumfrey:liteloader"}, {"optifine", "optifine:OptiFine"}}};
        for (const auto& [loader, coordinate] : coordinates) {
            if (entry.loader == loader) entry.loader_version = artifact_version(names, coordinate);
        }
        if (entry.loader == "fabric" && !entry.loader_version) entry.loader_version = artifact_version(names, "org.quiltmc:quilt-loader");
    }
    try {
        const auto setup_path = directory / "PCL" / "Setup.ini";
        if (fs::is_regular_file(detail::native_path(setup_path))) {
            const auto setup = parse_legacy_setup(detail::read_bounded(setup_path, settings_max_bytes));
            if (!entry.minecraft_version) {
                const auto it = setup.find("VersionVanillaName");
                if (it != setup.end() && metadata_version(it->second)) entry.minecraft_version = it->second;
            }
            if (entry.loader == "vanilla" || entry.loader == "legacy" || entry.loader == "unknown") {
                const std::array<std::pair<std::string_view, std::string_view>, 6> hints{{
                    {"VersionNeoForge", "neoforge"}, {"VersionForge", "forge"},
                    {"VersionFabric", "fabric"}, {"VersionLiteLoader", "liteloader"},
                    {"VersionOptiFine", "optifine"}, {"VersionQuilt", "quilt"}}};
                for (const auto& [key, loader] : hints) {
                    const auto it = setup.find(std::string(key));
                    if (it != setup.end() && !it->second.empty() && it->second != "False" && it->second != "0") {
                        entry.loader = loader; entry.loader_version = it->second; break;
                    }
                }
            }
        }
    } catch (...) {}
    const auto metadata_id = detail::string_at(descriptor, "id").value_or(entry.id);
    entry.icon = pick_icon(entry.loader, entry.minecraft_version.value_or(metadata_id), entry.version_type, entry.has_json);
    try {
        const auto local = read_local_settings(root, id);
        if (auto icon = detail::string_at(local.value, "icon"); icon && !icon->empty() && *icon != "auto") entry.icon = *icon;
    } catch (...) {}
    return entry;
}
fs::path journal_directory(const fs::path& root) { return root / ".nickel-pack-transactions"; }
fs::path journal_path(const fs::path& root, std::string_view owner) { return journal_directory(root) / (std::string(owner) + ".json"); }
bool valid_owner(std::string_view owner) {
    return owner.size() == 32 && std::all_of(owner.begin(), owner.end(), [](unsigned char c) { return std::isxdigit(c); });
}
void ensure_marker(const fs::path& directory, std::string_view owner) {
    reject_links(directory);
    if (!fs::is_directory(detail::native_path(directory)) || detail::read_bounded(directory / pending_file, 128) != owner)
        throw Error("ownership_changed", "Pending instance belongs to another transaction");
}
void cleanup_owned(const fs::path& root, const fs::path& directory, std::string_view owner) {
    ensure_marker(directory, owner);
    detail::verify_directory_tree(directory);
    const auto retired = directory.parent_path() / (".nickel-pack-cleanup-" + std::string(owner));
    if (directory != retired) detail::rename_no_replace(directory, retired);
    detail::remove_owned_tree(retired);
    std::error_code ec;
    fs::remove(detail::native_path(journal_path(root, owner)), ec);
}
} // namespace

std::string root_identity(std::string_view value) {
    auto result = detail::trim(value);
    std::replace(result.begin(), result.end(), '\\', '/');
#ifdef _WIN32
    result = folded(result);
#endif
    if (result.starts_with("//?/unc/")) result = "//" + result.substr(8);
    else if (result.starts_with("//?/")) result.erase(0, 4);
    while (result.size() > 1 && result.back() == '/') result.pop_back();
    return result;
}
void validate_name(std::string_view name) {
    if (name.empty() || name.size() > 160 || detail::trim(name) != name || name == "." || name.find("..") != std::string_view::npos || name.back() == '.')
        throw Error("invalid_name", "Invalid instance name");
    for (unsigned char c : name) {
        if (c < 32 || c == 127 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos)
            throw Error("invalid_name", "Instance name contains unsafe characters");
    }
    try { (void)Json(std::string(name)).dump(); }
    catch (...) { throw Error("invalid_name", "Instance name is not UTF-8"); }
    const auto lower = detail::ascii_lower(name);
    if (lower == "nickel.profile" || lower == "gamedata" || lower == "nickel.settings")
        throw Error("invalid_name", "Instance name conflicts with launcher metadata");
    auto stem = lower.substr(0, lower.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    constexpr std::array<std::string_view, 7> devices{"con", "prn", "aux", "nul", "clock$", "conin$", "conout$"};
    bool device = std::find(devices.begin(), devices.end(), stem) != devices.end();
    if (stem.starts_with("com") || stem.starts_with("lpt")) {
        const auto index = stem.substr(3);
        device = device || (index.size() == 1 && index[0] >= '1' && index[0] <= '9')
            || index == "\xc2\xb9" || index == "\xc2\xb2" || index == "\xc2\xb3";
    }
    if (device) throw Error("invalid_name", "Instance name is a reserved device name");
}
bool valid_minecraft_version(std::string_view value) noexcept {
    try { validate_name(value); return true; } catch (...) { return false; }
}
Registry registry_from_text(std::string_view text) noexcept {
    try {
        auto value = detail::parse_strict_json(text, settings_max_bytes);
        if (!value.is_object()) return {};
        Registry result;
        if (auto it = value.find("roots"); it != value.end()) {
            if (!it->is_array()) return {};
            for (const auto& root : *it) {
                if (!root.is_string()) return {};
                result.roots.push_back(root.get<std::string>());
            }
        }
        if (auto it = value.find("active"); it != value.end() && !it->is_null()) {
            if (!it->is_string()) return {};
            result.active = it->get<std::string>();
        }
        if (auto it = value.find("labels"); it != value.end()) {
            if (!it->is_object()) return {};
            for (const auto& label : it->items()) if (!label.value().is_string()) return {};
            result.labels = *it;
        }
        value.erase("roots"); value.erase("active"); value.erase("labels");
        result.unknown = value;
        return result;
    } catch (...) { return {}; }
}
Json registry_to_json(const Registry& registry) {
    Json result = registry.unknown.is_object() ? registry.unknown : Json::object();
    result["roots"] = registry.roots;
    result["active"] = registry.active ? Json(*registry.active) : Json(nullptr);
    if (registry.labels.is_object() && !registry.labels.empty()) result["labels"] = registry.labels;
    else result.erase("labels");
    return result;
}
std::vector<std::string> ordered_roots(const std::vector<std::string>& incoming,
                                      const std::vector<std::string>& previous,
                                      std::optional<std::string_view> default_root) {
    std::vector<std::string> unique, result;
    std::set<std::string> seen;
    for (const auto& root : incoming)
        if (!detail::trim(root).empty() && seen.insert(root_identity(root)).second) unique.push_back(root);
    for (const auto& root : previous) {
        auto it = std::find_if(unique.begin(), unique.end(), [&](const auto& item) { return root_identity(item) == root_identity(root); });
        if (it != unique.end()) { result.push_back(*it); unique.erase(it); }
    }
    result.insert(result.end(), unique.begin(), unique.end());
    if (default_root) {
        const auto it = std::find_if(result.begin(), result.end(), [&](const auto& root) { return root_identity(root) == root_identity(*default_root); });
        if (it != result.end()) std::rotate(result.begin(), it, it + 1);
    }
    return result;
}
Json profile_to_json(const NamedProfile& profile) {
    Json value = profile.unknown.is_object() ? profile.unknown : Json::object();
    value["schemaVersion"] = profile.schema_version;
    value["name"] = profile.name;
    value["minecraftVersion"] = profile.minecraft_version;
    value["loader"] = profile.loader ? Json(*profile.loader) : Json(nullptr);
    value["loaderVersion"] = profile.loader_version;
    value["loaderArtifact"] = profile.loader_artifact;
    value["ready"] = profile.ready;
    return value;
}
NamedProfile profile_from_json(const Json& value, std::string_view expected_name) {
    if (!value.is_object()) throw Error("invalid_profile", "Profile is not an object");
    NamedProfile profile;
    const auto schema = value.find("schemaVersion");
    if (schema == value.end() || detail::unsigned_value(*schema) != 1) throw Error("invalid_profile", "Unsupported profile schema");
    profile.name = detail::string_at(value, "name").value_or("");
    profile.minecraft_version = detail::string_at(value, "minecraftVersion").value_or("");
    validate_name(profile.name);
    validate_name(profile.minecraft_version);
    if (profile.name != expected_name) throw Error("invalid_profile", "Profile identity does not match directory");
    if (auto it = value.find("loader"); it != value.end() && !it->is_null()) {
        if (!it->is_string() || !known_loader(it->get<std::string>())) throw Error("invalid_profile", "Unknown profile loader");
        profile.loader = it->get<std::string>();
    }
    if (auto it = value.find("loaderVersion"); it != value.end()) {
        if (!it->is_string()) throw Error("invalid_profile", "Invalid loader version");
        profile.loader_version = it->get<std::string>();
    }
    if (profile.loader && !valid_build(profile.loader_version)) throw Error("invalid_profile", "Loader requires exact version");
    if (auto it = value.find("loaderArtifact"); it != value.end()) {
        if (!it->is_null() && !it->is_object()) throw Error("invalid_profile", "Invalid loader artifact");
        profile.loader_artifact = *it;
    }
    if (auto it = value.find("ready"); it != value.end()) {
        if (!it->is_boolean()) throw Error("invalid_profile", "Invalid ready state");
        profile.ready = it->get<bool>();
    }
    profile.unknown = value;
    for (const auto* key : {"schemaVersion", "name", "minecraftVersion", "loader", "loaderVersion", "loaderArtifact", "ready"}) profile.unknown.erase(key);
    return profile;
}
bool is_pending(const fs::path& directory) {
    return detail::path_exists(directory / pending_file);
}
std::optional<NamedProfile> load_profile(const fs::path& root, std::string_view id) {
    const auto directory = checked_instance_directory(root, id);
    if (is_pending(directory)) throw Error("pending_instance", "Instance installation is not complete");
    auto value = detail::read_optional_json(directory / profile_file, 64 * 1024);
    if (!value) return {};
    return profile_from_json(*value, id);
}
void save_profile(const fs::path& directory, const NamedProfile& profile) {
    reject_links(directory);
    (void)profile_from_json(profile_to_json(profile), path_to_utf8(directory.filename()));
    const auto text = profile_to_json(profile).dump(2);
    if (text.size() > 64 * 1024) throw Error("limit", "Profile exceeds 64 KiB");
    detail::atomic_write(directory / profile_file, text);
}
Json entry_to_json(const InstanceEntry& entry) {
    return {{"id", entry.id}, {"displayName", entry.display_name}, {"favorite", entry.favorite},
            {"favoriteAt", entry.favorite_at ? Json(*entry.favorite_at) : Json(nullptr)},
            {"category", entry.category}, {"type", entry.version_type}, {"loader", entry.loader},
            {"loaderVersion", entry.loader_version ? Json(*entry.loader_version) : Json(nullptr)},
            {"icon", entry.icon}, {"path", path_to_utf8(entry.directory)},
            {"releaseTime", entry.release_time ? Json(*entry.release_time) : Json(nullptr)},
            {"hasJson", entry.has_json}, {"minecraftVersion", entry.minecraft_version ? Json(*entry.minecraft_version) : Json(nullptr)},
            {"namedProfile", entry.named_profile}, {"ready", entry.ready}};
}
std::optional<fs::path> pick_version_json(const fs::path& directory, std::string_view id) {
    reject_links(directory);
    const auto exact = directory / path_from_utf8(std::string(id) + ".json");
    reject_links(exact);
    const auto native_exact = detail::native_path(exact);
    if (fs::is_regular_file(native_exact) && fs::file_size(native_exact) <= descriptor_max_bytes) return exact;
    if (detail::path_exists(directory / profile_file)) return {};
    std::vector<fs::path> candidates;
    for (const auto& item : detail::directory_entries(directory)) {
        const auto& file = item.path();
        reject_links(file, false);
        if (!item.is_regular_file() || detail::ascii_lower(path_to_utf8(file.extension())) != ".json") continue;
        const auto name = detail::ascii_lower(path_to_utf8(file.filename()));
        if (name.starts_with("nickel.") || name == "gamedata.json" || item.file_size() > descriptor_max_bytes) continue;
        try {
            const auto value = parse_legacy_json(detail::read_bounded(file, descriptor_max_bytes));
            if (detail::string_at(value, "id") && detail::string_at(value, "mainClass") && detail::string_at(value, "type")) candidates.push_back(file);
        } catch (...) {}
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.empty() ? std::optional<fs::path>{} : candidates.front();
}
std::vector<InstanceEntry> scan_instances(const fs::path& root) {
    std::lock_guard lock(domain_mutex);
    std::vector<InstanceEntry> result;
    if (!root.is_absolute()) return result;
    try {
        reject_links(root / "versions");
        if (!fs::is_directory(detail::native_path(root / "versions"))) return result;
        detail::DirectoryLock file_lock(root);
        for (const auto& item : detail::directory_entries(root / "versions")) {
            try {
                const auto& directory = item.path();
                reject_links(directory, false);
                if (!item.is_directory() || is_pending(directory)) continue;
                const auto id = path_to_utf8(directory.filename());
                if (id.starts_with(".nickel-pack-cleanup-") || id.starts_with(".nickel-instance-delete-")) continue;
                validate_name(id);
                auto entry = classify(root, directory, id);
                if (!is_pending(directory)) result.push_back(std::move(entry));
            } catch (...) {}
        }
    } catch (...) { return {}; }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.favorite != b.favorite) return a.favorite > b.favorite;
        if (a.favorite_at.value_or(0) != b.favorite_at.value_or(0)) return a.favorite_at.value_or(0) < b.favorite_at.value_or(0);
        if (a.release_time != b.release_time) return a.release_time > b.release_time;
        return a.id < b.id;
    });
    return result;
}
InstanceEntry instance_detail(const fs::path& root, std::string_view id) {
    const auto directory = checked_instance_directory(root, id);
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(root);
    if (is_pending(directory)) throw Error("pending_instance", "Instance is pending installation");
    return classify(root, directory, id);
}
void patch_instance_metadata(const fs::path& root, std::string_view id, const Json& changed,
                             std::stop_token cancellation) {
    detail::check_cancelled(cancellation);
    constexpr std::string_view fields[]{"name", "description", "category", "favorite", "favoriteAt", "icon"};
    detail::strict_keys(changed, fields);
    if (changed.dump().size() > 32 * 1024) throw Error("limit", "Metadata patch exceeds its input quota");
    const auto directory = checked_instance_directory(root, id);
    const bool icon_only = changed.contains("icon");
    if (icon_only && changed.size() != 1)
        throw Error("invalid_schema", "Icon and display metadata use separate atomic files");
    for (const auto key : {"name", "description", "category"}) {
        const auto it = changed.find(key);
        if (it == changed.end()) continue;
        if (!it->is_string()) throw Error("invalid_schema", "Display metadata must be text");
        const auto& text = it->get_ref<const std::string&>();
        const std::size_t maximum = std::string_view(key) == "description" ? 16 * 1024 : 512;
        if (text.size() > maximum) throw Error("limit", "Display metadata exceeds its field quota");
        if (std::string_view(key) == "name" && detail::trim(text).empty()) throw Error("invalid_name", "Display name is empty");
        if (text.find('\0') != std::string::npos) throw Error("invalid_schema", "Display metadata contains NUL");
    }
    if (changed.contains("favorite") && !changed["favorite"].is_boolean())
        throw Error("invalid_schema", "Favorite state must be boolean");
    if (changed.contains("favoriteAt")) {
        const auto timestamp = detail::unsigned_value(changed["favoriteAt"]);
        if (!changed.contains("favorite") || !changed["favorite"].get<bool>()
            || !timestamp || *timestamp > 9007199254740991ULL)
            throw Error("invalid_schema", "Favorite timestamp requires an enabled favorite and a safe integer");
    }
    Json icon;
    if (icon_only) {
        constexpr std::array<std::string_view, 12> icons{"grass", "command_block", "cobblestone", "anvil",
            "neoforge", "fabric", "grass_path", "egg", "gold_block", "redstone_block", "redstone_lamp_on", "redstone_lamp_off"};
        icon = changed["icon"];
        if (!icon.is_null()) {
            if (!icon.is_string()) throw Error("invalid_schema", "Instance icon must be a built-in key or null");
            const auto& key = icon.get_ref<const std::string&>();
            if (key == "auto" || key.empty()) icon = nullptr;
            else if (std::find(icons.begin(), icons.end(), std::string_view(key)) == icons.end())
                throw Error("invalid_icon", "Instance icon is not an approved built-in key");
        }
    }
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(root);
    detail::check_cancelled(cancellation);
    if (is_pending(directory)) throw Error("pending_instance", "Cannot edit pending instance");
    if (changed.empty()) return;
    if (icon_only) {
        auto settings = detail::read_optional_json(directory / settings_file, settings_max_bytes, true);
        Json value;
        if (settings) {
            if (!settings->is_object()) throw Error("invalid_settings", "Existing version settings are not an object");
            value = std::move(*settings);
        } else value = read_local_settings(root, id).value;
        if (!value.is_object()) value = Json::object();
        value["icon"] = std::move(icon);
        const auto text = value.dump(2);
        if (text.size() > settings_max_bytes) throw Error("limit", "Icon settings exceed their file quota");
        detail::check_cancelled(cancellation);
        detail::atomic_write(directory / settings_file, text);
        return;
    }
    const auto file = pick_version_json(directory, id);
    if (!file) throw Error("invalid_instance", "Version descriptor is unavailable");
    auto value = parse_legacy_json(detail::read_bounded(*file, descriptor_max_bytes));
    if (!value.is_object()) throw Error("invalid_instance", "Version descriptor is not an object");
    auto& custom = value["nickelCustom"];
    if (!custom.is_object()) custom = Json::object();
    for (const auto key : {"name", "description", "category"})
        if (changed.contains(key)) custom[key] = changed[key];
    if (changed.contains("favorite")) {
        const bool favorite = changed["favorite"].get<bool>();
        const auto previous = custom.find("favorite");
        const bool previously_favorite = previous != custom.end() && previous->is_boolean() && previous->get<bool>();
        const auto old_at = custom.find("favoriteAt");
        if (favorite && (!previously_favorite || old_at == custom.end() || !detail::unsigned_value(*old_at))) {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now < 0 || static_cast<std::uint64_t>(now) > 9007199254740991ULL)
                throw Error("invalid_timestamp", "System favorite timestamp is outside the safe integer range");
            custom["favoriteAt"] = changed.contains("favoriteAt") ? changed["favoriteAt"] : Json(now);
        }
        if (!favorite) custom["favoriteAt"] = nullptr;
        custom["favorite"] = favorite;
    }
    const auto text = value.dump(2);
    if (text.size() > descriptor_max_bytes) throw Error("limit", "Descriptor exceeds its file quota");
    detail::check_cancelled(cancellation);
    detail::atomic_write(*file, text);
}
void rename_display(const fs::path& root, std::string_view id, std::string_view name,
                    std::string_view description, std::string_view category, bool favorite,
                    std::uint64_t timestamp_ms) {
    Json changed{{"name", name}, {"description", description}, {"category", category}, {"favorite", favorite}};
    if (favorite) changed["favoriteAt"] = timestamp_ms;
    patch_instance_metadata(root, id, changed);
}
void delete_instance(const fs::path& root, std::string_view id) {
    const auto directory = checked_instance_directory(root, id);
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(root);
    if (is_pending(directory)) throw Error("pending_instance", "Pending instance is owned by an installation task");
    const auto retired = directory.parent_path() / (".nickel-instance-delete-" + detail::random_id());
    detail::verify_directory_tree(directory);
    detail::rename_no_replace(directory, retired);
    detail::remove_owned_tree(retired);
}
fs::path mods_directory(const fs::path& resource_root, std::string_view loader, std::string_view minecraft_version) {
    if (detail::ascii_lower(loader) == "liteloader") {
        validate_name(minecraft_version);
        return resource_root / "mods" / path_from_utf8(minecraft_version);
    }
    return resource_root / "mods";
}
InstanceRegistry::InstanceRegistry(fs::path config, fs::path default_root)
    : config_(std::move(config)), default_(std::move(default_root)) {
    if (!config_.is_absolute() || !default_.is_absolute()) throw Error("unsafe_path", "Registry paths must be absolute");
}
Registry InstanceRegistry::get() const {
    std::lock_guard lock(domain_mutex);
    reject_links(config_);
    auto registry = read_registry(config_);
    const auto default_text = path_to_utf8(default_);
    registry.roots = ordered_roots(registry.roots, {}, default_text);
    return registry;
}
void InstanceRegistry::set(const Registry& incoming) {
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(config_);
    auto old = read_registry(config_);
    auto result = incoming;
    const auto default_text = path_to_utf8(default_);
    result.roots = ordered_roots(incoming.roots, old.roots, default_text);
    for (const auto& unknown : old.unknown.items()) if (!result.unknown.contains(unknown.key())) result.unknown[unknown.key()] = unknown.value();
    write_registry(config_, result);
}
fs::path InstanceRegistry::ensure_default() {
    std::lock_guard lock(domain_mutex);
    reject_links(default_);
    detail::create_directories(default_);
    detail::DirectoryLock file_lock(config_);
    auto registry = read_registry(config_);
    const auto text = path_to_utf8(default_);
    if (std::none_of(registry.roots.begin(), registry.roots.end(), [&](const auto& item) { return root_identity(item) == root_identity(text); })) registry.roots.insert(registry.roots.begin(), text);
    registry.roots = ordered_roots(registry.roots, {}, text);
    if (!registry.active || registry.active->empty()) registry.active = text;
    write_registry(config_, registry);
    return default_;
}
fs::path InstanceRegistry::create_root(const fs::path& parent) {
    if (!parent.is_absolute()) throw Error("unsafe_path", "Parent directory must be absolute");
    const auto logical_parent = detail::logical_path(parent);
    reject_links(logical_parent);
    const auto root = logical_parent / ".minecraft";
    reject_links(root);
    detail::create_directories(root);
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(config_);
    auto registry = read_registry(config_);
    const auto text = path_to_utf8(root);
    if (std::none_of(registry.roots.begin(), registry.roots.end(), [&](const auto& item) { return root_identity(item) == root_identity(text); })) registry.roots.push_back(text);
    registry.active = text;
    const auto default_text = path_to_utf8(default_);
    registry.roots = ordered_roots(registry.roots, {}, default_text);
    write_registry(config_, registry);
    return root;
}
void InstanceRegistry::rename_root(const fs::path& root, std::string_view label) {
    if (detail::trim(label).empty() || label.size() > 512) throw Error("invalid_name", "Invalid root label");
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(config_);
    auto registry = read_registry(config_);
    const auto text = path_to_utf8(root);
    const auto it = std::find_if(registry.roots.begin(), registry.roots.end(), [&](const auto& item) { return root_identity(item) == root_identity(text); });
    if (it == registry.roots.end()) throw Error("missing_root", "Root is not registered");
    registry.labels[*it] = label;
    write_registry(config_, registry);
}
void InstanceRegistry::remove_root(const fs::path& root, bool delete_files,
                                    std::span<const fs::path> protected_directories) {
    if (!root.is_absolute() || root == root.root_path()) throw Error("unsafe_path", "Cannot remove filesystem root");
    reject_links(root);
    const auto canonical = root_identity(path_to_utf8(detail::canonical_path(root)));
    if (delete_files) {
        std::vector<fs::path> protected_paths(protected_directories.begin(), protected_directories.end());
        protected_paths.push_back(config_);
        for (const auto& path : protected_paths) {
            const auto value = root_identity(path_to_utf8(detail::canonical_path(path)));
            if (value == canonical || value.starts_with(canonical + '/'))
                throw Error("protected_path", "Game root contains a protected directory");
        }
        for (const auto& entry : detail::directory_entries(root, true)) {
            if (entry.path().filename() == pending_file) throw Error("pending_instance", "Root has an active or interrupted installation");
        }
    }
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(config_);
    auto registry = read_registry(config_);
    const auto text = path_to_utf8(root);
    const auto it = std::find_if(registry.roots.begin(), registry.roots.end(), [&](const auto& item) { return root_identity(item) == root_identity(text); });
    if (it == registry.roots.end()) throw Error("missing_root", "Root is not registered");
    const auto registered = *it;
    registry.roots.erase(it);
    registry.labels.erase(registered);
    if (registry.active && root_identity(*registry.active) == root_identity(registered)) registry.active = registry.roots.empty() ? std::optional<std::string>{} : registry.roots.front();
    auto game = registry_read(config_ / "game.json");
    if (!game.is_object()) throw Error("invalid_settings", "Global game settings is not an object");
    if (auto game_dir = detail::string_at(game, "gameDir"); game_dir && root_identity(*game_dir) == root_identity(registered)) {
        game["gameDir"] = registry.active.value_or("");
        detail::atomic_write(config_ / "game.json", game.dump());
    }
    write_registry(config_, registry);
    if (delete_files) detail::remove_owned_tree(root);
}
const fs::path& InstanceRegistry::config_directory() const noexcept { return config_; }

struct InstanceTransaction::State {
    fs::path root;
    fs::path directory;
    NamedProfile profile;
    std::string owner;
    bool committed = false;
    bool recovery = false;
    Json journal;
};
InstanceTransaction::InstanceTransaction(std::unique_ptr<State> state) : state_(std::move(state)) {}
InstanceTransaction::InstanceTransaction(InstanceTransaction&& other) noexcept = default;
InstanceTransaction& InstanceTransaction::operator=(InstanceTransaction&& other) noexcept {
    if (this != &other) {
        if (state_ && !state_->committed && !state_->recovery) { try { cleanup(); } catch (...) {} }
        state_ = std::move(other.state_);
    }
    return *this;
}
InstanceTransaction::~InstanceTransaction() {
    if (!state_) return;
    if (!state_->committed && !state_->recovery) { try { cleanup(); } catch (...) {} }
    std::lock_guard lock(domain_mutex);
    active_transactions.erase(state_->owner);
}
InstanceTransaction InstanceTransaction::reserve(const fs::path& root, std::string_view requested,
                                                 std::string_view minecraft,
                                                 std::optional<std::string> loader,
                                                 std::string_view build, bool choose_suffix) {
    validate_name(minecraft);
    if (loader && (!known_loader(*loader) || !valid_build(build))) throw Error("invalid_profile", "Loader requires a supported exact build");
    if (!choose_suffix) validate_name(requested);
    if (!root.is_absolute()) throw Error("unsafe_path", "Game root must be absolute");
    std::lock_guard lock(domain_mutex);
    reject_links(root / "versions");
    detail::create_directories(root / "versions");
    detail::DirectoryLock file_lock(root);
    auto state = std::make_unique<State>();
    state->root = detail::logical_path(root);
    state->owner = detail::random_id();
    detail::create_directories(journal_directory(state->root));
    for (std::size_t suffix = 0; suffix < 10000; ++suffix) {
        const auto name = choose_suffix ? candidate_name(requested, suffix) : std::string(requested);
        validate_name(name);
        if (occupied(state->root / "versions", name)) {
            if (choose_suffix) continue;
            throw Error("conflict", "Instance identity already exists");
        }
        state->directory = checked_instance_directory(state->root, name, false);
        if (!detail::create_directory_no_replace(state->directory)) {
            if (choose_suffix) continue;
            throw Error("conflict", "Cannot atomically reserve instance directory");
        }
        state->profile.name = name;
        state->profile.minecraft_version = minecraft;
        state->profile.loader = loader;
        state->profile.loader_version = build;
        state->journal = {{"schemaVersion", 1}, {"owner", state->owner}, {"instanceId", name},
                          {"pid", detail::current_pid()}, {"phase", "installing"}, {"root", path_to_utf8(state->root)}};
        try {
            detail::write_exclusive(state->directory / pending_file, state->owner);
            detail::atomic_write(journal_path(state->root, state->owner), state->journal.dump(), false);
            save_profile(state->directory, state->profile);
            active_transactions.insert(state->owner);
            return InstanceTransaction(std::move(state));
        } catch (...) {
            try {
                if (detail::path_exists(state->directory / pending_file)) cleanup_owned(state->root, state->directory, state->owner);
                else { std::error_code ec; fs::remove(detail::native_path(state->directory), ec); }
            } catch (...) {}
            throw;
        }
    }
    throw Error("conflict", "Too many same-name instances");
}
const fs::path& InstanceTransaction::directory() const { if (!state_) throw Error("transaction", "Moved transaction"); return state_->directory; }
const fs::path& InstanceTransaction::root() const { if (!state_) throw Error("transaction", "Moved transaction"); return state_->root; }
const NamedProfile& InstanceTransaction::profile() const { if (!state_) throw Error("transaction", "Moved transaction"); return state_->profile; }
void InstanceTransaction::check_owned() const {
    if (!state_ || state_->committed) throw Error("transaction", "Transaction is not pending");
    ensure_marker(state_->directory, state_->owner);
    const auto journal = detail::read_optional_json(journal_path(state_->root, state_->owner), 64 * 1024);
    if (!journal || detail::string_at(*journal, "owner") != state_->owner || detail::string_at(*journal, "instanceId") != state_->profile.name
        || detail::string_at(*journal, "root") != path_to_utf8(state_->root)) throw Error("ownership_changed", "Transaction journal has changed");
}
void InstanceTransaction::complete(const InstallIdentity& installed) {
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(root());
    check_owned();
    if (installed.minecraft_version != state_->profile.minecraft_version
        || (installed.loader ? detail::ascii_lower(*installed.loader) : "") != (state_->profile.loader ? detail::ascii_lower(*state_->profile.loader) : "")
        || (state_->profile.loader && installed.loader_version != state_->profile.loader_version))
        throw Error("identity_mismatch", "Installed game or loader does not match reserved identity");
    if (!installed.descriptor.is_object()) throw Error("invalid_instance", "Installed descriptor is not an object");
    auto descriptor = installed.descriptor;
    const auto existing = detail::read_optional_json(state_->directory / path_from_utf8(state_->profile.name + ".json"), descriptor_max_bytes, true);
    if (existing && existing->is_object()) {
        for (const auto& unknown : existing->items()) if (!descriptor.contains(unknown.key())) descriptor[unknown.key()] = unknown.value();
        for (const auto* key : {"nickelCustom", "nickelPack"}) if (existing->contains(key)) descriptor[key] = (*existing)[key];
    }
    descriptor["id"] = state_->profile.name;
    descriptor["inheritsFrom"] = state_->profile.minecraft_version;
    descriptor["nickelProfile"] = {{"minecraftVersion", state_->profile.minecraft_version},
                                    {"loader", state_->profile.loader ? Json(*state_->profile.loader) : Json(nullptr)},
                                    {"loaderVersion", state_->profile.loader_version}};
    const auto text = descriptor.dump(2);
    if (text.size() > descriptor_max_bytes) throw Error("limit", "Descriptor exceeds size limit");
    detail::atomic_write(state_->directory / path_from_utf8(state_->profile.name + ".json"), text);
    state_->profile.loader_artifact = installed.loader_artifact;
    state_->profile.ready = true;
    save_profile(state_->directory, state_->profile);
    state_->journal["phase"] = "ready";
    detail::atomic_write(journal_path(state_->root, state_->owner), state_->journal.dump());
}
void InstanceTransaction::commit(std::string_view name, const Json& source, std::stop_token cancellation) {
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(root());
    check_owned();
    detail::check_cancelled(cancellation);
    if (!state_->profile.ready) throw Error("pending_instance", "Cannot publish incomplete instance");
    if (name.empty() || name.size() > 512 || !source.is_object()) throw Error("invalid_schema", "Invalid publication metadata");
    const auto descriptor_path = state_->directory / path_from_utf8(state_->profile.name + ".json");
    const auto journal = journal_path(state_->root, state_->owner);
    const auto native_journal = detail::native_path(journal);
    const auto native_pending = detail::native_path(state_->directory / pending_file);
    auto descriptor = detail::read_optional_json(descriptor_path, descriptor_max_bytes, true);
    if (!descriptor || !descriptor->is_object()) throw Error("invalid_instance", "Completed descriptor is missing");
    auto& custom = (*descriptor)["nickelCustom"];
    if (!custom.is_object()) custom = Json::object();
    custom["name"] = name;
    (*descriptor)["nickelPack"] = source;
    detail::atomic_write(descriptor_path, descriptor->dump(2));
    detail::check_cancelled(cancellation);
    state_->journal["phase"] = "publishing";
    detail::atomic_write(journal, state_->journal.dump());
    check_owned();
    detail::check_cancelled(cancellation);
    fs::remove(native_pending);
    state_->committed = true;
    active_transactions.erase(state_->owner);
    // Publication is irreversible; journal cleanup uses a preallocated path and never throws.
    std::error_code ec;
    fs::remove(native_journal, ec);
}
void InstanceTransaction::cleanup() {
    if (!state_ || state_->committed) return;
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(root());
    check_owned();
    cleanup_owned(state_->root, state_->directory, state_->owner);
    state_->committed = true;
    active_transactions.erase(state_->owner);
}
void InstanceTransaction::leave_for_recovery(bool writers_drained) {
    if (!state_ || state_->committed) return;
    // Preserve the reservation even when locking, ownership checks or journal writes fail.
    state_->recovery = true;
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(root());
    check_owned();
    // A timed-out owned installer may still write; recovery must wait for this process to exit.
    if (writers_drained) state_->journal["pid"] = 0;
    detail::atomic_write(journal_path(state_->root, state_->owner), state_->journal.dump());
    active_transactions.erase(state_->owner);
}
RecoveryResult recover_interrupted_instances(const fs::path& root) {
    RecoveryResult result;
    if (!root.is_absolute()) throw Error("unsafe_path", "Recovery root must be absolute");
    reject_links(root);
    if (!fs::is_directory(detail::native_path(root))) return result;
    std::lock_guard lock(domain_mutex);
    detail::DirectoryLock file_lock(root);
    const auto canonical_root = detail::logical_path(root);
    const auto journals = journal_directory(root);
    reject_links(journals);
    if (!fs::is_directory(detail::native_path(journals))) return result;
    for (const auto& item : detail::directory_entries(journals)) {
        const auto label = path_to_utf8(item.path().filename());
        try {
            reject_links(item.path(), false);
            const auto journal = detail::read_optional_json(item.path(), 64 * 1024);
            if (!journal || !journal->is_object()) throw Error("invalid_schema", "Invalid recovery journal");
            const auto owner = detail::string_at(*journal, "owner").value_or("");
            const auto id = detail::string_at(*journal, "instanceId").value_or("");
            if (!valid_owner(owner) || label != owner + ".json" || (*journal)["schemaVersion"] != 1
                || detail::string_at(*journal, "root") != path_to_utf8(canonical_root)) throw Error("ownership_changed", "Recovery journal identity mismatch");
            validate_name(id);
            if (active_transactions.contains(owner)) { result.skipped.push_back(id); continue; }
            const auto pid_value = journal->find("pid");
            if (pid_value == journal->end() || !detail::unsigned_value(*pid_value)) throw Error("invalid_schema", "Invalid recovery process identity");
            const auto pid = *detail::unsigned_value(*pid_value);
            if (pid != 0 && detail::process_alive(pid)) { result.skipped.push_back(id); continue; }
            const auto directory = checked_instance_directory(root, id, false);
            if (!detail::path_exists(directory)) {
                // Retain recovery of the original root-level retirement layout.
                const fs::path retired[]{directory.parent_path() / (".nickel-pack-cleanup-" + owner),
                                         root / (".nickel-pack-cleanup-" + owner)};
                for (const auto& candidate : retired) {
                    reject_links(candidate);
                    if (!detail::path_exists(candidate)) continue;
                    ensure_marker(candidate, owner);
                    detail::remove_owned_tree(candidate);
                }
                fs::remove(detail::native_path(item.path()));
                result.removed.push_back(id);
                continue;
            }
            if (!is_pending(directory)) {
                const auto profile = load_profile(root, id);
                if (!profile || !profile->ready) throw Error("ownership_changed", "Published target identity is invalid");
                fs::remove(detail::native_path(item.path())); result.published.push_back(id); continue;
            }
            ensure_marker(directory, owner);
            const auto phase = detail::string_at(*journal, "phase").value_or("");
            if (phase == "publishing") {
                const auto profile_value = detail::read_optional_json(directory / profile_file, 64 * 1024);
                const auto descriptor = detail::read_optional_json(directory / path_from_utf8(id + ".json"), descriptor_max_bytes);
                if (!profile_value || !profile_from_json(*profile_value, id).ready || !descriptor || (*descriptor)["id"] != id || !descriptor->contains("nickelPack"))
                    throw Error("invalid_instance", "Publication checkpoint is incomplete");
                fs::remove(detail::native_path(directory / pending_file));
                fs::remove(detail::native_path(item.path())); result.published.push_back(id);
            } else {
                cleanup_owned(root, directory, owner); result.removed.push_back(id);
            }
        } catch (const std::exception& error) { result.errors.push_back(label + ": " + error.what()); }
    }
    return result;
}
} // namespace nickel::instances
