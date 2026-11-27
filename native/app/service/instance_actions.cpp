#include "app/application_state.hpp"

#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"
#include "core/instances/icons.hpp"
#include "core/instances/settings.hpp"
#include "core/minecraft/services_native.hpp"
#include "core/tasks/secure_fs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <functional>
#include <initializer_list>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace nickel::app {
namespace {

std::string root_argument(const Json& args) {
    std::optional<std::string> result;
    for (const auto field : {"root", "dir", "oldRoot"}) {
        if (!args.contains(field)) continue;
        const auto text = bounded_text(args, field, 32768);
        if (result && instances::root_identity(*result) != instances::root_identity(text))
            fail_static("invalid_argument");
        if (!result) result = text;
    }
    if (!result) fail_static("invalid_argument");
    return *result;
}

Json instance_arguments(const Json& args) {
    Json scope{{"root", root_argument(args)}, {"versionId", bounded_text(args, "versionId", 512)}};
    return scope;
}

const Json& object_patch(const Json& args) {
    const auto patch = args.find("patch");
    if (patch == args.end() || !patch->is_object() || patch->empty()
        || patch->dump().size() > instances::settings_max_bytes)
        fail_static("invalid_argument");
    return *patch;
}

Json instance_rows(ApplicationState& state, const std::filesystem::path& root) {
    Json rows = Json::array();
    for (const auto& entry : instances::scan_instances(root)) {
        auto row = instances::entry_to_json(entry);
        row["root"] = instances::path_to_utf8(root);
        rows.push_back(std::move(row));
    }
    return {{"root", instances::path_to_utf8(root)}, {"items", std::move(rows)},
            {"selectedInstance", state.selected_instance()}};
}

void clear_selection(ApplicationState& state, const std::filesystem::path& root,
                     const std::optional<std::string>& id) {
    const auto key = instances::root_identity(instances::path_to_utf8(root));
    const auto app = state.settings();
    const auto selected = app.find("selectedInstance");
    if (selected == app.end() || !selected->is_object()) return;
    const auto matches = [&](const Json& value) {
        const auto stored_root = value.find("root");
        const auto stored_id = value.find("versionId");
        return stored_root != value.end() && stored_root->is_string()
            && instances::root_identity(stored_root->get_ref<const std::string&>()) == key
            && (!id || (stored_id != value.end() && stored_id->is_string()
                        && stored_id->get_ref<const std::string&>() == *id));
    };
    if (!matches(*selected)) return;
    // A committed deletion still retires its selection after late request cancellation.
    state.repository.update("settings.json", Json::object(), [&](Json& settings) {
        if (!settings.is_object()) fail_static("storage");
        const auto value = settings.find("selectedInstance");
        if (value != settings.end() && value->is_object() && matches(*value))
            settings["selectedInstance"] = nullptr;
    });
}

void require_idle_root(ApplicationState& state, const std::filesystem::path& root) {
    if (state.tasks->conflicts(root)) fail_static("busy");
    std::vector<resources::InstanceIdentity> selected;
    {
        std::lock_guard lock(state.state_mutex);
        for (const auto& [id, launch] : state.launches) selected.push_back(launch->identity);
    }
    const auto key = instances::root_identity(instances::path_to_utf8(root));
    for (const auto& identity : selected)
        if (identity.rootId == key && state.running(identity)) fail_static("busy");
}

std::shared_ptr<MutationGate::Lease> instance_lease(ApplicationState& state,
        const resources::InstanceIdentity& identity, const Cancellation& cancellation) {
    auto lease = std::make_shared<MutationGate::Lease>(
        state.mutations.instance(identity.rootId, identity.instanceId, cancellation));
    const auto root = state.registered_root(identity.rootId);
    if (state.running(identity)
        || state.tasks->conflicts(instances::checked_instance_directory(root, identity.instanceId)))
        fail_static("busy");
    cancellation.check();
    return lease;
}

struct InstallArguments {
    std::filesystem::path root;
    std::string minecraft_version;
    std::string instance_name;
    std::optional<std::string> loader;
    std::string loader_version;
    std::string operation;
};

std::string optional_text(const Json& args, std::string_view field, std::size_t limit) {
    const auto found = args.find(std::string(field));
    if (found == args.end() || found->is_null()) return {};
    return bounded_text(args, field, limit, false);
}

InstallArguments install_arguments(ApplicationState& state, const Json& args) {
    InstallArguments result;
    result.root = state.registered_root(root_argument(args));
    result.minecraft_version = bounded_text(args,
        args.contains("minecraftVersion") ? "minecraftVersion" : "versionId", 160);
    if (!instances::valid_minecraft_version(result.minecraft_version)) fail_static("invalid_argument");
    result.instance_name = bounded_text(args, "instanceName", 160);
    instances::validate_name(result.instance_name);
    const auto requested = optional_text(args, "loader", 32);
    result.loader_version = optional_text(args, "loaderVersion", 160);
    const auto loader = minecraft::parse_loader_kind(requested);
    if (loader != minecraft::LoaderKind::Vanilla) {
        result.loader = std::string(minecraft::loader_name(loader));
        if (result.loader_version.empty() || result.loader_version == "latest"
            || result.loader_version == "recommended") fail_static("invalid_argument");
    } else if (!result.loader_version.empty()) fail_static("invalid_argument");
    if (args.contains("operationId")) result.operation = operation_id(args, "operationId");
    return result;
}

std::shared_ptr<tasks::Task> begin_install_task(ApplicationState& state,
        const std::filesystem::path& target, tasks::TaskKind kind,
        std::string label, const Cancellation& cancellation) {
    cancellation.check();
    // Task creation mutates this namespace while the accepted operation token is retained.
    tasks::detail::ParentGuard parent(target.parent_path(), true,
                                     tasks::detail::GuardMode::NamespaceMutation);
    tasks::TaskSpec specification;
    specification.kind = kind;
    specification.target = target;
    specification.label = std::move(label);
    specification.network = state.http->snapshot();
    specification.cancellation = cancellation;
    auto task = state.tasks->begin(specification);
    state.register_operation_task(cancellation, task);
    parent.verify();
    return task;
}

minecraft::Services install_services(ApplicationState& state,
                                     const std::shared_ptr<tasks::Task>& task) {
    minecraft::NativeServiceOptions options;
    options.task = task;
    // Snapshot capture and erasure belong to the task-aware factory.
    return minecraft::make_native_services(state.http, options);
}

minecraft::ProgressCallback install_progress(const std::shared_ptr<tasks::Task>& task) {
    return [task](const minecraft::Progress& progress) {
        const auto snapshot = task->snapshot();
        if (snapshot.stage != progress.stage) task->stage(progress.stage);
        task->files(progress.completed, progress.total ? std::optional<std::uint64_t>(progress.total) : std::nullopt);
    };
}

void fail_task(const std::shared_ptr<tasks::Task>& task, const std::exception& failure) {
    const auto* error = dynamic_cast<const ErrorException*>(&failure);
    const auto code = error ? error->error().code : std::string("internal");
    task->rollback(code, safe_public_message(code));
}

void publish_install(ApplicationState& state, const std::filesystem::path& root) noexcept {
    // Notification failures cannot undo a durable domain publication.
    try { state.publish(ui::UiTopic::Downloads, state.downloads_snapshot()); } catch (...) {}
    try { state.publish(ui::UiTopic::Instances, instance_rows(state, root)); } catch (...) {}
}

OperationResult pending_install(ApplicationState& state, const std::filesystem::path& root,
        const std::string& name, const std::string& operation,
        const std::shared_ptr<tasks::Task>& task) {
    OperationResult result;
    result.data = {{"taskId", task->id()}, {"id", name},
                   {"operationId", operation.empty() ? task->id() : operation},
                   {"root", instances::path_to_utf8(root)}, {"versionId", name}, {"pending", true}};
    result.changes.emplace_back(ui::UiTopic::Downloads, state.downloads_snapshot());
    return result;
}

instances::InstallIdentity installed_identity(const instances::InstanceTransaction& transaction,
        const minecraft::InstallRequest& request, const minecraft::InstalledVersion& installed) {
    transaction.check_owned();
    const auto expected = request.root / "versions" / instances::path_from_utf8(request.versionId)
        / instances::path_from_utf8(request.versionId + ".json");
    if (installed.root.lexically_normal() != request.root.lexically_normal()
        || installed.gameDirectory.lexically_normal() != request.gameDirectory.lexically_normal()
        || installed.versionId != request.versionId || installed.minecraftVersion != request.versionId
        || installed.profilePath.lexically_normal() != expected.lexically_normal())
        fail_static("integrity");
    instances::reject_links(expected);
    const auto persisted = instances::parse_legacy_json(nickel::read_text_bounded(expected,
        instances::descriptor_max_bytes));
    if (!persisted.is_object() || persisted != installed.profile
        || persisted.value("id", std::string{}) != request.versionId) fail_static("integrity");
    std::vector<minecraft::LoaderSelection> loaders;
    for (const auto& loader : installed.loaders)
        if (loader.kind != minecraft::LoaderKind::Vanilla) loaders.push_back(loader);
    const auto& profile = transaction.profile();
    if (profile.loader) {
        if (loaders.size() != 1 || minecraft::loader_name(loaders.front().kind) != *profile.loader
            || loaders.front().version != profile.loader_version) fail_static("integrity");
    } else if (!loaders.empty()) fail_static("integrity");
    return {profile.minecraft_version, profile.loader, profile.loader_version, persisted, nullptr};
}

void run_version_install(const std::shared_ptr<ApplicationState>& state, InstallArguments input,
        const std::shared_ptr<instances::InstanceTransaction>& transaction,
        const std::shared_ptr<tasks::Task>& task, const std::shared_ptr<MutationGate::Lease>&,
        const Json& global, const Cancellation&) {
    const auto cancellation = task->cancellation();
    std::optional<minecraft::Installer> installer;
    bool started = false;
    bool drained = true;
    try {
        cancellation.check();
        state->onboarding.require_finished();
        (void)state->registered_root(instances::path_to_utf8(input.root));
        transaction->check_owned();
        minecraft::InstallRequest request;
        // New named instances isolate Minecraft identity from the outer launch directory ID.
        request.root = transaction->directory() / "instances" / instances::path_from_utf8(transaction->profile().name);
        request.gameDirectory = instances::reserved_resource_directory(*transaction, global, cancellation.stop_token());
        request.versionId = input.minecraft_version;
        request.instanceName = input.minecraft_version;
        request.reservationId = transaction->profile().name;
        request.javaCandidates = state->discover_java(cancellation);
        const auto manual = global.find("javaPath");
        if (manual != global.end() && manual->is_string() && !manual->get_ref<const std::string&>().empty())
            request.manualJava = instances::path_from_utf8(manual->get_ref<const std::string&>());
        if (input.loader) request.loaders.push_back({minecraft::parse_loader_kind(*input.loader),
                                                   input.loader_version, std::nullopt});
        installer.emplace(install_services(*state, task));
        started = true;
        drained = false;
        std::optional<minecraft::InstalledVersion> installed;
        std::exception_ptr failure;
        try { installed = installer->install(request, cancellation, install_progress(task)); }
        catch (...) { failure = std::current_exception(); }
        try { drained = installer->drain_owned_installer(std::chrono::seconds(5)); }
        catch (...) { drained = false; }
        if (!drained) {
            transaction->leave_for_recovery(false);
            throw ErrorException(Error{"installer_drain", "Owned installer output did not reach ordered EOF"});
        }
        if (failure) std::rethrow_exception(failure);
        cancellation.check();
        transaction->check_owned();
        const auto installed_profile = installed_identity(*transaction, request, *installed);
        state->onboarding.require_finished();
        (void)state->registered_root(instances::path_to_utf8(input.root));
        cancellation.check();
        transaction->complete(installed_profile);
        cancellation.check();
        task->stage("publishing");
        const Json source{{"origin", "vanilla"}, {"minecraftVersion", input.minecraft_version}};
        task->commit_external([&] {
            state->onboarding.require_finished();
            (void)state->registered_root(instances::path_to_utf8(input.root));
            transaction->commit(input.instance_name, source, {});
        });
    } catch (const std::exception& failure) {
        if (started && !drained) transaction->leave_for_recovery(false);
        else transaction->cleanup();
        fail_task(task, failure);
    } catch (...) {
        if (started && !drained) transaction->leave_for_recovery(false);
        else transaction->cleanup();
        task->rollback("internal", safe_public_message("internal"));
    }
    publish_install(*state, input.root);
}

packs::ImportResult import_pack_graph(ApplicationState& state, const std::filesystem::path& root,
        const std::filesystem::path& source, const std::string& name, const Json& global,
        const std::shared_ptr<tasks::Task>& task, const Cancellation& cancellation) {
    cancellation.check();
    state.onboarding.require_finished();
    (void)state.registered_root(instances::path_to_utf8(root));
    auto plan = packs::inspect_pack(source, name, *state.archive, cancellation.stop_token());
    if (plan.instance_name != name) fail_static("integrity");
    packs::NativeInstallOptions options;
    options.installer_services = install_services(state, task);
    options.installer_progress = install_progress(task);
    options.download_progress = [task](const transport::TransferProgress& progress) { task->progress(progress); };
    options.final_publish = [task](const std::function<void()>& commit) { task->commit_external(commit); };
    options.java_candidates = state.discover_java(cancellation);
    const auto manual = global.find("javaPath");
    if (manual != global.end() && manual->is_string() && !manual->get_ref<const std::string&>().empty())
        options.manual_java = instances::path_from_utf8(manual->get_ref<const std::string&>());
    auto services = packs::native_install_services(state.http, std::move(options));
    services.archive = state.archive;
    return packs::import_pack(std::move(plan), root, services, state.onboarding, cancellation.stop_token());
}

void run_pack_import(const std::shared_ptr<ApplicationState>& state, const std::filesystem::path& root,
        const std::filesystem::path& source, const std::string& name, const Json& global,
        const std::shared_ptr<tasks::Task>& task, const std::shared_ptr<MutationGate::Lease>&,
        const Cancellation&) {
    const auto cancellation = task->cancellation();
    try {
        (void)import_pack_graph(*state, root, source, name, global, task, cancellation);
    } catch (const std::exception& failure) {
        fail_task(task, failure);
    } catch (...) {
        task->rollback("internal", safe_public_message("internal"));
    }
    publish_install(*state, root);
}

} // namespace

