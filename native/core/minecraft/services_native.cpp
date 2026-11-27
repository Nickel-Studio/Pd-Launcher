#include "services_native.hpp"
#include "maven.hpp"
#include "runtime.hpp"
#include "core/archive/archive.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/path_guard.hpp"
#include "core/network/download.hpp"
#include "core/network/hash.hpp"
#include "core/network/secure_memory.hpp"
#include "platform/java.hpp"
#include "platform/process.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>

namespace nickel::minecraft {
namespace {
namespace net = nickel::transport;

std::vector<net::Digest> digests(const FileHashes& hashes) {
    std::vector<net::Digest> result;
    if (hashes.sha1) result.push_back({net::HashAlgorithm::Sha1, *hashes.sha1});
    if (hashes.sha256) result.push_back({net::HashAlgorithm::Sha256, *hashes.sha256});
    if (hashes.md5) result.push_back({net::HashAlgorithm::Md5, *hashes.md5});
    net::detail::validate_digests(result);
    return result;
}

bool verify(const std::filesystem::path& path, const FileHashes& hashes,
            std::optional<std::uint64_t> size, const CancelCheck& check) {
    check(); reject_path_links(path);
    const auto native = foundation_detail::native_filesystem_path(path);
    std::error_code error;
    const auto status = std::filesystem::status(native, error);
    if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found) return false;
    if (error) throw ErrorException(Error{"minecraft.io", "Cannot inspect artifact for verification"});
    if (!std::filesystem::is_regular_file(status)) return false;
    if (size && std::filesystem::file_size(native) != *size) return false;
    const auto expected = digests(hashes);
    if (expected.empty()) return true;
    std::vector<net::HashAlgorithm> algorithms;
    for (const auto& value : expected) algorithms.push_back(value.algorithm);
    const auto actual = net::detail::hash_file(path, algorithms, [&] { check(); return false; });
    check();
    return std::all_of(expected.begin(), expected.end(), [&](const auto& value) {
        const auto found = std::find_if(actual.begin(), actual.end(), [&](const auto& item) { return item.algorithm == value.algorithm; });
        return found != actual.end() && net::detail::digest_equal(value.hex, found->hex);
    });
}

std::string sha256(const std::filesystem::path& path, const CancelCheck& check) {
    const auto values = net::detail::hash_file(path, {net::HashAlgorithm::Sha256}, [&] { check(); return false; });
    return values.at(0).hex;
}

std::string mirror_url(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) return std::string(url);
    const auto remainder = url.substr(scheme + 3);
    const auto end = remainder.find_first_of("/?#");
    auto host = std::string(remainder.substr(0, end));
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto path = end == std::string_view::npos ? std::string{} : std::string(remainder.substr(end));
    constexpr std::string_view base = "https://bmclapi2.bangbang93.com";
    if (host == "launchermeta.mojang.com" || host == "launcher.mojang.com" || host == "piston-meta.mojang.com" || host == "piston-data.mojang.com") return std::string(base) + path;
    if (host == "resources.download.minecraft.net") return std::string(base) + "/assets" + path;
    if (host == "libraries.minecraft.net" || host == "maven.fabricmc.net" || host == "maven.minecraftforge.net") return std::string(base) + "/maven" + path;
    if (host == "maven.neoforged.net") return std::string(base) + "/maven" + (path.starts_with("/releases/") ? path.substr(9) : path);
    if (host == "maven.quiltmc.org") return std::string(base) + "/maven" + (path.starts_with("/repository/release/") ? path.substr(19) : path);
    if (host == "meta.fabricmc.net") return std::string(base) + "/fabric-meta" + path;
    if (host == "meta.quiltmc.org") return std::string(base) + "/quilt-meta" + path;
    return std::string(url);
}

std::vector<net::RequestCandidate> candidates(std::string_view url, std::span<const std::string> fallbacks, bool mirrors) {
    std::vector<net::RequestCandidate> result;
    std::set<std::string> seen;
    const auto add = [&](std::string value, bool mirror) {
        if (!value.empty() && seen.insert(value).second) result.push_back({std::move(value), mirror, {}});
    };
    add(std::string(url), url.find("bmclapi") != std::string_view::npos);
    if (mirrors) add(mirror_url(url), true);
    for (const auto& value : fallbacks) { add(value, value.find("bmclapi") != std::string_view::npos); if (mirrors) add(mirror_url(value), true); }
    return result;
}

