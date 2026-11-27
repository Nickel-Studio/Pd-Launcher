#include "core/config/paths.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"
#include "platform/process.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

namespace nickel {
namespace {
using namespace foundation_detail;
constexpr std::array<std::string_view, 10> entries{
    "settings.json", "accounts.json", "active_account.json", "game.json", "instances.json",
    "secrets.local.json", "credentials.key", "offline-skins", "agents", "cache"};

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}
// Rejects C0 plus UTF-8 encoded C1 controls, which survive most trim and
// length-only validation paths.
bool controls(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 32 || c == 127) return true;
        if (c == 0xC2 && i + 1 < text.size()) {
            const auto next = static_cast<unsigned char>(text[i + 1]);
            if (next >= 0x80 && next <= 0x9F) return true;
        }
    }
    return false;
}

// Instance names double as directory names on every host: reject traversal, control
// characters, Windows device names (including superscript digits) and reserved names.
bool valid_instance_name(std::string_view name) {
    if (name.empty() || trim(name) != name || name.size() > 160 || name == "." ||
        name.find("..") != std::string_view::npos || name.back() == '.' || controls(name) ||
        name.find_first_of("<>:\"/\\|?*") != std::string_view::npos) return false;
    const auto end = name.find('.');
    std::string base(trim(name.substr(0, end)));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) {
        return c < 128 ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
    });
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
        base == "CLOCK$" || base == "CONIN$" || base == "CONOUT$") return false;
    if (base.starts_with("COM") || base.starts_with("LPT")) {
        const auto suffix = std::string_view(base).substr(3);
        if ((suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') ||
            suffix == "\xC2\xB9" || suffix == "\xC2\xB2" || suffix == "\xC2\xB3") return false;
    }
    std::string descriptor(name);
    std::transform(descriptor.begin(), descriptor.end(), descriptor.begin(), [](unsigned char c) {
        return c < 128 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    });
    return descriptor != "nickel.profile" && descriptor != "gamedata" && descriptor != "nickel.settings";
}

void check_fields(const Json& object, const std::set<std::string>& allowed) {
    if (!object.is_object()) fail("CorruptData", "Portable marker must be an object");
    for (auto i = object.begin(); i != object.end(); ++i) {
        if (!allowed.contains(i.key())) fail("Unsupported", "Portable marker contains an unknown field");
    }
}

std::string required_string(const Json& object, std::string_view key) {
    const auto found = object.find(std::string(key));
    if (found == object.end() || !found->is_string()) fail("CorruptData", "Portable marker is missing a string field");
    return found->get<std::string>();
}

std::optional<Json> read_json_if_valid(const std::filesystem::path& path, std::uint64_t limit) {
    if (path_missing(path)) return std::nullopt;
    reject_path_links(path);
    std::error_code error;
    const auto size = std::filesystem::file_size(native_filesystem_path(path), error);
    if (error || size > limit) return std::nullopt;
    const auto text = read_text_bounded(path, limit);
    auto value = Json::parse(text, nullptr, false);
    if (value.is_discarded()) return std::nullopt;
    return value;
}

std::optional<std::string> registry_choice(const Json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto roots = value.find("roots");
    if (roots != value.end() && (!roots->is_array() ||
        !std::all_of(roots->begin(), roots->end(), [](const Json& root) { return root.is_string(); }))) return std::nullopt;
    const auto active = value.find("active");
    if (active != value.end() && !active->is_null() && !active->is_string()) return std::nullopt;
    const auto labels = value.find("labels");
    if (labels != value.end() && (!labels->is_object() ||
        !std::all_of(labels->begin(), labels->end(), [](const Json& label) { return label.is_string(); }))) return std::nullopt;
    if (active != value.end() && active->is_string()) {
        const auto text = active->get<std::string>();
        if (!trim(text).empty()) return text;
    }
    if (roots != value.end() && !roots->empty()) return roots->front().get<std::string>();
    return std::nullopt;
}

} // namespace

PathContext PathContext::system() {
    PathContext context;
    context.executable = current_executable();
    const auto env = current_environment();
    auto environment = [&env](std::string_view key) -> std::filesystem::path {
        const auto item = env.find(std::string(key));
        if (item == env.end() || item->second.empty()) return {};
        return path_from_utf8(item->second);
    };
#ifdef _WIN32
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DONT_VERIFY, nullptr, &roaming))) {
        fail("IoError", "Cannot locate the system configuration directory");
    }
    context.system_config_base = std::filesystem::path(roaming);
    CoTaskMemFree(roaming);
    context.app_data_base = context.system_config_base / path_from_utf8(application_identifier);
