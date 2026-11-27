#include "installer.hpp"
#include "catalog.hpp"
#include "launcher.hpp"
#include "loader.hpp"
#include "maven.hpp"
#include "runtime.hpp"
#include "filesystem.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>
#include <set>
#include <utility>

namespace nickel::minecraft {
namespace {

std::string text(const Json& object, std::string_view key, std::string fallback = {}) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(std::string(key));
    if (it == object.end() || it->is_null()) return fallback;
    if (!it->is_string()) throw ErrorException(Error{"minecraft.metadata", "Invalid metadata string", {{"field", key}}});
    return it->get<std::string>();
}

std::string read_bounded(const std::filesystem::path& path, std::uint64_t limit, const CancelCheck& check) {
    check();
    reject_links(path);
    const auto native = filesystem_path(path);
    if (!std::filesystem::is_regular_file(native) || std::filesystem::file_size(native) > limit)
        throw ErrorException(Error{"minecraft.file", "Required metadata is absent or oversized", {{"file", path_utf8(path.filename())}}});
    std::ifstream input(native, std::ios::binary);
    if (!input) throw ErrorException(Error{"minecraft.io", "Cannot read Minecraft metadata"});
    std::string bytes;
    std::array<char, 65536> buffer{};
    while (input) {
        check();
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        bytes.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
        if (bytes.size() > limit) throw ErrorException(Error{"minecraft.limit", "Metadata byte limit exceeded"});
    }
    if (!input.eof()) throw ErrorException(Error{"minecraft.io", "Minecraft metadata read failed"});
    check();
    return bytes;
}

void directory(const std::filesystem::path& path, const CancelCheck& check) {
    check(); reject_links(path);
#ifdef NICKEL_MINECRAFT_NATIVE_SERVICES
    foundation_detail::DirectoryGuard directory(path, true);
    directory.verify();
#else
    std::filesystem::create_directories(path);
#endif
    reject_links(path); check();
}

void append_download(std::vector<DownloadFile>& files, DownloadFile file) {
    const auto found = std::find_if(files.begin(), files.end(), [&](const auto& old) { return old.destination == file.destination; });
    if (found == files.end()) { files.push_back(std::move(file)); return; }
    if (found->hashes.sha1 != file.hashes.sha1 || found->hashes.sha256 != file.hashes.sha256 || found->hashes.md5 != file.hashes.md5 ||
        (found->size && file.size && found->size != file.size))
        throw ErrorException(Error{"minecraft.integrity", "Conflicting artifact identity for one destination"});
    if (found->url.empty()) found->url = std::move(file.url);
    if (!found->size) found->size = file.size;
    found->maximumBytes = std::min(found->maximumBytes, file.maximumBytes);
}

void merge_loader(InstallPlan& plan, LoaderPlan layer) {
    if (layer.profile.is_object() && !layer.profile.empty()) plan.version.metadata = merge_version_profiles(plan.version.metadata, layer.profile);
    for (auto& file : layer.downloads) append_download(plan.downloads, std::move(file));
    for (auto& value : layer.writes) plan.writes.push_back(std::move(value));
    for (auto& value : layer.copies) plan.copies.push_back(std::move(value));
    for (auto& value : layer.archiveFiles) plan.archiveFiles.push_back(std::move(value));
    for (auto& value : layer.processors) plan.processors.push_back(std::move(value));
    for (auto& value : layer.directories) plan.directories.push_back(std::move(value));
}

void add_version_files(InstallPlan& plan, const Json& index, std::string_view resourcesBase) {
    if (!plan.version.metadata.contains("downloads") || !plan.version.metadata["downloads"].contains("client"))
        throw ErrorException(Error{"minecraft.metadata", "Minecraft profile has no client download"});
    auto client = artifact_download(plan.version.metadata["downloads"]["client"], plan.version.clientJar, FileKind::Client);
    append_download(plan.downloads, std::move(client));
    const auto natives = path_inside(plan.root, "versions/" + plan.version.versionId + "/natives");
    auto libraries = plan_libraries(plan.version.metadata, plan.root, plan.platform, natives);
    for (auto& file : libraries.downloads) append_download(plan.downloads, std::move(file));
    plan.classpath = std::move(libraries.classpath);
    plan.natives = std::move(libraries.natives);
    if (plan.version.metadata.contains("assetIndex")) {
        auto assets = plan_assets(plan.version.metadata, index, plan.root, plan.gameDirectory, resourcesBase);
        for (auto& file : assets.downloads) append_download(plan.downloads, std::move(file));
        for (auto& copy : assets.copies) plan.copies.push_back(std::move(copy));
    }
    if (plan.version.metadata.contains("logging") && plan.version.metadata["logging"].contains("client")) {
        const auto& logging = plan.version.metadata["logging"]["client"];
        if (logging.contains("file")) {
            const auto id = text(logging["file"], "id"); validate_version_id(id);
            auto file = artifact_download(logging["file"], path_inside(plan.root, "assets/log_configs/" + id), FileKind::Logging);
            file.maximumBytes = 16 * 1024 * 1024;
            append_download(plan.downloads, std::move(file));
        }
    }
}

void validate_processor_limits(const ProcessorPlan& processor) {
    if (!processor.maximumOutputBytes || !processor.maximumOutputFiles ||
        processor.maximumOutputFiles > 100000 || processor.outputs.size() > processor.maximumOutputFiles ||
        std::any_of(processor.outputs.begin(), processor.outputs.end(), [](const auto& output) { return !output.maximumBytes; }) ||
        (processor.managedDownload && !processor.managedDownload->maximumBytes))
        throw ErrorException(Error{"minecraft.processor.limit", "Invalid processor output quotas"});
    if (processor.timeout.count() < 0)
        throw ErrorException(Error{"minecraft.process.timeout", "Processor timeout must not be negative"});
}

void validate_plan_paths(const InstallPlan& plan) {
    if (plan.root.empty() || !plan.root.is_absolute() || plan.gameDirectory.empty() || !plan.gameDirectory.is_absolute())
        throw ErrorException(Error{"minecraft.path", "Install roots must be absolute"});
    reject_links(plan.root); reject_links(plan.gameDirectory);
    const auto allowed = [&](const std::filesystem::path& path) {
        if (path.empty() || !path.is_absolute() || (!path_is_inside(plan.root, path) && !path_is_inside(plan.gameDirectory, path)))
            throw ErrorException(Error{"minecraft.path", "Installer output escapes authorized roots"});
        reject_links(path);
    };
    allowed(plan.profilePath);
    for (const auto& file : plan.downloads) allowed(file.destination);
    for (const auto& file : plan.writes) allowed(file.destination);
    for (const auto& copy : plan.copies) { allowed(copy.source); allowed(copy.destination); }
    for (const auto& file : plan.archiveFiles) { allowed(file.archive); allowed(file.destination); }
    for (const auto& native : plan.natives) { allowed(native.archive); allowed(native.destination); }
    for (const auto& path : plan.directories) allowed(path);
    for (const auto& path : plan.classpath) allowed(path);
    if (plan.runtime) {
        allowed(plan.runtime->root); allowed(plan.runtime->executable);
        if (plan.runtime->archive) allowed(plan.runtime->archive->destination);
        for (const auto& path : plan.runtime->directories) allowed(path);
        for (const auto& file : plan.runtime->downloads) allowed(file.destination);
        for (const auto& link : plan.runtime->links) { allowed(link.destination); allowed(link.target); }
    }
    for (const auto& processor : plan.processors) {
        validate_processor_limits(processor);
        if (!processor.process.workingDirectory.empty()) allowed(processor.process.workingDirectory);
        if (!processor.mainClassJar.empty()) allowed(processor.mainClassJar);
        if (!processor.generatedMinecraft.empty()) allowed(processor.generatedMinecraft);
        if (processor.managedDownload) allowed(processor.managedDownload->destination);
        for (const auto& path : processor.classpath) allowed(path);
        for (const auto& output : processor.outputs) allowed(output.destination);
        for (const auto& file : processor.copiesBefore) { allowed(file.source); allowed(file.destination); }
        for (const auto& file : processor.writesBefore) allowed(file.destination);
    }
}

std::string classpath(std::span<const std::filesystem::path> entries, const Platform& platform) {
    std::string value;
    for (const auto& path : entries) {
        if (!value.empty()) value.push_back(platform.classpath_separator());
        value += java_path_argument(path_utf8(path), platform.os == "windows");
    }
    return value;
}

void validate_class(const Services& services, const std::filesystem::path& jar, std::string_view main,
                    unsigned javaMajor, const CancelCheck& check) {
    if (main.empty() || main.find_first_of("/\\: \t\r\n") != std::string_view::npos || main.starts_with('.') || main.ends_with('.') || main.find("..") != std::string_view::npos)
        throw ErrorException(Error{"minecraft.processor", "Invalid processor entry class"});
    auto entry = std::string(main); std::replace(entry.begin(), entry.end(), '.', '/'); entry += ".class";
    const auto bytes = services.archiveRead(jar, entry, 1024 * 1024, check);
    if (bytes.size() < 8 || static_cast<unsigned char>(bytes[0]) != 0xca || static_cast<unsigned char>(bytes[1]) != 0xfe ||
        static_cast<unsigned char>(bytes[2]) != 0xba || static_cast<unsigned char>(bytes[3]) != 0xbe)
        throw ErrorException(Error{"minecraft.processor", "Processor entry class is corrupt"});
    const auto major = (static_cast<unsigned>(static_cast<unsigned char>(bytes[6])) << 8) | static_cast<unsigned char>(bytes[7]);
    if (major < 45 || major > javaMajor + 44) throw ErrorException(Error{"minecraft.java", "Processor class requires a different Java version"});
}

void check_processor_outputs(const ProcessorPlan& processor, const CancelCheck& check) {
    check(); validate_processor_limits(processor);
    std::uint64_t total = 0, files = 0;
    const auto add = [&](const std::filesystem::path& path, std::uint64_t maximum) {
        check(); reject_links(path);
        const auto native = filesystem_path(path);
        if (!std::filesystem::exists(native)) return;
        if (!std::filesystem::is_regular_file(native)) throw ErrorException(Error{"minecraft.processor.output", "Processor output is not a regular file"});
        const auto size = std::filesystem::file_size(native);
        if (size > maximum || size > processor.maximumOutputBytes - total || ++files > processor.maximumOutputFiles)
            throw ErrorException(Error{"minecraft.processor.limit", "Processor output byte or file quota exceeded"});
        total += size;
    };
    for (const auto& output : processor.outputs) add(output.destination, output.maximumBytes);
    if (processor.managedDownload && std::none_of(processor.outputs.begin(), processor.outputs.end(), [&](const auto& output) {
        return output.destination == processor.managedDownload->destination;
    })) add(processor.managedDownload->destination, processor.managedDownload->maximumBytes);
    if (!processor.generatedMinecraft.empty()) {
        const auto generatedNative = filesystem_path(processor.generatedMinecraft);
        if (!std::filesystem::exists(generatedNative)) return;
        std::uint64_t entries = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(generatedNative)) {
            const auto logical = processor.generatedMinecraft / entry.path().lexically_relative(generatedNative);
            check(); reject_links(logical);
            if (++entries > processor.maximumOutputFiles * 2) throw ErrorException(Error{"minecraft.processor.limit", "Generated installer tree entry quota exceeded"});
            if (!entry.is_directory()) add(logical, processor.maximumOutputBytes);
        }
    }
}

