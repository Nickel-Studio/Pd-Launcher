#include "app/application_state.hpp"

#include "app/settings.hpp"
#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"
#include "core/instances/domain_io.hpp"

#include <QUuid>
#include <algorithm>

namespace nickel::app {

OperationResult ApplicationState::action_startup(ui::UiAction action, const Json& args,
                                                const Cancellation& cancellation) {
    cancellation.check();
    try {
        switch (action) {
        case ui::UiAction::AcceptEula: {
            const auto checked = args.find("checked");
            if (checked == args.end() || !checked->is_boolean()) fail_static("invalid_argument");
            onboarding.accept_eula(checked->get<bool>(), bounded_text(args, "eulaVersion", 128),
                                   unix_milliseconds() / 1000);
            break;
        }
        case ui::UiAction::AcknowledgeChangelog:
            onboarding.acknowledge_changelog(bounded_text(args, "version", 128));
            break;
        case ui::UiAction::FinishOnboarding:
            if (!onboarding.finish([&] { dispatch_startup(cancellation); })) dispatch_startup(cancellation);
            break;
        case ui::UiAction::RejectEula:
            onboarding.reject();
            break;
        default:
            fail_static("invalid_argument");
        }
    } catch (const ErrorException& error) {
        // Consent rejection remains an onboarding refusal, not an unknown internal failure.
        if (error.error().code == "exiting") fail_static("onboarding_required");
        throw;
    }
    OperationResult result;
    result.data = onboarding_snapshot();
    result.changes.emplace_back(ui::UiTopic::Onboarding, result.data);
    return result;
}

void ApplicationState::dispatch_startup(const Cancellation& cancellation) {
    onboarding.require_finished();
    cancellation.check();
    {
        std::lock_guard lock(state_mutex);
        if (startup_phase == StartupPhase::Completed) return;
        if (startup_phase == StartupPhase::InProgress) fail_static("busy");
        startup_phase = StartupPhase::InProgress;
        startup_error.clear();
    }
    try {
        auto requested = instances::parse_startup_arguments(options.startup_arguments);
        const auto executable_root = paths.executable_directory();
        const auto portable_status = packs::portable_bootstrap_status(executable_root);
        if (portable_status) {
            const auto root_key = instances::path_to_utf8(portable_status->game_directory);
            auto authority = mutations.root(instances::root_identity(root_key), cancellation);
            std::optional<instances::StartupLaunchRequest> portable_launch;
            if (portable_status->state == "installed" && portable_status->instance_id) {
                portable_launch = instances::StartupLaunchRequest{portable_status->game_directory, *portable_status->instance_id};
            } else {
                auto services = packs::native_install_services(http);
                const auto imported = packs::start_portable_bootstrap(executable_root, services, onboarding,
                                                                     cancellation.stop_token());
                if (!imported) fail_static("integrity");
                portable_launch = instances::StartupLaunchRequest{portable_status->game_directory, imported->instance_id};
            }
            cancellation.check();
            auto registered = roots.get();
            if (std::none_of(registered.roots.begin(), registered.roots.end(), [&](const auto& root) {
                return instances::root_identity(root) == instances::root_identity(root_key);
            })) {
                registered.roots.push_back(root_key);
                registered.active = root_key;
                roots.set(registered);
            }
            if (!requested) requested = std::move(portable_launch);
            publish(ui::UiTopic::Instances, {{"roots", root_snapshot()}, {"selectedInstance", selected_instance()}});
        }
        if (requested) {
            const auto root = registered_root(instances::path_to_utf8(requested->game_root));
            requested->game_root = root;
            instances::resolve_startup_launch(*requested, settings("game"), onboarding);
            const resources::InstanceIdentity selected{instances::root_identity(instances::path_to_utf8(root)), requested->instance_id};
            auto scope = instance_scope(selected, root);
            apply_settings_patch(repository, "settings.json", {{"selectedInstance", scope}}, budget, cancellation);
            publish(ui::UiTopic::Instances, {{"selectedInstance", selected_instance()}});
            cancellation.check();
            {
                std::lock_guard lock(state_mutex);
                if (startup_launch_id.empty())
                    startup_launch_id = "startup_" + QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8().toStdString();
                scope["launchId"] = startup_launch_id;
            }
            const auto accepted = begin_operation(scope);
            try {
                start_operation(accepted);
                const auto outcome = action_launch(ui::UiAction::LaunchGame, scope, accepted->cancellation);
                finish_operation(accepted, outcome.data, outcome.error_code);
                for (const auto& [topic, snapshot] : outcome.changes) publish(topic, snapshot);
            } catch (const std::exception& error) {
                finish_operation(accepted, Json::object(), public_error(error).code.toStdString());
                publish(ui::UiTopic::Downloads, downloads_snapshot());
                throw;
            }
            publish(ui::UiTopic::Downloads, downloads_snapshot());
        }
        {
            std::lock_guard lock(state_mutex);
            startup_launch_claimed = !startup_launch_id.empty() && launches.contains(startup_launch_id);
            startup_phase = StartupPhase::Completed;
            startup_ready = true;
        }
    } catch (const std::exception& error) {
        const auto code = public_error(error).code.toUtf8().toStdString();
        {
            std::lock_guard lock(state_mutex);
            startup_launch_claimed = !startup_launch_id.empty() && launches.contains(startup_launch_id);
            // The portable journal resumes imports; an owned launch record must never be spawned twice.
            startup_phase = startup_launch_claimed ? StartupPhase::Completed : StartupPhase::Pending;
            startup_ready = startup_launch_claimed;
            startup_error = code;
        }
        publish(ui::UiTopic::Onboarding, onboarding_snapshot());
        throw;
    }
    publish(ui::UiTopic::Onboarding, onboarding_snapshot());
}

} // namespace nickel::app
