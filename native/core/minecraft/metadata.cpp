#include "metadata.hpp"
#include "maven.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace nickel::minecraft {
namespace {

std::string text(const Json& value, std::string_view key, std::string fallback = {}) {
    if (!value.is_object()) return fallback;
    const auto found = value.find(std::string(key));
    if (found == value.end() || found->is_null()) return fallback;
    if (!found->is_string()) throw std::invalid_argument("Minecraft metadata string field has an invalid type: " + std::string(key));
    return found->get<std::string>();
}

std::optional<std::uint64_t> size_value(const Json& value, std::string_view key) {
    if (!value.is_object()) return {};
    const auto found = value.find(std::string(key));
    if (found == value.end() || found->is_null()) return {};
    if (!found->is_number_unsigned() && !(found->is_number_integer() && found->get<std::int64_t>() >= 0)) throw std::invalid_argument("Minecraft file size must be nonnegative");
    return found->get<std::uint64_t>();
}

std::optional<std::string> hash_value(const Json& value, std::string_view key, std::size_t length) {
    auto hash = text(value, key);
    if (hash.empty()) return {};
    if (hash.size() != length || !std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) throw std::invalid_argument("Invalid Minecraft " + std::string(key) + " digest");
    std::transform(hash.begin(), hash.end(), hash.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return hash;
}

void validate_url(std::string_view url) {
    if (url.empty()) return;
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos || (url.substr(0, scheme) != "https" && url.substr(0, scheme) != "http")) throw std::invalid_argument("Minecraft artifact URL must use HTTP or HTTPS");
    const auto rest = url.substr(scheme + 3);
    const auto authority = rest.substr(0, rest.find_first_of("/?#"));
    if (authority.empty() || authority.find('@') != std::string_view::npos || url.find('#') != std::string_view::npos ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; })) throw std::invalid_argument("Invalid Minecraft artifact URL");
}

Json merge_objects(Json parent, const Json& child) {
    if (!parent.is_object() || !child.is_object()) return child;
    for (const auto& [key, value] : child.items()) {
        if (value.is_null()) continue;
        if (parent.contains(key) && parent[key].is_object() && value.is_object()) parent[key] = merge_objects(parent[key], value);
        else parent[key] = value;
    }
    return parent;
}

std::string library_key(const Json& library) {
    return MavenCoordinate::parse(text(library, "name")).key();
}

std::vector<std::string> argument_values(const Json& entry, const Platform& platform) {
    if (entry.is_string()) return {entry.get<std::string>()};
    if (!entry.is_object()) throw std::invalid_argument("Invalid Minecraft argument entry");
    if (entry.contains("rules") && !rules_allow(parse_rules(entry["rules"]), platform)) return {};
    if (!entry.contains("value")) throw std::invalid_argument("Conditional Minecraft argument has no value");
    const auto& value = entry["value"];
    if (value.is_string()) return {value.get<std::string>()};
    if (!value.is_array()) throw std::invalid_argument("Conditional Minecraft argument value must be a string or array");
    std::vector<std::string> result;
    for (const auto& token : value) {
        if (!token.is_string()) throw std::invalid_argument("Minecraft argument array contains a non-string");
        result.emplace_back(token.get<std::string>());
    }
    return result;
}

void append_library_download(LibraryPlan& plan, DownloadFile file,
                             const Json& library, bool extract,
                             const std::filesystem::path& nativeDestination) {
    if (file.classpath) plan.classpath.emplace_back(file.destination);
    if (extract) {
        NativeArchive native{file.destination, nativeDestination};
        if (library.contains("extract") && library["extract"].contains("exclude")) {
            const auto& excludes = library["extract"]["exclude"];
            if (!excludes.is_array()) throw std::invalid_argument("Native exclusion list must be an array");
            for (const auto& exclusion : excludes) {
                if (!exclusion.is_string()) throw std::invalid_argument("Native exclusion prefix must be a string");
                native.excludePrefixes.emplace_back(exclusion.get<std::string>());
            }
        }
        plan.natives.emplace_back(std::move(native));
    }
    plan.downloads.emplace_back(std::move(file));
}

std::filesystem::path library_root(const Json& library, const std::filesystem::path& root) {
    const auto injected = text(library, "nickelLibraryRoot");
    if (injected.empty()) return root / "libraries";
    auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(injected.data()), injected.size()));
    if (!path.is_absolute() || !path_is_inside(root, path)) throw std::invalid_argument("Loader library root is outside the trusted Minecraft root");
    reject_links(path);
    return path;
}

}

