#include "mock_http.hpp"

namespace identity_test {
namespace {
const nickel::accounts::MsaConfig config{"fixture-client", "consumers", "nickel://auth"};

void test_purge_callback_reentry() {
    using namespace nickel::accounts;
    MemoryStorage storage;
    FixtureVault vault;
    AccountService accounts(storage, vault);
    MockHttp http;
    FakeClock clock;
    AuthService auth(accounts, http, clock.options());
    add_device(http, "FIXTURE_DEVICE", 10);
    const auto started = auth.device_start(config);
    clock.advance(5);
    PausePoint pause;
    std::atomic<bool> reentered{false};
    http.add("POST", "https://login.microsoftonline.com/consumers/oauth2/v2.0/token",
        [&](const auto&, std::stop_token stop) {
            std::stop_callback callback(stop, [&] { auth.purge_expired(); reentered = true; });
            pause.pause();
            return json_response(400, Json{{"error", "authorization_pending"}});
        });
    auto poll = std::async(std::launch::async, [&] {
        try { auth.device_poll(started.session_id, started.generation); return false; }
        catch (const IdentityError& error) { return error.code() == IdentityErrorCode::cancelled; }
    });
    pause.wait();
    clock.advance(10);
    auth.purge_expired();
    pause.release();
    require(reentered && poll.get() && storage.writes == 0, "expiry stop callbacks run outside auth gate");
    http.verify();
}

void test_canonical_cancel_commit_gate() {
    using namespace nickel::accounts;
    MemoryStorage storage;
    FixtureVault vault;
    AccountService accounts(storage, vault);
    MockHttp http;
    FakeClock clock;
    AuthService auth(accounts, http, clock.options());
    add_device(http);
    const auto started = auth.device_start(config);
    clock.advance(5);
    add_token(http);
    add_minecraft_chain(http);
    PausePoint pause;
    storage.before_write = [&] { pause.pause(); };
    nickel::Cancellation cancellation;
    auto poll = std::async(std::launch::async, [&] {
        return auth.device_poll(started.session_id, started.generation, cancellation);
    });
    pause.wait();
    auto cancelled = std::async(std::launch::async, [&] { cancellation.cancel(); });
    const auto state = cancelled.wait_for(std::chrono::milliseconds(30));
    pause.release();
    const auto result = poll.get();
    cancelled.get();
    require(state == std::future_status::timeout, "canonical caller cancellation waits for durable commit gate");
    require(result.status == DevicePollStatus::success && storage.writes == 1 && accounts.list().size() == 1,
            "canonical commit-first completes exactly once");
    http.verify();
}

void test_consumed_device_retry() {
    using namespace nickel::accounts;
    MemoryStorage storage;
    FixtureVault vault;
    AccountService accounts(storage, vault);
    MockHttp http;
    FakeClock clock;
    AuthService auth(accounts, http, clock.options());
    add_device(http);
    const auto started = auth.device_start(config);
    clock.advance(5);
    add_token(http);
    http.add("POST", "https://user.auth.xboxlive.com/user/authenticate", [](const auto&, auto) -> IdentityHttpResponse {
        throw nickel::ErrorException(nickel::Error{"transport.failure", "FIXTURE_SECRET_MUST_NOT_ESCAPE"});
    });
    expect_error(IdentityErrorCode::network, [&] { auth.device_poll(started.session_id, started.generation); });
    require(auth.device_poll(started.session_id, started.generation).retry_after == std::chrono::seconds(10),
            "network backoff enforced after device authorization");
    require(storage.writes == 0, "partial Microsoft chain never persists");
    clock.advance(10);
    add_minecraft_chain(http);
    require(auth.device_poll(started.session_id, started.generation).status == DevicePollStatus::success,
            "downstream retry reuses private OAuth token without replaying consumed device code");
    http.verify();
}
}

void run_lifecycle_tests() {
    test_purge_callback_reentry();
    test_canonical_cancel_commit_gate();
    test_consumed_device_retry();
    std::cout << "lifecycle: purge reentry, canonical cancel/commit gate and consumed-code retry passed\n";
}
} // namespace identity_test