void import_optifine(InstallPlan& plan, const ProcessorPlan& processor, const Services& services, const CancelCheck& check) {
    const auto generated = processor.generatedMinecraft;
    const auto versions = generated / "versions";
    std::vector<Json> candidates;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(filesystem_path(versions))) {
        const auto logical = versions / entry.path().filename();
        check(); reject_links(logical);
        if (++count > 128) throw ErrorException(Error{"minecraft.limit", "OptiFine generated profile limit exceeded"});
        if (!entry.is_directory()) continue;
        const auto name = path_utf8(logical.filename()); validate_version_id(name);
        if (name == plan.version.baseVersionId) continue;
        const auto path = logical / std::filesystem::u8path(name + ".json");
        if (!std::filesystem::is_regular_file(filesystem_path(path))) continue;
        auto profile = parse_metadata(read_bounded(path, 8 * 1024 * 1024, check));
        if (text(profile, "id") != name || text(profile, "inheritsFrom", plan.version.baseVersionId) != plan.version.baseVersionId ||
            text(profile, "mainClass").empty() || !profile.contains("libraries") || !profile["libraries"].is_array()) continue;
        if (std::none_of(profile["libraries"].begin(), profile["libraries"].end(), [](const auto& library) { return text(library, "name").starts_with("optifine:OptiFine:"); })) continue;
        candidates.push_back(std::move(profile));
    }
    if (candidates.size() != 1) throw ErrorException(Error{"minecraft.optifine", "Installer must generate exactly one matching OptiFine profile"});
    auto profile = std::move(candidates.front());
    const auto root = path_inside(plan.root, "loader/optifine");
    for (auto& library : profile["libraries"]) {
        const auto coordinate = MavenCoordinate::parse(text(library, "name"));
        const auto relative = coordinate.relative_path().generic_string();
        const auto source = path_inside(generated / "libraries", relative);
        const auto destination = path_inside(root / "libraries", relative);
        library["nickelLibraryRoot"] = path_utf8(root / "libraries");
        if (std::filesystem::is_regular_file(filesystem_path(source))) {
            copy_managed(services, {source, destination, {}}, check);
            library["downloads"]["artifact"] = Json{{"url", ""}, {"path", relative}, {"size", std::filesystem::file_size(filesystem_path(destination))}};
        } else if (coordinate.group == "optifine") throw ErrorException(Error{"minecraft.optifine", "Installer generated profile references a missing OptiFine library"});
    }
    const auto id = text(profile, "id");
    write_managed(services, {path_inside(root, "versions/" + id + '/' + id + ".json"), profile.dump(2)}, check);
    plan.version.metadata = merge_version_profiles(plan.version.metadata, profile);
    plan.version.metadata["id"] = plan.version.versionId;
    plan.version.metadata["jar"] = plan.version.clientVersionId;
    auto libraries = plan_libraries(plan.version.metadata, plan.root, plan.platform, path_inside(plan.root, "versions/" + plan.version.versionId + "/natives"));
    plan.classpath = std::move(libraries.classpath);
    plan.natives = std::move(libraries.natives);
    for (auto& file : libraries.downloads) {
        ensure_file(services, file, check, {}, plan.offline);
        append_download(plan.downloads, std::move(file));
    }
}

}