archive::Limits archive_limits(std::uint64_t entryLimit = 64ULL * 1024 * 1024) {
    archive::Limits result;
    result.max_archive_bytes = 512ULL * 1024 * 1024;
    result.max_entry_bytes = entryLimit;
    result.max_total_bytes = 1024ULL * 1024 * 1024;
    result.max_duration = std::chrono::minutes(5);
    return result;
}

template<class Process>
void stop_game(Process& process) {
    if constexpr (requires(Process& value) { value.stop_owned_game(); }) process.stop_owned_game();
    else throw ErrorException(Error{"minecraft.process.unsupported", "Foundation does not yet expose explicit owned-game stopping"});
}

class ProcessAdapter final : public minecraft::OwnedProcess {
public:
    void handoff(nickel::OwnedProcess process) noexcept { process_.emplace(std::move(process)); }
    std::uint64_t pid() const noexcept override { return process_ ? process_->pid() : 0; }
    const nickel::OwnedProcess* native_process() const noexcept override { return process_ ? &*process_ : nullptr; }
    bool running() const override { return process_ && process_->running(); }
    int wait(const CancelCheck& check, bool terminateOnCancellation, std::chrono::milliseconds timeout) override {
        if (timeout.count() < 0) throw ErrorException(Error{"minecraft.process.timeout", "Process timeout must not be negative"});
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            try { if (check) check(); }
            catch (...) {
                const auto error = std::current_exception();
                if (terminateOnCancellation && process_->purpose() == ProcessPurpose::Preparation) {
                    process_->terminate(); (void)process_->drain(std::chrono::seconds(10));
                }
                std::rethrow_exception(error);
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            auto interval = std::chrono::milliseconds(25);
            if (timeout != std::chrono::milliseconds::max())
                interval = std::max(std::chrono::milliseconds(0), std::min(interval, timeout - elapsed));
            const auto result = process_->wait(interval);
            if (result) {
                if (!result->stdout_eof || !result->stderr_eof) throw ErrorException(Error{"minecraft.process.drain", "Owned process wait returned before both output EOF markers"});
                return result->exit_code;
            }
            if (timeout != std::chrono::milliseconds::max() && std::chrono::steady_clock::now() - start >= timeout) {
                if (process_->purpose() == ProcessPurpose::Preparation) { process_->terminate(); (void)process_->drain(std::chrono::seconds(10)); }
                throw ErrorException(Error{"minecraft.process.timeout", "Owned process execution timed out"});
            }
        }
    }
    void terminate_owned() override {
        if (process_->purpose() == ProcessPurpose::Game) stop_game(*process_);
        else process_->terminate();
    }
    bool drain_owned(std::chrono::milliseconds timeout) override {
        const auto result = process_->drain(timeout);
        return result && result->stdout_eof && result->stderr_eof;
    }
private:
    mutable std::optional<nickel::OwnedProcess> process_;
};

template<class Options>
void set_shell_command(Options& options, const std::optional<std::string>& command) {
    if (!command) return;
    if constexpr (requires(Options value) { value.windows_shell_command; }) options.windows_shell_command = command;
    else throw ErrorException(Error{"minecraft.command.unsupported", "Foundation does not yet expose the raw Windows shell-command adapter"});
}

std::shared_ptr<minecraft::OwnedProcess> spawn(const ProcessSpec& spec, const OutputCallback& output) {
    ProcessOptions options;
    options.executable = spec.executable; options.arguments = spec.arguments; options.cwd = spec.workingDirectory;
    options.hide_console = spec.hideWindow;
    options.capture_limit = 0;
    options.purpose = spec.purpose == ProcessKind::Game ? ProcessPurpose::Game : spec.purpose == ProcessKind::Service ? ProcessPurpose::Service : ProcessPurpose::Preparation;
    options.high_performance_gpu = spec.purpose == ProcessKind::Game && spec.highPerformanceGpu;
    if (options.high_performance_gpu) {
        // The launch plan executes the selected Java, including the JAR-wrapper case.
        options.gpu_preference_executable = spec.executable;
        options.gpu_preference_warning = spec.gpuPreferenceWarning;
    }
    for (const auto& [name, value] : spec.environment) options.environment[name] = value;
    for (const auto& name : spec.unsetEnvironment) options.environment[name] = std::nullopt;
    set_shell_command(options, spec.windowsShellCommand);
    options.output = [output](ProcessStream stream, std::string_view bytes, bool eof) { if (output && !eof) output(stream == ProcessStream::Stderr, bytes); };
    auto adapter = std::make_shared<ProcessAdapter>();
    adapter->handoff(nickel::OwnedProcess::spawn(options));
    return adapter;
}