#elif defined(__APPLE__)
    context.macos = true;
    const auto home = environment("HOME");
    if (home.empty()) fail("IoError", "Cannot locate the user home directory");
    context.system_config_base = home / "Library/Application Support";
    context.app_data_base = context.system_config_base / path_from_utf8(application_identifier);
#else
    const auto home = environment("HOME");
    auto config = environment("XDG_CONFIG_HOME");
    auto data = environment("XDG_DATA_HOME");
    if (config.empty() || !config.is_absolute()) config = home / ".config";
    if (data.empty() || !data.is_absolute()) data = home / ".local/share";
    if (home.empty() && (!config.is_absolute() || !data.is_absolute())) fail("IoError", "Cannot locate user configuration roots");
    context.system_config_base = config;
    context.app_data_base = data / path_from_utf8(application_identifier);
#endif
    return context;
}

std::filesystem::path directory_for_executable(const std::filesystem::path& executable, bool macos) {
    if (executable.empty() || executable.parent_path().empty()) fail("InvalidArgument", "Cannot locate the executable directory");
    const auto directory = executable.parent_path();
    if (macos && directory.filename() == "MacOS") {
        const auto contents = directory.parent_path();
        const auto bundle = contents.parent_path();
        if (contents.filename() == "Contents" && bundle.extension() == ".app" && !bundle.parent_path().empty()) {
            return bundle.parent_path();
        }
    }
    return directory;
}

bool has_configuration(const std::filesystem::path& directory) {
    for (const auto name : entries) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(native_filesystem_path(directory / path_from_utf8(name)), error);
        if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) return true;
        if (!error && status.type() != std::filesystem::file_type::not_found) return true;
    }
    return false;
}

// The marker is attacker-controlled: the fixed payload/game/data layout and hex-only
// package id keep extraction targets from being steered by its strings.
PortableManifest parse_portable_manifest(std::string_view text) {
    if (text.size() > 64 * 1024 || !valid_utf8(text)) fail("LimitExceeded", "Portable marker is invalid or oversized");
    const auto value = Json::parse(text, nullptr, false);
    if (value.is_discarded()) fail("CorruptData", "Portable marker is invalid; extract the original archive again");
    check_fields(value, {"schemaVersion", "format", "packageId", "name", "version", "instanceName", "payload",
                         "gameDirectory", "dataDirectory", "launcher", "personalization"});
    const auto schema = value.find("schemaVersion");
    if (schema == value.end() || !schema->is_number_integer() || *schema != 1) fail("Unsupported", "Portable marker schema is unknown");
    PortableManifest result;
    result.format = required_string(value, "format");
    result.package_id = required_string(value, "packageId");
    result.name = required_string(value, "name");
    result.version = required_string(value, "version");
    result.instance_name = required_string(value, "instanceName");
    result.payload = required_string(value, "payload");
    result.game_directory = required_string(value, "gameDirectory");
    result.data_directory = required_string(value, "dataDirectory");
    if (result.format != "nickel-portable") fail("Unsupported", "Portable marker format is unknown");
    if (result.package_id.size() != 32 || !std::all_of(result.package_id.begin(), result.package_id.end(),
        [](unsigned char c) { return std::isxdigit(c) != 0; })) fail("CorruptData", "Portable package identity is invalid");
    for (const auto* field : {&result.name, &result.version}) {
        if (trim(*field).empty() || field->size() > 512 || controls(*field)) fail("CorruptData", "Portable package name or version is invalid");
    }
    if (!valid_instance_name(result.instance_name)) fail("CorruptData", "Portable instance name is invalid");
    if (result.payload != "Nickel/pack.mrpack" || result.data_directory != "Nickel" || result.game_directory != ".minecraft") {
        fail("UnsafePath", "Portable package directories do not match the fixed layout");
    }
    if (const auto personalization = value.find("personalization"); personalization != value.end() && !personalization->is_null()) {
        if (!personalization->is_string() || *personalization != "Nickel/personalization.json") {
            fail("UnsafePath", "Portable personalization path is invalid");
        }
        result.personalization = personalization->get<std::string>();
    }
    if (const auto launcher = value.find("launcher"); launcher != value.end() && !launcher->is_null()) {
        check_fields(*launcher, {"path", "os", "arch"});
        const auto path = required_string(*launcher, "path");
        const auto os = required_string(*launcher, "os");
        const auto arch = required_string(*launcher, "arch");
        if (!((os == "windows" && path == "Nickel.exe") || (os == "linux" && path == "Nickel-launcher")) ||
            arch.empty() || arch.size() > 32 || !std::all_of(arch.begin(), arch.end(),
                [](unsigned char c) { return (c < 128 && std::isalnum(c)) || c == '_'; })) {
            fail("CorruptData", "Portable launcher platform is invalid");
        }
        result.launcher = *launcher;
    }
    return result;
}

