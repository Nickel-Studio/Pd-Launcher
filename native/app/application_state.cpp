#include "app/application_state.hpp"

#include "app/core_adapters.hpp"
#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"
#include "app/service/trusted_launcher.hpp"
#include "core/accounts/http_port.hpp"
#include "core/config/path_guard.hpp"
#include "core/documents/qt_body.hpp"
#include "core/minecraft/services_native.hpp"
#include "core/resources/utility.hpp"
#include "core/tasks/secure_fs.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace nickel::app {
namespace {

AppServicesOptions checked_options(AppServicesOptions options) {
    if (options.executable.empty()) options.executable = options.paths.executable;
    if (options.paths.executable.empty()) options.paths.executable = options.executable;
    if (options.executable.empty() || !options.executable.is_absolute()
        || options.startup_arguments.size() > 128) fail_static("invalid_argument");
    for (const auto& argument : options.startup_arguments)
        if (argument.size() > 32768 || argument.find('\0') != std::string::npos) fail_static("invalid_argument");
    return options;
}

} // namespace

ApplicationState::ApplicationState(AppServicesOptions incoming)
    : options(checked_options(std::move(incoming))), paths(options.paths),
      repository(paths.config_root()), vault(repository.root()),
      budget(std::make_shared<transport::SharedBudget>()),
      http(std::make_shared<transport::HttpClient>(network_configuration(settings()).snapshot, budget)),
      identity_http(accounts::transport_identity_http(*http)),
      accounts(repository, vault), auth(accounts, identity_http), images(identity_http, image_decoder),
      offline_skins(skin_files, skin_archive, identity_http.get()),
      roots(repository.root(), paths.default_game_directory()), onboarding(repository.root(), build_version()),
      tasks(std::make_shared<tasks::Registry>(32, 2048)), cached_http(community_http(http)),
      archive(packs::native_archive_backend()),
      body_reader({}, resources::descriptionImageRules(), resources::externalLinkRules(),
                  documents::qt::normalizeMarkdownNoHtml) {
    repository.recover();
    const auto initial_gate = onboarding.status();
    startup_requires_explicit_finish_ = initial_gate.needs_eula || initial_gate.needs_changelog || initial_gate.exiting;
    budget->configure(network_configuration(settings()).limits);
    resource_services.context = [this](const auto& selected) { return context(selected); };
    resource_services.acquireMutation = [this](const auto& selected, const auto& revision) {
        return acquire_mutation(selected, revision);
    };
    resource_services = resources::bindResourceInstaller(http, tasks, std::move(resource_services));
    resource_services.installPack = [this](const resources::InstallRequest& request,
        const resources::DownloadPlan& plan, const Cancellation& cancellation,
        const resources::ProgressCallback& progress) {
        return install_pack(request, plan, cancellation, progress);
    };
    community = std::make_shared<resources::CommunityService>(cached_http,
        network_configuration(settings()).community, resource_services);
    resources::ModServices mod_services;
    mod_services.resources = resource_services;
    mod_services = resources::bindModServices(std::move(mod_services));
    mod_services = resources::bindModUpdater(http, tasks, std::move(mod_services));
    mods = std::make_unique<resources::ModsService>(std::move(mod_services), community.get());
    roots.ensure_default();
    initialize_cleanup();
    auto trusted = capture_trusted_launcher();
    if (trusted && trusted->matches_application(options.executable)
        && trusted->matches_application(options.paths.executable)) trusted_launcher_ = std::move(trusted);
}

ApplicationState::~ApplicationState() { shutdown(); }

