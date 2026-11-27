#include "launcher.hpp"
#include "maven.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <regex>
#include <set>
#include <stdexcept>

namespace nickel::minecraft {
namespace {

using Placeholders = std::map<std::string, std::string, std::less<>>;

std::string lower(std::string_view input) {
    std::string result(input);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string string_value(const Json& object, std::string_view key, std::string fallback = {}) {
    if (!object.is_object()) return fallback;
    const auto found = object.find(std::string(key));
    if (found == object.end() || found->is_null()) return fallback;
    if (!found->is_string()) throw std::invalid_argument("Invalid Minecraft launch string: " + std::string(key));
    return found->get<std::string>();
}

void validate_token(std::string_view token) {
    if (token.size() > 4 * 1024 * 1024 || token.find('\0') != std::string_view::npos) throw std::invalid_argument("Minecraft argument exceeds its size limit or contains NUL");
}

std::string expand(std::string_view input, const Placeholders& placeholders, bool strict = true) {
    validate_token(input);
    std::string result;
    while (true) {
        const auto start = input.find("${");
        if (start == std::string_view::npos) { result.append(input); break; }
        result.append(input.substr(0, start));
        input.remove_prefix(start + 2);
        const auto end = input.find('}');
        if (end == std::string_view::npos) throw std::invalid_argument("Unterminated Minecraft argument placeholder");
        const auto key = input.substr(0, end);
        const auto found = placeholders.find(key);
        if (found == placeholders.end()) {
            if (strict) throw std::invalid_argument("Unresolved Minecraft argument placeholder: " + std::string(key));
            result += "${" + std::string(key) + '}';
        } else result += found->second;
        input.remove_prefix(end + 1);
    }
    validate_token(result);
    return result;
}

std::vector<std::string> evaluate_arguments(const Json& entries, const Platform& platform) {
    if (entries.is_null()) return {};
    if (!entries.is_array()) throw std::invalid_argument("Minecraft argument collection must be an array");
    std::vector<std::string> result;
    for (const auto& entry : entries) {
        if (entry.is_string()) { result.push_back(entry.get<std::string>()); continue; }
        if (!entry.is_object() || !entry.contains("value")) throw std::invalid_argument("Invalid conditional Minecraft argument");
        if (entry.contains("rules") && !rules_allow(parse_rules(entry["rules"]), platform)) continue;
        const auto& value = entry["value"];
        if (value.is_string()) result.push_back(value.get<std::string>());
        else if (value.is_array()) for (const auto& token : value) {
            if (!token.is_string()) throw std::invalid_argument("Conditional Minecraft argument is not a string");
            result.push_back(token.get<std::string>());
        }
        else throw std::invalid_argument("Conditional Minecraft argument is not a string or array");
    }
    if (result.size() > 16384) throw std::invalid_argument("Minecraft argument count limit exceeded");
    return result;
}

std::string classpath_string(std::span<const std::filesystem::path> entries, const Platform& platform) {
    std::string result;
    for (const auto& entry : entries) {
        if (!result.empty()) result.push_back(platform.classpath_separator());
        result += java_path_argument(path_utf8(entry), platform.os == "windows");
    }
    return result;
}

void set_game_argument(std::vector<std::string>& arguments, std::string_view key, std::string value) {
    for (std::size_t index = 0; index < arguments.size();) {
        if (arguments[index] == key) {
            const auto count = index + 1 < arguments.size() ? 2 : 1;
            arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index), arguments.begin() + static_cast<std::ptrdiff_t>(index + count));
        } else if (arguments[index].starts_with(std::string(key) + '=')) arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index));
        else ++index;
    }
    arguments.emplace_back(key);
    arguments.emplace_back(std::move(value));
}

void set_game_switch(std::vector<std::string>& arguments, std::string_view key, bool enabled) {
    std::erase(arguments, key);
    if (enabled) arguments.emplace_back(key);
}