struct Installer::State {
    mutable std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<OwnedProcess>> processes;
};

Installer::Installer(Services services) : state_(std::make_shared<State>()), services_(std::move(services)) {
    if (!services_.verify || !services_.atomicWrite) throw ErrorException(Error{"minecraft.services", "Installer storage/hash adapters are unavailable"});
}

const Services& Installer::services() const noexcept { return services_; }

InstallPlan Installer::plan(const InstallRequest& request, const Cancellation& cancellation, const ProgressCallback& progress) {
    try {
        const auto operation = services_.forOperation ? services_.forOperation(cancellation) : services_;
        return plan_checked(request, operation, cancellation_check(operation, cancellation), progress);
    } catch (const ErrorException&) { throw; }
    catch (const std::exception& error) { throw ErrorException(Error{"minecraft.install.plan", error.what()}); }
}

InstalledVersion Installer::execute(InstallPlan plan, const Cancellation& cancellation, const ProgressCallback& progress) {
    try {
        const auto operation = services_.forOperation ? services_.forOperation(cancellation) : services_;
        return execute_checked(std::move(plan), operation, cancellation_check(operation, cancellation), progress);
    } catch (const ErrorException&) { throw; }
    catch (const std::exception& error) { throw ErrorException(Error{"minecraft.install.execute", error.what()}); }
}