struct Operation {
    using StopCallback = std::stop_callback<std::function<void()>>;
    Cancellation cancellation;
    std::unique_ptr<StopCallback> requestCancellation;
    std::unique_ptr<StopCallback> taskCancellation;
    std::shared_ptr<net::HttpClient> http;
    NativeServiceOptions options;
    net::detail::Erased<net::NetworkSnapshot> snapshot;
    std::mutex archiveMutex;

    Operation(std::shared_ptr<net::HttpClient> client, NativeServiceOptions selected,
              net::detail::Erased<net::NetworkSnapshot> captured)
        : http(std::move(client)), options(std::move(selected)), snapshot(std::move(captured)) {}
};

Json read_manifest(const std::filesystem::path& path) {
    reject_path_links(path);
    if (!std::filesystem::exists(foundation_detail::native_filesystem_path(path))) return Json::object();
    return parse_metadata(read_text_bounded(path, 8 * 1024 * 1024));
}

void extract_natives(const std::shared_ptr<Operation>& operation, const NativeArchive& native, const CancelCheck& check) {
    std::lock_guard lock(operation->archiveMutex);
    check();
    const auto limits = archive_limits();
    const auto entries = archive::Archive::list(native.archive, limits, operation->cancellation);
    const auto manifestPath = native.destination / ".nickel-native-files.json";
    auto manifest = read_manifest(manifestPath);
    if (!manifest.is_object()) throw ErrorException(Error{"minecraft.native", "Managed native manifest is invalid"});
    reject_path_links(native.destination);
    foundation_detail::DirectoryGuard directory(native.destination, true);
    for (const auto& entry : entries) {
        check();
        if (entry.directory || std::any_of(native.excludePrefixes.begin(), native.excludePrefixes.end(), [&](const auto& prefix) { return entry.name.starts_with(prefix); })) continue;
        const auto destination = path_inside(native.destination, entry.name);
        if (entry.size > 64ULL * 1024 * 1024) throw ErrorException(Error{"minecraft.native.limit", "Native entry exceeds its byte quota"});
        const auto bytes = archive::Archive::read_entry(native.archive, entry.name, entry.size, operation->cancellation, limits);
        if (bytes.size() != entry.size) throw ErrorException(Error{"minecraft.native.integrity", "Native entry size changed after validation"});
        net::detail::IncrementalHash hash(net::HashAlgorithm::Sha256); hash.update(bytes); const auto digest = hash.finish();
        FileHashes expected; expected.sha256 = digest;
        if (verify(destination, expected, entry.size, check)) { manifest[entry.name] = digest; continue; }
        if (std::filesystem::exists(foundation_detail::native_filesystem_path(destination))) {
            if (!manifest.contains(entry.name) || !manifest[entry.name].is_string()) throw ErrorException(Error{"minecraft.native.ownership", "Native extraction refuses to overwrite an unowned file"});
            FileHashes old; old.sha256 = manifest[entry.name].get<std::string>();
            if (!verify(destination, old, {}, check)) throw ErrorException(Error{"minecraft.native.conflict", "Managed native file was modified externally"});
        }
        atomic_write_file(destination, {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}, {false, true, &operation->cancellation});
        manifest[entry.name] = digest;
        atomic_write_text(manifestPath, manifest.dump(), {false, true, &operation->cancellation});
    }
    atomic_write_text(manifestPath, manifest.dump(), {false, true, &operation->cancellation});
    directory.verify(); check();
}

