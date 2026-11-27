#pragma once

#include "app/AppServices.hpp"
#include "app/service/mutation_gate.hpp"
#include "app/service/operation_queue.hpp"
#include "core/accounts/accounts.hpp"
#include "core/accounts/auth.hpp"
#include "core/config/repository.hpp"
#include "core/config/vault.hpp"
#include "core/diagnostics/diagnostics.hpp"
#include "core/diagnostics/junk.hpp"
#include "core/documents/body.hpp"
#include "core/instances/instances.hpp"
#include "core/instances/startup.hpp"
#include "core/media/images.hpp"
#include "core/media/offline_skin_native.hpp"
#include "core/minecraft/catalog.hpp"
#include "core/minecraft/launcher.hpp"
#include "core/network/http.hpp"
#include "core/packs/native_services.hpp"
#include "core/resources/adapters.hpp"
#include "core/resources/community.hpp"
#include "core/resources/mods.hpp"
#include "core/tasks/tasks.hpp"
#include "ui/DecodeImage.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace nickel::app {

struct CleanupCoordinator;
class TrustedLauncherReceipt;

// Transport-only sequence metadata is removed before conversion to public JSON.
inline constexpr char launch_delivery_ticket_key[] = "_launchDeliveryTicket";

struct OperationResult {
    Json data = Json::object();
    std::vector<std::pair<ui::UiTopic, Json>> changes;
    std::string error_code;
    std::vector<std::pair<std::string, QImage>> media;
};

struct DescriptionLease {
    documents::SafeBody body;
    std::unique_ptr<documents::BodyResources> resources;
    std::atomic<bool> released{false};
    Cancellation cancellation;
};

struct AuthSession {
    std::string operation_id;
    std::string session_id;
    std::uint64_t generation = 0;
    Cancellation cancellation;
    Json public_state = Json::object();
    std::string verified_url;
};

struct BackgroundOperation {
    std::string operation_id;
    std::string task_id;
    Cancellation cancellation;
    std::uint64_t generation = 0;
    bool handler_started = false;
    bool handler_complete = false;
    std::string error_code;
};

struct LaunchOperation {
    LaunchOperation(resources::InstanceIdentity selected, std::string id,
                    std::filesystem::path registered_game_root,
                    std::filesystem::path effective_resource_root)
        : identity(std::move(selected)), launch_id(std::move(id)),
          game_root(std::move(registered_game_root)), resource_root(std::move(effective_resource_root)) {}

    const resources::InstanceIdentity identity;
    const std::string launch_id;
    const std::filesystem::path game_root;
    const std::filesystem::path resource_root;
    Cancellation cancellation;
    std::shared_ptr<minecraft::Launcher> launcher;
    std::shared_ptr<minecraft::GameSession> game;
    std::shared_ptr<diagnostics::OutputSession> output;
    std::shared_ptr<diagnostics::LaunchLogStore> log_store;
    std::shared_ptr<tasks::Task> preparation_task;
    std::shared_ptr<MutationGate::Lease> lease;
    Json public_state = Json::object();
    diagnostics::DiagnosticInput diagnostic_context;
    std::optional<diagnostics::SafeReport> failure_report;
    std::optional<diagnostics::ErrorReportSnapshot> failure_snapshot;
    std::atomic<bool> preparing{true};
    std::atomic<bool> draining{false};
};