InstalledVersion Installer::install(const InstallRequest& request, const Cancellation& cancellation, const ProgressCallback& progress) {
    try {
        // Planning and execution share one frozen snapshot and one cancellation.
        const auto operation = services_.forOperation ? services_.forOperation(cancellation) : services_;
        const auto check = cancellation_check(operation, cancellation);
        return execute_checked(plan_checked(request, operation, check, progress), operation, check, progress);
    } catch (const ErrorException&) { throw; }
    catch (const std::exception& error) { throw ErrorException(Error{"minecraft.install", error.what()}); }
}

InstallPlan Installer::plan(const InstallRequest& request, std::stop_token stop, const ProgressCallback& progress) {
    Cancellation cancellation;
    std::stop_callback callback(stop, [&] { cancellation.cancel(); });
    return plan(request, cancellation, progress);
}

InstalledVersion Installer::execute(InstallPlan plan, std::stop_token stop, const ProgressCallback& progress) {
    Cancellation cancellation;
    std::stop_callback callback(stop, [&] { cancellation.cancel(); });
    return execute(std::move(plan), cancellation, progress);
}

InstalledVersion Installer::install(const InstallRequest& request, std::stop_token stop, const ProgressCallback& progress) {
    Cancellation cancellation;
    std::stop_callback callback(stop, [&] { cancellation.cancel(); });
    return install(request, cancellation, progress);
}