Json portable_manifest_json(const PortableManifest& manifest) {
    Json value{{"schemaVersion", manifest.schema_version}, {"format", manifest.format},
        {"packageId", manifest.package_id}, {"name", manifest.name}, {"version", manifest.version},
        {"instanceName", manifest.instance_name}, {"payload", manifest.payload},
        {"gameDirectory", manifest.game_directory}, {"dataDirectory", manifest.data_directory}};
    if (manifest.launcher) value["launcher"] = *manifest.launcher;
    if (manifest.personalization) value["personalization"] = *manifest.personalization;
    return value;
}

void validate_portable_manifest(const PortableManifest& manifest) {
    (void)parse_portable_manifest(portable_manifest_json(manifest).dump());
}

std::optional<PortableManifest> read_portable_manifest(const std::filesystem::path& root) {
    const auto path = root / path_from_utf8(portable_manifest_name);
    if (path_missing(path)) return std::nullopt;
    return parse_portable_manifest(read_text_bounded(path, 64 * 1024));
}

PathResolver::PathResolver(PathContext context) : context_(std::move(context)) {
    if (context_.executable.empty() || context_.system_config_base.empty() || context_.app_data_base.empty()) {
        fail("InvalidArgument", "All path-context roots must be supplied");
    }
}
const PathContext& PathResolver::context() const noexcept { return context_; }
std::filesystem::path PathResolver::executable_directory() const { return directory_for_executable(context_.executable, context_.macos); }
std::optional<PortableManifest> PathResolver::portable_manifest() const { return read_portable_manifest(executable_directory()); }

// A portable marker wins outright; otherwise an existing system config is preferred so
// an adjacent data directory cannot silently fork the profile.
std::filesystem::path PathResolver::config_root(bool create) const {
    std::filesystem::path selected;
    if (const auto manifest = portable_manifest()) {
        selected = executable_directory() / path_from_utf8(manifest->data_directory);
        reject_path_links(selected);
    } else {
        const auto system = context_.system_config_base / path_from_utf8(application_identifier);
        const auto adjacent = executable_directory() / path_from_utf8(data_directory_name);
        selected = has_configuration(system) ? system : adjacent;
    }
    reject_path_links(selected);
    if (create) {
        DirectoryGuard pinned(selected, true);
        pinned.verify();
    }
    return selected;
}

std::filesystem::path PathResolver::default_game_directory() const {
    if (const auto manifest = portable_manifest()) {
        const auto path = executable_directory() / path_from_utf8(manifest->game_directory);
        reject_path_links(path);
        return path;
    }
    const auto root = config_root();
    if (const auto game = read_json_if_valid(root / "game.json", 256 * 1024); game && game->is_object()) {
        const auto field = game->find("gameDir");
        if (field != game->end() && field->is_string()) {
            const auto text = field->get<std::string>();
            if (!trim(text).empty()) return path_from_utf8(text);
        }
    }
    if (const auto registry = read_json_if_valid(root / "instances.json", 256 * 1024)) {
        if (const auto path = registry_choice(*registry)) return path_from_utf8(*path);
    }
    const auto current = context_.app_data_base / "minecraft";
    std::error_code error;
    if (std::filesystem::is_directory(native_filesystem_path(current / "versions"), error) ||
        std::filesystem::is_regular_file(native_filesystem_path(current / "gameData.json"), error)) {
        return current;
    }
    return executable_directory() / path_from_utf8(game_directory_name);
}

std::filesystem::path PathResolver::bundled_game_directory() const {
    if (const auto manifest = portable_manifest()) {
        const auto path = executable_directory() / path_from_utf8(manifest->game_directory);
        reject_path_links(path);
        return path;
    }
    return executable_directory() / path_from_utf8(game_directory_name);
}

} // namespace nickel