void ApplicationState::shutdown() {
    if (stopping.exchange(true)) return;
    // Collect cancellations under state_mutex, then fire them outside it: callbacks may re-enter
    // this state machine, and holding the lock across a spawn would block or deadlock.
    operations_->close();
    std::vector<Cancellation> cancellation;
    std::vector<Cancellation> launch_cancellation;
    std::vector<Cancellation> operation_cancellation;
    std::vector<std::shared_ptr<DescriptionLease>> released;
    {
        std::lock_guard lock(state_mutex);
        for (const auto& [id, session] : auth_sessions) cancellation.push_back(session->cancellation);
        for (const auto& [id, launch] : launches)
            if (launch->preparing.load()) launch_cancellation.push_back(launch->cancellation);
        for (const auto& [id, operation] : background_operations_) operation_cancellation.push_back(operation->cancellation);
        for (const auto& [id, document] : documents) {
            document->released.store(true);
            released.push_back(document);
        }
        documents.clear();
        document_bytes_ = 0;
    }
    for (auto& value : cancellation) value.cancel();
    for (const auto& value : launch_cancellation) (void)cancel_background(value);
    for (const auto& value : operation_cancellation) (void)cancel_background(value);
    {
        std::lock_guard lock(state_mutex);
        for (const auto& [id, operation] : background_operations_) {
            if (operation->handler_complete || operation->handler_started || !operation->task_id.empty()) continue;
            operation->handler_complete = true;
            operation->error_code = "cancelled";
        }
    }
    auth.cancel_all();
    for (const auto& document : released) {
        document->cancellation.cancel();
        document->resources->cancel();
    }
    for (const auto& task : tasks->snapshots())
        if (task.status == tasks::TaskStatus::Running || task.status == tasks::TaskStatus::Cancelling)
            (void)tasks->cancel(task.id);
}

void ApplicationState::set_sink(StateSink sink) {
    std::lock_guard lock(sink_mutex_);
    sink_ = std::move(sink);
}

void ApplicationState::publish(ui::UiTopic topic, Json value) const {
    if (stopping.load()) return;
    if (topic == ui::UiTopic::Launch) {
        std::lock_guard lock(state_mutex);
        const auto id = value.value("launchId", std::string{});
        const auto found = launches.find(id.empty() ? last_launch_id : id);
        if (found == launches.end()) {
            if (!id.empty()) return;
            value = Json{{"state", "idle"}};
        } else value = found->second->public_state;
        if (launch_publication_ticket_ == std::numeric_limits<std::uint64_t>::max()) fail_static("busy");
        value[launch_delivery_ticket_key] = ++launch_publication_ticket_;
    }
    StateSink sink;
    {
        std::lock_guard lock(sink_mutex_);
        sink = sink_;
    }
    if (sink && !stopping.load()) sink(topic, std::move(value));
}

Json ApplicationState::settings(std::string_view section) const {
    const bool game = section == "game";
    if (!game && section != "app" && !section.empty()) fail_static("invalid_argument");
    Json result = game ? Json::object() : default_app_settings();
    const auto current = repository.read(game ? "game.json" : "settings.json", Json::object());
    if (!current.is_object()) fail_static("storage");
    result.update(current, true);
    return result;
}

std::filesystem::path ApplicationState::registered_root(std::string_view requested) const {
    if (requested.empty() || requested.size() > 32768 || requested.find('\0') != std::string::npos)
        fail_static("invalid_argument");
    const auto key = instances::root_identity(requested);
    const auto registered = roots.get();
    const auto found = std::find_if(registered.roots.begin(), registered.roots.end(), [&](const auto& root) {
        return instances::root_identity(root) == key;
    });
    if (found == registered.roots.end()) fail_static("unauthorized");
    const auto path = instances::path_from_utf8(*found);
    if (!path.is_absolute()) fail_static("unauthorized");
    instances::reject_links(path);
    return path;
}

resources::InstanceIdentity ApplicationState::identity(const Json& scope) const {
    const auto root = registered_root(bounded_text(scope, "root", 32768));
    const auto id = bounded_text(scope, "versionId", 512);
    instances::validate_name(id);
    instances::checked_instance_directory(root, id);
    return {instances::root_identity(instances::path_to_utf8(root)), id};
}

resources::InstanceContext ApplicationState::context(const resources::InstanceIdentity& selected) const {
    if (selected.rootId != instances::root_identity(selected.rootId)) fail_static("unauthorized");
    (void)registered_root(selected.rootId);
    return resources::resolveInstanceContext(roots, selected, settings("game"), running(selected));
}