void deduplicate_tweakers(std::vector<std::string>& arguments) {
    std::set<std::string> seen;
    for (std::size_t index = 0; index < arguments.size();) {
        if (arguments[index] == "--tweakClass") {
            if (index + 1 >= arguments.size()) throw std::invalid_argument("Minecraft tweakClass is missing its class name");
            if (!seen.insert(arguments[index + 1]).second) arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index), arguments.begin() + static_cast<std::ptrdiff_t>(index + 2));
            else index += 2;
        } else ++index;
    }
}

bool module_option(std::string_view argument) {
    return argument == "-p" || argument == "--module-path" || argument == "--upgrade-module-path" ||
           argument.starts_with("--module-path=") || argument.starts_with("--upgrade-module-path=");
}

bool agent_or_module(std::string_view argument) {
    return module_option(argument) || argument == "-m" || argument == "--module" || argument.starts_with("--module=") ||
           argument.starts_with("-javaagent:") || argument.starts_with("-agentpath:") || argument.starts_with("-agentlib:") ||
           argument.starts_with("-Xrun") || argument.starts_with('@') || argument.starts_with("-Djdk.module.path=");
}

std::vector<std::filesystem::path> module_paths(const std::vector<std::string>& arguments, const Platform& platform) {
    std::vector<std::filesystem::path> result;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!module_option(arguments[index])) continue;
        std::string_view value;
        const auto equal = arguments[index].find('=');
        if (equal != std::string::npos) value = std::string_view(arguments[index]).substr(equal + 1);
        else {
            if (index + 1 >= arguments.size()) throw std::invalid_argument("Java module path is missing its value");
            value = arguments[++index];
        }
        while (!value.empty()) {
            const auto end = value.find(platform.classpath_separator());
            const auto part = value.substr(0, end);
            if (!part.empty()) result.emplace_back(std::u8string(reinterpret_cast<const char8_t*>(part.data()), part.size()));
            if (end == std::string_view::npos) break;
            value.remove_prefix(end + 1);
        }
    }
    return result;
}

bool same_path(const std::filesystem::path& a, const std::filesystem::path& b, bool windows) {
    auto left = path_utf8(a.lexically_normal()), right = path_utf8(b.lexically_normal());
    std::replace(left.begin(), left.end(), '\\', '/');
    std::replace(right.begin(), right.end(), '\\', '/');
    return windows ? lower(left) == lower(right) : left == right;
}

AgentArtifact fixed_agent(const std::filesystem::path& directory, std::string filename,
                          std::uint64_t size, std::string digest, std::string url,
                          std::string attribute, std::string entryClass) {
    AgentArtifact result;
    result.file.destination = path_inside(directory, filename);
    result.file.size = size;
    result.file.maximumBytes = 4 * 1024 * 1024;
    result.file.hashes.sha256 = std::move(digest);
    result.file.url = std::move(url);
    result.file.kind = FileKind::Agent;
    result.manifestAttribute = std::move(attribute);
    result.entryClass = std::move(entryClass);
    return result;
}

void validate_auth_api(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos || (url.substr(0, scheme) != "http" && url.substr(0, scheme) != "https")) throw std::invalid_argument("Yggdrasil API root must use HTTP or HTTPS");
    const auto authority = url.substr(scheme + 3, url.substr(scheme + 3).find('/'));
    if (authority.empty() || authority.find('@') != std::string_view::npos || url.find('?') != std::string_view::npos || url.find('#') != std::string_view::npos ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; })) throw std::invalid_argument("Invalid Yggdrasil API root");
}

std::vector<std::string> settings_arguments(const Json& value, std::string_view key) {
    if (!value.contains(std::string(key)) || value[std::string(key)].is_null()) return {};
    const auto& arguments = value[std::string(key)];
    if (arguments.is_string()) return split_arguments(arguments.get<std::string>());
    if (!arguments.is_array()) throw std::invalid_argument("Effective launch arguments must be an array or string");
    std::vector<std::string> result;
    for (const auto& argument : arguments) {
        if (!argument.is_string()) throw std::invalid_argument("Effective launch argument must be a string");
        result.emplace_back(argument.get<std::string>());
    }
    return result;
}

}

