#include "adapters.hpp"
#include "utility.hpp"
#include "core/archive/archive.hpp"
#include "core/instances/settings.hpp"
#include "core/network/url_policy.hpp"
#include "core/tasks/secure_fs.hpp"

#include <algorithm>
#include <array>
#include <set>

namespace nickel::resources {
namespace {

bool redirectStatus(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

transport::HttpRequest transportRequest(const HttpRequest& input) {
    transport::HttpRequest result;
    result.url = checkedUrl(input.url, input.rules);
    if (input.method == "GET") result.method = transport::HttpMethod::Get;
    else if (input.method == "POST") result.method = transport::HttpMethod::Post;
    else fail("invalid_input", "Unsupported resource HTTP method");
    result.max_response_bytes = input.maximumBytes;
    result.timeout = input.timeout;
    result.redirect_policy = transport::RedirectPolicy::None;
    result.max_redirects = 0;
    result.retry.max_attempts = 1;
    result.trust = parseUrl(result.url).scheme == "http" ? transport::TrustPolicy::ControlledLoopback : transport::TrustPolicy::PublicHttps;
    result.allowed_origins = {urlOrigin(result.url)};
    for (const auto& [name, value] : input.headers) result.headers.push_back({name, value});
    const auto bytes = std::as_bytes(std::span(input.body));
    result.body.assign(bytes.begin(), bytes.end());
    return result;
}

archive::Limits archiveLimits(const ModLimits& limits) {
    archive::Limits result;
    result.max_entries = 10000;
    result.max_entry_bytes = limits.maximumArchiveBytes;
    result.max_total_bytes = limits.maximumArchiveBytes;
    result.max_archive_bytes = limits.maximumArchiveBytes;
    result.max_path_bytes = 1024;
    result.max_depth = 64;
    result.max_duration = std::chrono::seconds(30);
    return result;
}

bool equalContext(const InstanceContext& selected, const InstanceContext& current) {
    return selected.identity == current.identity && selected.revision == current.revision &&
        selected.resourceRoot == current.resourceRoot && selected.modsRoot == current.modsRoot &&
        selected.resourcepacksRoot == current.resourcepacksRoot && selected.shaderpacksRoot == current.shaderpacksRoot &&
        selected.datapacksRoot == current.datapacksRoot && selected.minecraftVersion == current.minecraftVersion &&
        selected.loader == current.loader && !current.running;
}

Path resourceDestination(const InstanceContext& context, ResourceKind kind) {
    Path destination;
    switch (kind) {
    case ResourceKind::Mod: destination = context.modsRoot; break;
    case ResourceKind::Resourcepack: destination = context.resourcepacksRoot; break;
    case ResourceKind::Shader: destination = context.shaderpacksRoot; break;
    case ResourceKind::Datapack: destination = context.datapacksRoot; break;
    case ResourceKind::Modpack: fail("invalid_input", "Modpack must create a new instance transaction");
    }
    const auto root = detail::checkedDirectory(context.resourceRoot);
    const auto normalized = destination.lexically_normal();
    const auto relative = normalized.lexically_relative(root);
    if (!destination.is_absolute() || relative.empty() || relative == "." || relative.is_absolute() ||
        !validArchiveRelativePath(utf8Path(relative)))
        fail("policy_denied", "Resolved resource destination escapes the instance context");
    tasks::detail::ParentGuard pin(normalized, true);
    return detail::checkedDirectory(normalized);
}

bool verifiedExisting(const Path& file, const VersionFile& expected, const Cancellation& cancel, std::uint64_t maximumBytes) {
    const auto identity = hashResourceFile(file, cancel, maximumBytes);
    return identity.sizeBytes == expected.sizeBytes &&
        (expected.sha1.empty() || identity.sha1 == checkedHash(expected.sha1, 20)) &&
        (expected.sha512.empty() || identity.sha512 == checkedHash(expected.sha512, 64));
}

void downloadStaged(transport::HttpClient& http, const transport::NetworkSnapshot& snapshot,
                    const DownloadPlan& plan, const Path& staged, const ModLimits& limits,
                    const Cancellation& cancel, const transport::Progress& progress) {
    if (plan.candidates.empty() || plan.candidates.size() > 2 || !plan.file.sizeBytes ||
        plan.file.sizeBytes > limits.maximumArchiveBytes || !validResourceFileName(plan.file.fileName) ||
        (checkedHash(plan.file.sha1, 20).empty() && checkedHash(plan.file.sha512, 64).empty()))
        fail("integrity", "Resource download plan is not bounded and hash-pinned");
    const auto pin = std::make_unique<tasks::detail::ParentGuard>(staged.parent_path());
    std::optional<Error> last;
    for (const auto& candidate : plan.candidates) {
        cancel.check();
        tasks::detail::FileIdentity owned{};
        bool created = false;
        try {
            auto output = tasks::detail::SecureFile::create_new(staged);
            owned = output.identity();
            created = true;
            auto request = candidate.request;
            request.maximumBytes = static_cast<std::size_t>(plan.file.sizeBytes);
            request.method = "GET";
            request.body.clear();
            request.followRedirects = true;
            const auto deadline = std::chrono::steady_clock::now() + request.timeout;
            std::set<std::string> visited;
            std::uint64_t received = 0;
            for (unsigned hop = 0;; ++hop) {
                request.url = checkedUrl(request.url, request.rules);
                if (!visited.insert(request.url).second) fail("policy_denied", "Resource redirect loop");
                auto native = transportRequest(request);
                native.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (native.timeout->count() <= 0) fail("timeout", "Resource download deadline exceeded");
                bool redirected = false;
                const auto head = http.stream(native, snapshot, cancel,
                    [&](std::span<const std::byte> bytes) {
                        cancel.check();
                        if (redirected) return;
                        if (bytes.size() > plan.file.sizeBytes - received) fail("too_large", "Resource download exceeds expected size");
                        output.write(bytes);
                        received += bytes.size();
                    }, progress, [&](const transport::ResponseHead& response) {
                        if (redirectStatus(response.status)) { redirected = true; return; }
                        if (response.status != 200) fail("http_error", "Resource download request failed", {{"httpStatus", response.status}});
                        if (response.content_length && *response.content_length != plan.file.sizeBytes)
                            fail("integrity", "Resource content length differs from the pinned file");
                    });
                if (!redirected) break;
                if (hop >= request.maximumRedirects) fail("policy_denied", "Resource redirect budget exhausted");
                const auto location = head.header("location");
                if (location.empty()) fail("invalid_response", "Resource redirect is missing its target");
                request.url = checkedUrl(transport::detail::redirect_url(request.url, location), request.rules);
            }
            if (received != plan.file.sizeBytes || output.size() != received) fail("integrity", "Resource download is truncated");
            output.flush();
            output.close();
            if (!verifiedExisting(staged, plan.file, cancel, limits.maximumArchiveBytes)) fail("integrity", "Downloaded resource hash differs from the pinned file");
            archive::Archive::validate(staged, archiveLimits(limits), cancel);
            cancel.check();
            return;
        } catch (const ErrorException& failure) {
            if (created) {
                auto ownedPin = tasks::detail::SecureFile::open_owned(staged, owned);
                ownedPin.close();
                detail::removeRegular(staged.parent_path(), utf8Path(staged.filename()));
            }
            if (failure.error().code == "cancelled") throw;
            last = failure.error();
        } catch (const std::exception&) {
            if (created) {
                auto ownedPin = tasks::detail::SecureFile::open_owned(staged, owned);
                ownedPin.close();
                detail::removeRegular(staged.parent_path(), utf8Path(staged.filename()));
            }
            last = Error{"io", "Shared resource download or archive validation failed"};
        }
    }
    throw ErrorException(last.value_or(Error{"network", "No resource file source succeeded"}));
}

} // namespace

void downloadResourceStaged(transport::HttpClient& http, const transport::NetworkSnapshot& snapshot,
                            const DownloadPlan& plan, const Path& staged, const ModLimits& limits,
                            const Cancellation& cancel, const transport::Progress& progress) {
    cancel.check();
    if (!limits.maximumArchiveBytes || limits.maximumArchiveBytes > 512ULL * 1024 * 1024)
        fail("invalid_input", "Resource download archive limit is outside the approved budget");
    if (!staged.is_absolute() || staged != staged.lexically_normal())
        fail("policy_denied", "Resource staging path must be an absolute owned leaf");
    detail::checkedMissingLeaf(staged.parent_path(), utf8Path(staged.filename()));
    downloadStaged(http, snapshot, plan, staged, limits, cancel, progress);
}

namespace {
HttpFetch makeHttpFetchBound(std::shared_ptr<transport::HttpClient> http,
                            std::optional<transport::NetworkSnapshot> pinned) {
    if (!http) fail("invalid_input", "Shared HttpClient is required");
    return [http = std::move(http), pinned = std::move(pinned)](const HttpRequest& input, const Cancellation& cancel) {
        cancel.check();
        const auto snapshot = pinned ? *pinned : http->snapshot();
        auto request = input;
        if (!request.maximumBytes || request.maximumBytes > 8 * 1024 * 1024 || request.maximumRedirects > 8 ||
            request.timeout.count() <= 0 || request.timeout.count() > 300000 || request.body.size() > 256 * 1024)
            fail("invalid_input", "Resource HTTP request exceeds adapter limits");
        const auto deadline = std::chrono::steady_clock::now() + request.timeout;
        std::set<std::string> visited;
        for (unsigned hop = 0;; ++hop) {
            request.url = checkedUrl(request.url, request.rules);
            if (!visited.insert(request.url).second) fail("policy_denied", "Provider redirect loop");
            auto native = transportRequest(request);
            native.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (native.timeout->count() <= 0) fail("timeout", "Provider request deadline exceeded");
            const auto reservation = http->budget()->reserve_buffer(request.maximumBytes, [&] { return cancel.cancelled(); });
            HttpResponse result;
            bool redirected = false;
            const auto head = http->stream(native, snapshot, cancel,
                [&](std::span<const std::byte> bytes) {
                    cancel.check();
                    if (redirected) return;
                    if (bytes.size() > request.maximumBytes - result.bytes.size()) fail("too_large", "Provider response exceeds adapter budget");
                    const auto* first = reinterpret_cast<const std::uint8_t*>(bytes.data());
                    result.bytes.insert(result.bytes.end(), first, first + bytes.size());
                }, {}, [&](const transport::ResponseHead& response) {
                    redirected = request.followRedirects && redirectStatus(response.status);
                });
            result.status = head.status;
            result.effectiveUrl = checkedUrl(head.effective_url, request.rules);
            for (const auto& header : head.headers) {
                const auto name = lower(header.name);
                if (const auto found = result.headers.find(name); found != result.headers.end()) found->second += ", " + header.value;
                else result.headers.emplace(name, header.value);
            }
            if (!redirected) return result;
            if (hop >= request.maximumRedirects) fail("policy_denied", "Provider redirect budget exhausted");
            const auto location = head.header("location");
            if (location.empty()) fail("invalid_response", "Provider redirect is missing its target");
            const auto next = checkedUrl(transport::detail::redirect_url(request.url, location), request.rules);
            if (request.method != "GET" && urlOrigin(next) != urlOrigin(request.url)) fail("policy_denied", "Cross-origin provider POST redirect denied");
            if (urlOrigin(next) != urlOrigin(request.url)) {
                std::erase_if(request.headers, [](const auto& header) {
                    const auto name = lower(header.first);
                    return name == "authorization" || name == "cookie" || name == "x-api-key";
                });
            }
            request.url = next;
        }
    };
}
} // namespace

HttpFetch makeHttpFetch(std::shared_ptr<transport::HttpClient> http) {
    return makeHttpFetchBound(std::move(http), {});
}

HttpFetch makeHttpFetch(std::shared_ptr<transport::HttpClient> http, const transport::NetworkSnapshot& snapshot) {
    return makeHttpFetchBound(std::move(http), snapshot);
}

InstanceContext resolveInstanceContext(const instances::InstanceRegistry& registry,
                                       const InstanceIdentity& identity, const Json& globalSettings, bool running) {
    if (identity.rootId.empty() || identity.instanceId.empty() || !validUtf8(identity.rootId))
        fail("invalid_input", "Instance resource identity is missing");
    instances::validate_name(identity.instanceId);
    const auto registered = registry.get();
    const auto found = std::find_if(registered.roots.begin(), registered.roots.end(), [&](const auto& root) {
        return instances::root_identity(root) == identity.rootId;
    });
    if (found == registered.roots.end()) fail("policy_denied", "Instance root is not registered");
    const auto root = detail::checkedDirectory(pathFromUtf8(*found));
    const auto entry = instances::instance_detail(root, identity.instanceId);
    if (!entry.ready || !entry.minecraft_version || !instances::valid_minecraft_version(*entry.minecraft_version))
        fail("not_found", "Instance resource context is not ready");
    const auto local = instances::read_local_settings(root, identity.instanceId);
    const auto effective = instances::resolve_settings(globalSettings, local.value);
    InstanceContext result;
    result.identity = identity;
    result.label = entry.display_name;
    result.minecraftVersion = *entry.minecraft_version;
    result.loader = lower(entry.loader);
    result.resourceRoot = detail::checkedDirectory(instances::resource_directory(root, identity.instanceId, globalSettings));
    result.modsRoot = instances::mods_directory(result.resourceRoot, result.loader, result.minecraftVersion);
    result.resourcepacksRoot = result.resourceRoot / "resourcepacks";
    result.shaderpacksRoot = result.resourceRoot / "shaderpacks";
    result.datapacksRoot = result.resourceRoot / "datapacks";
    result.updatesDisabled = effective.disable_mod_updates;
    result.running = running;
    Json revision = {instances::entry_to_json(entry), local.value, effective.isolation,
        result.updatesDisabled, running, utf8Path(result.resourceRoot)};
    if (const auto descriptor = instances::pick_version_json(entry.directory, identity.instanceId))
        revision.push_back(detail::fileRevision(*descriptor));
    const auto encoded = revision.dump();
    transport::detail::IncrementalHash hash(transport::HashAlgorithm::Sha256);
    hash.update(std::as_bytes(std::span(encoded)));
    result.revision = hash.finish();
    return result;
}

ResourceServices bindResourceInstaller(std::shared_ptr<transport::HttpClient> http,
                                       std::shared_ptr<tasks::Registry> registry,
                                       ResourceServices authority, ModLimits limits) {
    if (!http || !registry || !authority.context || !authority.acquireMutation ||
        !limits.maximumArchiveBytes || limits.maximumArchiveBytes > 512ULL * 1024 * 1024)
        fail("invalid_input", "Resource installer requires shared HTTP, tasks and instance authority");
    const auto contextResolver = authority.context;
    authority.install = [http = std::move(http), registry = std::move(registry), contextResolver, limits](
        const InstanceContext& selected, const DownloadPlan& plan, std::shared_ptr<void> lease,
        const Cancellation& cancel, const ProgressCallback& progress) {
        cancel.check();
        if (!lease || !equalContext(selected, contextResolver(selected.identity))) fail("conflict", "Instance resource authority changed before download");
        if (!validResourceFileName(plan.file.fileName) || !plan.file.sizeBytes || plan.file.sizeBytes > limits.maximumArchiveBytes ||
            (checkedHash(plan.file.sha1, 20).empty() && checkedHash(plan.file.sha512, 64).empty()))
            fail("integrity", "Resource plan is not bounded and hash-pinned");
        const auto root = resourceDestination(selected, plan.kind);
        // A read-only directory guard would block the shared handle-based publication.
        tasks::detail::ParentGuard pin(root, false, tasks::detail::GuardMode::NamespaceMutation);
        const auto target = root / pathFromUtf8(plan.file.fileName);
        if (std::filesystem::exists(target)) {
            detail::checkedLeaf(root, plan.file.fileName, false);
            if (!verifiedExisting(target, plan.file, cancel, limits.maximumArchiveBytes)) fail("conflict", "A different resource already occupies the destination");
            if (!equalContext(selected, contextResolver(selected.identity))) fail("conflict", "Instance changed before resource reuse");
            return InstallReceipt{{}, plan.file.fileName, target, true};
        }
        detail::checkedMissingLeaf(root, plan.file.fileName);
        tasks::TaskSpec specification;
        specification.kind = tasks::TaskKind::Resource;
        specification.target = target;
        specification.label = plan.file.fileName;
        specification.network = http->snapshot();
        const auto task = registry->begin(specification);
        std::stop_callback stopped(cancel.stop_token(), [task] { task->cancel(); });
        try {
            task->stage("download", plan.file.fileName);
            const auto staged = task->staging() / "resource.download";
            downloadStaged(*http, task->network(), plan, staged, limits, task->cancellation(), [&](const auto& value) {
                task->progress(value);
                if (progress) progress({"download", value.completed_bytes, value.total_bytes.value_or(0)});
            });
            task->cancellation().check();
            if (!equalContext(selected, contextResolver(selected.identity))) fail("conflict", "Instance changed before resource commit");
            task->stage("commit", plan.file.fileName);
            task->commit_files({{"resource.download", target}});
            return InstallReceipt{task->id(), plan.file.fileName, target, false};
        } catch (const ErrorException& failure) {
            task->rollback(failure.error().code, failure.error().message);
            throw;
        } catch (const std::exception&) {
            task->rollback("io", "Resource installation failed");
            fail("io", "Resource installation failed");
        }
    };
    return authority;
}

ModServices bindModServices(ModServices services, ModLimits limits) {
    const auto policy = archiveLimits(limits);
    services.archiveEntries = [policy](const Path& path, const Cancellation& cancel) {
        detail::FileReader pin(path, policy.max_archive_bytes);
        const auto entries = archive::Archive::inspect(path, policy, cancel);
        pin.verifyUnchanged();
        std::vector<ArchiveEntry> result;
        result.reserve(entries.size());
        for (const auto& entry : entries) result.push_back({entry.name, entry.size, !entry.directory, entry.directory});
        return result;
    };
    services.readArchiveEntry = [policy](const Path& path, const std::string& name, std::size_t maximumBytes, const Cancellation& cancel) {
        if (!maximumBytes || maximumBytes > 512 * 1024 || !validArchiveRelativePath(name))
            fail("policy_denied", "Mod archive entry exceeds approved read budget");
        detail::FileReader pin(path, policy.max_archive_bytes);
        const auto bytes = archive::Archive::read_entry(path, name, maximumBytes, cancel, policy);
        pin.verifyUnchanged();
        Bytes result(bytes.size());
        std::transform(bytes.begin(), bytes.end(), result.begin(), [](const auto byte) { return std::to_integer<std::uint8_t>(byte); });
        return result;
    };
    services.hashes = hashResourceFile;
    return services;
}

ModServices bindModUpdater(std::shared_ptr<transport::HttpClient> http,
                           std::shared_ptr<tasks::Registry> registry,
                           ModServices services, ModLimits limits) {
    if (!http || !registry || !services.resources.context || !services.resources.acquireMutation ||
        !limits.maximumArchiveBytes || limits.maximumArchiveBytes > 512ULL * 1024 * 1024)
        fail("invalid_input", "Mod updater requires shared HTTP, tasks and instance authority");
    services = bindModServices(std::move(services), limits);
    const auto contextResolver = services.resources.context;
    services.prepareInTask = [http](const DownloadPlan& approved, const ProviderSettings& settings, tasks::Task& operation) {
        const auto& cancel = operation.cancellation();
        cancel.check();
        CachedHttp cache(makeHttpFetch(http, operation.network()), 8 * 1024 * 1024, 1, 1);
        CommunityService provider(cache, settings);
        return provider.prepareDownload(approved.source, ResourceKind::Mod, approved.projectId, approved.versionId, cancel);
    };
    services.replaceInTask = [http, contextResolver, limits](
        const InstanceContext& selected, const std::string& originalName, const std::string& originalSha256,
        const DownloadPlan& plan, std::shared_ptr<void> lease, const std::string& stepId,
        tasks::Task& operation, bool final, const ProgressCallback& progress) {
        const auto& cancel = operation.cancellation();
        cancel.check();
        const auto current = contextResolver(selected.identity);
        if (!lease || selected.updatesDisabled || current.updatesDisabled || !equalContext(selected, current))
            fail("conflict", "Mod update authority changed before download");
        if (!splitModFileName(originalName) || plan.kind != ResourceKind::Mod || checkedHash(originalSha256, 32).empty())
            fail("integrity", "Mod replacement identity is invalid");
        if (stepId.empty() || stepId.size() > 128 || stepId == "." || stepId == ".." ||
            !std::all_of(stepId.begin(), stepId.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            })) fail("invalid_input", "A bounded stable Mod publication step ID is required");
        const auto root = resourceDestination(selected, ResourceKind::Mod);
        tasks::detail::ParentGuard pin(root, false, tasks::detail::GuardMode::NamespaceMutation);
        const auto target = detail::checkedLeaf(root, originalName);
        tasks::FileLease previous;
        {
            detail::FileReader originalPin(target, limits.maximumArchiveBytes);
            previous = tasks::lease_file(target, cancel);
            originalPin.verifyUnchanged();
            if (previous.sha256 != originalSha256) fail("conflict", "Mod changed after its update was identified");
        }
        const auto relative = stepId + ".download";
        ModUpdateReceipt receipt{operation.id(), originalName, target, stepId, {}, final, false};
        const auto fileProgress = operation.snapshot();
        operation.stage("download", originalName);
        operation.files(fileProgress.files_done, fileProgress.files_total);
        downloadStaged(*http, operation.network(), plan, operation.staging() / relative, limits, cancel,
            [&](const auto& value) {
                operation.progress(value);
                if (progress) progress({"download", value.completed_bytes, value.total_bytes.value_or(0)});
            });
        cancel.check();
        const auto beforeCommit = contextResolver(selected.identity);
        if (beforeCommit.updatesDisabled || !equalContext(selected, beforeCommit))
            fail("conflict", "Instance changed before Mod replacement commit");
        operation.stage("commit", originalName);
        // Stage clears totals; restore the operation count before its final success gate.
        operation.files(fileProgress.files_done, fileProgress.files_total);
        // Preserve the early native lease; a fresh lease would miss equal-byte substitution.
        auto committed = operation.commit_file_step(stepId, {relative, target, previous, {}}, final);
        receipt.destination = std::move(committed.published.destination);
        receipt.sha256 = std::move(committed.published.sha256);
        receipt.stepId = std::move(committed.step_id);
        receipt.final = committed.final;
        receipt.durabilityPending = committed.durability_pending;
        return receipt;
    };
    services.replace = [http = std::move(http), registry = std::move(registry), contextResolver, limits](
        const InstanceContext& selected, const std::string& originalName, const std::string& originalSha256,
        const DownloadPlan& plan, std::shared_ptr<void> lease, const Cancellation& cancel, const ProgressCallback& progress) {
        cancel.check();
        const auto current = contextResolver(selected.identity);
        if (!lease || selected.updatesDisabled || current.updatesDisabled || !equalContext(selected, current))
            fail("conflict", "Mod update authority changed before download");
        if (!splitModFileName(originalName) || plan.kind != ResourceKind::Mod || checkedHash(originalSha256, 32).empty())
            fail("integrity", "Mod replacement identity is invalid");
        const auto root = resourceDestination(selected, ResourceKind::Mod);
        tasks::detail::ParentGuard pin(root, false, tasks::detail::GuardMode::NamespaceMutation);
        const auto target = detail::checkedLeaf(root, originalName);
        tasks::FileLease previous;
        {
            detail::FileReader originalPin(target, limits.maximumArchiveBytes);
            previous = tasks::lease_file(target, cancel);
            originalPin.verifyUnchanged();
            if (previous.sha256 != originalSha256) fail("conflict", "Mod changed after its update was identified");
        }
        tasks::TaskSpec specification;
        specification.kind = tasks::TaskKind::Resource;
        specification.target = target;
        specification.label = originalName;
        specification.network = http->snapshot();
        const auto task = registry->begin(specification);
        std::stop_callback stopped(cancel.stop_token(), [task] { task->cancel(); });
        try {
            task->stage("download", originalName);
            const auto staged = task->staging() / "resource.download";
            downloadStaged(*http, task->network(), plan, staged, limits, task->cancellation(), [&](const auto& value) {
                task->progress(value);
                if (progress) progress({"download", value.completed_bytes, value.total_bytes.value_or(0)});
            });
            task->cancellation().check();
            const auto beforeCommit = contextResolver(selected.identity);
            if (beforeCommit.updatesDisabled || !equalContext(selected, beforeCommit))
                fail("conflict", "Instance changed before Mod replacement commit");
            task->stage("commit", originalName);
            // Legacy unregistered callers retain their one terminal per-file transaction.
            task->commit_files({{"resource.download", target, previous, {}}});
            return InstallReceipt{task->id(), originalName, target, false};
        } catch (const ErrorException& failure) {
            task->rollback(failure.error().code, failure.error().message);
            throw;
        } catch (const std::exception&) {
            task->rollback("io", "Mod replacement failed");
            fail("io", "Mod replacement failed");
        }
    };
    return services;
}

} // namespace nickel::resources