bool FileHashes::empty() const noexcept {
    return !sha1 && !sha256 && !md5;
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

Json parse_metadata(std::string_view input, std::size_t maximumBytes) {
    if (input.size() > maximumBytes) throw std::invalid_argument("Minecraft metadata size limit exceeded");
    std::map<int, std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 128) throw std::invalid_argument("Minecraft metadata nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) keys[depth + 1].clear();
        if (event == Json::parse_event_t::key && !keys[depth].insert(value.get<std::string>()).second) throw std::invalid_argument("Minecraft metadata has duplicate object keys");
        if (event == Json::parse_event_t::object_end) keys.erase(depth + 1);
        return true;
    };
    try {
        return Json::parse(input.begin(), input.end(), callback);
    } catch (const Json::exception& error) {
        throw std::invalid_argument("Invalid Minecraft JSON metadata: " + std::string(error.what()));
    }
}

std::vector<Rule> parse_rules(const Json& entries) {
    if (entries.is_null()) return {};
    if (!entries.is_array()) throw std::invalid_argument("Minecraft platform rules must be an array");
    std::vector<Rule> result;
    for (const auto& entry : entries) {
        if (!entry.is_object()) throw std::invalid_argument("Minecraft platform rule must be an object");
        const auto action = text(entry, "action");
        if (action != "allow" && action != "disallow") throw std::invalid_argument("Minecraft platform rule action is invalid");
        Rule rule;
        rule.allow = action == "allow";
        if (entry.contains("os") && !entry["os"].is_null()) {
            const auto& os = entry["os"];
            if (!os.is_object()) throw std::invalid_argument("Minecraft OS rule must be an object");
            if (os.contains("name") && !os["name"].is_null()) rule.osName = text(os, "name");
            if (os.contains("version") && !os["version"].is_null()) rule.osVersion = text(os, "version");
            if (os.contains("arch") && !os["arch"].is_null()) rule.architecture = text(os, "arch");
        }
        if (entry.contains("features") && !entry["features"].is_null()) {
            if (!entry["features"].is_object()) throw std::invalid_argument("Minecraft feature rule must be an object");
            for (const auto& [name, enabled] : entry["features"].items()) {
                if (!enabled.is_boolean()) throw std::invalid_argument("Minecraft feature rule value must be boolean");
                rule.features[name] = enabled.get<bool>();
            }
        }
        result.emplace_back(std::move(rule));
    }
    return result;
}

Json merge_version_profiles(const Json& parent, const Json& child) {
    if (!parent.is_object() || !child.is_object()) throw std::invalid_argument("Minecraft version profile must be an object");
    auto result = merge_objects(parent, child);
    result.erase("inheritsFrom");
    if (parent.contains("arguments") || child.contains("arguments")) {
        Json arguments = Json::object();
        for (auto type : {"jvm", "game"}) {
            auto merged = Json::array();
            for (const auto* layer : {&parent, &child}) {
                if (layer->contains("arguments") && (*layer)["arguments"].contains(type)) {
                    const auto& entries = (*layer)["arguments"][type];
                    if (!entries.is_array()) throw std::invalid_argument("Minecraft arguments must be an array");
                    for (const auto& entry : entries) merged.push_back(entry);
                }
            }
            if (!merged.empty()) arguments[type] = std::move(merged);
        }
        result["arguments"] = std::move(arguments);
    }
    Json libraries = Json::array();
    std::set<std::string> seen;
    for (const auto* layer : {&child, &parent}) {
        if (!layer->contains("libraries")) continue;
        if (!(*layer)["libraries"].is_array()) throw std::invalid_argument("Minecraft libraries must be an array");
        for (const auto& library : (*layer)["libraries"]) {
            const auto key = library_key(library);
            if (seen.insert(key).second) libraries.push_back(library);
        }
    }
    result["libraries"] = std::move(libraries);
    return result;
}