void extract_runtime(const std::shared_ptr<Operation>& operation, const std::filesystem::path& source,
                     const std::filesystem::path& destination, unsigned stripComponents, const CancelCheck& check) {
    std::lock_guard lock(operation->archiveMutex);
    check();
    archive::ExtractOptions options; options.limits = archive_limits(256ULL * 1024 * 1024);
    const auto entries = archive::Archive::list(source, options.limits, operation->cancellation);
    std::set<std::string> prefixes;
    if (stripComponents) {
        for (const auto& entry : entries) {
            std::size_t end = 0;
            bool enough = true;
            for (unsigned component = 0; component < stripComponents; ++component) {
                const auto next = entry.name.find('/', end);
                if (next == std::string::npos) { enough = false; break; }
                end = next + 1;
            }
            if (enough) prefixes.insert(entry.name.substr(0, end - 1));
            else if (!entry.directory) throw ErrorException(Error{"minecraft.runtime.path", "Runtime archive has a file outside its strip root"});
        }
        if (prefixes.size() != 1) throw ErrorException(Error{"minecraft.runtime.path", "Runtime archive must have one exact strip prefix"});
        options.subtrees.assign(prefixes.begin(), prefixes.end());
    }
    auto manifestPath = destination; manifestPath += ".nickel-runtime-files.json";
    const auto stored = read_manifest(manifestPath);
    if (!stored.is_object()) throw ErrorException(Error{"minecraft.runtime", "Managed runtime manifest is invalid"});
    if (stored.size() > options.limits.max_entries) throw ErrorException(Error{"minecraft.runtime.limit", "Managed runtime ownership entry quota exceeded"});
    Json manifest = Json::object();
    std::set<std::string> ownedKeys;
    for (const auto& [name, digest] : stored.items()) {
        check();
        if (!digest.is_string()) throw ErrorException(Error{"minecraft.runtime", "Managed runtime digest is invalid"});
        auto relative = name;
#ifdef _WIN32
        // Older manifests stored native separators; the archive API requires portable keys.
        std::replace(relative.begin(), relative.end(), '\\', '/');
#endif
        try { relative = archive::Archive::validate_relative_path(relative); }
        catch (const std::exception&) { throw ErrorException(Error{"minecraft.runtime.path", "Managed runtime ownership path is invalid"}); }
        if (!ownedKeys.insert(tasks::detail::case_key(relative)).second)
            throw ErrorException(Error{"minecraft.runtime.path", "Managed runtime ownership paths conflict"});
        const auto hashes = artifact_download(Json{{"url", ""}, {"sha256", digest}}, path_inside(destination, relative), FileKind::Java).hashes;
        if (!verify(path_inside(destination, relative), hashes, {}, check))
            throw ErrorException(Error{"minecraft.runtime.conflict", "Runtime files changed outside this installer"});
        manifest[relative] = digest;
        options.existing_owned_files.push_back(relative);
    }
    options.allow_owned_overlay = !manifest.empty();
    const auto receipt = archive::Archive::extract(source, destination, options, operation->cancellation);
    auto next = manifest;
    for (const auto& path : receipt.files) {
        const auto utf8 = path.lexically_relative(destination).generic_u8string();
        const auto relative = archive::Archive::validate_relative_path(std::string(utf8.begin(), utf8.end()));
        next[relative] = sha256(path, check);
    }
    atomic_write_text(manifestPath, next.dump(), {false, true, &operation->cancellation});
    check();
}

