#include "app/application_state.hpp"

#include "app/settings.hpp"
#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"

namespace nickel::app {

OperationResult ApplicationState::action(ui::UiAction action, const Json& args,
                                         const Cancellation& cancellation) {
    cancellation.check();
    if (stopping.load()) fail_static("cancelled");
    if (action == ui::UiAction::LaunchGame) {
        onboarding.require_finished();
        std::lock_guard lock(state_mutex);
        if (!startup_ready || startup_phase != StartupPhase::Completed) fail_static("onboarding_required");
    }
    switch (action) {
    case ui::UiAction::SetAccountType:
    case ui::UiAction::SetActiveAccount:
    case ui::UiAction::RemoveAccount:
    case ui::UiAction::BeginMicrosoftLogin:
    case ui::UiAction::CancelMicrosoftLogin:
    case ui::UiAction::LoginSkinServer:
    case ui::UiAction::SelectCharacter:
    case ui::UiAction::CancelCharacterSelection:
    case ui::UiAction::RefreshCharacters:
    case ui::UiAction::UpdateOfflineSkin:
    case ui::UiAction::ResetOfflineSkin:
    case ui::UiAction::UploadSkin:
    case ui::UiAction::ChangePlayerName:
        return action_accounts(action, args, cancellation);
    case ui::UiAction::ActivateRoot:
    case ui::UiAction::AddRoot:
    case ui::UiAction::CreateRoot:
    case ui::UiAction::RenameRoot:
    case ui::UiAction::RemoveRoot:
    case ui::UiAction::DeleteRoot:
    case ui::UiAction::SelectInstance:
    case ui::UiAction::RenameInstance:
    case ui::UiAction::SetInstanceIcon:
    case ui::UiAction::SetInstanceFavorite:
    case ui::UiAction::DeleteInstance:
    case ui::UiAction::PatchVersionSettings:
    case ui::UiAction::ResetVersionSettings:
    case ui::UiAction::InstallVersion:
    case ui::UiAction::ImportPack:
        return action_instances(action, args, cancellation);
    case ui::UiAction::InstallProject:
    case ui::UiAction::DownloadProjectFile:
    case ui::UiAction::ImportResources:
    case ui::UiAction::DeleteResource:
    case ui::UiAction::SetModEnabled:
    case ui::UiAction::RenameMod:
    case ui::UiAction::DeleteMods:
    case ui::UiAction::CheckModUpdates:
    case ui::UiAction::UpdateMods:
    case ui::UiAction::ExportMods:
    case ui::UiAction::ExportPack:
        return action_resources(action, args, cancellation);
    case ui::UiAction::PatchSettings: {
        auto section = bounded_text(args, "section", 16, false);
        if (section.empty()) section = "app";
        if (section != "app" && section != "game") fail_static("invalid_argument");
        const auto patch = args.find("patch");
        if (patch == args.end() || !patch->is_object()) fail_static("invalid_argument");
        if (section == "game") {
            auto proposed = settings("game");
            merge_settings_fields(proposed, *patch);
            instances::resolve_settings(proposed, nullptr);
        } else {
            if (patch->contains("selectedInstance") || patch->contains("accountType")) fail_static("unauthorized");
            if (const auto ssl = patch->find("verifyMicrosoftSsl"); ssl != patch->end()
                && (!ssl->is_boolean() || !ssl->get<bool>())) fail_static("unauthorized");
            if (const auto server = patch->find("littleskinServer"); server != patch->end()) {
                if (!server->is_string()) fail_static("invalid_argument");
                accounts::validate_server(server->get<std::string>());
            }
        }
        const auto updated = apply_settings_patch(repository, section == "game" ? "game.json" : "settings.json",
                                                  *patch, budget, cancellation);
        if (section == "app") refresh_network(settings());
        OperationResult result;
        result.data = {{"section", section}, {"values", updated}};
        result.changes.emplace_back(ui::UiTopic::Settings, result.data);
        if (section == "app") result.changes.emplace_back(ui::UiTopic::Accounts, accounts_snapshot());
        return result;
    }
    case ui::UiAction::LaunchGame:
    case ui::UiAction::CancelLaunch:
        return action_launch(action, args, cancellation);
    case ui::UiAction::RefreshDownloads:
    case ui::UiAction::CancelDownload:
    case ui::UiAction::DismissDownload:
    case ui::UiAction::DismissDownloadBubble:
    case ui::UiAction::ClearDownloadError:
        return action_downloads(action, args);
    case ui::UiAction::OpenPath:
    case ui::UiAction::OpenExternalUrl:
    case ui::UiAction::ExportLogs:
    case ui::UiAction::ExportErrorReport:
    case ui::UiAction::ScanJunk:
    case ui::UiAction::CleanJunk:
    case ui::UiAction::ClearLaunchError:
        return action_diagnostics(action, args, cancellation);
    case ui::UiAction::AcceptEula:
    case ui::UiAction::AcknowledgeChangelog:
    case ui::UiAction::FinishOnboarding:
    case ui::UiAction::RejectEula:
        return action_startup(action, args, cancellation);
    }
    fail_static("invalid_argument");
}

} // namespace nickel::app