// Single service-owned state machine. Public operations run on worker tasks; state_mutex guards
// the maps below and callbacks are always invoked outside it to avoid re-entrant deadlocks.
class ApplicationState final : public std::enable_shared_from_this<ApplicationState> {
public:
    using StateSink = std::function<void(ui::UiTopic, Json)>;
    using BackgroundWork = std::function<void(const Cancellation&)>;
    using ScheduleSink = std::function<bool(std::chrono::milliseconds, BackgroundWork)>;
    explicit ApplicationState(AppServicesOptions options);
    ~ApplicationState();
    void shutdown();
    void set_sink(StateSink sink);
    void publish(ui::UiTopic topic, Json value) const;
    void set_scheduler(ScheduleSink scheduler);
    void set_monitor_scheduler(ScheduleSink scheduler);
    bool schedule(std::chrono::milliseconds delay, BackgroundWork work) const;
    bool schedule_monitor(std::chrono::milliseconds delay, BackgroundWork work) const;
    void schedule_operation(BackgroundWork work, const Cancellation& cancellation,
                            std::shared_ptr<tasks::Task> task = {});
    void poll_microsoft(const std::shared_ptr<AuthSession>& session, const Cancellation& shutdown);
    void poll_game(const std::shared_ptr<LaunchOperation>& launch, const Cancellation& shutdown);

    OperationResult query(ui::UiQuery query, const Json& scope, const Cancellation& cancellation);
    OperationResult action(ui::UiAction action, const Json& args, const Cancellation& cancellation);
    void preflight_cancel(ui::UiAction action, const Json& args);
    [[nodiscard]] std::shared_ptr<BackgroundOperation> begin_operation(const Json& args);
    void start_operation(const std::shared_ptr<BackgroundOperation>& operation);
    void register_operation_task(const Cancellation& cancellation, const std::shared_ptr<tasks::Task>& task);
    [[nodiscard]] std::shared_ptr<tasks::Task> operation_task(const Cancellation& cancellation) const;
    void finish_operation(const std::shared_ptr<BackgroundOperation>& operation, const Json& result = Json::object(),
                          std::string error_code = {});
    [[nodiscard]] bool cancel_operation(std::string_view id);
    [[nodiscard]] Json pending_operation_snapshot(const std::set<std::string>& known_tasks = {}) const;
    [[nodiscard]] bool pending_operations(const Cancellation* excluding = nullptr) const;
    OperationResult action_downloads(ui::UiAction action, const Json& args);
    OperationResult query_community(ui::UiQuery query, const Json& scope, const Cancellation& cancellation);
    OperationResult action_accounts(ui::UiAction action, const Json& args, const Cancellation& cancellation);
    OperationResult action_instances(ui::UiAction action, const Json& args, const Cancellation& cancellation);
    resources::InstallReceipt install_pack(const resources::InstallRequest& request,
        const resources::DownloadPlan& approved_plan, const Cancellation& cancellation,
        const resources::ProgressCallback& progress);
    OperationResult action_resources(ui::UiAction action, const Json& args, const Cancellation& cancellation);
    OperationResult action_startup(ui::UiAction action, const Json& args, const Cancellation& cancellation);
    OperationResult action_launch(ui::UiAction action, const Json& args, const Cancellation& cancellation);
    OperationResult action_diagnostics(ui::UiAction action, const Json& args, const Cancellation& cancellation);
    OperationResult action_cleanup(ui::UiAction action, const Json& args, const Cancellation& cancellation);
    void initialize_cleanup();

    [[nodiscard]] Json settings(std::string_view section = "app") const;
    [[nodiscard]] std::filesystem::path registered_root(std::string_view root) const;
    [[nodiscard]] resources::InstanceIdentity identity(const Json& scope) const;
    [[nodiscard]] resources::InstanceContext context(const resources::InstanceIdentity& selected) const;
    [[nodiscard]] std::shared_ptr<void> acquire_mutation(const resources::InstanceIdentity& selected,
                                                       const std::string& revision);
    [[nodiscard]] bool running(const resources::InstanceIdentity& selected) const;
    [[nodiscard]] Json selected_instance() const;
    [[nodiscard]] Json root_snapshot() const;
    [[nodiscard]] Json accounts_snapshot() const;
    [[nodiscard]] Json onboarding_snapshot() const;
    [[nodiscard]] Json downloads_snapshot() const;
    [[nodiscard]] Json launch_snapshot() const;
    [[nodiscard]] std::shared_ptr<DescriptionLease> description(std::string_view key) const;
    [[nodiscard]] Json own_description(documents::SafeBody body);
    void release_description(std::string_view key);
    void dispatch_startup(const Cancellation& cancellation);
    void refresh_network(const Json& app_settings);
    [[nodiscard]] std::vector<minecraft::JavaCandidate> discover_java(const Cancellation& cancellation) const;
    [[nodiscard]] std::shared_ptr<LaunchOperation> owned_launch(const Json& target) const;
    [[nodiscard]] std::shared_ptr<diagnostics::LaunchLogStore> log_store_for_root(const std::filesystem::path& root) const;
    [[nodiscard]] packs::ExportServices export_services(bool modrinth_only, const Cancellation& cancellation) const;

