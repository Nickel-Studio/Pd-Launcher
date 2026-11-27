#include "runtime.hpp"
#include "maven.hpp"
#include "filesystem.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#undef CopyFile
#endif

namespace nickel::minecraft {
namespace {

std::atomic<std::uint64_t> temporarySequence{0};

std::string text(const Json& value, std::string_view key, std::string fallback = {}) {
    if (!value.is_object()) return fallback;
    const auto it = value.find(std::string(key));
    if (it == value.end() || it->is_null()) return fallback;
    if (!it->is_string()) throw std::invalid_argument("Java metadata string has an invalid type");
    return it->get<std::string>();
}

void make_directory(const std::filesystem::path& path) {
    reject_links(path);
#ifdef NICKEL_MINECRAFT_NATIVE_SERVICES
    foundation_detail::DirectoryGuard directory(path, true);
    directory.verify();
#else
    std::filesystem::create_directories(path);
#endif
    reject_links(path);
}

std::string read_text(const std::filesystem::path& path, std::uint64_t maximumBytes, const CancelCheck& check) {
    reject_links(path);
    const auto native = filesystem_path(path);
    if (!std::filesystem::is_regular_file(native) || std::filesystem::file_size(native) > maximumBytes) throw std::runtime_error("Managed metadata file is missing or oversized");
    std::ifstream stream(native, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open managed metadata file");
    std::string result;
    std::array<char, 65536> block{};
    while (stream) {
        check();
        stream.read(block.data(), static_cast<std::streamsize>(block.size()));
        result.append(block.data(), static_cast<std::size_t>(stream.gcount()));
        if (result.size() > maximumBytes) throw std::runtime_error("Managed metadata size limit exceeded");
    }
    if (!stream.eof()) throw std::runtime_error("Cannot read managed metadata file");
    check();
    return result;
}

class TemporaryFile {
public:
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFile() {
        if (!committed_) { try { std::error_code ignored; std::filesystem::remove(filesystem_path(path_), ignored); } catch (...) {} }
    }
    const std::filesystem::path& path() const noexcept { return path_; }
    void commit(const std::filesystem::path& destination) {
        reject_links(destination);
#ifdef _WIN32
        const auto sourceNative = filesystem_path(path_), destinationNative = filesystem_path(destination);
        if (!MoveFileExW(sourceNative.c_str(), destinationNative.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Managed copy commit failed");
        }
#else
        std::filesystem::rename(path_, destination);
#endif
        committed_ = true;
    }
private:
    std::filesystem::path path_;
    bool committed_ = false;
};

std::filesystem::path unique_copy_path(const std::filesystem::path& destination) {
    auto name = destination;
    name += ".nickel-copy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + '-' + std::to_string(temporarySequence.fetch_add(1));
    reject_links(name);
    if (std::filesystem::exists(filesystem_path(name))) throw std::runtime_error("Managed copy staging collision");
    return name;
}

Json managed_json(const Services& services, const Json& descriptor, const std::filesystem::path& destination,
                  const CancelCheck& check, bool offline) {
    auto file = artifact_download(descriptor, destination, FileKind::Metadata);
    file.maximumBytes = 16 * 1024 * 1024;
    ensure_file(services, file, check, {}, offline);
    return parse_metadata(read_text(destination, file.maximumBytes, check));
}

}

Json fetch_metadata(const Services& services, std::string_view url, const CancelCheck& check, std::uint64_t maximumBytes) {
    if (!services.fetch) throw std::logic_error("Minecraft HTTP adapter is unavailable");
    artifact_download(Json{{"url", url}}, std::filesystem::path("metadata.json"), FileKind::Metadata);
    check();
    const auto source = services.fetch(url, maximumBytes, check);
    check();
    return parse_metadata(source, static_cast<std::size_t>(maximumBytes));
}

void ensure_file(const Services& services, const DownloadFile& file, const CancelCheck& check,
                 const ProgressCallback& progress, bool offline) {
    check();
    reject_links(file.destination);
    const auto destinationNative = filesystem_path(file.destination);
    if (!services.verify) throw std::logic_error("Minecraft digest verification adapter is unavailable");
    if (file.size && *file.size > file.maximumBytes) throw std::invalid_argument("Minecraft download exceeds its byte quota");
    if (services.verify(file.destination, file.hashes, file.size, check)) {
        if (!std::filesystem::is_regular_file(destinationNative) || std::filesystem::file_size(destinationNative) > file.maximumBytes) throw std::runtime_error("Minecraft cached file violates its byte quota");
        check();
        return;
    }
    if (offline || file.url.empty()) throw std::runtime_error("Required Minecraft file is missing or corrupt: " + path_utf8(file.destination.filename()));
    if (!services.download) throw std::logic_error("Minecraft download adapter is unavailable");
    artifact_download(Json{{"url", file.url}}, file.destination, file.kind);
    for (const auto& fallback : file.fallbackUrls) artifact_download(Json{{"url", fallback}}, file.destination, file.kind);
    make_directory(file.destination.parent_path());
    check();
    services.download(file, check, progress);
    check();
    reject_links(file.destination);
    if (!services.verify(file.destination, file.hashes, file.size, check) ||
        !std::filesystem::is_regular_file(destinationNative) || std::filesystem::file_size(destinationNative) > file.maximumBytes) throw std::runtime_error("Minecraft downloaded file failed integrity verification");
    if (file.executable) {
        std::filesystem::permissions(destinationNative, std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add);
    }
    check();
}

void write_managed(const Services& services, const WriteFile& file, const CancelCheck& check) {
    if (!services.atomicWrite) throw std::logic_error("Minecraft atomic storage adapter is unavailable");
    check();
    make_directory(file.destination.parent_path());
    reject_links(file.destination);
    services.atomicWrite(file.destination, file.content, check);
    check();
}

void copy_managed(const Services& services, const CopyFile& file, const CancelCheck& check) {
    check();
    reject_links(file.source);
    reject_links(file.destination);
    const auto sourceNative = filesystem_path(file.source), destinationNative = filesystem_path(file.destination);
    if (!services.verify || !services.verify(file.source, file.hashes, {}, check)) throw std::runtime_error("Managed copy source is missing or corrupt");
    if (file.preserveExisting && std::filesystem::exists(destinationNative)) {
        if (!std::filesystem::is_regular_file(destinationNative)) throw std::runtime_error("Managed copy destination is not a regular file");
        return;
    }
    // Size alone cannot prove an unhashed generated artifact matches its source.
    if (file.source == file.destination || (!file.hashes.empty() && services.verify(file.destination, file.hashes, std::filesystem::file_size(sourceNative), check))) return;
    make_directory(file.destination.parent_path());
    TemporaryFile temporary(unique_copy_path(file.destination));
    std::ifstream input(sourceNative, std::ios::binary);
    std::ofstream output(filesystem_path(temporary.path()), std::ios::binary | std::ios::trunc);
    if (!input || !output) throw std::runtime_error("Cannot open managed copy streams");
    std::array<char, 65536> block{};
    std::uint64_t bytes = 0;
    while (input) {
        check();
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto count = input.gcount();
        if (count > 0) { output.write(block.data(), count); bytes += static_cast<std::uint64_t>(count); }
        if (!output) throw std::runtime_error("Managed copy write failed");
    }
    if (!input.eof()) throw std::runtime_error("Managed copy read failed");
    output.flush();
    if (!output) throw std::runtime_error("Managed copy flush failed");
    output.close();
    input.close();
    if (!services.verify(temporary.path(), file.hashes, bytes, check)) throw std::runtime_error("Managed copy integrity verification failed");
    check();
    temporary.commit(file.destination);
    check();
}

JavaCandidate probe_java(const Services& services, const std::filesystem::path& executable, const CancelCheck& check) {
    if (!services.spawn) throw std::logic_error("Minecraft owned process adapter is unavailable");
    check();
    if (!executable.is_absolute()) throw std::invalid_argument("Java executable must be an absolute path");
    reject_links(executable);
    if (!std::filesystem::is_regular_file(filesystem_path(executable))) throw std::invalid_argument("Java executable does not exist");
    struct Capture { std::mutex mutex; std::string bytes; };
    auto capture = std::make_shared<Capture>();
    ProcessSpec spec;
    spec.executable = executable;
    spec.workingDirectory = executable.parent_path();
    spec.arguments = {"-XshowSettings:properties", "-version"};
    spec.terminateOnDrop = true;
    auto child = services.spawn(spec, [capture](bool, std::string_view bytes) {
        std::lock_guard lock(capture->mutex);
        constexpr std::size_t limit = 256 * 1024;
        capture->bytes.append(bytes.substr(0, limit - capture->bytes.size()));
    });
    if (!child || !child->pid()) throw std::runtime_error("Java probe did not create an owned child");
    try {
        const auto exitCode = child->wait(check, true, std::chrono::seconds(15));
        if (!child->drain_owned(std::chrono::seconds(5))) throw std::runtime_error("Java probe output is not fully drained");
        check();
        if (exitCode != 0) throw std::runtime_error("Java probe exited unsuccessfully");
    } catch (...) {
        child->terminate_owned();
        child->drain_owned(std::chrono::seconds(5));
        throw;
    }
    std::lock_guard lock(capture->mutex);
    return parse_java_probe(executable, capture->bytes);
}

RuntimePlan plan_java_runtime(const Services& services, const InstallRequest& request,
                              const JavaRequirement& requirement, const Json& metadata, const CancelCheck& check) {
    check();
    if (request.offline) throw std::runtime_error("A compatible Java runtime is unavailable in offline mode");
    const auto arch = request.platform.normalized_architecture();
    const auto component = metadata.contains("javaVersion") ? text(metadata["javaVersion"], "component") : std::string{};
    if (!component.empty()) {
        try {
            validate_version_id(component);
            const auto platform = mojang_runtime_platform(request.platform.os, arch);
            const auto all = fetch_metadata(services, request.javaManifestUrl, check);
            if (all.contains(platform) && all[platform].contains(component) && all[platform][component].is_array()) {
                for (const auto& entry : all[platform][component]) {
                    const auto name = entry.contains("version") ? text(entry["version"], "name") : std::string{};
                    if (!requirement.accepts(parse_java_major(name)) || !entry.contains("manifest")) continue;
                    const auto root = path_inside(request.root, "runtime/" + component + '/' + platform);
                    const auto manifest = managed_json(services, entry["manifest"], path_inside(request.root, ".nickel/runtime/" + component + '-' + platform + ".json"), check, false);
                    return plan_mojang_runtime(manifest, root, request.platform);
                }
            }
        } catch (...) { check(); }
    }
    const auto major = compatible_adoptium_major(requirement);
    const auto os = request.platform.os == "osx" ? "mac" : request.platform.os;
    const auto url = "https://api.adoptium.net/v3/assets/latest/" + std::to_string(major) +
                     "/hotspot?architecture=" + url_encode_segment(adoptium_architecture(arch)) +
                     "&image_type=jre&os=" + url_encode_segment(os) + "&vendor=eclipse&heap_size=normal";
    const auto assets = fetch_metadata(services, url, check);
    if (!assets.is_array()) throw std::invalid_argument("Adoptium runtime metadata must be an array");
    for (const auto& entry : assets) {
        if (!entry.contains("binary") || !entry["binary"].contains("package")) continue;
        const auto& package = entry["binary"]["package"];
        const auto filename = text(package, "name");
        if (filename.empty()) continue;
        const auto relative = validate_relative_path(filename);
        if (relative.has_parent_path()) throw std::invalid_argument("Java runtime archive name is not a basename");
        Json descriptor{{"url", text(package, "link")}, {"sha256", text(package, "checksum")}};
        if (package.contains("size")) descriptor["size"] = package["size"];
        RuntimePlan result;
        result.root = path_inside(request.root, "runtime/adoptium-" + std::to_string(major) + '-' + arch);
        result.archive = artifact_download(descriptor, path_inside(request.root, ".nickel/runtime/" + filename), FileKind::Java);
        if (result.archive->hashes.empty() || !result.archive->size || *result.archive->size == 0) throw std::invalid_argument("Java runtime archive lacks integrity metadata");
        result.executable = result.root / (request.platform.os == "windows" ? "bin/java.exe" : request.platform.os == "osx" ? "Contents/Home/bin/java" : "bin/java");
        result.directories.push_back(result.root);
        check();
        return result;
    }
    throw std::runtime_error("No compatible 64-bit Java runtime is available");
}

void execute_java_runtime(const Services& services, const RuntimePlan& runtime,
                           const CancelCheck& check, const ProgressCallback& progress, bool offline) {
    check();
    reject_links(runtime.root);
    for (const auto& directory : runtime.directories) {
        if (!path_is_inside(runtime.root, directory)) throw std::invalid_argument("Java directory escapes its managed root");
        check();
        make_directory(directory);
    }
    if (runtime.archive) {
        ensure_file(services, *runtime.archive, check, progress, offline);
        if (!services.extractRuntime) throw std::logic_error("Minecraft safe runtime archive adapter is unavailable");
        services.extractRuntime(runtime.archive->destination, runtime.root, runtime.stripComponents, check);
    }
    for (const auto& file : runtime.downloads) {
        if (!path_is_inside(runtime.root, file.destination)) throw std::invalid_argument("Java download escapes its managed root");
        ensure_file(services, file, check, progress, offline);
    }
    std::vector<RuntimeLink> pending = runtime.links;
    for (std::size_t iteration = 0; !pending.empty() && iteration <= runtime.links.size(); ++iteration) {
        bool changed = false;
        std::erase_if(pending, [&](const auto& link) {
            check();
            if (!path_is_inside(runtime.root, link.destination) || !path_is_inside(runtime.root, link.target)) throw std::invalid_argument("Java runtime link escapes its managed root");
            reject_links(link.target);
            reject_links(link.destination);
            const auto targetNative = filesystem_path(link.target), destinationNative = filesystem_path(link.destination);
            if (!std::filesystem::exists(targetNative)) return false;
            make_directory(link.destination.parent_path());
            if (std::filesystem::exists(destinationNative)) {
                if (!std::filesystem::is_regular_file(destinationNative)) throw std::runtime_error("Java runtime link destination is occupied");
            } else {
#ifdef _WIN32
                if (std::filesystem::is_directory(targetNative)) throw std::runtime_error("Windows Java directory links are not allowed");
                std::filesystem::create_hard_link(targetNative, destinationNative);
#else
                if (std::filesystem::is_directory(targetNative)) std::filesystem::create_directory_symlink(link.target.lexically_relative(link.destination.parent_path()), destinationNative);
                else std::filesystem::create_symlink(link.target.lexically_relative(link.destination.parent_path()), destinationNative);
#endif
            }
            changed = true;
            return true;
        });
        if (!changed) break;
    }
    if (!pending.empty()) throw std::runtime_error("Java runtime links have missing or cyclic targets");
    check();
    reject_links(runtime.executable);
    const auto executableNative = filesystem_path(runtime.executable);
    if (!path_is_inside(runtime.root, runtime.executable) || !std::filesystem::is_regular_file(executableNative)) throw std::runtime_error("Managed Java archive has no expected executable");
    std::filesystem::permissions(executableNative, std::filesystem::perms::owner_exec |
                                 std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add);
    check();
}

}