std::shared_ptr<void> ApplicationState::acquire_mutation(const resources::InstanceIdentity& selected,
                                                        const std::string& revision) {
    const auto root = registered_root(selected.rootId);
    auto lease = std::make_shared<MutationGate::Lease>(
        mutations.instance(instances::root_identity(instances::path_to_utf8(root)), selected.instanceId, Cancellation{}));
    const auto current = context(selected);
    if (current.running || current.revision != revision) fail_static("busy");
    return lease;
}

bool ApplicationState::running(const resources::InstanceIdentity& selected) const {
    struct Snapshot {
        bool busy;
        std::shared_ptr<minecraft::GameSession> game;
        std::filesystem::path resource_root;
    };
    std::vector<Snapshot> snapshot;
    {
        std::lock_guard lock(state_mutex);
        for (const auto& [id, launch] : launches) {
            const auto state = launch->public_state.value("state", std::string{});
            const auto phase = launch->public_state.value("phase", std::string{});
            const bool busy = launch->preparing.load() || launch->draining.load()
                || state == "running" || state == "draining" || state == "launching"
                || phase == "running" || phase == "draining" || phase == "launching";
            if (launch->identity == selected && busy) return true;
            snapshot.push_back({busy, launch->game, launch->resource_root});
        }
    }
    std::optional<std::filesystem::path> selected_resources;
    for (const auto& value : snapshot) {
        if (!value.busy && (!value.game || !value.game->running())) continue;
        if (!selected_resources) {
            const auto root = registered_root(selected.rootId);
            selected_resources = instances::resource_directory(root, selected.instanceId, settings("game"));
        }
        if (tasks::detail::paths_overlap(*selected_resources, value.resource_root)) return true;
    }
    return false;
}

Json ApplicationState::selected_instance() const {
    const auto app = settings();
    const auto found = app.find("selectedInstance");
    if (found == app.end() || found->is_null()) return nullptr;
    if (!found->is_object()) return nullptr;
    try {
        const auto selected = identity(*found);
        const auto root = registered_root(selected.rootId);
        const auto entry = instances::instance_detail(root, selected.instanceId);
        auto result = instance_scope(selected, root);
        result.update({{"displayName", entry.display_name}, {"loader", entry.loader}, {"icon", entry.icon}});
        return result;
    } catch (const ErrorException&) {
        return nullptr;
    }
}

Json ApplicationState::root_snapshot() const {
    const auto registered = roots.get();
    Json rows = Json::array();
    for (const auto& root : registered.roots) {
        std::string label = root;
        const auto found = registered.labels.find(root);
        if (found != registered.labels.end() && found->is_string()) label = found->get<std::string>();
        rows.push_back({{"id", root}, {"dir", root}, {"label", label},
                        {"active", registered.active && instances::root_identity(*registered.active) == instances::root_identity(root)}});
    }
    return {{"items", rows}, {"active", registered.active ? Json(*registered.active) : Json("")}};
}

Json ApplicationState::accounts_snapshot() const {
    const auto app = settings();
    const bool privacy = app.value("privacyMode", false);
    Json rows = Json::array();
    for (const auto& account : accounts.list()) rows.push_back(account_dto(account, privacy));
    Json current_login;
    {
        std::lock_guard lock(state_mutex);
        current_login = login;
    }
    return {{"items", rows}, {"activeId", accounts.active_id()},
            {"accountType", app.value("accountType", std::string("microsoft"))},
            {"offlineUsername", app.value("offlineUsername", std::string("Player"))}, {"login", current_login}};
}