InstallPlan Installer::plan_checked(const InstallRequest& original, const Services& services, const CancelCheck& check, const ProgressCallback& progress) {
    check();
    auto request = original;
    if (request.root.empty() || !request.root.is_absolute() || request.gameDirectory.empty() || !request.gameDirectory.is_absolute())
        throw ErrorException(Error{"minecraft.path", "Install root and game directory must be absolute"});
    reject_links(request.root); reject_links(request.gameDirectory);
    directory(request.root, check); directory(request.gameDirectory, check);
    std::optional<Json> manifest;
    const auto manifestPath = path_inside(request.root, ".nickel/version-manifest.json");
    const auto load_manifest = [&]() -> const Json& {
        if (!manifest) {
            if (request.offline) manifest = parse_metadata(read_bounded(manifestPath, 16 * 1024 * 1024, check));
            else {
                manifest = fetch_metadata(services, request.manifestUrl, check);
                write_managed(services, {manifestPath, manifest->dump()}, check);
            }
        }
        return *manifest;
    };
    if (request.versionId == "latest_release" || request.versionId == "latest_snapshot" || request.versionId == "r" || request.versionId == "lr" || request.versionId == "s" || request.versionId == "ls")
        request.versionId = resolve_version_alias(request.versionId, load_manifest());
    validate_version_id(request.versionId);
    std::map<std::string, Json, std::less<>> profiles;
    std::vector<DownloadFile> metadataFiles;
    if (request.profile.is_object()) {
        if (text(request.profile, "id", request.versionId) != request.versionId) throw ErrorException(Error{"minecraft.identity", "Supplied version profile identity differs from request"});
        profiles[request.versionId] = request.profile;
    }
    const auto resolver = [&](std::string_view id) -> Json {
        check();
        if (const auto found = profiles.find(id); found != profiles.end()) return found->second;
        const auto path = path_inside(request.root, "versions/" + std::string(id) + '/' + std::string(id) + ".json");
        Json profile;
        if (std::filesystem::is_regular_file(filesystem_path(path))) profile = parse_metadata(read_bounded(path, 16 * 1024 * 1024, check));
        else {
            if (request.offline) throw ErrorException(Error{"minecraft.offline", "Inherited version metadata is unavailable offline", {{"versionId", id}}});
            const auto entry = manifest_version_entry(id, load_manifest());
            auto file = artifact_download(entry, path, FileKind::Metadata); file.maximumBytes = 16 * 1024 * 1024;
            ensure_file(services, file, check, progress);
            metadataFiles.push_back(file);
            profile = parse_metadata(read_bounded(path, file.maximumBytes, check));
        }
        profiles.emplace(std::string(id), profile);
        return profile;
    };
    auto vanilla = resolve_version(request.versionId, request.root, resolver);
    InstallPlan plan;
    plan.root = request.root; plan.gameDirectory = request.gameDirectory; plan.platform = request.platform; plan.offline = request.offline;
    plan.version = vanilla;
    plan.downloads = std::move(metadataFiles);
    for (const auto& profile : vanilla.inheritance) {
        const auto id = text(profile, "id");
        plan.writes.push_back({path_inside(request.root, "versions/" + id + '/' + id + ".json"), profile.dump(2)});
    }
    std::vector<LoaderBuild> builds;
    Catalog catalog(services);
    for (const auto& loader : request.loaders) {
        check();
        if (loader.kind == LoaderKind::Vanilla) continue;
        auto build = catalog.resolve(loader, vanilla.baseVersionId, check);
        plan.loaders.push_back({build.kind, build.version}); builds.push_back(std::move(build));
    }
    validate_loader_combination(plan.loaders, vanilla.baseVersionId);
    std::stable_sort(builds.begin(), builds.end(), [](const auto& left, const auto& right) { return left.kind != LoaderKind::OptiFine && right.kind == LoaderKind::OptiFine; });
    unsigned minimumProcessorJava = 0;
    for (const auto& build : builds) {
        if (progress) progress({"loader-plan", std::string(loader_name(build.kind))});
        auto layer = plan_loader(build, request, vanilla, plan.loaders, services, check, progress);
        minimumProcessorJava = std::max(minimumProcessorJava, layer.minimumJava);
        merge_loader(plan, std::move(layer));
    }
    const auto instance = request.instanceName.empty() ? request.versionId : request.instanceName;
    validate_version_id(instance);
    plan.version.versionId = instance;
    plan.version.metadata["id"] = instance;
    plan.version.metadata["jar"] = vanilla.clientVersionId;
    plan.version.metadata.erase("inheritsFrom");
    plan.profilePath = path_inside(request.root, "versions/" + instance + '/' + instance + ".json");
    Json index = request.assetIndex;
    if (plan.version.metadata.contains("assetIndex")) {
        const auto& descriptor = plan.version.metadata["assetIndex"];
        const auto id = text(descriptor, "id"); validate_version_id(id);
        auto file = artifact_download(descriptor, path_inside(request.root, "assets/indexes/" + id + ".json"), FileKind::Metadata);
        file.maximumBytes = 64 * 1024 * 1024;
        if (!index.is_object()) { ensure_file(services, file, check, progress, request.offline); index = parse_metadata(read_bounded(file.destination, file.maximumBytes, check), 64 * 1024 * 1024); }
        else plan.writes.push_back({file.destination, index.dump()});
        if (!request.assetIndex.is_object()) append_download(plan.downloads, std::move(file));
    }
    add_version_files(plan, index, request.resourcesBase);
    for (const auto& mod : request.dependencyMods) {
        if (std::find(mod.minecraftVersions.begin(), mod.minecraftVersions.end(), vanilla.baseVersionId) == mod.minecraftVersions.end() || mod.file.hashes.empty() ||
            std::none_of(plan.loaders.begin(), plan.loaders.end(), [&](const auto& loader) { return std::find(mod.loaders.begin(), mod.loaders.end(), loader.kind) != mod.loaders.end(); }))
            throw ErrorException(Error{"minecraft.dependency", "Dependency mod is not pinned to this Minecraft/loader context"});
        if (!path_is_inside(request.gameDirectory / "mods", mod.file.destination)) throw ErrorException(Error{"minecraft.path", "Dependency mod destination escapes the mods directory"});
        append_download(plan.downloads, mod.file);
    }
    std::optional<unsigned> metadataMajor;
    if (plan.version.metadata.contains("javaVersion") && plan.version.metadata["javaVersion"].contains("majorVersion")) metadataMajor = plan.version.metadata["javaVersion"]["majorVersion"].get<unsigned>();
    plan.javaRequirement = loader_java_requirement(vanilla.baseVersionId, plan.loaders, text(vanilla.metadata, "releaseTime"), metadataMajor);
    if (minimumProcessorJava) {
        const std::array requirements{plan.javaRequirement, JavaRequirement{minimumProcessorJava, {}, minimumProcessorJava, "loader processor class"}};
        plan.javaRequirement = intersect_java_requirements(requirements);
    }
    auto candidates = request.javaCandidates;
    if (request.manualJava) plan.java = validate_manual_java(plan.javaRequirement, probe_java(services, *request.manualJava, check));
    else {
        if (candidates.empty() && services.discoverJava) candidates = services.discoverJava(check);
        plan.java = select_java(plan.javaRequirement, candidates);
        if (plan.java.needsDownload) {
            if (!request.installJava) throw ErrorException(Error{"minecraft.java", "Compatible Java is missing and runtime installation is disabled"});
            plan.runtime = plan_java_runtime(services, request, plan.javaRequirement, plan.version.metadata, check);
        }
    }
    validate_plan_paths(plan); check();
    return plan;
}