Services bound_services(std::shared_ptr<net::HttpClient> http, NativeServiceOptions options,
                        net::detail::Erased<net::NetworkSnapshot> snapshot, Cancellation cancellation) {
    auto operation = std::make_shared<Operation>(std::move(http), std::move(options), std::move(snapshot));
    const auto signal = [combined = operation->cancellation]() mutable { combined.cancel(); };
    operation->requestCancellation = std::make_unique<Operation::StopCallback>(cancellation.stop_token(), signal);
    if (operation->options.task) operation->taskCancellation = std::make_unique<Operation::StopCallback>(operation->options.task->cancellation().stop_token(), signal);
    Services result;
    result.cancellationCheck = [operation](const Cancellation& cancel) { cancel.check(); operation->cancellation.check(); };
    result.verify = verify;
    result.spawn = spawn;
    result.atomicWrite = [operation](const std::filesystem::path& path, std::string_view bytes, const CancelCheck& check) {
        check(); atomic_write_text(path, bytes, {false, true, &operation->cancellation}); check();
    };
    result.fetch = [operation](std::string_view url, std::uint64_t maximumBytes, const CancelCheck& check) {
        check();
        net::HttpRequest request; request.max_response_bytes = maximumBytes; request.trust = operation->options.trust; request.retry.max_attempts = 2;
        const auto choices = candidates(url, {}, operation->options.useMirrors);
        const auto response = operation->http->fetch_candidates(choices, net::SourceDomain::VersionList, request, operation->snapshot.get(), operation->cancellation);
        if (response.status < 200 || response.status >= 300) throw ErrorException(Error{"minecraft.http", "Metadata HTTP request failed", {{"status", response.status}}});
        check(); return response.text();
    };
    result.download = [operation](const DownloadFile& file, const CancelCheck& check, const ProgressCallback& progress) {
        check();
        net::DownloadRequest request;
        request.candidates = candidates(file.url, file.fallbackUrls, operation->options.useMirrors);
        request.destination = file.destination; request.expected_size = file.size; request.digests = digests(file.hashes);
        request.max_bytes = file.maximumBytes; request.service = "minecraft"; request.trust = operation->options.trust;
        request.parallel_ranges = operation->options.parallelRanges;
        request.require_digest = !request.digests.empty();
        if (file.destination.extension() == ".jar" || file.destination.extension() == ".zip") {
            const auto limit = archive_limits();
            request.validator = [limit](const std::filesystem::path& path, const Cancellation& cancel) { archive::Archive::validate(path, limit, cancel); };
        }
        operation->http->download(request, operation->snapshot.get(), operation->cancellation, [operation, progress](const net::TransferProgress& update) {
            if (operation->options.task) operation->options.task->progress(update);
            if (progress) progress({"download", {}, 0, 0, update.transferred_bytes, update.total_bytes.value_or(0)});
        });
        check();
    };
    result.archiveEntries = [operation](const std::filesystem::path& path, const CancelCheck& check) {
        check(); const auto entries = archive::Archive::list(path, archive_limits(), operation->cancellation);
        std::vector<std::string> names; names.reserve(entries.size());
        for (const auto& entry : entries) names.push_back(entry.directory && !entry.name.ends_with('/') ? entry.name + '/' : entry.name);
        check(); return names;
    };
    result.archiveRead = [operation](const std::filesystem::path& path, std::string_view entry, std::uint64_t maximumBytes, const CancelCheck& check) {
        check();
        const auto bytes = archive::Archive::read_entry(path, entry, std::min<std::uint64_t>(maximumBytes, 64ULL * 1024 * 1024), operation->cancellation, archive_limits());
        check(); return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    result.extractNatives = [operation](const NativeArchive& native, const CancelCheck& check) { extract_natives(operation, native, check); };
    result.extractRuntime = [operation](const std::filesystem::path& source, const std::filesystem::path& destination, unsigned strip, const CancelCheck& check) { extract_runtime(operation, source, destination, strip, check); };
    result.discoverJava = [operation](const CancelCheck& check) {
        check(); std::vector<JavaCandidate> candidates;
        for (const auto& runtime : discover_java({}, &operation->cancellation)) candidates.push_back({runtime.executable, runtime.version, runtime.vendor, runtime.architecture, runtime.major, runtime.is_64_bit ? 64U : 32U, true});
        check(); return candidates;
    };
    result.validateLaunchable = [](const std::filesystem::path& path, const CancelCheck& check) {
        check(); reject_path_links(path);
        if (std::filesystem::exists(foundation_detail::native_filesystem_path(path / ".nickel-pack.pending"))) throw ErrorException(Error{"minecraft.pending", "Pending instance cannot launch"});
    };
    return result;
}

}

Services make_native_services(std::shared_ptr<transport::HttpClient> http, const NativeServiceOptions& options) {
    if (!http) throw ErrorException(Error{"minecraft.services", "Native HTTP client is required"});
    if (!options.parallelRanges || options.parallelRanges > 8) throw ErrorException(Error{"minecraft.services", "Invalid transfer range concurrency"});
    // Raw options never own a snapshot; the only factory copy is guarded from its first allocation.
    NativeServiceOptions settings;
    settings.trust = options.trust; settings.task = options.task;
    settings.useMirrors = options.useMirrors; settings.parallelRanges = options.parallelRanges;
    net::detail::Erased<std::optional<net::NetworkSnapshot>> fixedSnapshot;
    if (!settings.task) net::detail::copy_sensitive(fixedSnapshot.get(), options.snapshot);
    const auto bind = [http, settings = std::move(settings), fixedSnapshot = std::move(fixedSnapshot)](const Cancellation& cancellation) {
        if (settings.task)
            return bound_services(http, settings, net::detail::Erased<net::NetworkSnapshot>(settings.task->network()), cancellation);
        if (fixedSnapshot.get())
            return bound_services(http, settings, net::detail::Erased<net::NetworkSnapshot>(*fixedSnapshot.get()), cancellation);
        return bound_services(http, settings, net::detail::Erased<net::NetworkSnapshot>(http->snapshot()), cancellation);
    };
    auto base = bind(Cancellation{});
    base.forOperation = bind;
    return base;
}

}