ResolvedVersion resolve_version(std::string_view versionId, const std::filesystem::path& root,
                                const ProfileResolver& resolver, std::size_t maximumDepth) {
    validate_version_id(versionId);
    if (!resolver || !maximumDepth) throw std::invalid_argument("Minecraft profile resolver is unavailable");
    ResolvedVersion result;
    result.versionId = versionId;
    std::set<std::string> seen;
    std::string current(versionId);
    while (true) {
        validate_version_id(current);
        if (result.inheritance.size() >= maximumDepth) throw std::invalid_argument("Minecraft inheritance depth limit exceeded");
        if (!seen.insert(current).second) throw std::invalid_argument("Minecraft inheritance cycle detected");
        auto profile = resolver(current);
        if (!profile.is_object()) throw std::invalid_argument("Minecraft inherited profile is invalid");
        const auto id = text(profile, "id", current);
        if (id != current) throw std::invalid_argument("Minecraft profile identity does not match its requested version");
        profile["id"] = current;
        result.inheritance.push_back(profile);
        if (result.clientVersionId.empty()) {
            const auto explicitJar = text(profile, "jar");
            if (!explicitJar.empty()) result.clientVersionId = explicitJar;
            else if (profile.contains("downloads") && profile["downloads"].contains("client")) result.clientVersionId = current;
        }
        const auto parent = text(profile, "inheritsFrom");
        if (parent.empty()) { result.baseVersionId = current; break; }
        current = parent;
    }
    result.metadata = result.inheritance.back();
    for (auto iterator = result.inheritance.rbegin() + 1; iterator != result.inheritance.rend(); ++iterator) result.metadata = merge_version_profiles(result.metadata, *iterator);
    result.metadata["id"] = result.versionId;
    result.metadata.erase("inheritsFrom");
    if (text(result.metadata, "mainClass").empty()) throw std::invalid_argument("Minecraft profile has no main class");
    if (result.clientVersionId.empty()) result.clientVersionId = result.baseVersionId;
    validate_version_id(result.clientVersionId);
    result.clientJar = path_inside(root, "versions/" + result.clientVersionId + '/' + result.clientVersionId + ".jar");
    result.metadata["jar"] = result.clientVersionId;
    return result;
}

std::string resolve_version_alias(std::string_view versionId, const Json& manifest) {
    if (versionId == "latest_release" || versionId == "r" || versionId == "lr") {
        if (!manifest.contains("latest")) throw std::invalid_argument("Minecraft manifest has no latest release");
        return text(manifest["latest"], "release");
    }
    if (versionId == "latest_snapshot" || versionId == "s" || versionId == "ls") {
        if (!manifest.contains("latest")) throw std::invalid_argument("Minecraft manifest has no latest snapshot");
        return text(manifest["latest"], "snapshot");
    }
    return std::string(versionId);
}

Json manifest_version_entry(std::string_view versionId, const Json& manifest) {
    const auto resolved = resolve_version_alias(versionId, manifest);
    validate_version_id(resolved);
    if (!manifest.contains("versions") || !manifest["versions"].is_array()) throw std::invalid_argument("Minecraft manifest has no versions array");
    for (const auto& entry : manifest["versions"]) if (text(entry, "id") == resolved) {
        validate_url(text(entry, "url"));
        return entry;
    }
    throw std::invalid_argument("Minecraft version is absent from the manifest: " + resolved);
}

DownloadFile artifact_download(const Json& artifact, const std::filesystem::path& destination,
                                FileKind kind, std::string_view coordinate) {
    if (!artifact.is_object()) throw std::invalid_argument("Minecraft download artifact must be an object");
    DownloadFile result;
    result.destination = destination;
    result.url = text(artifact, "url");
    validate_url(result.url);
    result.hashes.sha1 = hash_value(artifact, "sha1", 40);
    result.hashes.sha256 = hash_value(artifact, "sha256", 64);
    result.hashes.md5 = hash_value(artifact, "md5", 32);
    result.size = size_value(artifact, "size");
    result.kind = kind;
    result.coordinate = coordinate;
    return result;
}