Json ApplicationState::onboarding_snapshot() const {
    const auto status = onboarding.status();
    bool ready;
    StartupPhase phase;
    std::string error;
    {
        std::lock_guard lock(state_mutex);
        ready = startup_ready;
        phase = startup_phase;
        error = startup_error;
    }
    return {{"needsEula", status.needs_eula}, {"needsChangelog", status.needs_changelog},
            {"exiting", status.exiting}, {"eulaVersion", instances::eula_version},
            {"eulaUrl", instances::eula_url}, {"currentVersion", build_version()},
            {"acceptedAt", status.accepted_at ? Json(*status.accepted_at) : Json(nullptr)},
            {"startupReady", ready && !status.needs_eula && !status.needs_changelog && !status.exiting},
            {"startupState", phase == StartupPhase::Completed ? "completed" : phase == StartupPhase::InProgress ? "running" : "pending"},
            {"startupError", error.empty() ? Json(nullptr) : Json{{"code", error}, {"message", safe_public_message(error)}}},
            {"changelog", build_changelog()}};
}

Json ApplicationState::downloads_snapshot() const {
    std::set<std::string> dismissed;
    std::set<std::string> cleared;
    bool bubble;
    std::map<std::string, std::string> operation_ids;
    {
        std::lock_guard lock(state_mutex);
        dismissed = dismissed_downloads;
        cleared = cleared_download_errors;
        bubble = download_bubble_dismissed;
        for (const auto& [id, operation] : background_operations_)
            if (!operation->task_id.empty()) operation_ids.emplace(operation->task_id, id);
    }
    const auto snapshots = tasks->snapshots();
    std::set<std::string> known_tasks;
    for (const auto& snapshot : snapshots) known_tasks.insert(snapshot.id);
    Json rows = pending_operation_snapshot(known_tasks);
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Json& row) {
        return dismissed.contains(row.value("id", std::string{}));
    }), rows.end());
    for (auto& row : rows)
        if (cleared.contains(row.value("id", std::string{}))) { row["error"] = nullptr; row["message"] = ""; }
    for (const auto& snapshot : snapshots) {
        const auto operation = operation_ids.find(snapshot.id);
        if (dismissed.contains(snapshot.id) || (operation != operation_ids.end() && dismissed.contains(operation->second))) continue;
        auto row = task_dto(snapshot);
        if (operation != operation_ids.end()) row["operationId"] = operation->second;
        if (cleared.contains(snapshot.id) || (operation != operation_ids.end() && cleared.contains(operation->second))) {
            row["error"] = nullptr;
            row["message"] = "";
        }
        rows.push_back(std::move(row));
    }
    return {{"items", rows}, {"bubbleDismissed", bubble}};
}

Json ApplicationState::launch_snapshot() const {
    std::lock_guard lock(state_mutex);
    const auto found = launches.find(last_launch_id);
    return found == launches.end() ? Json{{"state", "idle"}} : found->second->public_state;
}

std::shared_ptr<diagnostics::LaunchLogStore> ApplicationState::log_store_for_root(const std::filesystem::path& root) const {
    const auto key = instances::root_identity(instances::path_to_utf8(root));
    std::vector<std::pair<std::string, std::shared_ptr<diagnostics::LaunchLogStore>>> candidates;
    const auto referenced = [&](const std::string& id) {
        return std::any_of(launches.begin(), launches.end(), [&](const auto& item) {
            return instances::root_identity(instances::path_to_utf8(item.second->resource_root)) == id;
        });
    };
    {
        std::lock_guard lock(state_mutex);
        if (const auto found = root_log_stores_.find(key); found != root_log_stores_.end()) return found->second;
        if (root_log_stores_.size() >= 16) for (const auto& [id, logs] : root_log_stores_) {
            if (logs.use_count() == 1 && !referenced(id)) candidates.emplace_back(id, logs);
        }
    }
    for (const auto& [id, logs] : candidates) {
        const auto& log_paths = logs->paths();
        bool absent = true;
        for (const auto& path : {log_paths.logs, log_paths.crash_reports}) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(foundation_detail::native_filesystem_path(path), error);
            if ((error && error != std::errc::no_such_file_or_directory)
                || status.type() != std::filesystem::file_type::not_found) { absent = false; break; }
        }
        if (!absent) continue;
        std::lock_guard lock(state_mutex);
        const auto found = root_log_stores_.find(id);
        if (found != root_log_stores_.end() && found->second == logs && logs.use_count() == 2 && !referenced(id))
            root_log_stores_.erase(found);
    }
    auto logs = std::make_shared<diagnostics::LaunchLogStore>(root);
    {
        std::lock_guard lock(state_mutex);
        if (const auto found = root_log_stores_.find(key); found != root_log_stores_.end()) return found->second;
        // Existing logs require retained secrets; only unreferenced roots with absent log paths can retire.
        if (root_log_stores_.size() >= 16) fail_static("busy");
        root_log_stores_.emplace(key, logs);
    }
    return logs;
}

