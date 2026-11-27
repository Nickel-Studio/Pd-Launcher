#include "loader.hpp"
#include "maven.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>

namespace nickel::minecraft {
namespace {

std::string text(const Json& value, std::string_view key, std::string fallback = {}) {
    if (!value.is_object()) return fallback;
    const auto found = value.find(std::string(key));
    if (found == value.end() || found->is_null()) return fallback;
    if (!found->is_string()) throw std::invalid_argument("Loader profile string has an invalid type");
    return found->get<std::string>();
}

std::string replace_all(std::string value, std::string_view key, std::string_view replacement) {
    for (auto at = value.find(key); at != std::string::npos; at = value.find(key, at + replacement.size())) value.replace(at, key.size(), replacement);
    return value;
}

std::string archive_entry(std::string value) {
    if (value.starts_with('/')) value.erase(0, 1);
    return validate_relative_path(value).generic_string();
}

std::filesystem::path library_path(const std::filesystem::path& root, std::string_view coordinate) {
    return path_inside(root / "libraries", MavenCoordinate::parse(coordinate).relative_path().generic_string());
}

bool client_processor(const Json& processor) {
    if (!processor.is_object()) throw std::invalid_argument("Forge processor must be an object");
    if (!processor.contains("sides")) return true;
    if (!processor["sides"].is_array()) throw std::invalid_argument("Forge processor sides must be an array");
    return std::any_of(processor["sides"].begin(), processor["sides"].end(), [](const auto& side) { return side == "client"; });
}

std::vector<std::string> string_array(const Json& object, std::string_view name) {
    const auto field = object.find(std::string(name));
    if (field == object.end()) return {};
    if (!field->is_array()) throw std::invalid_argument("Loader argument field must be an array");
    std::vector<std::string> result;
    for (const auto& value : *field) {
        if (!value.is_string()) throw std::invalid_argument("Loader arguments must be strings");
        result.push_back(value.get<std::string>());
    }
    return result;
}

bool dependency_matches(const DependencyMod& mod, std::string_view mc, LoaderKind loader) {
    return std::find(mod.minecraftVersions.begin(), mod.minecraftVersions.end(), mc) != mod.minecraftVersions.end() &&
           std::find(mod.loaders.begin(), mod.loaders.end(), loader) != mod.loaders.end() && !mod.file.hashes.empty();
}

Json normalize_profile(Json profile, const LoaderBuild& build, const std::filesystem::path& root,
                       const std::set<std::filesystem::path>& localFiles = {}) {
    if (!profile.is_object()) throw std::invalid_argument("Loader profile must be an object");
    const auto id = text(profile, "id");
    validate_version_id(id);
    const auto parent = text(profile, "inheritsFrom", build.minecraftVersion);
    if (parent != build.minecraftVersion && parent != fabric_minecraft_id(build.minecraftVersion)) throw std::invalid_argument("Loader profile targets another Minecraft version");
    profile["inheritsFrom"] = build.minecraftVersion;
    profile["jar"] = build.minecraftVersion;
    if (!profile.contains("libraries")) profile["libraries"] = Json::array();
    if (!profile["libraries"].is_array()) throw std::invalid_argument("Loader libraries must be an array");
    for (auto& library : profile["libraries"]) {
        const auto coordinate = MavenCoordinate::parse(text(library, "name"));
        library["nickelLibraryRoot"] = path_utf8(root / "libraries");
        const auto destination = library_path(root, coordinate.str());
        if (localFiles.contains(destination)) {
            library["downloads"]["artifact"]["url"] = "";
            library["downloads"]["artifact"]["path"] = coordinate.relative_path().generic_string();
        }
    }
    const auto libraryRoot = path_utf8(root / "libraries");
    const auto loaderRoot = path_utf8(root);
    const auto profileDir = path_utf8(path_inside(root, "versions/" + id));
    if (profile.contains("arguments") && profile["arguments"].contains("jvm")) {
        auto& arguments = profile["arguments"]["jvm"];
        if (!arguments.is_array()) throw std::invalid_argument("Loader JVM arguments must be an array");
        const auto expand = [&](std::string token) {
            token = replace_all(std::move(token), "${library_directory}", libraryRoot);
            token = replace_all(std::move(token), "${libraries_directory}", libraryRoot);
            token = replace_all(std::move(token), "${loader_directory}", loaderRoot);
            return replace_all(std::move(token), "${version_directory}", profileDir);
        };
        for (auto& argument : arguments) {
            if (argument.is_string()) argument = expand(argument.get<std::string>());
            else if (argument.is_object() && argument.contains("value")) {
                if (argument["value"].is_string()) argument["value"] = expand(argument["value"].get<std::string>());
                else if (argument["value"].is_array()) {
                    for (auto& token : argument["value"]) {
                        if (!token.is_string()) throw std::invalid_argument("Loader JVM argument is not a string");
                        token = expand(token.get<std::string>());
                    }
                }
            }
        }
    }
    return profile;
}

DownloadFile loader_artifact(const LoaderBuild& build, const std::filesystem::path& root) {
    const auto filename = validate_relative_path(build.artifact.filename);
    if (filename.has_parent_path()) throw std::invalid_argument("Loader artifact filename is not a basename");
    Json value{{"url", build.artifact.url}};
    if (build.artifact.hashes.sha1) value["sha1"] = *build.artifact.hashes.sha1;
    if (build.artifact.hashes.sha256) value["sha256"] = *build.artifact.hashes.sha256;
    if (build.artifact.hashes.md5) value["md5"] = *build.artifact.hashes.md5;
    if (build.artifact.size) value["size"] = *build.artifact.size;
    auto result = artifact_download(value, path_inside(root, ".nickel/installers/" + build.artifact.filename), FileKind::Loader);
    result.maximumBytes = 256 * 1024 * 1024;
    if (build.artifact.rawMetadata.is_object() && build.artifact.rawMetadata.contains("fallbackUrls")) result.fallbackUrls = string_array(build.artifact.rawMetadata, "fallbackUrls");
    return result;
}

LoaderPlan forge_plan(const LoaderBuild& build, const InstallRequest& request, const ResolvedVersion& vanilla,
                      const Services& services, const CancelCheck& check, const ProgressCallback& progress) {
    if (!services.archiveEntries || !services.archiveRead) throw std::logic_error("Forge safe archive adapter is unavailable");
    LoaderPlan result;
    result.selection = {build.kind, build.version};
    const auto artifact = loader_artifact(build, request.root);
    ensure_file(services, artifact, check, progress, request.offline);
    result.downloads.push_back(artifact);
    const auto entries = services.archiveEntries(artifact.destination, check);
    if (entries.size() > 100000) throw std::invalid_argument("Loader installer entry count exceeds its quota");
    std::set<std::string> uniqueEntries;
    for (const auto& entry : entries) {
        if (entry.empty()) throw std::invalid_argument("Loader installer has an empty archive entry");
        auto name = entry;
        if (name.ends_with('/')) name.pop_back();
        const auto normalized = validate_relative_path(name).generic_string();
        std::string folded = normalized;
        if (request.platform.os == "windows") std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!uniqueEntries.insert(folded).second) throw std::invalid_argument("Loader installer has duplicate archive paths");
    }
    const auto install = parse_metadata(services.archiveRead(artifact.destination, "install_profile.json", 16 * 1024 * 1024, check));
    Json profile;
    if (install.contains("versionInfo") && install["versionInfo"].is_object()) profile = install["versionInfo"];
    else profile = parse_metadata(services.archiveRead(artifact.destination, archive_entry(text(install, "json", "/version.json")), 16 * 1024 * 1024, check));
    const auto loaderRoot = path_inside(request.root, "loader/" + std::string(loader_name(build.kind)));
    const auto libsRoot = loaderRoot / "libraries";
    result.directories.push_back(libsRoot);
    std::map<std::filesystem::path, FileHashes> knownHashes;
    const auto add_hashes = [&](const Json& libraries) {
        if (!libraries.is_array()) throw std::invalid_argument("Forge install libraries must be an array");
        for (const auto& library : libraries) {
            const auto coordinate = MavenCoordinate::parse(text(library, "name"));
            if (library.contains("downloads") && library["downloads"].contains("artifact")) {
                const auto& descriptor = library["downloads"]["artifact"];
                const auto relative = text(descriptor, "path", coordinate.relative_path().generic_string());
                const auto file = artifact_download(descriptor, path_inside(libsRoot, relative), FileKind::Processor, coordinate.str());
                knownHashes[file.destination] = file.hashes;
            }
        }
    };
    add_hashes(profile.value("libraries", Json::array()));
    add_hashes(install.value("libraries", Json::array()));
    std::set<std::filesystem::path> localFiles;
    const auto add_embedded = [&](std::string entry, const std::filesystem::path& destination) {
        if (!localFiles.insert(destination).second) return;
        if (std::find(entries.begin(), entries.end(), entry) == entries.end()) throw std::invalid_argument("Forge installer is missing a required embedded entry");
        result.archiveFiles.push_back({artifact.destination, std::move(entry), destination, knownHashes[destination]});
    };
    for (const auto& entry : entries) {
        if (entry.starts_with("maven/") && !entry.ends_with('/')) add_embedded(entry, path_inside(libsRoot, entry.substr(6)));
    }
    if (install.contains("install") && install["install"].is_object()) {
        const auto& old = install["install"];
        const auto coordinate = text(old, "path");
        const auto embedded = text(old, "filePath");
        if (!coordinate.empty() && !embedded.empty()) add_embedded(archive_entry(embedded), library_path(loaderRoot, coordinate));
    }
    const auto data = install.value("data", Json::object());
    if (!data.is_object()) throw std::invalid_argument("Forge data must be an object");
    for (const auto& [key, value] : data.items()) {
        static_cast<void>(key);
        const auto client = text(value, "client");
        if (client.starts_with('/')) {
            const auto entry = archive_entry(client);
            add_embedded(entry, path_inside(libsRoot, entry));
        }
    }
    for (const auto& library : install.value("libraries", Json::array())) {
        const auto coordinate = MavenCoordinate::parse(text(library, "name"));
        if (library.contains("rules") && !rules_allow(parse_rules(library["rules"]), request.platform)) continue;
        Json descriptor;
        if (library.contains("downloads") && library["downloads"].contains("artifact")) descriptor = library["downloads"]["artifact"];
        else {
            auto repository = text(library, "url", build.kind == LoaderKind::Forge ? "https://maven.minecraftforge.net" : "https://maven.neoforged.net/releases");
            while (!repository.empty() && repository.back() == '/') repository.pop_back();
            descriptor = Json{{"url", repository + '/' + coordinate.relative_path().generic_string()}};
        }
        const auto relative = text(descriptor, "path", coordinate.relative_path().generic_string());
        auto file = artifact_download(descriptor, path_inside(libsRoot, relative), FileKind::Processor, coordinate.str());
        if (!localFiles.contains(file.destination)) result.downloads.push_back(std::move(file));
    }
    const auto processors = install.value("processors", Json::array());
    if (!processors.is_array()) throw std::invalid_argument("Forge processors must be an array");
    if (processors.size() > 1024) throw std::invalid_argument("Forge processor count exceeds its quota");
    for (const auto& processor : processors) {
        if (!client_processor(processor)) continue;
        ProcessorPlan plan;
        plan.label = text(processor, "jar");
        plan.mainClassJar = library_path(loaderRoot, plan.label);
        plan.classpath.push_back(plan.mainClassJar);
        for (const auto& coordinate : string_array(processor, "classpath")) plan.classpath.push_back(library_path(loaderRoot, coordinate));
        plan.process.workingDirectory = loaderRoot;
        plan.process.terminateOnDrop = true;
        for (const auto& argument : string_array(processor, "args")) plan.process.arguments.push_back(resolve_processor_argument(argument, data, loaderRoot, artifact.destination, vanilla));
        if (processor.contains("outputs")) {
            if (!processor["outputs"].is_object()) throw std::invalid_argument("Forge processor outputs must be an object");
            for (const auto& [name, hash] : processor["outputs"].items()) {
                const auto output = std::filesystem::u8path(resolve_processor_argument(name, data, loaderRoot, artifact.destination, vanilla));
                if (!path_is_inside(libsRoot, output)) throw std::invalid_argument("Forge processor output escapes its library root");
                if (!hash.is_string()) throw std::invalid_argument("Forge processor output hash is not a string");
                auto digest = resolve_processor_argument(hash.get<std::string>(), data, loaderRoot, artifact.destination, vanilla);
                if (digest.starts_with('\'') && digest.ends_with('\'')) digest = digest.substr(1, digest.size() - 2);
                const auto hashes = artifact_download(Json{{"url", ""}, {"sha1", digest}}, output, FileKind::Processor).hashes;
                plan.outputs.push_back({output, hashes});
                localFiles.insert(output);
            }
        }
        const auto task = std::find(plan.process.arguments.begin(), plan.process.arguments.end(), "--task");
        const auto downloadsMappings = task != plan.process.arguments.end() && task + 1 != plan.process.arguments.end() && *(task + 1) == "DOWNLOAD_MOJMAPS";
        if (downloadsMappings) {
            if (!plan.label.starts_with("net.minecraftforge:installertools:") && !plan.label.starts_with("net.neoforged.installertools:installertools:")) throw std::invalid_argument("Unknown network-capable Forge processor is not permitted");
            const auto argument = [&](std::string_view key, std::string fallback = {}) {
                const auto it = std::find(plan.process.arguments.begin(), plan.process.arguments.end(), key);
                return it != plan.process.arguments.end() && it + 1 != plan.process.arguments.end() ? *(it + 1) : fallback;
            };
            if (argument("--version") != vanilla.baseVersionId || argument("--side", "client") != "client") throw std::invalid_argument("Forge mapping request targets another Minecraft version or side");
            const auto output = std::filesystem::u8path(argument("--output"));
            if (!path_is_inside(libsRoot, output)) throw std::invalid_argument("Forge mapping download escapes its library root");
            if (!vanilla.metadata.contains("downloads") || !vanilla.metadata["downloads"].contains("client_mappings")) throw std::invalid_argument("Minecraft metadata lacks client mappings");
            plan.managedDownload = artifact_download(vanilla.metadata["downloads"]["client_mappings"], output, FileKind::Processor);
            if (plan.managedDownload->hashes.empty() || !plan.managedDownload->size || *plan.managedDownload->size == 0) throw std::invalid_argument("Minecraft mappings lack integrity metadata");
            plan.process = {};
            localFiles.insert(output);
        }
        result.processors.push_back(std::move(plan));
    }
    result.profile = normalize_profile(std::move(profile), build, loaderRoot, localFiles);
    const auto id = text(result.profile, "id");
    result.writes.push_back({path_inside(loaderRoot, "versions/" + id + '/' + id + ".json"), result.profile.dump(2)});
    return result;
}

LoaderPlan optifine_plan(const LoaderBuild& build, const InstallRequest& request, const ResolvedVersion& vanilla,
                         std::span<const LoaderSelection> companions, const Services& services,
                         const CancelCheck& check, const ProgressCallback& progress) {
    LoaderPlan result;
    result.selection = {build.kind, build.version};
    const auto matching = [&](std::string_view name) {
        return std::any_of(request.dependencyMods.begin(), request.dependencyMods.end(), [&](const auto& mod) {
            std::string normalized = mod.name;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return normalized == name && dependency_matches(mod, vanilla.baseVersionId, LoaderKind::Fabric);
        });
    };
    const auto compatibility = optifine_compatibility(vanilla.baseVersionId, build.artifact.forgeRequirement,
                                                      companions, matching("optifabric"), matching("fabric api"));
    if (compatibility.route == OptiFineRoute::Manual || compatibility.route == OptiFineRoute::Incompatible) throw std::invalid_argument(compatibility.reason);
    const auto artifact = loader_artifact(build, request.root);
    ensure_file(services, artifact, check, progress, request.offline);
    if (!services.archiveEntries || !services.archiveRead) throw std::logic_error("OptiFine archive adapter is unavailable");
    const auto entries = services.archiveEntries(artifact.destination, check);
    if (std::find(entries.begin(), entries.end(), "optifine/OptiFineTweaker.class") == entries.end() &&
        std::find(entries.begin(), entries.end(), "optifine/Installer.class") == entries.end()) throw std::invalid_argument("OptiFine downloaded artifact is not a recognized installer JAR");
    result.downloads.push_back(artifact);
    const auto root = path_inside(request.root, "loader/optifine");
    if (compatibility.route == OptiFineRoute::Mods) {
        const auto destination = path_inside(request.gameDirectory, compatibility.modsSubdirectory + '/' + build.artifact.filename);
        result.copies.push_back({artifact.destination, destination, artifact.hashes});
        result.directories.push_back(destination.parent_path());
        result.profile = Json::object();
        return result;
    }
    result.profile = optifine_launchwrapper_profile(vanilla.baseVersionId, build.artifact.filename);
    if (compatibility.route == OptiFineRoute::LaunchWrapper) {
        const auto destination = library_path(root, "optifine:OptiFine:" + optifine_library_version(build.artifact.filename));
        result.copies.push_back({artifact.destination, destination, artifact.hashes});
        result.profile = normalize_profile(std::move(result.profile), build, root, {destination});
        const auto id = text(result.profile, "id");
        result.writes.push_back({path_inside(root, "versions/" + id + '/' + id + ".json"), result.profile.dump(2)});
        return result;
    }
    const auto classBytes = services.archiveRead(artifact.destination, "optifine/Installer.class", 16 * 1024 * 1024, check);
    if (classBytes.size() < 8 || static_cast<unsigned char>(classBytes[0]) != 0xca ||
        static_cast<unsigned char>(classBytes[1]) != 0xfe || static_cast<unsigned char>(classBytes[2]) != 0xba ||
        static_cast<unsigned char>(classBytes[3]) != 0xbe) throw std::invalid_argument("OptiFine installer has an invalid Java class header");
    const auto major = (static_cast<unsigned>(static_cast<unsigned char>(classBytes[6])) << 8) | static_cast<unsigned char>(classBytes[7]);
    result.minimumJava = std::max(8U, major > 44 ? major - 44 : 0U);
    std::string reservation = request.reservationId.empty() ? request.instanceName : request.reservationId;
    validate_version_id(reservation);
    const auto home = path_inside(root, "installer-tmp/" + reservation);
    const auto minecraft = home / ".minecraft";
    const auto versionDir = path_inside(minecraft, "versions/" + vanilla.baseVersionId);
    const auto vanillaJson = path_inside(request.root, "versions/" + vanilla.baseVersionId + '/' + vanilla.baseVersionId + ".json");
    ProcessorPlan processor;
    processor.label = "OptiFine Installer";
    processor.process.workingDirectory = home;
    processor.process.terminateOnDrop = true;
    // The installer's exception handler uses Swing; fail instead of waiting for an error dialog.
    processor.process.arguments = {"-Djava.awt.headless=true", "-Duser.home=" + path_utf8(home), "-Dminecraft.launcher.brand=Palladium", "-cp", path_utf8(artifact.destination), "optifine.Installer"};
    processor.process.environment = {{"HOME", path_utf8(home)}, {"APPDATA", path_utf8(home)}, {"USERPROFILE", path_utf8(home)}};
    processor.copiesBefore = {{vanillaJson, versionDir / (vanilla.baseVersionId + ".json"), {}},
                              {vanilla.clientJar, versionDir / (vanilla.baseVersionId + ".jar"), {}}};
    processor.writesBefore.push_back({minecraft / "launcher_profiles.json", R"({"profiles":{},"settings":{"enableSnapshots":false},"version":3})"});
    processor.generatedMinecraft = minecraft;
    result.processors.push_back(std::move(processor));
    result.directories.push_back(home);
    result.profile = Json::object();
    return result;
}

}