void validate_launch_id(std::string_view id) {
    const auto allowed = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    };
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), allowed))
        throw ErrorException(Error{"minecraft.launchId", "launchId must be 1-128 ASCII letters, digits, hyphens or underscores"});
}

std::vector<std::string> split_arguments(std::string_view input) {
    std::vector<std::string> arguments;
    std::string token;
    char quote = 0;
    bool started = false;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto c = input[index];
        if (c == '\0') throw std::invalid_argument("Launch argument contains NUL");
        if (c == '\\' && index + 1 < input.size() && (input[index + 1] == quote || input[index + 1] == '"')) {
            token.push_back(input[++index]); started = true;
        } else if (quote && c == quote) quote = 0;
        else if (!quote && (c == '\'' || c == '"')) { quote = c; started = true; }
        else if (!quote && std::isspace(static_cast<unsigned char>(c))) {
            if (started) { arguments.emplace_back(std::move(token)); token.clear(); started = false; }
        } else { token.push_back(c); started = true; }
    }
    if (quote) throw std::invalid_argument("Launch argument quotation is not closed");
    if (started) arguments.emplace_back(std::move(token));
    return arguments;
}

std::string manifest_attribute(std::string_view manifest, std::string_view key) {
    if (manifest.size() > 64 * 1024) throw std::invalid_argument("JAR manifest size limit exceeded");
    std::optional<std::string> value;
    bool selected = false, hasHeader = false;
    while (!manifest.empty()) {
        const auto newline = manifest.find('\n');
        auto line = manifest.substr(0, newline);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) break;
        if (line.front() == ' ') {
            if (!hasHeader) throw std::invalid_argument("JAR manifest continuation has no preceding field");
            if (selected) value->append(line.substr(1));
        } else {
            const auto separator = line.find(": ");
            if (separator == std::string_view::npos) throw std::invalid_argument("Invalid JAR manifest header");
            hasHeader = true;
            selected = lower(line.substr(0, separator)) == lower(key);
            if (selected) {
                if (value) throw std::invalid_argument("Duplicate JAR manifest entry point");
                value = std::string(line.substr(separator + 2));
            }
        }
        if (newline == std::string_view::npos) break;
        manifest.remove_prefix(newline + 1);
    }
    if (!value || value->empty()) throw std::invalid_argument("JAR manifest has no " + std::string(key));
    validate_token(*value);
    return *value;
}

std::string redact_output(std::string_view output, std::span<const std::string> secrets) {
    if (output.size() > 128 * 1024) return "[discarded oversized process output]";
    std::string result(output);
    for (const auto& secret : secrets) {
        if (secret.empty()) continue;
        for (auto at = result.find(secret); at != std::string::npos; at = result.find(secret, at + 10)) result.replace(at, secret.size(), "[redacted]");
        const auto encoded = url_encode_segment(secret);
        if (encoded != secret) for (auto at = result.find(encoded); at != std::string::npos; at = result.find(encoded, at + 10)) result.replace(at, encoded.size(), "[redacted]");
    }
    static const std::regex bearer(R"((Bearer\s+)[A-Za-z0-9._~+/-]+=*)", std::regex::icase);
    static const std::regex assignments(R"re(((?:access[_-]?token|client[_-]?token|refresh[_-]?token|password|authorization)\s*[=:]\s*)[^\s,;"}]+)re", std::regex::icase);
    result = std::regex_replace(result, bearer, "$1[redacted]");
    result = std::regex_replace(result, assignments, "$1[redacted]");
    return result;
}

