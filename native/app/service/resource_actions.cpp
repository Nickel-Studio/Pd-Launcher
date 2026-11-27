#include "app/application_state.hpp"

#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"
#include "core/archive/archive.hpp"
#include "core/minecraft/services_native.hpp"
#include "core/network/control.hpp"
#include "core/network/download.hpp"
#include "core/network/errors.hpp"
#include "core/resources/export.hpp"
#include "core/resources/utility.hpp"
#include "core/tasks/secure_fs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace nickel::app {
namespace {
namespace fs = std::filesystem;
using Identity = resources::InstanceIdentity;
using Context = resources::InstanceContext;
using Path = fs::path;
using Task = tasks::Task;
using Strings = resources::Strings;
constexpr std::uint64_t resource_limit = 512ULL * 1024 * 1024;
constexpr std::uint64_t copy_buffer_bytes = 64 * 1024;

bool flag(const Json& value, std::string_view field, bool fallback = false) {
    const auto found = value.find(std::string(field));
    if (found == value.end()) return fallback;
    if (!found->is_boolean()) fail_static("invalid_argument");
    return found->get<bool>();
}

void reject_download_overrides(const Json& args) {
    if (!args.is_object()) fail_static("invalid_argument");
    for (const auto* field : {"url", "downloadUrl", "downloads", "source_path", "sourcePath",
                              "destination", "launcher", "executable", "artifact", "originalSha256",
                              "hashes", "notices", "opts"})
        if (args.contains(field)) fail_static("unauthorized");
}

Strings text_array(const Json& value, std::string_view field, std::size_t maximum_bytes,
                   bool allow_empty = false) {
    const auto found = value.find(std::string(field));
    if (found == value.end() || !found->is_array() || found->size() > 1024
        || (!allow_empty && found->empty())) fail_static("invalid_argument");
    Strings result;
    std::set<std::string> unique;
    for (const auto& item : *found) {
        if (!item.is_string()) fail_static("invalid_argument");
        const auto text = item.get<std::string>();
        if (text.empty() || text.size() > maximum_bytes || text.find('\0') != std::string::npos
            || !resources::validUtf8(text)
            || !unique.insert(tasks::detail::case_key(text)).second) fail_static("invalid_argument");
        result.push_back(text);
    }
    return result;
}

void require_launcher_materials(const packs::ExportServices& services) {
    if (!services.archive || !services.launcher || !services.validate_launcher_artifact
        || services.launcher_notices.empty()) {
        throw ErrorException(Error{"missing_service", "Trusted launcher artifact and embedded license/source material are unavailable"});
    }
}

std::string update_key(const Identity& selected) {
    return Json::array({selected.rootId, selected.instanceId}).dump();
}

struct Authority {
    Context frozen;
    Path root;
    Path instance;
    tasks::detail::FileIdentity root_identity;
    tasks::detail::FileIdentity instance_identity;
    tasks::detail::FileIdentity resource_identity;
    std::shared_ptr<void> mutation;
};

Authority authorize(ApplicationState& state, const Json& args, const Cancellation& cancel,
                    bool mutation) {
    cancel.check();
    const auto selected = state.identity(args);
    Authority result;
    result.root = state.registered_root(selected.rootId);
    result.instance = instances::checked_instance_directory(result.root, selected.instanceId);
    result.frozen = state.context(selected);
    if (result.frozen.identity != selected || result.frozen.revision.empty()) fail_static("unauthorized");
    if (const auto revision = args.find("revision"); revision != args.end()) {
        if (!revision->is_string() || revision->get<std::string>() != result.frozen.revision)
            fail_static("conflict");
    }
    result.root_identity = tasks::detail::directory_identity(result.root);
    result.instance_identity = tasks::detail::directory_identity(result.instance);
    result.resource_identity = tasks::detail::directory_identity(result.frozen.resourceRoot);
    if (mutation) result.mutation = state.acquire_mutation(selected, result.frozen.revision);
    cancel.check();
    return result;
}

void verify_authority(ApplicationState& state, const Authority& expected,
                      const Cancellation& cancel, bool idle = true) {
    cancel.check();
    const auto current = state.context(expected.frozen.identity);
    if (state.registered_root(expected.frozen.identity.rootId) != expected.root
        || tasks::detail::directory_identity(expected.root) != expected.root_identity
        || tasks::detail::directory_identity(expected.instance) != expected.instance_identity
        || tasks::detail::directory_identity(expected.frozen.resourceRoot) != expected.resource_identity
        || current.identity != expected.frozen.identity || current.revision != expected.frozen.revision
        || current.resourceRoot != expected.frozen.resourceRoot || current.modsRoot != expected.frozen.modsRoot
        || current.resourcepacksRoot != expected.frozen.resourcepacksRoot
        || current.shaderpacksRoot != expected.frozen.shaderpacksRoot
        || current.datapacksRoot != expected.frozen.datapacksRoot
        || (idle && current.running)) fail_static("conflict");
}

Path resource_directory(const Context& context, resources::ResourceKind kind) {
    Path result;
    switch (kind) {
    case resources::ResourceKind::Mod: result = context.modsRoot; break;
    case resources::ResourceKind::Resourcepack: result = context.resourcepacksRoot; break;
    case resources::ResourceKind::Shader: result = context.shaderpacksRoot; break;
    case resources::ResourceKind::Datapack: result = context.datapacksRoot; break;
    case resources::ResourceKind::Modpack: fail_static("invalid_argument");
    }
    result = tasks::detail::checked_absolute(result);
    const auto relative = result.lexically_relative(context.resourceRoot);
    if (relative.empty() || relative == "." || relative.is_absolute()
        || !resources::validArchiveRelativePath(resources::utf8Path(relative))) fail_static("unauthorized");
    tasks::detail::reject_links(result);
    return result;
}

Path save_path(ApplicationState& state, const Json& args) {
    const auto selected = resources::pathFromUtf8(bounded_text(args, "path", 32768));
    if (!selected.is_absolute()) fail_static("unauthorized");
    const auto target = tasks::detail::checked_absolute(selected);
    tasks::detail::ParentGuard parent(target.parent_path());
    tasks::detail::reject_links(target);
    if (fs::exists(target)) fail_static("conflict");
    if (tasks::detail::paths_overlap(target, state.repository.root())
        || tasks::detail::paths_overlap(target, state.options.executable)) fail_static("unauthorized");
    for (const auto& root : state.roots.get().roots)
        if (tasks::detail::paths_overlap(target, state.registered_root(root))) fail_static("unauthorized");
    parent.verify();
    return target;
}

std::vector<resources::ModEntry> selected_mods(ApplicationState& state, const Identity& selected,
                                               const Strings& ids, const Cancellation& cancel) {
    const auto available = state.mods->list(selected, cancel);
    std::vector<resources::ModEntry> result;
    for (const auto& id : ids) {
        if (!resources::splitModFileName(id)) fail_static("invalid_argument");
        const auto found = std::find_if(available.begin(), available.end(), [&](const auto& mod) {
            return mod.fileName == id;
        });
        if (found == available.end()) fail_static("not_found");
        result.push_back(*found);
    }
    return result;
}

void clear_updates(ApplicationState& state, const Identity& selected) noexcept {
    try {
        std::lock_guard lock(state.state_mutex);
        state.mod_updates.erase(update_key(selected));
    } catch (...) { }
}

Json scoped_snapshot(ApplicationState& state, const Authority& authority, ui::UiQuery query,
                     const Cancellation& cancel) {
    verify_authority(state, authority, cancel, false);
    const auto scope = instance_scope(authority.frozen.identity, authority.root);
    return state.query(query, scope, cancel).data;
}

void publish_resources(ApplicationState& state, const Authority& authority, bool include_mods) noexcept {
    // Construct cancellation inside the guard: even diagnostic setup can allocate after commit.
    if (include_mods) {
        try { state.publish(ui::UiTopic::Mods, scoped_snapshot(state, authority, ui::UiQuery::Mods, Cancellation{})); }
        catch (...) { }
    }
    try {
        state.publish(ui::UiTopic::Resources,
                      scoped_snapshot(state, authority, ui::UiQuery::InstalledResources, Cancellation{}));
    } catch (...) { }
}

struct CopyReceipt {
    std::uint64_t bytes = 0;
    std::string sha256;
};

CopyReceipt copy_pinned(ApplicationState& state, const tasks::FileLease& expected,
                        const Path& destination, const Cancellation& cancel,
                        std::optional<tasks::detail::FileRevision> expected_revision = {}) {
    return transport::detail::boundary("io", "Resource snapshot could not be copied", [&] {
        cancel.check();
        if (expected.size > resource_limit) fail_static("too_large");
        const auto reservation = state.budget->reserve_buffer(copy_buffer_bytes, [&] { return cancel.cancelled(); });
        tasks::detail::ParentGuard source_parent(expected.destination.parent_path());
        tasks::detail::ParentGuard target_parent(destination.parent_path());
        auto input = tasks::detail::SecureFile::open_read(expected.destination);
        if (input.identity() != expected.identity || input.size() != expected.size) fail_static("conflict");
        const auto revision = input.revision();
        if (expected_revision && revision != *expected_revision) fail_static("conflict");
        auto output = tasks::detail::SecureFile::create_new(destination);
        transport::detail::IncrementalHash hash(transport::HashAlgorithm::Sha256);
        std::array<std::byte, copy_buffer_bytes> buffer{};
        std::uint64_t copied = 0;
        for (;;) {
            cancel.check();
            const auto count = input.read(buffer);
            if (!count) break;
            if (count > expected.size - copied) fail_static("conflict");
            const auto bytes = std::span<const std::byte>(buffer).first(count);
            hash.update(bytes);
            output.write(bytes);
            copied += count;
        }
        const auto digest = hash.finish();
        if (copied != expected.size || input.size() != expected.size || input.revision() != revision
            || !transport::detail::digest_equal(expected.sha256, digest)) fail_static("conflict");
        source_parent.verify();
        target_parent.verify();
        cancel.check();
        output.flush();
        return CopyReceipt{copied, digest};
    });
}

using TaskWork = std::function<void(ApplicationState&, const std::shared_ptr<Task>&)>;

OperationResult queue_task(ApplicationState& state, tasks::TaskSpec specification,
                           const Json& args, const Cancellation& cancel, TaskWork work,
                           std::shared_ptr<void> lifetime = {}) {
    cancel.check();
    specification.network = state.http->snapshot();
    specification.cancellation = cancel;
    const auto task = state.tasks->begin(specification);
    try {
        state.register_operation_task(cancel, task);
        task->stage("queued");
        const auto owner = state.shared_from_this();
        state.schedule_operation([owner, task, lifetime = std::move(lifetime), work = std::move(work)]
                                 (const Cancellation&) {
            try {
                task->cancellation().check();
                if (owner->stopping.load()) fail_static("cancelled");
                work(*owner, task);
            } catch (const ErrorException& failure) {
                task->rollback(failure.error().code, safe_public_message(failure.error().code));
            } catch (const std::exception&) {
                task->rollback("io", safe_public_message("io"));
            }
            try { owner->publish(ui::UiTopic::Downloads, owner->downloads_snapshot()); }
            catch (...) { }
        }, task->cancellation(), task);
    } catch (const ErrorException& failure) {
        task->rollback(failure.error().code, safe_public_message(failure.error().code));
        throw;
    } catch (const std::exception&) {
        task->rollback("busy", safe_public_message("busy"));
        fail_static("busy");
    }
    OperationResult result;
    result.data = {{"queued", true}, {"taskId", task->id()}};
    if (args.contains("operationId")) result.data["operationId"] = operation_id(args, "operationId");
    result.changes.emplace_back(ui::UiTopic::Downloads, state.downloads_snapshot());
    return result;
}

tasks::TaskSpec resource_task(const Authority& authority, std::string label,
                              tasks::TaskKind kind = tasks::TaskKind::Resource) {
    tasks::TaskSpec result;
    result.kind = kind;
    result.target = authority.frozen.resourceRoot;
    result.label = std::move(label);
    return result;
}

tasks::TaskSpec nonpublication_task(ApplicationState& state, std::string label) {
    tasks::TaskSpec result;
    result.kind = tasks::TaskKind::Resource;
    result.target = state.repository.root() / resources::pathFromUtf8(".resource-operation-" + tasks::detail::unique_id());
    result.label = std::move(label);
    return result;
}

resources::ResourceKind inspect_local_resource(ApplicationState& state, const Path& source,
                                                const Cancellation& cancel) {
    const packs::ArchiveLimits limits{10000, resource_limit, resource_limit};
    const auto entries = state.archive->inspect(source, limits, cancel.stop_token());
    struct Release {
        packs::ArchiveBackend& archive;
        Path path;
        ~Release() { archive.release(path); }
    } release{*state.archive, source};
    packs::validate_archive_entries(entries, limits);
    if (entries.empty()) fail_static("integrity");
    bool mod = false, shader = false, data = false, assets = false, metadata = false;
    for (const auto& entry : entries) {
        cancel.check();
        if (entry.directory) continue;
        if (entry.name == "modrinth.index.json" || entry.name == "manifest.json") fail_static("invalid_argument");
        mod = mod || entry.name == "fabric.mod.json" || entry.name == "quilt.mod.json"
            || entry.name == "mcmod.info" || entry.name == "META-INF/mods.toml"
            || entry.name == "META-INF/neoforge.mods.toml";
        shader = shader || entry.name.starts_with("shaders/");
        data = data || entry.name.starts_with("data/");
        assets = assets || entry.name.starts_with("assets/");
        metadata = metadata || entry.name == "pack.mcmeta";
    }
    const auto extension = resources::lower(resources::utf8Path(source.extension()));
    if (extension == ".jar" || extension == ".litemod" || mod) return resources::ResourceKind::Mod;
    if (extension != ".zip" || (shader && (data || assets)) || (data && assets)) fail_static("invalid_argument");
    if (shader) return resources::ResourceKind::Shader;
    if (!metadata) fail_static("invalid_argument");
    const auto text = state.archive->read(source, "pack.mcmeta", 256 * 1024, cancel.stop_token());
    const auto document = Json::parse(text, nullptr, false);
    if (!document.is_object() || !document.contains("pack") || !document["pack"].is_object())
        fail_static("integrity");
    return data ? resources::ResourceKind::Datapack : resources::ResourceKind::Resourcepack;
}

std::pair<resources::ResourceKind, std::string> resource_token(std::string_view token) {
    const auto colon = token.find(':');
    if (colon == std::string_view::npos) fail_static("invalid_argument");
    const auto kind = resources::parseKind(token.substr(0, colon));
    const std::string name(token.substr(colon + 1));
    if (kind == resources::ResourceKind::Mod || kind == resources::ResourceKind::Modpack
        || !resources::validResourceFileName(name)) fail_static("invalid_argument");
    return {kind, name};
}

bool pinned_file_matches(const Path& file, const resources::VersionFile& expected,
                         const Cancellation& cancel) {
    const auto hashes = resources::hashResourceFile(file, cancel, resource_limit);
    return hashes.sizeBytes == expected.sizeBytes
        && (!expected.sha1.empty() || !expected.sha512.empty())
        && (expected.sha1.empty() || transport::detail::digest_equal(expected.sha1, hashes.sha1))
        && (expected.sha512.empty() || transport::detail::digest_equal(expected.sha512, hashes.sha512));
}

} // namespace