LibraryPlan plan_libraries(const Json& metadata, const std::filesystem::path& root,
                           const Platform& platform, const std::filesystem::path& nativesDirectory) {
    LibraryPlan result;
    if (!metadata.contains("libraries")) return result;
    if (!metadata["libraries"].is_array()) throw std::invalid_argument("Minecraft libraries must be an array");
    struct Candidate { Json library; MavenCoordinate coordinate; };
    std::vector<Candidate> libraries;
    std::unordered_map<std::string, std::size_t> indexes;
    for (const auto& library : metadata["libraries"]) {
        if (library.contains("clientreq") && library["clientreq"].is_boolean() && !library["clientreq"].get<bool>()) continue;
        if (library.contains("rules") && !rules_allow(parse_rules(library["rules"]), platform)) continue;
        auto coordinate = MavenCoordinate::parse(text(library, "name"));
        if (!native_classifier_matches(coordinate.classifier, platform)) continue;
        const auto found = indexes.find(coordinate.key());
        if (found == indexes.end()) {
            indexes.emplace(coordinate.key(), libraries.size());
            libraries.push_back({library, coordinate});
        } else if (compare_versions(coordinate.version, libraries[found->second].coordinate.version) > 0) {
            libraries[found->second] = {library, coordinate};
        }
    }
    const auto nativeDestination = native_library_directory(metadata, nativesDirectory, platform);
    const auto hasSlf4j2 = std::any_of(libraries.begin(), libraries.end(), [](const auto& candidate) { return candidate.coordinate.artifact == "log4j-slf4j2-impl"; });
    for (const auto& candidate : libraries) {
        const auto& library = candidate.library;
        const auto& coordinate = candidate.coordinate;
        if (hasSlf4j2 && coordinate.artifact == "log4j-slf4j-impl") continue;
        const auto libsRoot = library_root(library, root);
        auto repository = text(library, "url", "https://libraries.minecraft.net/");
        while (!repository.empty() && repository.back() == '/') repository.pop_back();
        const auto modernNative = coordinate.classifier.starts_with("natives-");
        const Json downloads = library.value("downloads", Json::object());
        if (downloads.contains("artifact") && !downloads["artifact"].is_null()) {
            const auto& artifact = downloads["artifact"];
            const auto relative = text(artifact, "path", coordinate.relative_path().generic_string());
            auto file = artifact_download(artifact, path_inside(libsRoot, relative), modernNative ? FileKind::Native : FileKind::Library, coordinate.str());
            if (file.url.empty() && !repository.empty() && !downloads["artifact"].contains("url")) file.url = repository + '/' + relative;
            file.classpath = coordinate.extension == "jar";
            append_library_download(result, std::move(file), library, modernNative, nativeDestination);
        } else if (!library.contains("natives") || !downloads.contains("classifiers")) {
            const auto relative = coordinate.relative_path().generic_string();
            Json artifact{{"url", repository + '/' + relative}};
            auto file = artifact_download(artifact, path_inside(libsRoot, relative), modernNative ? FileKind::Native : FileKind::Library, coordinate.str());
            file.classpath = coordinate.extension == "jar";
            append_library_download(result, std::move(file), library, modernNative, nativeDestination);
        }
        if (library.contains("natives") && library["natives"].is_object()) {
            const auto native = library["natives"].find(platform.os);
            if (native == library["natives"].end()) continue;
            if (!native->is_string()) throw std::invalid_argument("Native classifier template must be a string");
            auto classifier = native->get<std::string>();
            const auto arch = platform.normalized_architecture();
            const auto at = classifier.find("${arch}");
            if (at != std::string::npos) classifier.replace(at, 7, arch == "x86" ? "32" : "64");
            if (!native_classifier_matches(classifier, platform)) continue;
            if (!downloads.contains("classifiers") || !downloads["classifiers"].contains(classifier)) throw std::invalid_argument("Minecraft native classifier has no artifact: " + classifier);
            const auto& artifact = downloads["classifiers"][classifier];
            auto nativeCoordinate = coordinate;
            nativeCoordinate.classifier = classifier;
            const auto relative = text(artifact, "path", nativeCoordinate.relative_path().generic_string());
            auto file = artifact_download(artifact, path_inside(libsRoot, relative), FileKind::Native, nativeCoordinate.str());
            file.classpath = false;
            append_library_download(result, std::move(file), library, true, nativeDestination);
        }
    }
    std::set<std::filesystem::path> destinations;
    std::erase_if(result.downloads, [&](const auto& file) { return !destinations.insert(file.destination).second; });
    return result;
}

std::filesystem::path native_library_directory(const Json& metadata, const std::filesystem::path& base,
                                               const Platform& platform) {
    constexpr std::string_view prefix = "-Djava.library.path=${natives_directory}";
    if (metadata.contains("arguments") && metadata["arguments"].contains("jvm")) {
        for (const auto& entry : metadata["arguments"]["jvm"]) {
            for (const auto& argument : argument_values(entry, platform)) {
                if (!argument.starts_with(prefix)) continue;
                auto suffix = std::string_view(argument).substr(prefix.size());
                if (suffix.empty()) return base;
                if (suffix.front() != '/' && suffix.front() != '\\') throw std::invalid_argument("Native library directory has an invalid suffix");
                suffix.remove_prefix(1);
                return path_inside(base, suffix);
            }
        }
    }
    return base;
}