std::string resolve_processor_argument(std::string_view argument, const Json& data,
                                       const std::filesystem::path& root, const std::filesystem::path& installer,
                                       const ResolvedVersion& vanilla) {
    const auto libraries = root / "libraries";
    std::string result(argument);
    if (!data.is_object()) throw std::invalid_argument("Forge processor data must be an object");
    for (std::size_t pass = 0; pass <= data.size(); ++pass) {
        const auto original = result;
        for (const auto& [key, value] : data.items()) {
            auto replacement = text(value, "client");
            if (replacement.starts_with('\'') && replacement.ends_with('\'') && replacement.size() >= 2) replacement = replacement.substr(1, replacement.size() - 2);
            else if (replacement.starts_with('/')) replacement = path_utf8(path_inside(libraries, replacement.substr(1)));
            result = replace_all(std::move(result), '{' + key + '}', replacement);
        }
        if (result == original) break;
        if (pass == data.size()) throw std::invalid_argument("Forge processor data contains a cyclic expansion");
    }
    result = replace_all(std::move(result), "{SIDE}", "client");
    result = replace_all(std::move(result), "{ROOT}", path_utf8(root));
    result = replace_all(std::move(result), "{MINECRAFT_JAR}", path_utf8(vanilla.clientJar));
    result = replace_all(std::move(result), "{MINECRAFT_VERSION}", vanilla.baseVersionId);
    result = replace_all(std::move(result), "{INSTALLER}", path_utf8(installer));
    result = replace_all(std::move(result), "{LIBRARY_DIR}", path_utf8(libraries));
    if (result.find('{') != std::string::npos || result.find('}') != std::string::npos) throw std::invalid_argument("Forge processor has an unknown placeholder");
    for (auto at = result.find('['); at != std::string::npos; at = result.find('[', at + 1)) {
        const auto end = result.find(']', at + 1);
        if (end == std::string::npos) throw std::invalid_argument("Forge processor has an unclosed Maven reference");
        const auto path = path_utf8(library_path(root, std::string_view(result).substr(at + 1, end - at - 1)));
        result.replace(at, end - at + 1, path);
    }
    if (result.find('\0') != std::string::npos || result.find('\r') != std::string::npos || result.find('\n') != std::string::npos) throw std::invalid_argument("Forge processor argument contains a control character");
    return result;
}

