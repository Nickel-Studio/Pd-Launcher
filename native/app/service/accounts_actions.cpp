#include "app/application_state.hpp"

#include "app/settings.hpp"
#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"
#include "core/instances/domain_io.hpp"
#include "platform/services.hpp"

#include <algorithm>
#include <stop_token>

namespace nickel::app {
namespace {

Json profile_list(const accounts::ProfileList& profiles) {
    Json rows = Json::array();
    for (const auto& profile : profiles.profiles) rows.push_back({{"id", profile.id}, {"name", profile.name}});
    return {{"sessionId", profiles.session_id}, {"state", "select"}, {"profiles", rows}};
}

std::shared_ptr<AuthSession> store_profiles(ApplicationState& state, const accounts::ProfileList& profiles) {
    auto session = std::make_shared<AuthSession>();
    session->session_id = profiles.session_id;
    session->generation = profiles.generation;
    session->public_state = profile_list(profiles);
    std::lock_guard lock(state.state_mutex);
    if (state.auth_sessions.size() >= 32) fail_static("busy");
    state.auth_sessions.emplace(session->session_id, session);
    state.login = session->public_state;
    return session;
}

void forget_session(ApplicationState& state, const std::shared_ptr<AuthSession>& session, Json outcome) {
    std::lock_guard lock(state.state_mutex);
    const auto key = session->operation_id.empty() ? session->session_id : session->operation_id;
    const auto found = state.auth_sessions.find(key);
    if (found == state.auth_sessions.end() || found->second != session) return;
    session->public_state = outcome;
    state.login = std::move(outcome);
    state.auth_sessions.erase(found);
}

} // namespace

OperationResult ApplicationState::action_accounts(ui::UiAction action, const Json& args,
                                                  const Cancellation& cancellation) {
    cancellation.check();
    OperationResult result;
    switch (action) {
    case ui::UiAction::SetAccountType: {
        const auto type = bounded_text(args, "type", 32);
        const auto parsed = accounts::parse_account_type(type);
        if (parsed == accounts::AccountType::yggdrasil) fail_static("invalid_argument");
        result.data = apply_settings_patch(repository, "settings.json", {{"accountType", type}}, budget, cancellation);
        result.changes.emplace_back(ui::UiTopic::Settings, result.data);
        break;
    }
    case ui::UiAction::SetActiveAccount: {
        const auto id = bounded_text(args, "id", 128);
        const auto account = accounts.find(id);
        if (!account) fail_static("not_found");
        if (args.contains("type") && accounts::parse_account_type(bounded_text(args, "type", 32)) != account->type)
            fail_static("invalid_argument");
        Json committed;
        try {
            accounts.set_active(id, [&] {
                auto current = repository.inspect("settings.json", Json::object(), settings_max_bytes);
                if (current.state != JsonReadResult::State::Missing && current.state != JsonReadResult::State::Valid)
                    fail_static("storage");
                // Reads may fall back for display, but a sibling write must preserve damaged original data.
                committed = std::move(current.value);
                merge_settings_fields(committed, {{"accountType", accounts::account_type_name(account->type)}});
                repository.write("settings.json", committed, settings_max_bytes);
            }, cancellation);
        } catch (const accounts::IdentityError& error) {
            if (error.code() == accounts::IdentityErrorCode::storage) fail_static("storage");
            throw;
        }
        refresh_network(settings());
        result.data = {{"section", "app"}, {"values", committed}};
        result.changes.emplace_back(ui::UiTopic::Settings, result.data);
        break;
    }
    case ui::UiAction::RemoveAccount: {
        const auto id = bounded_text(args, "id", 128);
        if (!accounts.find(id)) fail_static("not_found");
        auth.cancel_account_operations(id);
        images.invalidate(id);
        cancellation.check();
        accounts.remove(id);
        break;
    }
    case ui::UiAction::BeginMicrosoftLogin: {
        onboarding.require_finished();
        if (!microsoft) fail_static("unsupported");
        auto session = std::make_shared<AuthSession>();
        session->operation_id = operation_id(args, "operationId");
        session->public_state = {{"operationId", session->operation_id}, {"state", "requesting"}};
        {
            std::lock_guard lock(state_mutex);
            if (auth_sessions.size() >= 32 || auth_sessions.contains(session->operation_id)) fail_static("busy");
            auth_sessions.emplace(session->operation_id, session);
            login = session->public_state;
        }
        try {
            std::stop_callback request_cancel(cancellation.stop_token(),
                [token = session->cancellation]() mutable { token.cancel(); });
            const auto started = auth.device_start(*microsoft, session->cancellation);
            session->cancellation.check();
            {
                std::lock_guard lock(state_mutex);
                const auto found = auth_sessions.find(session->operation_id);
                if (found == auth_sessions.end() || found->second != session) fail_static("cancelled");
                session->session_id = started.session_id;
                session->generation = started.generation;
                session->verified_url = started.verification_uri;
                session->public_state = {{"operationId", session->operation_id}, {"sessionId", started.session_id},
                    {"code", started.user_code}, {"verificationUrl", session->verified_url}, {"state", "waiting"}};
                login = session->public_state;
                result.data = session->public_state;
            }
            const auto weak = weak_from_this();
            if (!schedule(std::chrono::duration_cast<std::chrono::milliseconds>(started.interval),
                [weak, session](const Cancellation& shutdown) {
                    if (const auto state = weak.lock()) state->poll_microsoft(session, shutdown);
                })) {
                auth.device_cancel(started.session_id, started.generation);
                fail_static("busy");
            }
        } catch (...) {
            session->cancellation.cancel();
            if (!session->session_id.empty()) auth.device_cancel(session->session_id, session->generation);
            forget_session(*this, session, {{"operationId", session->operation_id}, {"state", "error"},
                                            {"message", safe_public_message("authentication_required")}});
            publish(ui::UiTopic::Accounts, accounts_snapshot());
            throw;
        }
        result.changes.emplace_back(ui::UiTopic::Accounts, accounts_snapshot());
        return result;
    }
    case ui::UiAction::CancelMicrosoftLogin: {
        const auto id = operation_id(args, "operationId");
        std::shared_ptr<AuthSession> session;
        {
            std::lock_guard lock(state_mutex);
            const auto found = auth_sessions.find(id);
            if (found == auth_sessions.end()) fail_static("not_found");
            session = found->second;
            if (args.contains("sessionId") && !bounded_text(args, "sessionId", 128, false).empty()
                && bounded_text(args, "sessionId", 128) != session->session_id) fail_static("unauthorized");
        }
        session->cancellation.cancel();
        if (!session->session_id.empty()) auth.device_cancel(session->session_id, session->generation);
        forget_session(*this, session, {{"operationId", id}, {"state", "cancelled"}});
        break;
    }
    case ui::UiAction::LoginSkinServer: {
        onboarding.require_finished();
        const auto type = args.contains("type") ? accounts::parse_account_type(bounded_text(args, "type", 32))
            : accounts::AccountType::littleskin;
        if (type != accounts::AccountType::littleskin && type != accounts::AccountType::yggdrasil) fail_static("invalid_argument");
        auto server = bounded_text(args, "server", 4096, false);
        if (server.empty()) server = settings().value("littleskinServer", std::string("https://littleskin.cn"));
        const auto outcome = auth.skin_login(server, bounded_text(args, "username", 1024),
            accounts::Secret(bounded_text(args, "password", 4096)), type, cancellation);
        if (outcome.selection) result.data = store_profiles(*this, *outcome.selection)->public_state;
        else if (outcome.account) result.data = account_dto(*outcome.account, settings().value("privacyMode", false));
        else fail_static("integrity");
        break;
    }
    case ui::UiAction::SelectCharacter:
    case ui::UiAction::CancelCharacterSelection: {
        const auto id = bounded_text(args, "sessionId", 128);
        std::shared_ptr<AuthSession> session;
        {
            std::lock_guard lock(state_mutex);
            const auto found = auth_sessions.find(id);
            if (found == auth_sessions.end() || !found->second->operation_id.empty()) fail_static("not_found");
            session = found->second;
        }
        if (action == ui::UiAction::SelectCharacter) {
            std::stop_callback cancelled(session->cancellation.stop_token(),
                [token = cancellation]() mutable { token.cancel(); });
            const auto selected = auth.skin_select(session->session_id, session->generation,
                bounded_text(args, "profileId", 128), cancellation);
            images.invalidate(selected.id);
            result.data = account_dto(selected, settings().value("privacyMode", false));
            forget_session(*this, session, {{"state", "success"}});
        } else {
            session->cancellation.cancel();
            auth.skin_cancel(session->session_id, session->generation, cancellation);
            forget_session(*this, session, {{"state", "cancelled"}});
        }
        break;
    }
    case ui::UiAction::RefreshCharacters: {
        const auto id = bounded_text(args, "accountId", 128);
        if (!accounts.find(id)) fail_static("not_found");
        result.data = store_profiles(*this, auth.skin_profiles(id, cancellation))->public_state;
        break;
    }
    case ui::UiAction::UpdateOfflineSkin:
    case ui::UiAction::ResetOfflineSkin: {
        Json patch;
        if (action == ui::UiAction::ResetOfflineSkin)
            patch = {{"offlineSkin", "random"}, {"offlineSkinPath", ""}, {"offlineSkinName", ""}, {"offlineSkinSlim", false}};
        else {
            const auto path = instances::path_from_utf8(bounded_text(args, "path", 32768));
            if (!path.is_absolute()) fail_static("invalid_argument");
            const auto encoded = images.read_selected_file(path, cancellation);
            images.decode_bounded(encoded, cancellation);
            patch = {{"offlineSkin", "custom"}, {"offlineSkinPath", instances::path_to_utf8(path)}};
        }
        if (args.contains("root") && args.contains("versionId")) {
            auto scoped = args;
            scoped["patch"] = patch;
            return action_instances(ui::UiAction::PatchVersionSettings, scoped, cancellation);
        }
        result.data = {{"section", "game"}, {"values", apply_settings_patch(repository, "game.json", patch, budget, cancellation)}};
        result.changes.emplace_back(ui::UiTopic::Settings, result.data);
        images.clear();
        break;
    }
    case ui::UiAction::UploadSkin:
    case ui::UiAction::ChangePlayerName: {
        const auto id = bounded_text(args, "accountId", 128);
        const auto account = accounts.find(id);
        if (!account || account->type == accounts::AccountType::offline) fail_static("unauthorized");
        const auto url = account->type == accounts::AccountType::microsoft ? std::string("https://www.minecraft.net/msaprofile")
            : accounts::site_base(accounts::validate_server(account->server, true)) + "/user/characters";
        cancellation.check();
        nickel::open_url(url);
        result.data = {{"opened", true}};
        return result;
    }
    default:
        fail_static("invalid_argument");
    }
    result.changes.emplace_back(ui::UiTopic::Accounts, accounts_snapshot());
    if (result.data.empty()) result.data = accounts_snapshot();
    return result;
}

void ApplicationState::poll_microsoft(const std::shared_ptr<AuthSession>& session, const Cancellation& shutdown) {
    if (stopping.load() || session->cancellation.cancelled()) return;
    {
        std::lock_guard lock(state_mutex);
        const auto found = auth_sessions.find(session->operation_id);
        if (found == auth_sessions.end() || found->second != session) return;
    }
    std::stop_callback stop(shutdown.stop_token(), [token = session->cancellation]() mutable { token.cancel(); });
    auto retry = std::chrono::milliseconds(5000);
    try {
        const auto outcome = auth.device_poll(session->session_id, session->generation, session->cancellation);
        if (outcome.status == accounts::DevicePollStatus::success) {
            if (!outcome.account) fail_static("integrity");
            images.invalidate(outcome.account->id);
            forget_session(*this, session, {{"operationId", session->operation_id}, {"state", "success"}});
            publish(ui::UiTopic::Accounts, accounts_snapshot());
            return;
        }
        retry = std::max(outcome.retry_after, std::chrono::milliseconds(50));
    } catch (const accounts::IdentityError& error) {
        if (error.code() != accounts::IdentityErrorCode::network) {
            forget_session(*this, session, {{"operationId", session->operation_id}, {"state", "error"},
                {"message", safe_public_message(error.error().code)}});
            publish(ui::UiTopic::Accounts, accounts_snapshot());
            return;
        }
        retry = std::chrono::seconds(10);
    } catch (const std::exception& error) {
        forget_session(*this, session, {{"operationId", session->operation_id}, {"state", "error"},
            {"message", public_error(error).message.toUtf8().toStdString()}});
        publish(ui::UiTopic::Accounts, accounts_snapshot());
        return;
    }
    if (stopping.load() || session->cancellation.cancelled()) return;
    const auto weak = weak_from_this();
    if (!schedule(retry, [weak, session](const Cancellation& cancellation) {
        if (const auto state = weak.lock()) state->poll_microsoft(session, cancellation);
    })) {
        auth.device_cancel(session->session_id, session->generation);
        forget_session(*this, session, {{"operationId", session->operation_id}, {"state", "error"},
            {"message", safe_public_message("busy")}});
        publish(ui::UiTopic::Accounts, accounts_snapshot());
    }
}

} // namespace nickel::app