AssetPlan plan_assets(const Json& metadata, const Json& index, const std::filesystem::path& root,
                      const std::filesystem::path& gameDirectory, std::string_view resourcesBase) {
    AssetPlan result;
    result.assetsRoot = root / "assets";
    result.gameAssets = result.assetsRoot;
    if (!metadata.contains("assetIndex")) return result;
    const auto id = text(metadata["assetIndex"], "id");
    validate_version_id(id);
    if (!index.is_object() || !index.contains("objects") || !index["objects"].is_object()) throw std::invalid_argument("Minecraft asset index has no objects map");
    result.virtualAssets = index.value("virtual", false);
    result.mapToResources = index.value("map_to_resources", false);
    const auto legacyId = text(metadata, "assets", id);
    const auto legacy = legacyId == "legacy" || legacyId == "pre-1.6";
    if (legacy) { result.assetsRoot = gameDirectory / "resources"; result.gameAssets = result.assetsRoot; }
    else if (result.virtualAssets) result.gameAssets = root / "assets" / "virtual" / id;
    if (index["objects"].size() > 500000) throw std::invalid_argument("Minecraft asset count limit exceeded");
    std::set<std::string> seen;
    std::string base(resourcesBase);
    while (!base.empty() && base.back() == '/') base.pop_back();
    validate_url(base);
    for (const auto& [name, object] : index["objects"].items()) {
        const auto relative = validate_relative_path(name);
        const auto hash = hash_value(object, "hash", 40);
        if (!hash) throw std::invalid_argument("Minecraft asset index contains a missing SHA-1");
        const auto objectPath = path_inside(root, "assets/objects/" + hash->substr(0, 2) + '/' + *hash);
        if (seen.insert(*hash).second) {
            Json artifact{{"url", base + '/' + hash->substr(0, 2) + '/' + *hash}, {"sha1", *hash}};
            const auto size = size_value(object, "size");
            if (!size) throw std::invalid_argument("Minecraft asset has no size");
            artifact["size"] = *size;
            auto file = artifact_download(artifact, objectPath, FileKind::Asset);
            file.maximumBytes = 256ULL * 1024 * 1024;
            result.downloads.emplace_back(std::move(file));
        }
        FileHashes hashes; hashes.sha1 = hash;
        if (legacy || result.mapToResources) result.copies.push_back({objectPath, path_inside(gameDirectory / "resources", relative.generic_string()), hashes, false});
        if (result.virtualAssets) result.copies.push_back({objectPath, path_inside(root / "assets" / "virtual" / id, relative.generic_string()), hashes, false});
    }
    return result;
}

RuntimePlan plan_mojang_runtime(const Json& manifest, const std::filesystem::path& runtimeRoot,
                                const Platform& platform) {
    if (!manifest.contains("files") || !manifest["files"].is_object()) throw std::invalid_argument("Mojang runtime manifest has no files map");
    if (manifest["files"].size() > 100000) throw std::invalid_argument("Mojang runtime manifest file count limit exceeded");
    RuntimePlan result;
    result.root = runtimeRoot;
    for (const auto& [name, entry] : manifest["files"].items()) {
        const auto destination = path_inside(runtimeRoot, name);
        const auto type = text(entry, "type");
        if (type == "directory") result.directories.emplace_back(destination);
        else if (type == "file") {
            if (!entry.contains("downloads") || !entry["downloads"].contains("raw")) throw std::invalid_argument("Mojang runtime file has no raw download");
            auto file = artifact_download(entry["downloads"]["raw"], destination, FileKind::Java);
            file.executable = entry.value("executable", false);
            const auto filename = destination.filename().string();
            if (destination.parent_path().filename() == "bin" && (filename == "java" || filename == "javaw.exe" || filename == "java.exe")) {
                if (result.executable.empty() || filename == (platform.os == "windows" ? "javaw.exe" : "java")) result.executable = destination;
            }
            result.downloads.push_back(std::move(file));
        } else if (type == "link") {
            const auto targetText = text(entry, "target");
            if (targetText.empty() || targetText.front() == '/' || targetText.front() == '\\' || targetText.find(':') != std::string::npos) throw std::invalid_argument("Mojang runtime link is not relative");
            const auto target = (destination.parent_path() / std::filesystem::path(targetText)).lexically_normal();
            if (!path_is_inside(runtimeRoot, target)) throw std::invalid_argument("Mojang runtime link escapes the runtime root");
            result.links.push_back({destination, target});
        } else throw std::invalid_argument("Unknown Mojang runtime file type");
    }
    if (result.executable.empty()) throw std::invalid_argument("Mojang runtime manifest has no Java executable");
    return result;
}

Json download_plan_json(std::span<const DownloadFile> files) {
    auto result = Json::array();
    for (const auto& file : files) {
        Json value{{"destination", path_utf8(file.destination)}, {"url", file.url}, {"coordinate", file.coordinate}, {"kind", static_cast<int>(file.kind)}};
        if (file.size) value["size"] = *file.size;
        if (file.hashes.sha1) value["sha1"] = *file.hashes.sha1;
        if (file.hashes.sha256) value["sha256"] = *file.hashes.sha256;
        if (file.hashes.md5) value["md5"] = *file.hashes.md5;
        result.push_back(std::move(value));
    }
    return result;
}

}