    AppServicesOptions options;
    PathResolver paths;
    JsonRepository repository;
    Vault vault;
    std::shared_ptr<transport::SharedBudget> budget;
    std::shared_ptr<transport::HttpClient> http;
    std::shared_ptr<accounts::IdentityHttp> identity_http;
    accounts::AccountService accounts;
    accounts::AuthService auth;
    ui::DecodeImage image_decoder;
    media::ImageService images;
    media::NativeSkinFileStore skin_files;
    media::NativeClientSkinArchive skin_archive;
    media::OfflineSkinService offline_skins;
    instances::InstanceRegistry roots;
    instances::OnboardingGate onboarding;
    std::shared_ptr<tasks::Registry> tasks;
    resources::CachedHttp cached_http;
    resources::ResourceServices resource_services;
    std::shared_ptr<resources::CommunityService> community;
    std::unique_ptr<resources::ModsService> mods;
    std::shared_ptr<packs::ArchiveBackend> archive;
    documents::BodyReader body_reader;
    MutationGate mutations;
    std::optional<accounts::MsaConfig> microsoft;

    mutable std::mutex state_mutex;
    Json login = Json::object();
    std::map<std::string, std::shared_ptr<AuthSession>> auth_sessions;
    std::map<std::string, std::shared_ptr<LaunchOperation>> launches;
    std::map<std::string, std::shared_ptr<DescriptionLease>> documents;
    std::map<std::string, resources::Project> projects;
    std::map<std::string, std::vector<resources::ModUpdate>> mod_updates;
    std::set<std::string> dismissed_downloads;
    std::set<std::string> cleared_download_errors;
    bool download_bubble_dismissed = false;
    enum class StartupPhase { Pending, InProgress, Completed };
    StartupPhase startup_phase = StartupPhase::Pending;
    bool startup_launch_claimed = false;
    std::string startup_launch_id;
    std::string startup_error;
    bool startup_ready = false;
    std::string last_launch_id;
    std::atomic<bool> stopping{false};

private:
    friend struct AppServicesTestProbe;
    std::shared_ptr<const TrustedLauncherReceipt> trusted_launcher_;
    // Bootstrap may resume an existing consent record, but must not finish this session's first-use gate.
    bool startup_requires_explicit_finish_ = true;
    bool cancel_background(const Cancellation& cancellation, std::shared_ptr<tasks::Task> task = {}) const;
    std::map<std::string, std::shared_ptr<BackgroundOperation>> background_operations_;
    std::map<std::string, std::weak_ptr<tasks::Task>> operation_tasks_;
    std::uint64_t operation_generation_ = 0;
    mutable std::uint64_t launch_publication_ticket_ = 0;
    mutable std::map<std::string, std::shared_ptr<diagnostics::LaunchLogStore>> root_log_stores_;
    mutable std::mutex sink_mutex_;
    StateSink sink_;
    ScheduleSink scheduler_;
    ScheduleSink monitor_scheduler_;
    std::shared_ptr<OperationQueue> operations_ = std::make_shared<OperationQueue>();
    std::shared_ptr<CleanupCoordinator> cleanup_;
    std::size_t document_bytes_ = 0;
};

} // namespace nickel::app