std::shared_ptr<DescriptionLease> ApplicationState::description(std::string_view key) const {
    std::lock_guard lock(state_mutex);
    const auto found = documents.find(std::string(key));
    if (found == documents.end() || found->second->released.load()) fail_static("not_found");
    return found->second;
}

Json ApplicationState::own_description(documents::SafeBody body) {
    if (!body.valid()) fail_static("integrity");
    auto lease = std::make_shared<DescriptionLease>();
    lease->body = std::move(body);
    lease->resources = std::make_unique<documents::BodyResources>(cached_http, documents::ImageLimits{},
        ui::decodeDescriptionImage);
    lease->resources->setBody(lease->body);
    Json image_rows = Json::array();
    for (const auto& image : lease->body.images())
        image_rows.push_back({{"key", image.key}, {"alt", image.alt}, {"width", image.width}, {"height", image.height}});
    Json link_rows = Json::array();
    for (const auto& link : lease->body.links()) link_rows.push_back({{"key", link.key}, {"label", link.label}});
    const auto key = lease->body.documentId();
    Json result{{"documentKey", key}, {"sanitizedHtml", lease->body.html()},
                {"images", image_rows}, {"links", link_rows}};
    {
        std::lock_guard lock(state_mutex);
        constexpr std::size_t budget_bytes = 16 * 1024 * 1024;
        const auto charge = lease->body.chargedBytes();
        if (stopping.load()) fail_static("cancelled");
        if (documents.size() >= 8 || charge > budget_bytes - document_bytes_) fail_static("busy");
        if (!documents.emplace(key, lease).second) fail_static("internal");
        document_bytes_ += charge;
    }
    return result;
}

void ApplicationState::release_description(std::string_view key) {
    std::shared_ptr<DescriptionLease> lease;
    {
        std::lock_guard lock(state_mutex);
        const auto found = documents.find(std::string(key));
        if (found == documents.end()) return;
        lease = found->second;
        lease->released.store(true);
        document_bytes_ -= lease->body.chargedBytes();
        documents.erase(found);
    }
    lease->cancellation.cancel();
    lease->resources->cancel();
}

std::vector<minecraft::JavaCandidate> ApplicationState::discover_java(const Cancellation& cancellation) const {
    const auto services = minecraft::make_native_services(http);
    if (!services.discoverJava) fail_static("unsupported");
    return services.discoverJava(minecraft::cancellation_check(services, cancellation));
}

std::shared_ptr<LaunchOperation> ApplicationState::owned_launch(const Json& target) const {
    const auto id = operation_id(target, "launchId");
    const auto root = bounded_text(target, "root", 32768);
    const auto version = bounded_text(target, "versionId", 512);
    instances::validate_name(version);
    const resources::InstanceIdentity selected{instances::root_identity(root), version};
    std::lock_guard lock(state_mutex);
    const auto found = launches.find(id);
    if (found == launches.end() || found->second->identity != selected) fail_static("unauthorized");
    return found->second;
}

void ApplicationState::refresh_network(const Json& app_settings) {
    const auto configuration = network_configuration(app_settings);
    http->set_snapshot(configuration.snapshot);
    budget->configure(configuration.limits);
    community->setSettings(configuration.community);
}

} // namespace nickel::app
