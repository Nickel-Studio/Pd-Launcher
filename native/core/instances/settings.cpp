#include "settings.hpp"
#include "domain_io.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <regex>
#include <set>

namespace nickel::instances {
namespace {
constexpr std::array<std::string_view, 7> bool_fields{"versionIsolation", "disableModUpdates", "disableFileCheck", "disableWrapper", "disableLwjglAgent", "highPerformanceGpu", "offlineSkinSlim"};
constexpr std::array<std::string_view, 20> text_fields{"windowTitle", "customInfo", "javaPath", "launcherVisibility", "processPriority", "windowMode", "memoryManagement", "offlineSkin", "offlineSkinName", "offlineSkinPath", "serverLogin", "authServer", "serverRegistration", "serverName", "serverAddress", "jvmArgs", "gameArgs", "preLaunchCommand", "icon", "memoryMode"};
constexpr std::array<std::string_view, 3> number_fields{"memoryMb", "windowWidth", "windowHeight"};
constexpr std::array<std::string_view, 23> portable_fields{"windowTitle", "customInfo", "versionIsolation", "launcherVisibility", "windowMode", "windowWidth", "windowHeight", "memoryMode", "memoryMb", "memoryManagement", "highPerformanceGpu", "processPriority", "serverLogin", "serverName", "serverRegistration", "authServer", "serverAddress", "jvmArgs", "gameArgs", "disableModUpdates", "disableFileCheck", "disableWrapper", "disableLwjglAgent"};
template<class Container> bool has(const Container& values, std::string_view key) {
    return std::find(values.begin(), values.end(), key) != values.end();
}
bool choice(std::string_view value, std::initializer_list<std::string_view> values) {
    return std::find(values.begin(), values.end(), value) != values.end();
}
std::string validated_choice(std::string value, std::initializer_list<std::string_view> values, std::string_view field) {
    if (!choice(value, values)) throw Error("invalid_settings", "Invalid setting: " + std::string(field));
    return value;
}
const Json* inherited(const Json& global, const Json& local, std::string_view key) {
    if (local.is_object()) {
        const auto it = local.find(std::string(key));
        if (it != local.end() && !it->is_null() && *it != "global") return &*it;
    }
    if (global.is_object()) {
        const auto it = global.find(std::string(key));
        if (it != global.end()) return &*it;
    }
    return nullptr;
}
std::string text(const Json& global, const Json& local, std::string_view key, std::string_view fallback = "") {
    const auto* value = inherited(global, local, key);
    return value && value->is_string() ? value->get<std::string>() : std::string(fallback);
}
bool boolean(const Json& global, const Json& local, std::string_view key, bool fallback = false) {
    const auto* value = inherited(global, local, key);
    return value && value->is_boolean() ? value->get<bool>() : fallback;
}
std::uint64_t number(const Json& global, const Json& local, std::string_view key, std::uint64_t fallback) {
    const auto* value = inherited(global, local, key);
    return value ? detail::unsigned_value(*value).value_or(fallback) : fallback;
}
std::optional<std::string> setup_text(const std::map<std::string, std::string>& setup, std::string_view key) {
    const auto it = setup.find(std::string(key));
    if (it == setup.end() || detail::trim(it->second).empty()) return {};
    return it->second;
}
std::optional<std::int64_t> setup_number(const std::map<std::string, std::string>& setup, std::string_view key) {
    const auto value = setup_text(setup, key);
    if (!value) return {};
    const auto raw = detail::trim(*value);
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), result);
    return error == std::errc{} && end == raw.data() + raw.size() ? std::optional(result) : std::nullopt;
}
std::optional<bool> setup_bool(const std::map<std::string, std::string>& setup, std::string_view key) {
    const auto value = setup_text(setup, key);
    if (!value) return {};
    const auto lower = detail::ascii_lower(detail::trim(*value));
    if (lower == "true" || lower == "1") return true;
    if (lower == "false" || lower == "0") return false;
    return {};
}
std::uint64_t slider_memory(std::int64_t slider) {
    double value = slider <= 12 ? static_cast<double>(slider) * 0.1 + 0.3
        : slider <= 25 ? (static_cast<double>(slider) - 12) * 0.5 + 1.5
        : slider <= 33 ? static_cast<double>(slider) - 25 + 8
        : (static_cast<double>(slider) - 33) * 2 + 16;
    return static_cast<std::uint64_t>(std::clamp(std::round(value * 1024.0), 512.0, 65536.0));
}
std::string gc_mode(std::int64_t code, bool local) {
    constexpr std::array<std::string_view, 5> modes{"auto-zgc", "generational-zgc", "g1", "custom", "g1-tuned"};
    const auto index = local ? code - 1 : code;
    return index >= 0 && index < 5 ? std::string(modes[static_cast<std::size_t>(index)]) : "";
}
LocalSettings legacy_settings(const fs::path& root, std::string_view id) {
    LocalSettings result;
    const auto local_path = root / "versions" / path_from_utf8(id) / "PCL" / "Setup.ini";
    reject_links(local_path);
    if (!fs::is_regular_file(detail::native_path(local_path))) return result;
    const auto local = parse_legacy_setup(detail::read_bounded(local_path, settings_max_bytes));
    std::map<std::string, std::string> global;
    const auto global_path = root / "PCL" / "Setup.ini";
    try {
        if (fs::is_regular_file(detail::native_path(global_path))) global = parse_legacy_setup(detail::read_bounded(global_path, settings_max_bytes));
    } catch (const Error& error) { if (error.code() == "unsafe_path") throw; }
    result.value = Json::object();
    result.source = "pcl";
    std::set<std::string> consumed;
    const auto local_value = [&](std::string_view key) { consumed.insert(std::string(key)); return setup_text(local, key); };
    const auto inherited_text = [&](std::string_view local_key, std::string_view global_key, std::string_view output_key) {
        auto value = local_value(local_key);
        if (!value) value = setup_text(global, global_key);
        if (value) result.value[std::string(output_key)] = *value;
    };
    const auto inherited_bool = [&](std::string_view local_key, std::string_view global_key, std::string_view output_key) {
        consumed.insert(std::string(local_key));
        auto value = setup_bool(local, local_key);
        if (!value) value = setup_bool(global, global_key);
        if (value) result.value[std::string(output_key)] = *value;
    };
    consumed.insert("VersionArgumentIndieV2");
    if (auto value = setup_bool(local, "VersionArgumentIndieV2")) result.value["versionIsolation"] = *value;
    inherited_text("VersionArgumentTitle", "LaunchArgumentTitle", "windowTitle");
    if (auto value = local_value("CustomInfo")) result.value["customInfo"] = *value;
    if (auto value = local_value("VersionArgumentJavaSelect")) {
        try {
            const auto parsed = parse_legacy_json(*value, settings_max_bytes);
            auto path = detail::string_at(parsed, "Path");
            if (!path) path = detail::string_at(parsed, "path");
            if (path && !path->empty()) result.value["javaPath"] = *path;
        } catch (...) {
            const auto lower = detail::ascii_lower(detail::trim(*value));
            if ((value->find('/') != std::string::npos || value->find('\\') != std::string::npos)
                && (lower.ends_with(".exe") || lower.ends_with("/java") || lower.ends_with("\\java"))) result.value["javaPath"] = detail::trim(*value);
        }
    }
    consumed.insert("VersionRamType"); consumed.insert("VersionRamCustom");
    const auto ram_type = setup_number(local, "VersionRamType").value_or(2);
    if (ram_type == 0) result.value["memoryMode"] = "auto";
    else if (ram_type == 1) { result.value["memoryMode"] = "custom"; result.value["memoryMb"] = slider_memory(setup_number(local, "VersionRamCustom").value_or(15)); }
    else {
        const auto global_type = setup_number(global, "LaunchRamType").value_or(2);
        result.value["memoryMode"] = global_type == 0 ? "auto" : global_type == 1 ? "custom" : "global";
        if (global_type == 1) result.value["memoryMb"] = slider_memory(setup_number(global, "LaunchRamCustom").value_or(15));
    }
    consumed.insert("VersionAdvanceGC");
    auto local_gc = setup_number(local, "VersionAdvanceGC");
    auto mode = local_gc && *local_gc > 0 ? gc_mode(*local_gc, true) : gc_mode(setup_number(global, "LaunchAdvanceGC").value_or(-1), false);
    if (mode.empty() && local_gc) mode = "g1-tuned";
    if (!mode.empty()) result.value["memoryManagement"] = mode;
    inherited_text("VersionAdvanceJvm", "LaunchAdvanceJvm", "jvmArgs");
    inherited_text("VersionAdvanceGame", "LaunchAdvanceGame", "gameArgs");
    inherited_text("VersionAdvanceRun", "LaunchAdvanceRun", "preLaunchCommand");
    inherited_bool("VersionAdvanceDisableJLW", "LaunchAdvanceDisableJLW", "disableWrapper");
    inherited_bool("VersionAdvanceDisableLUA", "LaunchAdvanceDisableLUA", "disableLwjglAgent");
    consumed.insert("VersionAdvanceDisableModUpdate");
    if (auto value = setup_bool(local, "VersionAdvanceDisableModUpdate")) result.value["disableModUpdates"] = *value;
    consumed.insert("VersionAdvanceAssetsV2"); consumed.insert("VersionAdvanceAssets");
    auto disable_file = setup_bool(local, "VersionAdvanceAssetsV2");
    if (!disable_file && setup_number(local, "VersionAdvanceAssets") == 2) disable_file = true;
    if (disable_file) result.value["disableFileCheck"] = *disable_file;
    consumed.insert("VersionServerLogin");
    const auto login = setup_number(local, "VersionServerLogin").value_or(-1);
    if (login == 0) result.value["serverLogin"] = "any";
    if (login == 1) result.value["serverLogin"] = "microsoft";
    if (login == 2) result.value["serverLogin"] = "offline";
    if (login == 4) result.value["serverLogin"] = "thirdParty";
    for (const auto& [input, output] : std::array<std::pair<std::string_view, std::string_view>, 4>{{
        {"VersionServerAuthServer", "authServer"}, {"VersionServerAuthRegister", "serverRegistration"},
        {"VersionServerAuthName", "serverName"}, {"VersionServerEnter", "serverAddress"}}}) {
        if (auto value = local_value(input)) result.value[std::string(output)] = *value;
    }
    for (const auto* key : {"VersionVanillaName", "VersionForge", "VersionFabric", "VersionNeoForge", "VersionOptiFine", "VersionLiteLoader", "VersionQuilt"}) consumed.insert(key);
    for (const auto& [key, value] : local) {
        (void)value;
        if (!consumed.contains(key) && !key.starts_with("VersionCache") && !key.starts_with("Cache")) result.unmapped_legacy_keys.push_back(key);
    }
    return result;
}
bool safe_portable_text(const Json& value, std::size_t limit) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() <= limit && !contains_host_path(text) && !contains_credentials(text)
        && std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 && c != '\n' && c != '\r' && c != '\t'; });
}
bool portable_valid(std::string_view key, const Json& value) {
    if (value.is_null()) return true;
    if (has(bool_fields, key)) return value.is_boolean();
    if (key == "windowTitle" || key == "customInfo" || key == "jvmArgs" || key == "gameArgs") return safe_portable_text(value, 16 * 1024);
    if (key == "serverName") return safe_portable_text(value, 256);
    if (key == "serverRegistration" || key == "authServer" || key == "serverAddress") return safe_portable_text(value, 4096);
    const auto selected = [&](std::initializer_list<std::string_view> values) { return value.is_string() && choice(value.get_ref<const std::string&>(), values); };
    if (key == "launcherVisibility") return selected({"keep", "hide", "hide-close", "close", "minimize"});
    if (key == "windowMode") return selected({"default", "custom", "fullscreen", "same-as-launcher", "maximize"});
    if (key == "memoryMode") return selected({"global", "auto", "custom"});
    if (key == "memoryManagement") return selected({"auto-zgc", "generational-zgc", "g1", "g1-tuned", "custom"});
    if (key == "processPriority") return selected({"low", "normal", "high"});
    if (key == "serverLogin") return selected({"any", "offline", "microsoft", "thirdParty"});
    const auto number = detail::unsigned_value(value);
    if (key == "memoryMb") return number && *number >= 512 && *number <= 65536;
    if (key == "windowWidth") return number && *number >= 320 && *number <= 16384;
    if (key == "windowHeight") return number && *number >= 240 && *number <= 16384;
    return false;
}
} // namespace