InstalledVersion Installer::execute_checked(InstallPlan plan, const Services& services, const CancelCheck& check, const ProgressCallback& progress) {
    check(); validate_plan_paths(plan);
    if (plan.runtime) {
        if (progress) progress({"java", "Preparing compatible runtime"});
        execute_java_runtime(services, *plan.runtime, check, progress, plan.offline);
        plan.java = validate_manual_java(plan.javaRequirement, probe_java(services, plan.runtime->executable, check));
        plan.java.source = "downloaded";
    }
    if (!plan.java.path || !plan.java.compatible || plan.java.bits != 64 || !plan.javaRequirement.accepts(plan.java.major))
        throw ErrorException(Error{"minecraft.java", "Installer has no verified compatible 64-bit Java"});
    for (const auto& path : plan.directories) directory(path, check);
    for (const auto& file : plan.writes) if (file.destination != plan.profilePath) write_managed(services, file, check);
    // Generated/embedded libraries must be materialized before a no-URL library is checked.
    std::set<std::filesystem::path> generated;
    for (const auto& file : plan.archiveFiles) generated.insert(file.destination);
    for (const auto& file : plan.copies) generated.insert(file.destination);
    for (const auto& processor : plan.processors) for (const auto& output : processor.outputs) generated.insert(output.destination);
    std::uint64_t done = 0;
    for (const auto& file : plan.downloads) {
        check();
        if (file.url.empty() && generated.contains(file.destination)) continue;
        ensure_file(services, file, check, progress, plan.offline);
        if (progress) progress({"files", path_utf8(file.destination.filename()), ++done, plan.downloads.size()});
    }
    for (const auto& file : plan.archiveFiles) {
        check();
        if (!services.archiveRead) throw ErrorException(Error{"minecraft.services", "Archive reader is unavailable"});
        const auto bytes = services.archiveRead(file.archive, file.entry, file.maximumBytes, check);
        if (bytes.size() > file.maximumBytes) throw ErrorException(Error{"minecraft.limit", "Embedded artifact exceeds its quota"});
        write_managed(services, {file.destination, bytes}, check);
        if (!services.verify(file.destination, file.hashes, bytes.size(), check)) throw ErrorException(Error{"minecraft.integrity", "Embedded artifact verification failed"});
    }
    for (const auto& file : plan.copies) copy_managed(services, file, check);
    for (auto& processor : plan.processors) {
        check_processor_outputs(processor, check);
        const auto outputs_valid = [&] {
            return !processor.outputs.empty() && std::all_of(processor.outputs.begin(), processor.outputs.end(), [&](const auto& output) {
                return !output.hashes.empty() && services.verify(output.destination, output.hashes, {}, check);
            });
        };
        if (outputs_valid()) continue;
        if (processor.managedDownload) {
            ensure_file(services, *processor.managedDownload, check, progress, plan.offline);
            check_processor_outputs(processor, check);
            if (!processor.outputs.empty() && !outputs_valid()) throw ErrorException(Error{"minecraft.processor.integrity", "Managed mappings differ from the processor output hashes"});
            continue;
        }
        if (!services.spawn || !services.archiveRead) throw ErrorException(Error{"minecraft.services", "Owned processor adapter is unavailable"});
        for (const auto& file : processor.copiesBefore) copy_managed(services, file, check);
        for (const auto& file : processor.writesBefore) write_managed(services, file, check);
        auto spec = processor.process;
        spec.executable = *plan.java.path; spec.terminateOnDrop = true; spec.purpose = ProcessKind::Preparation;
        directory(spec.workingDirectory, check);
        if (!processor.mainClassJar.empty()) {
            for (const auto& jar : processor.classpath) if (!services.verify(jar, {}, {}, check)) throw ErrorException(Error{"minecraft.processor", "Processor classpath file is missing"});
            const auto main = manifest_attribute(services.archiveRead(processor.mainClassJar, "META-INF/MANIFEST.MF", 64 * 1024, check), "Main-Class");
            validate_class(services, processor.mainClassJar, main, plan.java.major, check);
            spec.arguments.insert(spec.arguments.begin(), {"-cp", classpath(processor.classpath, plan.platform), main});
        }
        struct Capture { std::mutex mutex; std::string tail; };
        auto capture = std::make_shared<Capture>();
        if (progress) progress({"processor", processor.label});
        auto child = services.spawn(spec, [capture](bool, std::string_view bytes) {
            std::lock_guard lock(capture->mutex);
            constexpr std::size_t limit = 64 * 1024;
            if (bytes.size() >= limit) capture->tail.assign(bytes.substr(bytes.size() - limit));
            else { if (capture->tail.size() + bytes.size() > limit) capture->tail.erase(0, capture->tail.size() + bytes.size() - limit); capture->tail.append(bytes); }
        });
        if (!child || !child->pid()) throw ErrorException(Error{"minecraft.process", "Processor did not create an owned child"});
        { std::lock_guard lock(state_->mutex); state_->processes.emplace(child->pid(), child); }
        bool drained = false;
        try {
            const auto checked = [&] { check(); check_processor_outputs(processor, check); };
            const auto code = child->wait(checked, true, processor.timeout);
            drained = child->drain_owned(std::chrono::seconds(10));
            if (!drained) throw ErrorException(Error{"minecraft.process.drain", "Processor stdout/stderr did not reach EOF"});
            if (code != 0) throw ErrorException(Error{"minecraft.processor.exit", "Loader processor exited unsuccessfully", {{"processor", processor.label}, {"exitCode", code}}});
            checked();
        } catch (...) {
            const auto error = std::current_exception();
            try { child->terminate_owned(); drained = child->drain_owned(std::chrono::seconds(10)); } catch (...) {}
            if (drained) { std::lock_guard lock(state_->mutex); state_->processes.erase(child->pid()); }
            std::rethrow_exception(error);
        }
        { std::lock_guard lock(state_->mutex); state_->processes.erase(child->pid()); }
        if (!processor.generatedMinecraft.empty()) import_optifine(plan, processor, services, check);
        if (!processor.outputs.empty() && !outputs_valid()) throw ErrorException(Error{"minecraft.processor.integrity", "Processor outputs failed their exact declared hashes"});
    }
    for (const auto& file : plan.downloads) ensure_file(services, file, check, progress, plan.offline);
    for (const auto& native : plan.natives) {
        check();
        if (!services.extractNatives) throw ErrorException(Error{"minecraft.services", "Native extraction adapter is unavailable"});
        services.extractNatives(native, check);
    }
    check();
    plan.version.metadata["id"] = plan.version.versionId;
    plan.version.metadata["jar"] = plan.version.clientVersionId;
    plan.version.metadata.erase("inheritsFrom");
    write_managed(services, {plan.profilePath, plan.version.metadata.dump(2)}, check);
    InstalledVersion installed;
    installed.versionId = plan.version.versionId; installed.minecraftVersion = plan.version.baseVersionId;
    installed.root = plan.root; installed.gameDirectory = plan.gameDirectory; installed.profilePath = plan.profilePath;
    installed.profile = std::move(plan.version.metadata); installed.java = plan.java; installed.loaders = plan.loaders;
    installed.classpath = plan.classpath; installed.files = plan.downloads;
    check();
    // Publication and removal of pending are exclusively the instances owner's commit.
    if (progress) progress({"prepared", "All Minecraft files and processors verified", 1, 1});
    return installed;
}