LaunchPlan build_launch_plan(const LaunchRequest& request) {
    validate_launch_id(request.launchId);
    if (!request.settings.launchable) throw std::invalid_argument("Pending or incomplete instances cannot be launched");
    const auto& version = request.version;
    if (version.root.empty() || version.gameDirectory.empty() || !version.root.is_absolute() || !version.gameDirectory.is_absolute()) throw std::invalid_argument("Minecraft trusted root and game directory must be absolute");
    reject_links(version.root); reject_links(version.gameDirectory);
    validate_version_id(version.versionId);
    if (!version.profile.is_object()) throw std::invalid_argument("Installed Minecraft profile is invalid");
    if (string_value(version.profile, "id", version.versionId) != version.versionId) throw std::invalid_argument("Installed Minecraft profile identity is inconsistent");
    if (request.identity.name.empty() || request.identity.uuid.empty()) throw std::invalid_argument("Launch identity has no selected player");
    validate_token(request.identity.name); validate_token(request.identity.uuid); validate_token(request.identity.accessToken);
    auto platform = request.platform;
    if (request.java && !request.java->architecture.empty()) platform.architecture = request.java->architecture;
    platform.features["is_demo_user"] = request.settings.demo;
    platform.features["has_custom_resolution"] = request.settings.width.has_value() && request.settings.height.has_value();
    platform.features["has_quick_plays_support"] = !request.settings.serverAddress.empty();
    platform.features["is_quick_play_multiplayer"] = !request.settings.serverAddress.empty();
    const auto nativesBase = path_inside(version.root, "versions/" + version.versionId + "/natives");
    auto libraries = plan_libraries(version.profile, version.root, platform, nativesBase);
    LaunchPlan result;
    result.launchId = request.launchId;
    result.versionId = version.versionId;
    result.mainClass = string_value(version.profile, "mainClass");
    if (result.mainClass.empty() || std::any_of(result.mainClass.begin(), result.mainClass.end(), [](unsigned char c) { return std::isspace(c) || c == '\0'; })) throw std::invalid_argument("Minecraft mainClass is invalid");
    result.nativesDirectory = native_library_directory(version.profile, nativesBase, platform);
    result.classpath = std::move(libraries.classpath);
    for (const auto& entry : version.classpath) {
        if (!entry.is_absolute() || !path_is_inside(version.root, entry)) throw std::invalid_argument("Minecraft classpath escapes the trusted root");
        if (std::none_of(result.classpath.begin(), result.classpath.end(), [&](const auto& existing) { return same_path(entry, existing, platform.os == "windows"); })) result.classpath.push_back(entry);
    }
    const auto baseId = version.minecraftVersion.empty() ? version.versionId : version.minecraftVersion;
    const auto clientId = string_value(version.profile, "jar", baseId);
    validate_version_id(clientId);
    const auto clientJar = path_inside(version.root, "versions/" + clientId + '/' + clientId + ".jar");
    auto metadataMajor = std::optional<unsigned>{};
    if (version.profile.contains("javaVersion") && version.profile["javaVersion"].contains("majorVersion")) metadataMajor = version.profile["javaVersion"]["majorVersion"].get<unsigned>();
    const auto requirement = loader_java_requirement(baseId, version.loaders, string_value(version.profile, "releaseTime"), metadataMajor);
    if (request.java) result.java = validate_manual_java(requirement, *request.java);
    else if (!request.javaCandidates.empty()) result.java = select_java(requirement, request.javaCandidates);
    else result.java = version.java;
    if (!result.java.path || !result.java.compatible || result.java.bits < 64 || !requirement.accepts(result.java.major)) throw std::invalid_argument("No compatible 64-bit Java runtime is selected");
    const auto maximumHeap = manual_memory_mb(request.settings.memoryMb, request.settings.physicalMemoryMb);
    if (request.settings.initialMemoryMb && (*request.settings.initialMemoryMb > maximumHeap || *request.settings.initialMemoryMb == 0)) throw std::invalid_argument("Initial heap must be positive and cannot exceed maximum heap");
    AssetPlan assets;
    assets.assetsRoot = version.root / "assets"; assets.gameAssets = assets.assetsRoot;
    if (request.assetIndex.is_object()) assets = plan_assets(version.profile, request.assetIndex, version.root, version.gameDirectory);
    else {
        const auto legacy = string_value(version.profile, "assets");
        if (legacy == "legacy" || legacy == "pre-1.6") assets.assetsRoot = assets.gameAssets = version.gameDirectory / "resources";
    }
    result.assetCopies = std::move(assets.copies);
    Placeholders placeholders{
        {"auth_player_name", request.identity.name}, {"auth_uuid", request.identity.uuid},
        {"auth_access_token", request.identity.accessToken}, {"auth_session", request.identity.accessToken},
        {"auth_xuid", request.identity.xuid}, {"clientid", request.identity.clientId},
        {"user_type", baseId.starts_with("1.16") && request.identity.type != "offline" ? "Xbox" : request.identity.userType},
        {"user_properties", request.identity.userProperties.empty() ? "{}" : request.identity.userProperties},
        {"version_name", version.versionId}, {"version_type", request.settings.customInfo.empty() ? string_value(version.profile, "type", "release") : request.settings.customInfo},
        {"game_directory", path_utf8(version.gameDirectory)}, {"assets_root", path_utf8(assets.assetsRoot)}, {"game_assets", path_utf8(assets.gameAssets)},
        {"assets_index_name", version.profile.contains("assetIndex") ? string_value(version.profile["assetIndex"], "id") : string_value(version.profile, "assets")},
        {"natives_directory", path_utf8(nativesBase)}, {"library_directory", path_utf8(version.root / "libraries")},
        {"classpath_separator", std::string(1, platform.classpath_separator())}, {"classpath", classpath_string(result.classpath, platform)},
        {"launcher_name", "Palladium"}, {"launcher_version", "native"}, {"primary_jar", path_utf8(clientJar)},
        {"resolution_width", std::to_string(request.settings.width.value_or(854))}, {"resolution_height", std::to_string(request.settings.height.value_or(480))},
        {"quickPlayPath", path_utf8(version.gameDirectory / "quickPlay" / (request.launchId + ".json"))},
        {"quickPlayMultiplayer", request.settings.serverAddress}, {"quickPlaySingleplayer", ""}, {"quickPlayRealms", ""}
    };
    std::vector<std::string> jvm, game;
    if (version.profile.contains("arguments")) {
        const auto& arguments = version.profile["arguments"];
        jvm = evaluate_arguments(arguments.value("jvm", Json::array()), platform);
        game = evaluate_arguments(arguments.value("game", Json::array()), platform);
    }
    if (version.profile.contains("minecraftArguments") && version.profile["minecraftArguments"].is_string()) {
        auto legacy = split_arguments(version.profile["minecraftArguments"].get<std::string>());
        legacy.insert(legacy.end(), game.begin(), game.end());
        game = std::move(legacy);
    }
    jvm.insert(jvm.end(), request.settings.jvmArguments.begin(), request.settings.jvmArguments.end());
    game.insert(game.end(), request.settings.gameArguments.begin(), request.settings.gameArguments.end());
    for (auto& argument : jvm) argument = expand(argument, placeholders);
    for (auto& argument : game) argument = expand(argument, placeholders);
    for (std::size_t index = 0; index < jvm.size();) {
        const auto& argument = jvm[index];
        if (argument == "-cp" || argument == "-classpath" || argument == "--class-path" || argument == "--classpath") {
            if (index + 1 >= jvm.size()) throw std::invalid_argument("Java classpath option is missing its value");
            jvm.erase(jvm.begin() + static_cast<std::ptrdiff_t>(index), jvm.begin() + static_cast<std::ptrdiff_t>(index + 2));
        } else if (argument.starts_with("--class-path=") || argument.starts_with("--classpath=") || argument.starts_with("-Xmx") || argument.starts_with("-Xms")) jvm.erase(jvm.begin() + static_cast<std::ptrdiff_t>(index));
        else ++index;
    }
    const auto modules = module_paths(jvm, platform);
    const auto moduleLoader = !modules.empty() && std::any_of(version.loaders.begin(), version.loaders.end(), [](const auto& loader) { return loader.kind == LoaderKind::Forge || loader.kind == LoaderKind::NeoForge; });
    if (!moduleLoader) result.classpath.push_back(clientJar);
    std::erase_if(result.classpath, [&](const auto& path) { return std::any_of(modules.begin(), modules.end(), [&](const auto& module) { return same_path(path, module, platform.os == "windows"); }); });
    std::vector<std::filesystem::path> unique;
    for (const auto& entry : result.classpath) if (std::none_of(unique.begin(), unique.end(), [&](const auto& path) { return same_path(path, entry, platform.os == "windows"); })) unique.push_back(entry);
    result.classpath = std::move(unique);
    auto set_property = [&](std::string_view name, std::string value) {
        if (std::none_of(jvm.begin(), jvm.end(), [&](const auto& argument) { return argument.starts_with(std::string(name) + '='); })) jvm.emplace_back(std::string(name) + '=' + value);
    };
    set_property("-Djava.library.path", path_utf8(result.nativesDirectory));
    set_property("-Djna.tmpdir", path_utf8(nativesBase / "jna"));
    set_property("-Dorg.lwjgl.system.SharedLibraryExtractPath", path_utf8(nativesBase / "lwjgl"));
    set_property("-Dio.netty.native.workdir", path_utf8(nativesBase / "netty"));
    if (platform.os == "osx" && std::find(jvm.begin(), jvm.end(), "-XstartOnFirstThread") == jvm.end()) jvm.emplace_back("-XstartOnFirstThread");
    if (platform.os == "linux") { set_property("-DGLFW_PLATFORM", "x11"); result.process.environment["GLFW_PLATFORM"] = "x11"; result.process.environment["WAYLAND_SOCKET"] = "invalid"; }
    if (version.profile.contains("logging") && version.profile["logging"].contains("client")) {
        const auto& logging = version.profile["logging"]["client"];
        if (logging.contains("file")) {
            const auto loggingId = string_value(logging["file"], "id");
            validate_version_id(loggingId);
            auto loggingPlaceholders = placeholders;
            loggingPlaceholders["path"] = path_utf8(path_inside(version.root, "assets/log_configs/" + loggingId));
            const auto argument = string_value(logging, "argument");
            if (!argument.empty()) jvm.push_back(expand(argument, loggingPlaceholders));
        }
    }
    apply_gc_plan(jvm, gc_arguments(request.settings.memoryManagement, result.java.major, request.settings.windowsBuild, platform.os == "windows"));
    if (request.settings.initialMemoryMb) jvm.emplace_back("-Xms" + std::to_string(*request.settings.initialMemoryMb) + 'M');
    jvm.emplace_back("-Xmx" + std::to_string(maximumHeap) + 'M');
    if (request.settings.bypassOffline) {
        for (auto host : {"auth", "account", "session", "services"}) jvm.emplace_back(std::string("-Dminecraft.api.") + host + ".host=https://invalidAuthServer.invalid");
    }
    set_game_argument(game, "--gameDir", path_utf8(version.gameDirectory));
    if (!request.settings.customInfo.empty()) set_game_argument(game, "--versionType", request.settings.customInfo);
    if (request.settings.width) set_game_argument(game, "--width", std::to_string(*request.settings.width));
    if (request.settings.height) set_game_argument(game, "--height", std::to_string(*request.settings.height));
    set_game_switch(game, "--fullscreen", request.settings.fullscreen);
    set_game_switch(game, "--demo", request.settings.demo);
    if (!request.settings.serverAddress.empty()) {
        const auto& address = request.settings.serverAddress;
        if (address.find('/') != std::string::npos || std::any_of(address.begin(), address.end(), [](unsigned char c) { return std::isspace(c) || c == '\0'; })) throw std::invalid_argument("Server address must contain only host and optional port");
        const auto modern = !baseId.starts_with("1.") || compare_versions(baseId, "1.20") >= 0;
        if (modern) set_game_argument(game, "--quickPlayMultiplayer", address);
        else {
            auto host = address;
            std::string port = "25565";
            const auto colon = address.rfind(':');
            if (colon != std::string::npos && address.find(':') == colon) {
                unsigned parsed = 0;
                const auto raw = std::string_view(address).substr(colon + 1);
                auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
                if (error != std::errc{} || end != raw.data() + raw.size() || parsed == 0 || parsed > 65535) throw std::invalid_argument("Server port is invalid");
                host = address.substr(0, colon); port = std::to_string(parsed);
            }
            set_game_argument(game, "--server", host); set_game_argument(game, "--port", port);
        }
    }
    deduplicate_tweakers(game);
    const auto agentDirectory = version.root / "agents";
    if (!request.identity.yggdrasilApi.empty()) {
        validate_auth_api(request.identity.yggdrasilApi);
        auto agent = fixed_agent(agentDirectory, "authlib-injector-1.2.8.jar", 349681,
                                "9c7f4343e6c82034958ffb48c14a2cb0c85928be7283103ce17da00c6d5a7b10",
                                "https://authlib-injector.yushi.moe/artifact/56/authlib-injector-1.2.8.jar",
                                "Premain-Class", "moe.yushi.authlibinjector.Premain");
        agent.file.fallbackUrls = {"https://bmclapi2.bangbang93.com/mirrors/authlib-injector/artifact/56/authlib-injector-1.2.8.jar"};
        jvm.push_back("-javaagent:" + path_utf8(agent.file.destination) + '=' + request.identity.yggdrasilApi);
        jvm.emplace_back("-Dauthlibinjector.noLogFile");
        result.agents.push_back(std::move(agent));
    }
    const auto affectedLwjgl = std::any_of(libraries.downloads.begin(), libraries.downloads.end(), [](const auto& library) { return library.coordinate == "org.lwjgl:lwjgl:3.4.0" || library.coordinate == "org.lwjgl:lwjgl:3.4.1"; });
    if (!request.settings.disableLwjglAgent && result.java.major >= 25 && affectedLwjgl && std::none_of(jvm.begin(), jvm.end(), [](const auto& arg) { return arg.starts_with("-javaagent:") && arg.find("lwjgl-unsafe-agent-2.0.jar") != std::string::npos; })) {
        auto agent = fixed_agent(agentDirectory, "lwjgl-unsafe-agent-2.0.jar", 9821,
                                "6c07d3508dc090a12076876bdc227a19ce195a41c9e2c6535c3f82213b1b2015",
                                "https://github.com/HMCL-dev/lwjgl-unsafe-agent/releases/download/2.0/lwjgl-unsafe-agent-2.0.jar",
                                "Premain-Class", "org.glavo.lwjgl.UnsafeAgent");
        jvm.push_back("-javaagent:" + path_utf8(agent.file.destination));
        result.agents.push_back(std::move(agent));
    }
    const auto nonAscii = [](std::string_view value) { return std::any_of(value.begin(), value.end(), [](unsigned char c) { return c >= 128; }); };
    const auto nonAsciiPath = nonAscii(path_utf8(version.gameDirectory)) || nonAscii(path_utf8(*result.java.path)) || std::any_of(result.classpath.begin(), result.classpath.end(), [&](const auto& entry) { return nonAscii(path_utf8(entry)); });
    std::optional<std::filesystem::path> wrapper;
    if (!request.settings.disableWrapper && platform.os == "windows" && request.settings.windowsUtf8 && nonAsciiPath && result.java.major >= 6 && result.java.major <= 18 && request.identity.yggdrasilApi.empty() && std::none_of(jvm.begin(), jvm.end(), [](const auto& arg) { return agent_or_module(arg); })) {
        auto agent = fixed_agent(agentDirectory, "java_launch_wrapper-1.4.4.jar", 16345,
                                "88eb3ee58854e916437831697ef6a2fd493da9bc4ffb018f620cd88f090952aa",
                                "https://github.com/00ll00/java_launch_wrapper/releases/download/v1.4.4/java_launch_wrapper-1.4.4.jar",
                                "Main-Class", "oolloo.jlw.Wrapper");
        wrapper = agent.file.destination;
        result.agents.push_back(std::move(agent));
    }
    jvm.emplace_back("-cp"); jvm.emplace_back(classpath_string(result.classpath, platform));
    if (wrapper) { jvm.emplace_back("-jar"); jvm.emplace_back(path_utf8(*wrapper)); }
    jvm.push_back(result.mainClass);
    jvm.insert(jvm.end(), game.begin(), game.end());
    for (auto& argument : jvm) argument = java_path_argument(argument, platform.os == "windows");
    result.process.executable = *result.java.path;
    result.process.highPerformanceGpu = request.settings.highPerformanceGpu;
    result.process.workingDirectory = version.gameDirectory;
    result.process.arguments = std::move(jvm);
    result.process.hideWindow = false;
    if (!request.identity.accessToken.empty()) result.secrets.push_back(request.identity.accessToken);
    if (!request.identity.clientId.empty()) result.secrets.push_back(request.identity.clientId);
    return result;
}