Json default_version_settings() {
    Json result = Json::object();
    for (const auto key : bool_fields) result[std::string(key)] = nullptr;
    for (const auto key : text_fields) if (!key.starts_with("unused")) result[std::string(key)] = nullptr;
    for (const auto key : number_fields) result[std::string(key)] = nullptr;
    result["memoryMode"] = "global";
    return result;
}
Json normalize_version_settings(const Json& value) {
    auto result = default_version_settings();
    if (value.is_object()) {
        for (const auto& field : value.items()) {
            const auto& key = field.key();
            const auto& data = field.value();
            if (key == "settingsSource") continue;
            const bool inherits = data.is_null() || data == "global";
            bool valid = true;
            if (has(bool_fields, key)) valid = inherits || data.is_boolean();
            else if (has(text_fields, key)) valid = inherits || data.is_string();
            else if (has(number_fields, key)) valid = inherits || detail::unsigned_value(data).has_value();
            if (valid) result[key] = data;
        }
    }
    auto mode = detail::string_at(result, "memoryMode").value_or("global");
    if (!choice(mode, {"global", "auto", "custom"}) || (mode == "custom" && !detail::unsigned_value(result["memoryMb"]))) mode = "global";
    result["memoryMode"] = mode;
    if (auto management = detail::string_at(result, "memoryManagement")) {
        auto normalized = detail::trim(*management);
        if (normalized.empty()) normalized = "g1-tuned";
        result["memoryManagement"] = choice(normalized, {"auto-zgc", "generational-zgc", "g1", "g1-tuned", "custom"}) ? Json(normalized) : Json(nullptr);
    }
    return result;
}
std::map<std::string, std::string> parse_legacy_setup(std::string_view text) {
    if (text.size() > settings_max_bytes) throw Error("limit", "Legacy setup exceeds size limit");
    std::map<std::string, std::string> result;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.starts_with("\xef\xbb\xbf")) line.remove_prefix(3);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos && colon != 0) result[std::string(line.substr(0, colon))] = std::string(line.substr(colon + 1));
        start = end + 1;
    }
    return result;
}
LocalSettings read_local_settings(const fs::path& root, std::string_view id) {
    const auto directory = checked_instance_directory(root, id);
    if (is_pending(directory)) throw Error("pending_instance", "Pending instance has no public local settings");
    try {
        const auto value = detail::read_optional_json(directory / settings_file, settings_max_bytes, true);
        if (value && value->is_object()) return {*value, "nickel", {}};
    } catch (const Error& error) { if (error.code() == "unsafe_path") throw; }
    try { return legacy_settings(root, id); }
    catch (const Error& error) { if (error.code() == "unsafe_path") throw; return {}; }
}
LocalSettings read_reserved_local_settings(const InstanceTransaction& transaction, std::stop_token cancellation) {
    detail::check_cancelled(cancellation);
    detail::DirectoryLock lock(transaction.root());
    transaction.check_owned();
    const auto document = detail::read_optional_json(transaction.directory() / profile_file, 64 * 1024);
    if (!document) throw Error("ownership_changed", "Pending reservation profile is missing");
    const auto persisted = profile_from_json(*document, transaction.profile().name);
    if (profile_to_json(persisted) != profile_to_json(transaction.profile()))
        throw Error("ownership_changed", "Pending reservation profile differs from its owner");
    LocalSettings result;
    const auto value = detail::read_optional_json(transaction.directory() / settings_file, settings_max_bytes, true);
    if (value) {
        if (!value->is_object()) throw Error("invalid_settings", "Pending reservation settings must be an object");
        result.value = *value;
    } else result = legacy_settings(transaction.root(), transaction.profile().name);
    transaction.check_owned();
    detail::check_cancelled(cancellation);
    return result;
}
fs::path reserved_resource_directory(const InstanceTransaction& transaction, const Json& global,
                                     std::stop_token cancellation) {
    const auto local = read_reserved_local_settings(transaction, cancellation);
    const auto result = resolve_isolation(global, local.value) ? transaction.directory() : transaction.root();
    reject_links(result);
    transaction.check_owned();
    detail::check_cancelled(cancellation);
    return result;
}
Json get_version_settings(const fs::path& root, std::string_view id) {
    const auto raw = read_local_settings(root, id);
    auto result = normalize_version_settings(raw.value);
    result["settingsSource"] = raw.source;
    return result;
}
void set_version_settings(const fs::path& root, std::string_view id, const Json& value) {
    const auto directory = checked_instance_directory(root, id);
    if (!value.is_object()) throw Error("invalid_settings", "Version settings must be a JSON object");
    if (value.dump().size() > settings_max_bytes) throw Error("limit", "Settings exceed 256 KiB");
    detail::DirectoryLock lock(root);
    if (is_pending(directory)) throw Error("pending_instance", "Cannot edit pending instance");
    auto result = normalize_version_settings(value);
    try {
        if (auto existing = detail::read_optional_json(directory / settings_file, settings_max_bytes, true); existing && existing->is_object()) {
            for (const auto& field : existing->items()) {
                if (!has(bool_fields, field.key()) && !has(text_fields, field.key()) && !has(number_fields, field.key())
                    && field.key() != "settingsSource" && !result.contains(field.key())) result[field.key()] = field.value();
            }
        }
    } catch (const Error& error) { if (error.code() == "unsafe_path") throw; }
    const auto output = result.dump(2);
    if (output.size() > settings_max_bytes) throw Error("limit", "Normalized settings exceed 256 KiB");
    detail::atomic_write(directory / settings_file, output);
}
std::vector<std::string> split_arguments(std::string_view raw) {
    if (raw.size() > settings_max_bytes) throw Error("limit", "Arguments exceed size limit");
    std::vector<std::string> result;
    std::string current;
    char quote = 0;
    bool started = false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\0') throw Error("invalid_arguments", "Arguments contain NUL");
        if (c == '\\' && i + 1 < raw.size() && (raw[i + 1] == quote || raw[i + 1] == '"')) {
            current += raw[++i]; started = true;
        } else if (quote != 0 && c == quote) quote = 0;
        else if (quote == 0 && (c == '"' || c == '\'')) { quote = c; started = true; }
        else if (quote == 0 && std::isspace(static_cast<unsigned char>(c))) {
            if (started) { result.push_back(std::move(current)); current.clear(); started = false; }
        } else { current += c; started = true; }
    }
    if (quote != 0) throw Error("invalid_arguments", "Argument quote is not closed");
    if (started) result.push_back(std::move(current));
    return result;
}
bool resolve_isolation(const Json& global, const Json& local) { return boolean(global, local, "versionIsolation", true); }
std::uint64_t automatic_memory_mb(const MemoryContext& memory) {
    if (!memory.available_mb) return 4096;
    const auto total = memory.total_mb.value_or(std::max<std::uint64_t>(*memory.available_mb, 4096));
    double available = std::round(static_cast<double>(std::min(*memory.available_mb, total)) / 1024 * 10) / 10;
    const auto mods = static_cast<double>(memory.mod_count);
    const auto targets = memory.kind == VersionKind::moddable ? std::array<double, 4>{0.5 + mods / 150, 1.5 + mods / 90, 2.7 + mods / 50, 4.5 + mods / 25}
        : memory.kind == VersionKind::optifine ? std::array<double, 4>{0.5, 1.5, 3, 5} : std::array<double, 4>{0.5, 1.5, 2.5, 4};
    double give = std::min(available, targets[1]);
    available -= targets[1];
    for (const auto& [target, factor] : std::array<std::pair<double, double>, 3>{{{targets[2] - targets[1], 0.7}, {targets[3] - targets[2], 0.4}, {targets[3], 0.15}}}) {
        if (available >= 0.1) { give += std::min(available * factor, target); available -= target / factor; }
    }
    const auto rounded = static_cast<std::uint64_t>(std::round(std::round(std::max(give, targets[0]) * 10) / 10 * 1024));
    return std::clamp<std::uint64_t>(rounded, 512, std::max<std::uint64_t>(total, 512));
}
EffectiveSettings resolve_settings(const Json& global, const Json& local, const MemoryContext& memory) {
    EffectiveSettings result;
    const auto mode = validated_choice(text(global, local, "memoryMode", "auto"), {"auto", "custom"}, "memoryMode");
    result.memory_mb = mode == "auto" ? automatic_memory_mb(memory) : number(global, local, "memoryMb", 4096);
    if (mode == "custom" && (result.memory_mb < 512 || (memory.total_mb && *memory.total_mb > 0 && result.memory_mb > *memory.total_mb)))
        throw Error("invalid_memory", "Manual memory does not fit physical memory");
    result.isolation = resolve_isolation(global, local);
    result.java_path = text(global, local, "javaPath");
    result.window_title = text(global, local, "windowTitle");
    result.custom_info = text(global, local, "customInfo");
    result.visibility = validated_choice(text(global, local, "launcherVisibility", "keep"), {"keep", "hide", "hide-close", "close", "minimize"}, "launcherVisibility");
    result.priority = validated_choice(text(global, local, "processPriority", "normal"), {"low", "normal", "high"}, "processPriority");
    result.window_mode = validated_choice(text(global, local, "windowMode", "default"), {"default", "custom", "fullscreen", "same-as-launcher", "maximize"}, "windowMode");
    result.width = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(number(global, local, "windowWidth", 854), 320, 16384));
    result.height = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(number(global, local, "windowHeight", 480), 240, 16384));
    result.high_performance_gpu = boolean(global, local, "highPerformanceGpu");
    result.disable_wrapper = boolean(global, local, "disableWrapper");
    result.disable_lwjgl_agent = boolean(global, local, "disableLwjglAgent");
    result.offline_skin = validated_choice(text(global, local, "offlineSkin", "random"), {"random", "steve", "alex", "official", "custom"}, "offlineSkin");
    result.offline_skin_name = text(global, local, "offlineSkinName");
    result.offline_skin_path = text(global, local, "offlineSkinPath");
    result.offline_skin_slim = boolean(global, local, "offlineSkinSlim");
    result.jvm_args = split_arguments(text(global, local, "jvmArgs"));
    result.game_args = split_arguments(text(global, local, "gameArgs"));
    result.pre_launch_command = text(global, local, "preLaunchCommand");
    auto management = detail::trim(text(global, local, "memoryManagement", "g1-tuned"));
    if (management.empty()) management = "g1-tuned";
    result.memory_management = validated_choice(management, {"auto-zgc", "generational-zgc", "g1", "g1-tuned", "custom"}, "memoryManagement");
    result.disable_file_check = boolean(global, local, "disableFileCheck");
    result.disable_mod_updates = boolean(global, local, "disableModUpdates");
    result.server_login = validated_choice(text(global, local, "serverLogin", "any"), {"any", "offline", "microsoft", "thirdParty"}, "serverLogin");
    result.server_name = text(global, local, "serverName");
    if (result.server_name.size() > 256 || std::any_of(result.server_name.begin(), result.server_name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw Error("invalid_settings", "Server name contains control characters or is too long");
    result.auth_server = text(global, local, "authServer");
    result.server_address = text(global, local, "serverAddress");
    result.server_registration = text(global, local, "serverRegistration");
    return result;
}
std::vector<std::string> gc_arguments(std::string_view mode, std::uint32_t java, std::optional<std::uint32_t> windows_build) {
    const auto selected = validated_choice(std::string(mode.empty() ? "g1-tuned" : mode), {"auto-zgc", "generational-zgc", "g1", "g1-tuned", "custom"}, "memoryManagement");
    if (selected == "custom") return {};
#ifdef _WIN32
    const bool windows_supported = windows_build && *windows_build >= 17763;
#else
    const bool windows_supported = !windows_build || *windows_build >= 17763;
#endif
    const bool zgc = windows_supported && ((selected == "auto-zgc" && java >= 15) || (selected == "generational-zgc" && java >= 21));
    std::vector<std::string> result{"-XX:+UnlockExperimentalVMOptions"};
    if (java >= 24) result.emplace_back("-XX:+UseCompactObjectHeaders");
    if (zgc) {
        result.emplace_back("-XX:+UseZGC");
        if (java >= 21 && java <= 22) result.emplace_back("-XX:+ZGenerational");
    } else {
        result.insert(result.end(), {"-XX:+UseG1GC", "-XX:G1NewSizePercent=20", "-XX:G1ReservePercent=20", "-XX:G1HeapRegionSize=32M", "-XX:MaxGCPauseMillis=50"});
        if (java <= 7) result.emplace_back("-XX:MaxPermSize=512m");
        if (selected == "g1-tuned") {
            result.emplace_back("-XX:+PerfDisableSharedMem");
            if (java == 8) result.emplace_back("-XX:+ParallelRefProcEnabled");
            if (java >= 12) result.insert(result.end(), {"-XX:MinHeapFreeRatio=25", "-XX:MaxHeapFreeRatio=40"});
        }
    }
    return result;
}
fs::path resource_directory(const fs::path& root, std::string_view id, const Json& global) {
    const auto directory = checked_instance_directory(root, id);
    if (is_pending(directory)) throw Error("pending_instance", "Pending instance has no public resource root");
    return resolve_isolation(global, read_local_settings(root, id).value) ? directory : root;
}
std::uint64_t count_mods(const fs::path& directory) {
    reject_links(directory);
    if (!fs::is_directory(detail::native_path(directory))) return 0;
    std::uint64_t count = 0;
    for (const auto& entry : detail::directory_entries(directory)) {
        reject_links(entry.path(), false);
        if (entry.is_regular_file() && choice(detail::ascii_lower(path_to_utf8(entry.path().extension())), {".jar", ".zip", ".litemod"})) ++count;
    }
    return count;
}
bool contains_host_path(std::string_view input) {
    const auto lower = detail::ascii_lower(input);
    if (lower.find("%userprofile%") != std::string::npos || lower.find("%appdata%") != std::string::npos
        || lower.find("${home}") != std::string::npos || lower.find("$home") != std::string::npos
        || lower.find("~/") != std::string::npos || lower.find("~\\") != std::string::npos
        || lower.find("file://") != std::string::npos || lower.find("\\\\") != std::string::npos) return true;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const bool token_start = i == 0 || !std::isalnum(static_cast<unsigned char>(input[i - 1]));
        if (token_start && i + 2 < input.size() && std::isalpha(static_cast<unsigned char>(input[i]))
            && input[i + 1] == ':' && (input[i + 2] == '/' || input[i + 2] == '\\')) return true;
        if (input[i] == '/' && (i == 0 || std::isspace(static_cast<unsigned char>(input[i - 1]))
            || std::string_view("='\"([:,").find(input[i - 1]) != std::string_view::npos)) {
            if (i > 0 && input[i - 1] == ':' && i + 1 < input.size() && input[i + 1] == '/') {
                const auto prefix = lower.substr(0, i - 1);
                if (prefix.ends_with("https") || prefix.ends_with("http") || prefix.ends_with("wss")
                    || prefix.ends_with("ws") || prefix.ends_with("ftp")) continue;
            }
            return true;
        }
    }
    return false;
}
bool contains_credentials(std::string_view input) {
    std::string folded;
    folded.reserve(input.size());
    for (unsigned char c : input) if (c != '-' && c != '_' && !std::isspace(c)) folded += static_cast<char>(std::tolower(c));
    constexpr std::array<std::string_view, 24> markers{"accesstoken", "clienttoken", "refreshtoken", "clientsecret", "clientcredential", "sessiontoken", "bearertoken", "apikey", "password", "authorization", "privatekey", "secretkey", "invitationcode", "invitecode", "authtoken", "apitoken", "passwd", "-----begin", "bearer", "basic", "accounts.json", "usercache.json", "usernamecache.json", "launcheraccounts"};
    if (std::any_of(markers.begin(), markers.end(), [&](auto marker) { return folded.find(marker) != std::string::npos; })) return true;
    if (folded.find("\\u") != std::string::npos || folded.find("\\x") != std::string::npos) return true;
    const auto url = folded.find("://");
    if (url != std::string::npos) {
        const auto end = folded.find('/', url + 3);
        const auto at = folded.find('@', url + 3);
        if (at != std::string::npos && (end == std::string::npos || at < end)) return true;
    }
    static const std::regex jwt(R"(eyJ[A-Za-z0-9_-]{5,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{16,})");
    return std::regex_search(input.begin(), input.end(), jwt);
}
SettingsProjection project_portable_settings(const Json& value) {
    SettingsProjection result;
    if (!value.is_object()) return result;
    for (const auto field : portable_fields) {
        const auto it = value.find(std::string(field));
        if (it == value.end() || it->is_null()) continue;
        if (portable_valid(field, *it)) { result.settings[std::string(field)] = *it; result.included.emplace_back(field); }
        else result.omitted.emplace_back(field);
    }
    return result;
}
Json validate_portable_settings(const Json& value) {
    detail::strict_keys(value, portable_fields);
    if (value.dump().size() > settings_max_bytes) throw Error("limit", "Portable settings exceed size limit");
    for (const auto& field : value.items()) if (!portable_valid(field.key(), field.value())) throw Error("unsafe_settings", "Unsafe portable setting: " + field.key());
    return value;
}
Json merge_portable_settings(const Json& existing, const Json& incoming) {
    auto checked = validate_portable_settings(incoming);
    if (!existing.is_null() && !existing.is_object()) throw Error("invalid_settings", "Existing settings is not an object");
    auto result = existing.is_object() ? existing : Json::object();
    result.update(checked);
    if (result.dump().size() > settings_max_bytes) throw Error("limit", "Merged settings exceed size limit");
    return result;
}
void apply_portable_settings(const fs::path& directory, const Json& incoming) {
    reject_links(directory);
    if (!fs::is_directory(detail::native_path(directory))) throw Error("missing_instance", "Cannot apply settings to missing instance");
    const auto existing = detail::read_optional_json(directory / settings_file, settings_max_bytes);
    const auto merged = merge_portable_settings(existing.value_or(Json(nullptr)), incoming);
    detail::atomic_write(directory / settings_file, merged.dump(2));
}
} // namespace nickel::instances