resources::InstallReceipt ApplicationState::install_pack(const resources::InstallRequest& request,
        const resources::DownloadPlan& approved_plan, const Cancellation& cancellation,
        const resources::ProgressCallback& progress) {
    static_assert(std::is_nothrow_move_constructible_v<resources::InstallReceipt>);
    cancellation.check();
    if (stopping.load()) fail_static("cancelled");
    if (request.kind != resources::ResourceKind::Modpack || approved_plan.kind != request.kind
        || approved_plan.source != request.source || request.projectId.empty() || approved_plan.projectId.empty()
        || approved_plan.versionId != request.versionId || request.newInstanceName.empty()
        || request.newInstanceName.size() > 160) fail_static("invalid_argument");
    instances::validate_name(request.newInstanceName);
    const auto root = registered_root(request.instance.rootId);
    const auto name = request.newInstanceName;
    const auto root_key = instances::root_identity(instances::path_to_utf8(root));
    auto lease = mutations.instance(root_key, name, cancellation);
    (void)registered_root(root_key);
    const auto target = instances::checked_instance_directory(root, name, false);
    auto task = operation_task(cancellation);
    if (task) {
        const auto snapshot = task->snapshot();
        if (snapshot.target.lexically_normal() != target.lexically_normal()
            || snapshot.status != tasks::TaskStatus::Running) fail_static("integrity");
    }
    if (std::filesystem::exists(target) || (!task && tasks->conflicts(target))
        || running(resources::InstanceIdentity{root_key, name})) fail_static("busy");
    onboarding.require_finished();
    const auto global = settings("game");
    if (!task) task = begin_install_task(*this, target, tasks::TaskKind::Modpack, name, cancellation);
    try {
        resources::InstallReceipt receipt{task->id(), name, target, false};
        task->stage("download");
        const auto staged = task->staging() / "pack.download";
        resources::downloadResourceStaged(*http, task->network(), approved_plan, staged,
            resources::ModLimits{}, task->cancellation(), [&](const transport::TransferProgress& value) {
                task->progress(value);
                if (progress) progress({"download", value.completed_bytes, value.total_bytes.value_or(0)});
            });
        task->cancellation().check();
        (void)registered_root(root_key);
        (void)import_pack_graph(*this, root, staged, name, global, task, task->cancellation());
        publish_install(*this, root);
        return receipt;
    } catch (const std::exception& failure) {
        fail_task(task, failure);
        publish_install(*this, root);
        throw;
    } catch (...) {
        task->rollback("internal", safe_public_message("internal"));
        publish_install(*this, root);
        throw;
    }
}