Json optifine_launchwrapper_profile(std::string_view mc, std::string_view filename) {
    const auto version = optifine_library_version(filename);
    return Json{{"id", std::string(mc) + "-OptiFine_" + version}, {"inheritsFrom", mc}, {"jar", mc},
                {"type", "release"}, {"mainClass", "net.minecraft.launchwrapper.Launch"},
                {"libraries", Json::array({Json{{"name", "optifine:OptiFine:" + version}}, Json{{"name", "net.minecraft:launchwrapper:1.12"}}})},
                {"arguments", {{"game", Json::array({"--tweakClass", "optifine.OptiFineTweaker"})}}}};
}

LoaderPlan plan_loader(const LoaderBuild& build, const InstallRequest& request, const ResolvedVersion& vanilla,
                        std::span<const LoaderSelection> companions, const Services& services,
                        const CancelCheck& check, const ProgressCallback& progress) {
    check();
    if (build.minecraftVersion != vanilla.baseVersionId || !build.installable) throw std::invalid_argument("Loader build is not installable for this Minecraft version");
    if (build.kind == LoaderKind::Forge || build.kind == LoaderKind::NeoForge) return forge_plan(build, request, vanilla, services, check, progress);
    if (build.kind == LoaderKind::OptiFine) return optifine_plan(build, request, vanilla, companions, services, check, progress);
    LoaderPlan result;
    result.selection = {build.kind, build.version};
    if (build.kind == LoaderKind::Vanilla) { result.profile = Json::object(); return result; }
    auto profile = build.artifact.profile;
    if (profile.is_null()) {
        if (build.kind == LoaderKind::LiteLoader) profile = liteloader_profile(vanilla.baseVersionId, build.artifact.rawMetadata);
        else profile = fetch_metadata(services, build.artifact.url, check);
    }
    const auto root = path_inside(request.root, "loader/" + std::string(loader_name(build.kind)));
    result.profile = normalize_profile(std::move(profile), build, root);
    const auto id = text(result.profile, "id");
    result.writes.push_back({path_inside(root, "versions/" + id + '/' + id + ".json"), result.profile.dump(2)});
    if (build.kind == LoaderKind::LiteLoader) result.directories.push_back(path_inside(request.gameDirectory, "mods/" + vanilla.baseVersionId));
    check();
    return result;
}

}