std::vector<std::string> LaunchPlan::redacted_arguments() const {
    std::vector<std::string> result;
    bool privateValue = false;
    for (const auto& argument : process.arguments) {
        result.push_back(privateValue ? "[redacted]" : redact_output(argument, secrets));
        privateValue = argument == "--accessToken" || argument == "--session" || argument == "--clientId";
    }
    return result;
}

Json launch_plan_json(const LaunchPlan& plan) {
    return Json{{"launchId", plan.launchId}, {"versionId", plan.versionId}, {"mainClass", plan.mainClass},
                {"java", path_utf8(plan.process.executable)}, {"workingDirectory", path_utf8(plan.process.workingDirectory)},
                {"arguments", plan.redacted_arguments()}, {"nativesDirectory", path_utf8(plan.nativesDirectory)}};
}

LaunchSettings launch_settings_from_json(const Json& value) {
    if (!value.is_object()) throw std::invalid_argument("Effective launch settings must be an object");
    LaunchSettings settings;
    settings.memoryMb = value.value("memoryMb", std::uint64_t{4096});
    if (value.contains("initialMemoryMb") && !value["initialMemoryMb"].is_null()) settings.initialMemoryMb = value["initialMemoryMb"].get<std::uint64_t>();
    settings.memoryManagement = string_value(value, "memoryManagement", "g1-tuned");
    settings.jvmArguments = settings_arguments(value, "jvmArgs");
    settings.gameArguments = settings_arguments(value, "gameArgs");
    settings.serverAddress = string_value(value, "serverAddress");
    settings.customInfo = string_value(value, "customInfo");
    settings.preLaunchCommand = string_value(value, "preLaunchCommand");
    const auto commandTimeout = value.value("preLaunchTimeoutMs", std::uint64_t{60000});
    if (!commandTimeout || commandTimeout > 3600000) throw std::invalid_argument("Pre-launch timeout must be 1-3600000 ms");
    settings.preLaunchTimeout = std::chrono::milliseconds(commandTimeout);
    settings.disableLwjglAgent = value.value("disableLwjglAgent", false);
    settings.disableWrapper = value.value("disableWrapper", false);
    settings.windowsUtf8 = value.value("windowsUtf8", false);
    settings.bypassOffline = value.value("bypassOffline", false);
    settings.demo = value.value("demo", false);
    settings.verifyFiles = !value.value("disableFileCheck", false);
    settings.launchable = value.value("launchable", true) && !value.value("pending", false);
    settings.highPerformanceGpu = value.value("highPerformanceGpu", false);
    const auto window = string_value(value, "windowMode");
    settings.fullscreen = window == "fullscreen";
    if (window == "custom" || window == "same-as-launcher") {
        const auto width = value.value("width", 854U), height = value.value("height", 480U);
        if (!width || !height || width > 16384 || height > 16384) throw std::invalid_argument("Minecraft window dimensions are invalid");
        settings.width = width; settings.height = height;
    }
    return settings;
}

}