OperationResult ApplicationState::action_instances(ui::UiAction action, const Json& args,
                                                   const Cancellation& cancellation) {
    cancellation.check();
    if (!args.is_object() || stopping.load())
        fail_static(stopping.load() ? "cancelled" : "invalid_argument");
    OperationResult result;
    switch (action) {
    case ui::UiAction::AddRoot: {
        const auto root = instances::path_from_utf8(root_argument(args));
        if (!root.is_absolute()) fail_static("unauthorized");
        instances::reject_links(root);
        if (!std::filesystem::is_directory(root)) fail_static("not_found");
        auto lease = mutations.root(instances::root_identity(instances::path_to_utf8(root)), cancellation);
        cancellation.check();
        {
            std::lock_guard lock(state_mutex);
            auto registry = roots.get();
            const auto key = instances::root_identity(instances::path_to_utf8(root));
            const auto existing = std::find_if(registry.roots.begin(), registry.roots.end(),
                [&](const auto& registered) { return instances::root_identity(registered) == key; });
            if (existing == registry.roots.end()) {
                registry.roots.push_back(instances::path_to_utf8(root));
                registry.active = registry.roots.back();
            } else registry.active = *existing;
            cancellation.check();
            roots.set(registry);
        }
        result.data = root_snapshot();
        result.changes.emplace_back(ui::UiTopic::Instances, Json{{"roots", result.data}});
        return result;
    }
    case ui::UiAction::CreateRoot: {
        const auto parent = instances::path_from_utf8(bounded_text(args, "parent", 32768));
        if (!parent.is_absolute()) fail_static("unauthorized");
        instances::reject_links(parent);
        if (!std::filesystem::is_directory(parent)) fail_static("not_found");
        const auto target = parent / ".minecraft";
        auto lease = mutations.root(instances::root_identity(instances::path_to_utf8(target)), cancellation);
        cancellation.check();
        std::filesystem::path created;
        {
            std::lock_guard lock(state_mutex);
            created = roots.create_root(parent);
        }
        result.data = {{"root", instances::path_to_utf8(created)}, {"roots", root_snapshot()}};
        result.changes.emplace_back(ui::UiTopic::Instances, Json{{"roots", root_snapshot()}});
        return result;
    }
    case ui::UiAction::ActivateRoot:
    case ui::UiAction::RenameRoot:
    case ui::UiAction::RemoveRoot:
    case ui::UiAction::DeleteRoot: {
        const auto root = registered_root(root_argument(args));
        auto lease = mutations.root(instances::root_identity(instances::path_to_utf8(root)), cancellation);
        (void)registered_root(instances::path_to_utf8(root));
        cancellation.check();
        if (action == ui::UiAction::ActivateRoot) {
            std::lock_guard lock(state_mutex);
            auto registry = roots.get();
            const auto key = instances::root_identity(instances::path_to_utf8(root));
            const auto found = std::find_if(registry.roots.begin(), registry.roots.end(),
                [&](const auto& value) { return instances::root_identity(value) == key; });
            if (found == registry.roots.end()) fail_static("unauthorized");
            registry.active = *found;
            roots.set(registry);
        } else if (action == ui::UiAction::RenameRoot) {
            const auto label = bounded_text(args, args.contains("label") ? "label" : "fullName", 512);
            std::lock_guard lock(state_mutex);
            roots.rename_root(root, label);
        } else {
            require_idle_root(*this, root);
            std::vector<std::filesystem::path> protected_directories{repository.root(), paths.executable_directory()};
            for (const auto& path : {options.paths.system_config_base, options.paths.app_data_base})
                if (!path.empty()) protected_directories.push_back(path);
            {
                std::lock_guard lock(state_mutex);
                roots.remove_root(root, action == ui::UiAction::DeleteRoot, protected_directories);
            }
            clear_selection(*this, root, std::nullopt);
        }
        result.data = root_snapshot();
        result.changes.emplace_back(ui::UiTopic::Instances,
            Json{{"roots", result.data}, {"selectedInstance", selected_instance()}});
        if (action == ui::UiAction::RemoveRoot || action == ui::UiAction::DeleteRoot)
            result.changes.emplace_back(ui::UiTopic::Settings, Json{{"section", "game"}, {"values", settings("game")}});
        return result;
    }
    case ui::UiAction::SelectInstance: {
        const auto selected = identity(instance_arguments(args));
        const auto root = registered_root(selected.rootId);
        auto lease = mutations.instance(selected.rootId, selected.instanceId, cancellation);
        (void)registered_root(selected.rootId);
        const auto entry = instances::instance_detail(root, selected.instanceId);
        if (!entry.has_json || !entry.ready) fail_static("not_found");
        const auto scope = instance_scope(selected, root);
        repository.update("settings.json", Json::object(), [&](Json& settings) {
            if (!settings.is_object()) fail_static("storage");
            settings["selectedInstance"] = scope;
        }, nickel::settings_max_bytes, &cancellation);
        result.data = selected_instance();
        result.changes.emplace_back(ui::UiTopic::Instances,
            Json{{"root", instances::path_to_utf8(root)}, {"selectedInstance", result.data}});
        return result;
    }
    case ui::UiAction::RenameInstance:
    case ui::UiAction::SetInstanceFavorite:
    case ui::UiAction::SetInstanceIcon: {
        const auto selected = identity(instance_arguments(args));
        const auto root = registered_root(selected.rootId);
        Json patch;
        std::optional<std::filesystem::path> selected_file;
        if (action == ui::UiAction::RenameInstance) {
            patch = args.contains("patch") ? object_patch(args)
                : Json{{"name", bounded_text(args, "fullName", 512)}};
            if (patch.contains("icon")) fail_static("invalid_argument");
        } else if (action == ui::UiAction::SetInstanceFavorite) {
            const auto favorite = args.find("favorite");
            if (favorite == args.end() || !favorite->is_boolean()) fail_static("invalid_argument");
            patch = {{"favorite", *favorite}};
        } else if (args.contains("path")) {
            if (args.contains("icon") || args.contains("patch")) fail_static("invalid_argument");
            selected_file = instances::path_from_utf8(bounded_text(args, "path", 32768));
            if (!selected_file->is_absolute()) fail_static("unauthorized");
        } else {
            const auto icon = args.find("icon");
            if (icon == args.end() || (!icon->is_null() && !icon->is_string())) fail_static("invalid_argument");
            patch = {{"icon", *icon}};
        }
        auto lease = instance_lease(*this, selected, cancellation);
        if (selected_file) {
            // The domain pins and hashes the input, then publishes verified PNG before its reference.
            (void)instances::set_instance_icon(root, selected.instanceId, *selected_file,
                                               images, cancellation.stop_token());
        } else instances::patch_instance_metadata(root, selected.instanceId, patch, cancellation.stop_token());
        result = query(ui::UiQuery::VersionDetail, instance_scope(selected, root), Cancellation{});
        result.changes.emplace_back(ui::UiTopic::Instances, instance_rows(*this, root));
        result.changes.emplace_back(ui::UiTopic::Version, result.data);
        return result;
    }
    case ui::UiAction::DeleteInstance: {
        const auto selected = identity(instance_arguments(args));
        const auto root = registered_root(selected.rootId);
        auto lease = instance_lease(*this, selected, cancellation);
        instances::delete_instance(root, selected.instanceId);
        clear_selection(*this, root, selected.instanceId);
        result.data = instance_scope(selected, root);
        result.data["deleted"] = true;
        result.changes.emplace_back(ui::UiTopic::Instances, instance_rows(*this, root));
        result.changes.emplace_back(ui::UiTopic::Version,
            Json{{"root", instances::path_to_utf8(root)}, {"versionId", selected.instanceId}, {"deleted", true}});
        return result;
    }
    case ui::UiAction::PatchVersionSettings:
    case ui::UiAction::ResetVersionSettings: {
        const auto selected = identity(instance_arguments(args));
        const auto root = registered_root(selected.rootId);
        const Json patch = action == ui::UiAction::PatchVersionSettings ? object_patch(args) : Json::object();
        auto lease = instance_lease(*this, selected, cancellation);
        auto current = instances::read_local_settings(root, selected.instanceId).value;
        if (!current.is_object()) current = Json::object();
        if (action == ui::UiAction::ResetVersionSettings) {
            const auto defaults = instances::default_version_settings();
            for (const auto& [key, value] : defaults.items()) current[key] = value;
        } else current.update(patch, true);
        cancellation.check();
        instances::set_version_settings(root, selected.instanceId, current);
        result = query(ui::UiQuery::VersionSettings, instance_scope(selected, root), Cancellation{});
        result.changes.emplace_back(ui::UiTopic::Version, result.data);
        return result;
    }
    case ui::UiAction::InstallVersion: {
        auto input = install_arguments(*this, args);
        onboarding.require_finished();
        const auto state = shared_from_this();
        const auto global = settings("game");
        const auto root_key = instances::root_identity(instances::path_to_utf8(input.root));
        auto lease = std::make_shared<MutationGate::Lease>(mutations.instance(root_key, input.instance_name, cancellation));
        (void)registered_root(root_key);
        const auto target = instances::checked_instance_directory(input.root, input.instance_name, false);
        if (tasks->conflicts(target)) fail_static("busy");
        cancellation.check();
        auto transaction = std::make_shared<instances::InstanceTransaction>(instances::InstanceTransaction::reserve(
            input.root, input.instance_name, input.minecraft_version, input.loader, input.loader_version));
        transaction->check_owned();
        auto task = begin_install_task(*this, transaction->directory(), tasks::TaskKind::Version,
                                      input.instance_name, cancellation);
        try {
            schedule_operation([state, input, transaction, task, lease, global](const Cancellation& stop) {
                run_version_install(state, input, transaction, task, lease, global, stop);
            }, task->cancellation(), task);
        } catch (...) {
            transaction->cleanup();
            task->rollback("busy", safe_public_message("busy"));
            throw;
        }
        return pending_install(*this, input.root, transaction->profile().name, input.operation, task);
    }
    case ui::UiAction::ImportPack: {
        const auto root = registered_root(root_argument(args));
        const auto path = instances::path_from_utf8(bounded_text(args, "path", 32768));
        const auto name = bounded_text(args, "instanceName", 160);
        const auto operation = args.contains("operationId") ? operation_id(args, "operationId") : std::string{};
        instances::validate_name(name);
        if (!path.is_absolute()) fail_static("unauthorized");
        instances::reject_links(path);
        if (!std::filesystem::is_regular_file(path)) fail_static("not_found");
        onboarding.require_finished();
        const auto state = shared_from_this();
        const auto global = settings("game");
        const auto root_key = instances::root_identity(instances::path_to_utf8(root));
        auto lease = std::make_shared<MutationGate::Lease>(mutations.instance(root_key, name, cancellation));
        (void)registered_root(root_key);
        const auto target = instances::checked_instance_directory(root, name, false);
        if (std::filesystem::exists(target) || tasks->conflicts(target)) fail_static("busy");
        auto task = begin_install_task(*this, target, tasks::TaskKind::Modpack, name, cancellation);
        try {
            schedule_operation([state, root, path, name, global, task, lease](const Cancellation& stop) {
                run_pack_import(state, root, path, name, global, task, lease, stop);
            }, task->cancellation(), task);
        } catch (...) {
            task->rollback("busy", safe_public_message("busy"));
            throw;
        }
        return pending_install(*this, root, name, operation, task);
    }
    default:
        fail_static("invalid_argument");
    }
}

} // namespace nickel::app