std::vector<std::uint64_t> Installer::owned_installer_pids() const {
    std::lock_guard lock(state_->mutex);
    std::vector<std::uint64_t> result;
    for (const auto& [pid, child] : state_->processes) { (void)child; result.push_back(pid); }
    return result;
}

bool Installer::drain_owned_installer(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) throw ErrorException(Error{"minecraft.process.timeout", "Drain timeout must not be negative"});
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<OwnedProcess>> children;
    { std::lock_guard lock(state_->mutex); for (const auto& [pid, child] : state_->processes) { (void)pid; children.push_back(child); } }
    bool complete = true;
    for (const auto& child : children) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        const auto remaining = elapsed < timeout ? timeout - elapsed : std::chrono::milliseconds(0);
        if (!child->drain_owned(remaining)) { complete = false; continue; }
        std::lock_guard lock(state_->mutex); state_->processes.erase(child->pid());
    }
    return complete;
}

Json installed_version_json(const InstalledVersion& version) {
    auto loaders = Json::array();
    for (const auto& loader : version.loaders) loaders.push_back(Json{{"kind", loader_name(loader.kind)}, {"version", loader.version}});
    return Json{{"versionId", version.versionId}, {"minecraftVersion", version.minecraftVersion}, {"root", path_utf8(version.root)},
                {"gameDirectory", path_utf8(version.gameDirectory)}, {"profilePath", path_utf8(version.profilePath)},
                {"profile", version.profile}, {"loaders", std::move(loaders)}, {"files", download_plan_json(version.files)}};
}

}