OperationResult ApplicationState::action_resources(ui::UiAction action, const Json& args,
                                                    const Cancellation& cancellation) {
    cancellation.check();
    if (stopping.load()) fail_static("cancelled");
    reject_download_overrides(args);
    if (args.contains("operationId")) (void)operation_id(args, "operationId");
    onboarding.require_finished();

    switch (action) {
    case ui::UiAction::InstallProject: {
        const auto source = selected_source(args);
        const auto kind = selected_kind(args);
        const auto project = bounded_text(args, "projectId", 128);
        const auto version = bounded_text(args, "projectVersionId", 128);
        if (!resources::validIdentifier(source, project) || !resources::validIdentifier(source, version))
            fail_static("invalid_argument");
        if (kind == resources::ResourceKind::Modpack) {
            const auto root_argument = bounded_text(args, "root", 32768);
            if (!resources::pathFromUtf8(root_argument).is_absolute()) fail_static("unauthorized");
            const auto root = registered_root(root_argument);
            const auto root_identity = tasks::detail::directory_identity(root);
            std::optional<Authority> origin;
            if (!bounded_text(args, "versionId", 512, false).empty())
                origin = authorize(*this, args, cancellation, false);
            const auto name = bounded_text(args, "instanceName", 512);
            instances::validate_name(name);
            const auto target = instances::checked_instance_directory(root, name, false);
            if (!resource_services.installPack) fail_static("missing_service");
            resources::InstallRequest request;
            request.instance = {instances::root_identity(resources::utf8Path(root)), name};
            request.source = source;
            request.kind = kind;
            request.projectId = project;
            request.versionId = version;
            request.newInstanceName = name;
            if (fs::exists(target) || tasks->conflicts(target) || running(request.instance)) fail_static("busy");
            tasks::detail::FileIdentity versions_identity;
            {
                tasks::detail::ParentGuard root_pin(root, false, tasks::detail::GuardMode::NamespaceMutation);
                if (tasks::detail::directory_identity(root) != root_identity) fail_static("conflict");
                tasks::detail::ParentGuard parent(target.parent_path(), true, tasks::detail::GuardMode::NamespaceMutation);
                parent.verify();
                root_pin.verify();
                versions_identity = tasks::detail::directory_identity(target.parent_path());
            }
            tasks::TaskSpec specification;
            specification.target = target;
            specification.kind = tasks::TaskKind::Modpack;
            specification.label = name;
            return queue_task(*this, specification, args, cancellation,
                [root, root_identity, versions_identity, target, origin, request](ApplicationState& state, const auto& task) {
                    const auto& cancel = task->cancellation();
                    if (state.registered_root(request.instance.rootId) != root
                        || tasks::detail::directory_identity(root) != root_identity
                        || tasks::detail::directory_identity(target.parent_path()) != versions_identity)
                        fail_static("conflict");
                    if (origin) verify_authority(state, *origin, cancel, false);
                    task->stage("resolve");
                    const auto plan = state.community->prepareDownload(request.source, request.kind,
                        request.projectId, request.versionId, cancel);
                    if (origin) verify_authority(state, *origin, cancel, false);
                    if (state.registered_root(request.instance.rootId) != root
                        || tasks::detail::directory_identity(root) != root_identity
                        || tasks::detail::directory_identity(target.parent_path()) != versions_identity)
                        fail_static("conflict");
                    // The instance bridge reuses this registered Task and owns its irreversible FinalPublish.
                    const auto receipt = state.resource_services.installPack(request, plan, cancel, {});
                    if (receipt.taskId != task->id() || receipt.destination.lexically_normal() != target.lexically_normal())
                        fail_static("integrity");
                });
        }
        const auto authority = authorize(*this, args, cancellation, true);
        return queue_task(*this, resource_task(authority, project), args, cancellation,
            [authority, source, kind, project, version](ApplicationState& state, const auto& task) {
                const auto& cancel = task->cancellation();
                verify_authority(state, authority, cancel);
                task->stage("resolve");
                const auto plan = state.community->prepareDownload(source, kind, project, version, cancel);
                const auto directory = resource_directory(authority.frozen, kind);
                const auto target = directory / resources::pathFromUtf8(plan.file.fileName);
                if (fs::exists(target)) {
                    resources::detail::checkedLeaf(directory, plan.file.fileName, false);
                    if (!pinned_file_matches(target, plan.file, cancel)) fail_static("conflict");
                    verify_authority(state, authority, cancel);
                    task->stage("verified_existing");
                    task->complete_work();
                } else {
                    task->stage("download", plan.file.fileName);
                    resources::downloadResourceStaged(*state.http, task->network(), plan,
                        task->staging() / "resource.download", {}, cancel,
                        [task](const auto& progress) { task->progress(progress); });
                    verify_authority(state, authority, cancel);
                    task->stage("commit", plan.file.fileName);
                    task->commit_files({{"resource.download", target}});
                }
                clear_updates(state, authority.frozen.identity);
                publish_resources(state, authority, kind == resources::ResourceKind::Mod);
            }, authority.mutation);
    }
    case ui::UiAction::DownloadProjectFile: {
        const auto destination = save_path(*this, args);
        std::optional<Authority> origin;
        const auto root = bounded_text(args, "root", 32768, false);
        const auto instance = bounded_text(args, "versionId", 512, false);
        if (!instance.empty() && root.empty()) fail_static("unauthorized");
        if (!root.empty()) {
            (void)registered_root(root);
            if (!instance.empty()) origin = authorize(*this, args, cancellation, false);
        }
        tasks::TaskSpec specification;
        specification.target = destination;
        specification.kind = tasks::TaskKind::Resource;
        specification.label = resources::utf8Path(destination.filename());
        const auto kind_text = bounded_text(args, "kind", 32);
        if (kind_text == "loaderInstaller") {
            const auto game_version = bounded_text(args, "minecraftVersion", 128);
            const auto loader = bounded_text(args, "loader", 32);
            const auto loader_version = bounded_text(args, "loaderVersion", 128);
            if (!instances::valid_minecraft_version(game_version)) fail_static("invalid_argument");
            specification.kind = tasks::TaskKind::Installer;
            auto result = queue_task(*this, specification, args, cancellation,
                [destination, origin, game_version, loader, loader_version](ApplicationState& state, const auto& task) {
                    const auto& cancel = task->cancellation();
                    if (origin) verify_authority(state, *origin, cancel, false);
                    minecraft::NativeServiceOptions options;
                    options.task = task;
                    options.parallelRanges = 1;
                    const auto services = minecraft::make_native_services(state.http, options);
                    minecraft::Catalog catalog(services);
                    minecraft::LoaderRequest request;
                    request.kind = minecraft::parse_loader_kind(loader);
                    request.version = loader_version;
                    const auto check = minecraft::cancellation_check(services, cancel);
                    task->stage("resolve");
                    const auto selected = catalog.resolve(request, game_version, check);
                    if (selected.artifact.hashes.empty() || selected.artifact.url.empty()
                        || !resources::validResourceFileName(selected.artifact.filename)) fail_static("integrity");
                    minecraft::DownloadFile file;
                    file.destination = task->staging() / "loader.jar";
                    file.url = selected.artifact.url;
                    file.hashes = selected.artifact.hashes;
                    file.size = selected.artifact.size;
                    file.maximumBytes = resource_limit;
                    file.kind = minecraft::FileKind::Loader;
                    task->stage("download");
                    services.download(file, check, {});
                    archive::Archive::validate_executable_jar(file.destination, {"META-INF/MANIFEST.MF"}, cancel);
                    if (origin) verify_authority(state, *origin, cancel, false);
                    task->stage("commit");
                    task->commit_files({{"loader.jar", destination}});
                });
            result.data["path"] = resources::utf8Path(destination);
            return result;
        }
        const auto source = selected_source(args);
        const auto kind = selected_kind(args);
        const auto project = bounded_text(args, "projectId", 128);
        const auto version = bounded_text(args, "projectVersionId", 128);
        if (!resources::validIdentifier(source, project) || !resources::validIdentifier(source, version))
            fail_static("invalid_argument");
        auto result = queue_task(*this, specification, args, cancellation,
            [destination, origin, source, kind, project, version](ApplicationState& state, const auto& task) {
                const auto& cancel = task->cancellation();
                if (origin) verify_authority(state, *origin, cancel, false);
                task->stage("resolve");
                const auto plan = state.community->prepareDownload(source, kind, project, version, cancel);
                task->stage("download", plan.file.fileName);
                resources::downloadResourceStaged(*state.http, task->network(), plan,
                    task->staging() / "resource.download", {}, cancel,
                    [task](const auto& progress) { task->progress(progress); });
                if (origin) verify_authority(state, *origin, cancel, false);
                task->stage("commit", plan.file.fileName);
                task->commit_files({{"resource.download", destination}});
            });
        result.data["path"] = resources::utf8Path(destination);
        return result;
    }
    case ui::UiAction::ImportResources: {
        const auto authority = authorize(*this, args, cancellation, true);
        const auto input_paths = text_array(args, "paths", 32768);
        struct LocalInput {
            tasks::FileLease file;
            tasks::detail::FileRevision revision;
        };
        std::vector<LocalInput> inputs;
        for (const auto& text : input_paths) {
            cancellation.check();
            const auto selected = resources::pathFromUtf8(text);
            if (!selected.is_absolute()) fail_static("unauthorized");
            const auto path = resources::detail::checkedRegularFile(selected);
            auto input = tasks::detail::SecureFile::open_read(path);
            const auto identity = input.identity();
            const auto revision = input.revision();
            const auto size = input.size();
            if (!size || size > resource_limit) fail_static("too_large");
            const auto name = resources::utf8Path(path.filename());
            if (!resources::validResourceFileName(name)) fail_static("invalid_argument");
            auto file = tasks::lease_file(path, cancellation);
            if (file.identity != identity || file.size != size || input.size() != size
                || input.revision() != revision) fail_static("conflict");
            inputs.push_back({std::move(file), revision});
        }
        return queue_task(*this, resource_task(authority, "Import resources"), args, cancellation,
            [authority, inputs](ApplicationState& state, const auto& task) {
                const auto& cancel = task->cancellation();
                std::vector<tasks::PublishFile> publications;
                std::set<std::string> destinations;
                bool mods_changed = false;
                task->stage("validate");
                for (std::size_t index = 0; index < inputs.size(); ++index) {
                    verify_authority(state, authority, cancel);
                    const auto& source = inputs[index];
                    const auto& path = source.file.destination;
                    const auto staged_name = "import-" + std::to_string(index) + resources::utf8Path(path.extension());
                    const auto staged = task->staging() / resources::pathFromUtf8(staged_name);
                    copy_pinned(state, source.file, staged, cancel, source.revision);
                    const auto kind = inspect_local_resource(state, staged, cancel);
                    const auto name = resources::utf8Path(path.filename());
                    if (kind == resources::ResourceKind::Mod && !resources::splitModFileName(name))
                        fail_static("invalid_argument");
                    const auto target = resource_directory(authority.frozen, kind) / path.filename();
                    if (fs::exists(target) || !destinations.insert(tasks::detail::case_key(resources::utf8Path(target))).second)
                        fail_static("conflict");
                    publications.push_back({staged_name, target});
                    task->files(index + 1, inputs.size());
                    mods_changed = mods_changed || kind == resources::ResourceKind::Mod;
                }
                verify_authority(state, authority, cancel);
                task->stage("commit");
                task->commit_files(publications);
                clear_updates(state, authority.frozen.identity);
                publish_resources(state, authority, mods_changed);
            }, authority.mutation);
    }
    case ui::UiAction::DeleteResource: {
        const auto authority = authorize(*this, args, cancellation, true);
        const auto ids = selected_ids(args);
        std::vector<tasks::detail::RegularFileToken> tokens;
        for (const auto& id : ids) {
            const auto [kind, name] = resource_token(id);
            const auto path = resources::detail::checkedLeaf(resource_directory(authority.frozen, kind), name, false);
            tokens.push_back(tasks::detail::capture_regular_file(authority.frozen.resourceRoot, path,
                [&] { return cancellation.cancelled(); }));
        }
        const auto commit_gate = transport::detail::cancellation_gate(cancellation);
        Strings removed;
        try {
            for (std::size_t index = 0; index < tokens.size(); ++index) {
                std::lock_guard commit(*commit_gate);
                verify_authority(*this, authority, cancellation);
                transport::detail::boundary("io", "Selected resource could not be safely removed", [&] {
                    tasks::detail::remove_regular_file(tokens[index], [&] { return cancellation.cancelled(); });
                });
                removed.push_back(ids[index]);
            }
        } catch (const ErrorException& failure) {
            if (!removed.empty()) {
                publish_resources(*this, authority, false);
                throw ErrorException(Error{"partial_failure", "Some selected resources were removed before the operation stopped",
                    {{"deletedIds", removed}, {"failureCode", failure.error().code}, {"partial", true}}});
            }
            throw;
        }
        OperationResult result;
        result.data = {{"deletedIds", removed}, {"partial", false}};
        result.changes.emplace_back(ui::UiTopic::Resources,
            scoped_snapshot(*this, authority, ui::UiQuery::InstalledResources, Cancellation{}));
        return result;
    }
    case ui::UiAction::SetModEnabled:
    case ui::UiAction::DeleteMods: {
        const auto authority = authorize(*this, args, cancellation, false);
        const auto ids = selected_ids(args);
        selected_mods(*this, authority.frozen.identity, ids, cancellation);
        verify_authority(*this, authority, cancellation);
        OperationResult result;
        if (action == ui::UiAction::SetModEnabled) {
            if (!args.contains("enabled")) fail_static("invalid_argument");
            result.data = {{"ids", mods->setEnabled(authority.frozen.identity, ids, flag(args, "enabled"), cancellation)}};
        } else {
            result.data = {{"deleted", mods->remove(authority.frozen.identity, ids, cancellation)}};
        }
        clear_updates(*this, authority.frozen.identity);
        result.changes.emplace_back(ui::UiTopic::Mods,
            scoped_snapshot(*this, authority, ui::UiQuery::Mods, Cancellation{}));
        return result;
    }
    case ui::UiAction::RenameMod: {
        const auto authority = authorize(*this, args, cancellation, true);
        const auto id = bounded_text(args, "id", 512);
        const auto name = bounded_text(args, "name", 512);
        const auto selected = selected_mods(*this, authority.frozen.identity, {id}, cancellation).front();
        const auto original = resources::splitModFileName(id);
        const auto desired = resources::splitModFileName(name);
        if (!original || !desired || original->first == desired->first
            || resources::lower(resources::utf8Path(resources::pathFromUtf8(original->first).extension()))
                != resources::lower(resources::utf8Path(resources::pathFromUtf8(desired->first).extension())))
            fail_static("invalid_argument");
        std::vector<std::pair<tasks::FileLease, Path>> changes;
        for (const auto& physical : selected.physicalFiles) {
            const auto split = resources::splitModFileName(physical);
            if (!split || resources::lower(split->first) != resources::lower(original->first)) fail_static("conflict");
            const auto target_name = desired->first + (split->second == resources::ModState::Disabled ? ".disabled"
                : split->second == resources::ModState::Old ? ".old" : "");
            resources::detail::checkedMissingLeaf(authority.frozen.modsRoot, target_name);
            const auto source = resources::detail::checkedLeaf(authority.frozen.modsRoot, physical);
            changes.emplace_back(tasks::lease_file(source, cancellation),
                authority.frozen.modsRoot / resources::pathFromUtf8(target_name));
        }
        if (changes.empty() || changes.size() > 3) fail_static("conflict");
        return queue_task(*this, resource_task(authority, "Rename Mod"), args, cancellation,
            [authority, changes](ApplicationState& state, const auto& task) {
                const auto& cancel = task->cancellation();
                std::vector<tasks::PublishFile> publications;
                task->stage("snapshot");
                for (std::size_t index = 0; index < changes.size(); ++index) {
                    verify_authority(state, authority, cancel);
                    const auto staged = "rename-" + std::to_string(index);
                    copy_pinned(state, changes[index].first, task->staging() / resources::pathFromUtf8(staged), cancel);
                    publications.push_back({staged, changes[index].second, changes[index].first, {}});
                }
                verify_authority(state, authority, cancel);
                task->stage("commit");
                // Keep the original FileLease: a same-byte inode replacement must still fail.
                task->commit_files(publications);
                clear_updates(state, authority.frozen.identity);
                publish_resources(state, authority, true);
            }, authority.mutation);
    }
    case ui::UiAction::CheckModUpdates: {
        const auto authority = authorize(*this, args, cancellation, false);
        if (authority.frozen.updatesDisabled) fail_static("policy_denied");
        auto ids = text_array(args, "ids", 512, true);
        if (ids.empty()) {
            for (const auto& mod : mods->list(authority.frozen.identity, cancellation)) ids.push_back(mod.fileName);
            if (ids.size() > 1024) fail_static("too_large");
        }
        if (ids.empty()) fail_static("not_found");
        selected_mods(*this, authority.frozen.identity, ids, cancellation);
        return queue_task(*this, nonpublication_task(*this, "Check Mod updates"), args, cancellation,
            [authority, ids](ApplicationState& state, const auto& task) {
                const auto& cancel = task->cancellation();
                verify_authority(state, authority, cancel, false);
                task->stage("identify");
                const auto updates = state.mods->checkUpdates(authority.frozen.identity, ids, cancel);
                verify_authority(state, authority, cancel, false);
                task->files(updates.size(), ids.size());
                task->complete_work();
                {
                    std::lock_guard lock(state.state_mutex);
                    if (state.mod_updates.size() >= 64 && !state.mod_updates.contains(update_key(authority.frozen.identity)))
                        state.mod_updates.erase(state.mod_updates.begin());
                    state.mod_updates[update_key(authority.frozen.identity)] = updates;
                }
                auto snapshot = scoped_snapshot(state, authority, ui::UiQuery::Mods, Cancellation{});
                for (auto& row : snapshot["items"]) {
                    const auto found = std::find_if(updates.begin(), updates.end(), [&](const auto& update) {
                        return update.fileName == row.value("id", std::string{});
                    });
                    if (found == updates.end()) continue;
                    row["identified"] = found->identified;
                    row["updateAvailable"] = bool(found->available);
                    if (found->available) {
                        row["updateVersionId"] = found->available->versionId;
                        row["updateFileName"] = found->available->file.fileName;
                    }
                }
                state.publish(ui::UiTopic::Mods, std::move(snapshot));
            });
    }
    case ui::UiAction::UpdateMods: {
        const auto authority = authorize(*this, args, cancellation, false);
        const auto ids = selected_ids(args);
        selected_mods(*this, authority.frozen.identity, ids, cancellation);
        if (authority.frozen.running || authority.frozen.updatesDisabled) fail_static("policy_denied");
        std::vector<resources::ModUpdate> confirmed;
        {
            std::lock_guard lock(state_mutex);
            const auto found = mod_updates.find(update_key(authority.frozen.identity));
            if (found == mod_updates.end()) fail_static("conflict");
            for (const auto& id : ids) {
                const auto update = std::find_if(found->second.begin(), found->second.end(), [&](const auto& value) {
                    return value.fileName == id;
                });
                if (update == found->second.end() || update->instance != authority.frozen.identity
                    || update->instanceRevision != authority.frozen.revision || !update->available
                    || resources::checkedHash(update->originalSha256, 32).empty()) fail_static("conflict");
                confirmed.push_back(*update);
            }
        }
        auto result = queue_task(*this, resource_task(authority, "Update Mods"), args, cancellation,
            [authority, confirmed](ApplicationState& state, const auto& task) {
                verify_authority(state, authority, task->cancellation());
                task->stage("update");
                task->files(0, confirmed.size());
                state.mods->updateConfirmed(authority.frozen.identity, confirmed, *task,
                    [task, total = confirmed.size()](const resources::Progress& value) {
                        // The domain owns byte progress and the terminal gate, including pending/failure.
                        if (value.stage == "committed") task->files(value.completedBytes, total);
                    });
                // Do not copy receipts, check late cancellation or complete an already-terminal Task.
                clear_updates(state, authority.frozen.identity);
                publish_resources(state, authority, true);
            });
        result.data["perFileCommit"] = true;
        return result;
    }
    case ui::UiAction::ExportMods: {
        const auto authority = authorize(*this, args, cancellation, true);
        const auto ids = selected_ids(args);
        const auto selected = selected_mods(*this, authority.frozen.identity, ids, cancellation);
        const auto destination = save_path(*this, args);
        std::vector<tasks::FileLease> files;
        std::uint64_t bytes = 0;
        for (const auto& mod : selected) {
            const auto file = tasks::lease_file(resources::detail::checkedLeaf(authority.frozen.modsRoot, mod.fileName), cancellation);
            if (file.size > resource_limit || file.size > packs::max_pack_bytes - bytes) fail_static("too_large");
            bytes += file.size;
            files.push_back(file);
        }
        tasks::TaskSpec specification;
        specification.target = destination;
        specification.additional_targets = {authority.frozen.resourceRoot};
        specification.kind = tasks::TaskKind::Export;
        specification.label = "Export Mods";
        auto result = queue_task(*this, specification, args, cancellation,
            [authority, destination, files](ApplicationState& state, const auto& task) {
                const auto& cancel = task->cancellation();
                std::vector<packs::ArchiveInput> inputs;
                task->stage("snapshot");
                for (std::size_t index = 0; index < files.size(); ++index) {
                    verify_authority(state, authority, cancel);
                    const auto staged = task->staging() / resources::pathFromUtf8("mod-" + std::to_string(index));
                    copy_pinned(state, files[index], staged, cancel);
                    inputs.push_back({resources::utf8Path(files[index].destination.filename()), staged, {}});
                    task->files(index + 1, files.size());
                }
                const auto zip = task->staging() / "mods.zip";
                task->stage("archive");
                state.archive->write(zip, inputs, {}, cancel.stop_token());
                archive::Archive::validate(zip, {}, cancel);
                state.archive->release(zip);
                verify_authority(state, authority, cancel);
                task->stage("commit");
                task->commit_files({{"mods.zip", destination}});
            }, authority.mutation);
        result.data["path"] = resources::utf8Path(destination);
        return result;
    }
    case ui::UiAction::ExportPack: {
        const auto authority = authorize(*this, args, cancellation, true);
        const auto destination = save_path(*this, args);
        packs::ExportRequest request;
        request.game_root = authority.root;
        request.instance_id = authority.frozen.identity.instanceId;
        request.name = bounded_text(args, "name", 512);
        request.version = bounded_text(args, "version", 512);
        request.selections = text_array(args, "files", 4096, true);
        const auto format = bounded_text(args, "format", 32);
        if (format != "mrpack" && format != "portable") fail_static("invalid_argument");
        request.format = format == "portable" ? packs::ExportFormat::portable : packs::ExportFormat::mrpack;
        request.include_launcher = flag(args, "includeLauncher");
        request.include_personalization = flag(args, "includePersonalization");
        request.bundle_resources = flag(args, "bundleResources", request.format == packs::ExportFormat::portable);
        request.modrinth_only = flag(args, "modrinthOnly");
        if (request.format == packs::ExportFormat::mrpack && (request.include_launcher || request.include_personalization))
            fail_static("invalid_argument");
        if (request.include_launcher) {
            // Admission checks availability only; do not retain its receipt for queued work.
            require_launcher_materials(export_services(request.modrinth_only, cancellation));
        }
        const auto global = settings("game");
        const auto appearance = settings();
        tasks::TaskSpec specification;
        specification.target = destination;
        specification.additional_targets = {authority.frozen.resourceRoot};
        specification.kind = tasks::TaskKind::Export;
        specification.label = "Export pack";
        auto result = queue_task(*this, specification, args, cancellation,
            [authority, destination, request, global, appearance](ApplicationState& state, const auto& task) {
                const auto& cancel = task->cancellation();
                verify_authority(state, authority, cancel);
                task->stage("validate");
                auto services = state.export_services(request.modrinth_only, cancel);
                if (request.include_launcher) require_launcher_materials(services);
                const auto preview = packs::preview_export(authority.root, request.instance_id, global, cancel.stop_token());
                for (const auto& token : request.selections)
                    if (std::none_of(preview.files.begin(), preview.files.end(), [&](const auto& file) { return file.path == token; }))
                        fail_static("unauthorized");
                if (request.include_personalization) {
                    services.personalization = appearance;
                    if (const auto background = appearance.find("backgroundImage"); background != appearance.end()
                        && background->is_string() && !background->get_ref<const std::string&>().empty())
                        services.background = resources::detail::checkedRegularFile(
                            resources::pathFromUtf8(background->get<std::string>()));
                }
                const auto name = request.format == packs::ExportFormat::portable ? "pack.zip" : "pack.mrpack";
                const auto staged = task->staging() / name;
                task->stage("export");
                packs::export_pack(request, staged, global, services, state.onboarding, cancel.stop_token());
                services.archive->release(staged);
                verify_authority(state, authority, cancel);
                task->stage("commit");
                task->commit_files({{name, destination}});
            }, authority.mutation);
        result.data["path"] = resources::utf8Path(destination);
        return result;
    }
    default:
        fail_static("invalid_argument");
    }
}

} // namespace nickel::app
