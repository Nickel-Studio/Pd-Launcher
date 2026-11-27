#include "mock_http.hpp"

#include <future>
#include <sstream>

namespace identity_test {
using namespace nickel::accounts;
constexpr const char* skin_api = "https://fixture.invalid/mc/api/yggdrasil";
const MsaConfig msa_config{"fixture-client","consumers","nickel://auth"};

namespace {
void test_device_chain_and_backoff() {
    MemoryStorage storage;
    FixtureVault vault;
    AccountService accounts(storage,vault);
    MockHttp http;
    FakeClock clock;
    std::string logs;
    auto options = clock.options();
    options.log = [&](const SafeIdentityEvent& event) { logs += std::string(event.stage)+":"+std::string(event.outcome)+":"+std::to_string(event.http_status); };
    AuthService auth(accounts,http,options);
    add_device(http);
    const auto started = auth.device_start(msa_config);
    require(started.session_id.starts_with("msa_") && started.generation && started.interval == std::chrono::seconds(5), "private device session handle");
    require(auth.device_poll(started.session_id,started.generation).retry_after == std::chrono::milliseconds(5000), "no early polling");
    require(http.calls == 1, "no early network request");
    clock.advance(5);
    http.add_json("POST","https://login.microsoftonline.com/consumers/oauth2/v2.0/token",400,Json{{"error","slow_down"},{"error_description","FIXTURE_SERVER_SECRET"}});
    auto polled = auth.device_poll(started.session_id,started.generation);
    require(polled.status == DevicePollStatus::slow_down && polled.retry_after == std::chrono::seconds(10), "slow_down increments by five seconds");
    require(auth.device_poll(started.session_id,started.generation).retry_after == std::chrono::seconds(10), "slow_down timer enforced");
    clock.advance(10);
    http.add_json("POST","https://login.microsoftonline.com/consumers/oauth2/v2.0/token",400,Json{{"error","slow_down"}});
    polled = auth.device_poll(started.session_id,started.generation);
    require(polled.retry_after == std::chrono::seconds(15), "repeated slow_down cumulative");
    clock.advance(15);
    http.add_json("POST","https://login.microsoftonline.com/consumers/oauth2/v2.0/token",400,Json{{"error","authorization_pending"}});
    require(auth.device_poll(started.session_id,started.generation).retry_after == std::chrono::seconds(15), "pending retains backoff");
    clock.advance(15);
    add_token(http);
    add_minecraft_chain(http);
    polled = auth.device_poll(started.session_id,started.generation);
    require(polled.status == DevicePollStatus::success && polled.account && polled.account->type == AccountType::microsoft, "Microsoft full chain commits");
    const auto value = Json::parse(storage.accounts)[0];
    require(value["accessToken"] == "FIXTURE_MC_SECRET" && value["refreshToken"] == "FIXTURE_REFRESH_SECRET", "Minecraft token and OAuth refresh stored");
    require(value["expiresAt"] == 1700007245ULL, "uses actual Minecraft expires_in");
    require(public_account_json(*polled.account).find("FIXTURE_MC_SECRET") == std::string::npos, "login result public secret strip");
    require(logs.find("FIXTURE_") == std::string::npos && logs.find("device_code") == std::string::npos, "OAuth secure static logs");
    const auto launch = auth.prepare_launch(polled.account->id);
    require(launch.access_token.view() == "FIXTURE_MC_SECRET" && launch.user_type == "msa" && launch.xuid == "1234567890", "launcher native credentials only");
    const auto account_id = polled.account->id;
    add_token(http,true,false);
    add_minecraft_chain(http);
    require(auth.microsoft_refresh(account_id,msa_config).id == account_id, "refresh account ID reused");
    require(Json::parse(storage.accounts)[0]["refreshToken"] == "FIXTURE_REFRESH_SECRET", "missing rotated refresh preserves old");
    require(accounts.list().size() == 1, "no duplicate refresh account");
    add_device(http,"FIXTURE_NEW_DEVICE");
    auto again = auth.device_start(msa_config);
    clock.advance(5);
    add_token(http);
    add_minecraft_chain(http);
    require(auth.device_poll(again.session_id,again.generation).account->id == account_id, "Microsoft login deduplicates normalized UUID");
    require(accounts.list().size() == 1, "dedup linearized list commit");
    http.verify();
}

void test_device_expiry_serial_cancel() {
    MemoryStorage storage;
    FixtureVault vault;
    AccountService accounts(storage,vault);
    MockHttp http;
    FakeClock clock;
    AuthService auth(accounts,http,clock.options());
    add_device(http,"fixture-expiring",10);
    auto expired = auth.device_start(msa_config);
    clock.advance(10);
    expect_error(IdentityErrorCode::expired,[&] { auth.device_poll(expired.session_id,expired.generation); });
    auth.purge_expired();
    require(storage.writes == 0, "expiry no commit");
    add_device(http);
    auto serial = auth.device_start(msa_config);
    clock.advance(5);
    PausePoint pause;
    http.add("POST","https://login.microsoftonline.com/consumers/oauth2/v2.0/token",[&](const auto&,auto) {
        pause.pause();
        return json_response(400,Json{{"error","authorization_pending"}});
    });
    auto poll = std::async(std::launch::async,[&] {
        try { auth.device_poll(serial.session_id,serial.generation); return IdentityErrorCode::invalid_input; }
        catch (const IdentityError& error) { return error.code(); }
    });
    pause.wait();
    expect_error(IdentityErrorCode::busy,[&] { auth.device_poll(serial.session_id,serial.generation); });
    auth.device_cancel(serial.session_id,serial.generation+1);
    require(storage.writes == 0, "wrong generation cannot commit");
    auth.device_cancel(serial.session_id,serial.generation);
    pause.release();
    require(poll.get() == IdentityErrorCode::cancelled && http.maximum_active == 1, "serial poll and cancel generation");
    require(storage.writes == 0, "cancelled serial poll no commit");
    http.verify();
}

void test_commit_cancel_linearization(bool cancel_first) {
    MemoryStorage storage;
    FixtureVault vault;
    AccountService accounts(storage,vault);
    MockHttp http;
    FakeClock clock;
    AuthService auth(accounts,http,clock.options());
    add_device(http);
    const auto started = auth.device_start(msa_config);
    clock.advance(5);
    add_token(http);
    PausePoint pause;
    if (!cancel_first) storage.before_write = [&] { pause.pause(); };
    add_minecraft_chain(http,true,cancel_first ? std::function<void()>([&] { pause.pause(); }) : std::function<void()>{});
    auto result = std::async(std::launch::async,[&] {
        try { auth.device_poll(started.session_id,started.generation); return false; }
        catch (const IdentityError& error) { require(error.code() == IdentityErrorCode::cancelled, "cancel race code"); return true; }
    });
    pause.wait();
    if (cancel_first) {
        auth.device_cancel(started.session_id,started.generation);
        pause.release();
        require(result.get() && storage.writes == 0 && accounts.list().empty(), "cancel wins before commit");
    } else {
        auto cancelled = std::async(std::launch::async,[&] { auth.device_cancel(started.session_id,started.generation); });
        require(cancelled.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout, "cancel waits for linearized durable write");
        pause.release();
        require(!result.get(), "commit wins race");
        cancelled.get();
        require(storage.writes == 1 && accounts.list().size() == 1, "commit wins before cancel returns");
    }
    http.verify();
}

void test_entitlement_and_refresh_stale() {
    MemoryStorage storage;
    FixtureVault vault;
    AccountService accounts(storage,vault);
    MockHttp http;
    FakeClock clock;
    AuthService auth(accounts,http,clock.options());
    add_device(http);
    auto session = auth.device_start(msa_config);
    clock.advance(5);
    add_token(http);
    add_minecraft_chain(http,false);
    expect_error(IdentityErrorCode::no_entitlement,[&] { auth.device_poll(session.session_id,session.generation); });
    require(storage.writes == 0, "no entitlement cannot commit");
    add_device(http);
    session = auth.device_start(msa_config);
    clock.advance(5);
    add_token(http);
    add_minecraft_chain(http);
    const auto id = auth.device_poll(session.session_id,session.generation).account->id;
    add_token(http,true);
    add_minecraft_chain(http,true,[&] { accounts.remove(id); });
    expect_error(IdentityErrorCode::stale_commit,[&] { auth.microsoft_refresh(id,msa_config); });
    require(accounts.list().empty(), "removed account never resurrected by refresh");
    http.verify();
}

void test_ygg_sessions_and_restore() {
    MemoryStorage storage;
    FixtureVault vault;
    AccountService accounts(storage,vault);
    MockHttp http;
    FakeClock clock;
    AuthService auth(accounts,http,clock.options());
    add_ygg_auth(http,skin_api,true);
    auto outcome = auth.skin_login("https://fixture.invalid/mc/api/yggdrasil/","fixture@example.invalid",Secret("FIXTURE_PASSWORD"));
    require(outcome.status == SkinLoginOutcome::Status::select && outcome.selection && storage.writes == 0 && vault.seals == 1, "multi profile selection private only");
    auto selection = *outcome.selection;
    expect_error(IdentityErrorCode::invalid_profile,[&] { auth.skin_select(selection.session_id,selection.generation,"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"); });
    require(http.calls == 1, "invalid choice keeps session and no HTTP");
    add_ygg_refresh(http,skin_api,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const auto account = auth.skin_select(selection.session_id,selection.generation,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    require(account.name == "Second" && account.server == "https://fixture.invalid/mc", "site root public account");
    const auto persisted = Json::parse(storage.accounts)[0];
    require(persisted["password"].get<std::string>().starts_with("v1:") && persisted["password"] != "FIXTURE_PASSWORD", "Vault seal password");
    require(persisted["accessToken"] == "FIXTURE_YGG_ROTATED" && !persisted.contains("refreshToken"), "skin binding stored token schema");
    require(public_account_json(account).find("FIXTURE_YGG_ROTATED") == std::string::npos, "skin public strip");
    expect_error(IdentityErrorCode::expired,[&] { auth.skin_select(selection.session_id,selection.generation,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); });
    add_ygg_auth(http,skin_api,true);
    selection = auth.skin_profiles(account.id);
    require(vault.opens == 1 && accounts.list().size() == 1, "profiles Vault reauthenticate no duplicate");
    add_ygg_auth(http,skin_api,true);
    add_ygg_refresh(http,skin_api,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    auth.skin_cancel(selection.session_id,selection.generation);
    require(accounts.find(account.id)->name == "Second" && accounts.list().size() == 1, "cancel restores original profile same account");
    http.add_json("POST",std::string(skin_api)+"/authserver/validate",204,Json::object());
    require(auth.skin_validate(account.id), "Ygg 204 valid");
    add_ygg_refresh(http,skin_api,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    require(auth.skin_refresh(account.id).id == account.id, "skin refresh same ID");
    http.add_json("POST",std::string(skin_api)+"/authserver/validate",403,Json{{"errorMessage","FIXTURE_LEAK_SECRET"}});
    require(!auth.skin_validate(account.id), "Ygg 403 invalid");
    http.add_json("POST",std::string(skin_api)+"/authserver/invalidate",204,Json::object());
    auth.skin_invalidate(account.id);
    require(!Json::parse(storage.accounts)[0].contains("accessToken"), "invalidate clears token");
    http.verify();
}

void test_ygg_retry_cancel_stale_expiry() {
    MemoryStorage storage;
    FixtureVault vault;
    AccountService accounts(storage,vault);
    MockHttp http;
    FakeClock clock;
    AuthService auth(accounts,http,clock.options());
    add_ygg_auth(http,skin_api,true);
    auto selection = *auth.skin_login("https://fixture.invalid/mc","fixture@example.invalid",Secret("FIXTURE_PASSWORD")).selection;
    http.add_json("POST",std::string(skin_api)+"/authserver/refresh",500,Json{{"errorMessage","FIXTURE_PASSWORD FIXTURE_TOKEN"}});
    expect_error(IdentityErrorCode::network,[&] { auth.skin_select(selection.session_id,selection.generation,"069a79f444e94726a5befca90e38aaf5"); });
    add_ygg_refresh(http,skin_api);
    const auto account = auth.skin_select(selection.session_id,selection.generation,"069a79f444e94726a5befca90e38aaf5");
    add_ygg_auth(http,skin_api,true);
    selection = auth.skin_profiles(account.id);
    PausePoint pause;
    add_ygg_refresh(http,skin_api,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",[&] { pause.pause(); });
    auto selected = std::async(std::launch::async,[&] {
        try { auth.skin_select(selection.session_id,selection.generation,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); return IdentityErrorCode::invalid_input; }
        catch (const IdentityError& error) { return error.code(); }
    });
    pause.wait();
    accounts.remove(account.id);
    pause.release();
    require(selected.get() == IdentityErrorCode::stale_commit && accounts.list().empty(), "skin select cannot resurrect removed account");
    auth.skin_cancel(selection.session_id,selection.generation);
    add_ygg_auth(http,skin_api,true);
    selection = *auth.skin_login("https://fixture.invalid/mc","fixture@example.invalid",Secret("FIXTURE_PASSWORD")).selection;
    clock.advance(600);
    expect_error(IdentityErrorCode::expired,[&] { auth.skin_select(selection.session_id,selection.generation,"069a79f444e94726a5befca90e38aaf5"); });
    auth.purge_expired();
    auth.skin_cancel(selection.session_id,selection.generation);
    expect_error(IdentityErrorCode::invalid_server,[&] { auth.skin_login("https://user:pass@bad.invalid","fixture@example.invalid",Secret("FIXTURE_PASSWORD")); });
    http.verify();
}
}

void run_auth_tests() {
    test_device_chain_and_backoff();
    test_device_expiry_serial_cancel();
    test_commit_cancel_linearization(true);
    test_commit_cancel_linearization(false);
    test_entitlement_and_refresh_stale();
    test_ygg_sessions_and_restore();
    test_ygg_retry_cancel_stale_expiry();
    std::cout << "auth: controlled OAuth/XBL/XSTS/entitlement/profile/refresh/backoff/expiry/serial/cancel-commit/Ygg binding/restore passed\n";
}

} // namespace identity_test
