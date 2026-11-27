#include "auth.hpp"
#include "detail/internal.hpp"
#include "detail/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace nickel::accounts {
namespace {
using namespace detail;
using SteadyTime = std::chrono::steady_clock::time_point;
constexpr std::string_view oauth_host = "https://login.microsoftonline.com/";
constexpr std::string_view oauth_scope = "XboxLive.signin offline_access";

struct ScopeExit {
    std::function<void()> action;
    ~ScopeExit() { if (action) action(); }
};

std::string oauth_base(const MsaConfig& config) {
    if (trim(config.client_id).empty() || config.client_id.size() > 256 || !valid_utf8(config.client_id))
        throw IdentityError(IdentityErrorCode::invalid_input);
    const auto authority = trim(config.authority);
    if (authority != "consumers" && authority != "common" && authority != "organizations" && !valid_profile_id(authority))
        throw IdentityError(IdentityErrorCode::invalid_input);
    return std::string(oauth_host) + authority + "/oauth2/v2.0";
}

void verification_url(std::string_view url) {
    const auto parsed = parse_url(url, true);
    if (parsed.scheme != "https" || parsed.port != 443 ||
        (parsed.host != "microsoft.com" && parsed.host != "www.microsoft.com" &&
         parsed.host != "login.microsoftonline.com" && parsed.host != "login.live.com" && parsed.host != "aka.ms"))
        throw IdentityError(IdentityErrorCode::invalid_response);
}

std::string auth_server(AccountType type, const std::optional<std::string>& input, bool stored = false) {
    if (type == AccountType::littleskin) return validate_server(input, stored);
    if (type != AccountType::yggdrasil || !input || trim(*input).empty()) throw IdentityError(IdentityErrorCode::invalid_server);
    auto raw = trim(*input);
    const auto lower = ascii_lower(raw);
    if ((lower.starts_with("http:") || lower.starts_with("https:")) && raw.find("://") == std::string::npos)
        throw IdentityError(IdentityErrorCode::invalid_server);
    if (raw.find("://") == std::string::npos) raw = "https://" + raw;
    while (raw.ends_with('/')) raw.pop_back();
    return parse_url(raw).str();
}

void require_skin(const PublicAccount& account) {
    if (account.type != AccountType::littleskin && account.type != AccountType::yggdrasil)
        throw IdentityError(IdentityErrorCode::invalid_input);
}

Secret account_token(AccountState& state, const std::optional<std::string>& token) {
    if (!token || token->empty()) throw IdentityError(IdentityErrorCode::missing_credentials);
    return token->starts_with("v1:") ? state.open(*token) : Secret(*token);
}

SkinProfile current_profile(const PublicAccount& account) {
    if (!valid_profile_id(account.uuid) || account.name.empty()) throw IdentityError(IdentityErrorCode::no_profile);
    return {normalize_uuid(account.uuid), account.name};
}
}

struct AuthService::Impl {
    struct Operation {
        std::uint64_t generation = 0;
        std::string account_id;
        nickel::Cancellation cancellation;
        SteadyTime expires = SteadyTime::max();
        bool active = true;
        bool cancelled = false;
        bool in_flight = false;
    };
    struct DeviceSession {
        std::shared_ptr<Operation> operation;
        MsaConfig config;
        Secret device_code;
        std::chrono::seconds interval{5};
        SteadyTime next_poll{};
        std::optional<OAuthTokens> authorized;
    };
    struct SkinSession {
        std::shared_ptr<Operation> operation;
        StoredAccount candidate;
        std::optional<AccountSnapshot> expected;
        std::optional<SkinProfile> original;
        std::vector<SkinProfile> profiles;
        std::mutex mutation_lane;
    };

    AccountState& accounts;
    std::shared_ptr<IdentityHttp> http;
    AuthOptions options;
    mutable std::mutex gate;
    std::uint64_t sequence = 0;
    std::unordered_map<std::uint64_t, std::shared_ptr<Operation>> live;
    std::unordered_map<std::string, std::shared_ptr<Operation>> account_operations;
    std::unordered_map<std::string, std::shared_ptr<DeviceSession>> devices;
    std::unordered_map<std::string, std::shared_ptr<SkinSession>> skins;
    std::unordered_map<std::string, std::string> xuids;

    Impl(AccountState& state, std::shared_ptr<IdentityHttp> transport, AuthOptions config)
        : accounts(state), http(std::move(transport)), options(std::move(config)) {
        if (!http || !options.max_sessions || options.max_sessions > 256 ||
            options.skin_session_ttl <= std::chrono::seconds::zero() || options.skin_session_ttl > std::chrono::hours(1) ||
            options.refresh_skew < std::chrono::seconds::zero()) throw IdentityError(IdentityErrorCode::invalid_input);
        if (!options.clock.monotonic) options.clock.monotonic = [] { return std::chrono::steady_clock::now(); };
        if (!options.clock.wall) options.clock.wall = [] { return std::chrono::system_clock::now(); };
    }

    SteadyTime now() const { return options.clock.monotonic(); }
    std::uint64_t wall_secs() const {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(options.clock.wall().time_since_epoch()).count();
        if (seconds < 0) throw IdentityError(IdentityErrorCode::invalid_input);
        return static_cast<std::uint64_t>(seconds);
    }

    void close_locked(const std::shared_ptr<Operation>& op, bool cancel = false) {
        op->active = false;
        op->cancelled = op->cancelled || cancel;
        live.erase(op->generation);
        if (!op->account_id.empty()) {
            const auto found = account_operations.find(op->account_id);
            if (found != account_operations.end() && found->second == op) account_operations.erase(found);
        }
    }

    void purge() {
        std::vector<std::shared_ptr<Operation>> expired;
        {
            std::lock_guard lock(gate);
            const auto time = now();
            for (auto it = devices.begin(); it != devices.end();) {
                if (!it->second->operation->active || it->second->operation->expires <= time) {
                    expired.push_back(it->second->operation);
                    close_locked(it->second->operation, true);
                    it = devices.erase(it);
                } else ++it;
            }
            for (auto it = skins.begin(); it != skins.end();) {
                if (!it->second->operation->active || it->second->operation->expires <= time) {
                    expired.push_back(it->second->operation);
                    close_locked(it->second->operation, true);
                    it = skins.erase(it);
                } else ++it;
            }
        }
        // stop callbacks may reenter the service; never invoke them under gate.
        for (const auto& op : expired) op->cancellation.cancel();
    }

    std::shared_ptr<Operation> begin(std::string account_id = {}) {
        purge();
        std::lock_guard lock(gate);
        if (live.size() >= options.max_sessions) throw IdentityError(IdentityErrorCode::quota);
        if (!account_id.empty() && account_operations.contains(account_id)) throw IdentityError(IdentityErrorCode::busy);
        auto op = std::make_shared<Operation>();
        op->generation = ++sequence;
        op->account_id = std::move(account_id);
        live[op->generation] = op;
        if (!op->account_id.empty()) account_operations[op->account_id] = op;
        return op;
    }

    void close(const std::shared_ptr<Operation>& op) {
        std::lock_guard lock(gate);
        close_locked(op);
    }

    void cancel_operation(const std::shared_ptr<Operation>& op) noexcept {
        {
            std::lock_guard lock(gate);
            if (!op->active) return;
            close_locked(op, true);
        }
        op->cancellation.cancel();
    }

    void check_locked(const std::shared_ptr<Operation>& op, std::stop_token caller) const {
        if (op->cancelled || op->cancellation.cancelled() || caller.stop_requested() || !op->active)
            throw IdentityError(IdentityErrorCode::cancelled);
        if (now() >= op->expires) throw IdentityError(IdentityErrorCode::expired);
    }

    void checkpoint(const std::shared_ptr<Operation>& op, std::stop_token caller) const {
        std::lock_guard lock(gate);
        check_locked(op, caller);
    }

    PublicAccount commit(const std::shared_ptr<Operation>& op, StoredAccount account,
                         const std::optional<AccountSnapshot>& expected,
                         bool deduplicate, std::stop_token caller) {
        // Explicit cancel and durable commit share this gate: either cancel wins,
        // or a complete repository write wins before cancellation can return.
        std::lock_guard lock(gate);
        check_locked(op, caller);
        auto result = accounts.commit(std::move(account), expected, deduplicate);
        close_locked(op);
        return result;
    }

    void flight_finished(const std::shared_ptr<Operation>& op) {
        std::lock_guard lock(gate);
        op->in_flight = false;
    }

    StoredAccount microsoft_account(const MinecraftSession& session, const OAuthTokens& oauth,
                                    const std::optional<AccountSnapshot>& existing = std::nullopt) {
        StoredAccount account;
        if (existing) account = existing->account;
        account.public_account.type = AccountType::microsoft;
        account.public_account.name = session.profile.name;
        account.public_account.uuid = dash_uuid(session.profile.id);
        account.public_account.server.reset();
        account.password.reset();
        account.client_token.reset();
        account.access_token = std::string(session.token.view());
        if (oauth.refresh) account.refresh_token = std::string(oauth.refresh->view());
        account.expires_at = session.expires_at;
        return account;
    }
};

AuthService::AuthService(AccountService& accounts, IdentityHttp& http, AuthOptions options)
    : AuthService(accounts, std::shared_ptr<IdentityHttp>(&http, [](IdentityHttp*) {}), std::move(options)) {}
AuthService::AuthService(AccountService& accounts, std::shared_ptr<IdentityHttp> http, AuthOptions options)
    : impl_(std::make_unique<Impl>(*accounts.impl_, std::move(http), std::move(options))) {}
AuthService::~AuthService() { cancel_all(); }

DeviceCodeStart AuthService::device_start(const MsaConfig& config, std::stop_token stop) {
    const auto base = oauth_base(config);
    check_stop(stop);
    auto op = impl_->begin();
    bool keep = false;
    ScopeExit cleanup{[&] { if (!keep) impl_->close(op); }};
    std::stop_callback link(stop, [this, op] { impl_->cancel_operation(op); });
    auto response = post_form(*impl_->http, join_url(base, "devicecode"),
                              {{"client_id", trim(config.client_id)}, {"scope", std::string(oauth_scope)}},
                              op->cancellation.stop_token(), impl_->options.log, "msa.device_start");
    EraseJson erase{response.body};
    impl_->checkpoint(op, stop);
    if (response.status != 200) throw IdentityError(IdentityErrorCode::network, response.status);
    auto code = string_field(response.body, "device_code", true);
    EraseString erase_code{code};
    DeviceCodeStart result;
    result.session_id = "msa_" + random_hex();
    result.user_code = string_field(response.body, "user_code", true, 256);
    result.verification_uri = string_field(response.body, "verification_uri", true, 2048);
    verification_url(result.verification_uri);
    auto complete = string_field(response.body, "verification_uri_complete", false, 4096);
    if (!complete.empty()) { verification_url(complete); result.verification_uri_complete = std::move(complete); }
    result.interval = std::chrono::seconds(uint_field(response.body, "interval", 5, 3600));
    if (result.interval <= std::chrono::seconds::zero()) throw IdentityError(IdentityErrorCode::invalid_response);
    result.expires_in = std::chrono::seconds(uint_field(response.body, "expires_in", 0, 3600));
    if (result.expires_in <= std::chrono::seconds::zero()) throw IdentityError(IdentityErrorCode::invalid_response);
    result.generation = op->generation;
    auto session = std::make_shared<Impl::DeviceSession>();
    session->operation = op;
    session->config = config;
    session->config.client_id = trim(config.client_id);
    session->device_code = Secret(code);
    session->interval = result.interval;
    {
        std::lock_guard lock(impl_->gate);
        impl_->check_locked(op, stop);
        op->expires = impl_->now() + result.expires_in;
        session->next_poll = impl_->now() + result.interval;
        impl_->devices[result.session_id] = std::move(session);
        keep = true;
    }
    return result;
}

DevicePollResult AuthService::device_poll(std::string_view id, std::uint64_t generation, std::stop_token stop) {
    std::shared_ptr<Impl::DeviceSession> session;
    {
        std::lock_guard lock(impl_->gate);
        const auto found = impl_->devices.find(std::string(id));
        if (found == impl_->devices.end()) throw IdentityError(IdentityErrorCode::expired);
        session = found->second;
        const auto& op = session->operation;
        if (op->generation != generation) throw IdentityError(IdentityErrorCode::cancelled);
        impl_->check_locked(op, stop);
        if (op->in_flight) throw IdentityError(IdentityErrorCode::busy);
        const auto time = impl_->now();
        if (time < session->next_poll) return {DevicePollStatus::pending, std::nullopt,
            std::chrono::ceil<std::chrono::milliseconds>(session->next_poll - time)};
        op->in_flight = true;
        session->next_poll = time + session->interval;
    }
    const auto op = session->operation;
    ScopeExit flight{[&] { impl_->flight_finished(op); }};
    std::stop_callback link(stop, [this, op] { impl_->cancel_operation(op); });
    try {
        if (!session->authorized) {
            auto response = post_form(*impl_->http, join_url(oauth_base(session->config), "token"),
                {{"client_id", session->config.client_id}, {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
                 {"device_code", std::string(session->device_code.view())}}, op->cancellation.stop_token(), impl_->options.log, "msa.device_poll");
            EraseJson erase{response.body};
            impl_->checkpoint(op, stop);
            if (response.status != 200) {
                const auto error = string_field(response.body, "error");
                if (error == "authorization_pending" || error == "slow_down" || response.status == 429 || response.status >= 500) {
                    std::lock_guard lock(impl_->gate);
                    impl_->check_locked(op, stop);
                    const bool slowed = error == "slow_down" || response.status == 429 || response.status >= 500;
                    if (slowed) session->interval = std::min(std::chrono::seconds(3600), session->interval + std::chrono::seconds(5));
                    session->interval = std::max(session->interval, response.retry_after);
                    session->next_poll = impl_->now() + session->interval;
                    return {slowed ? DevicePollStatus::slow_down : DevicePollStatus::pending, std::nullopt,
                            std::chrono::duration_cast<std::chrono::milliseconds>(session->interval)};
                }
                if (error == "authorization_declined" || error == "access_denied") throw IdentityError(IdentityErrorCode::declined, response.status);
                if (error == "expired_token" || error == "bad_verification_code") throw IdentityError(IdentityErrorCode::expired, response.status);
                throw IdentityError(IdentityErrorCode::unauthorized, response.status);
            }
            session->authorized.emplace(oauth_tokens(response.body));
            session->device_code.clear();
        }
        const auto minecraft = microsoft_chain(*impl_->http, session->authorized->access, impl_->wall_secs(),
            op->cancellation.stop_token(), impl_->options.log, [&] { impl_->checkpoint(op, stop); });
        auto account = impl_->microsoft_account(minecraft, *session->authorized);
        auto public_account = impl_->commit(op, std::move(account), std::nullopt, true, stop);
        {
            std::lock_guard lock(impl_->gate);
            impl_->devices.erase(std::string(id));
            impl_->xuids[public_account.id] = minecraft.xuid;
        }
        return {DevicePollStatus::success, std::move(public_account), std::chrono::milliseconds{0}};
    } catch (const IdentityError& error) {
        std::lock_guard lock(impl_->gate);
        if (error.code() == IdentityErrorCode::network && op->active && !op->cancelled) {
            session->interval = std::min(std::chrono::seconds(3600), session->interval * 2);
            session->next_poll = impl_->now() + session->interval;
        } else {
            impl_->close_locked(op, true);
            impl_->devices.erase(std::string(id));
        }
        throw;
    }
}

void AuthService::device_cancel(std::string_view id, std::uint64_t generation) noexcept {
    std::shared_ptr<Impl::Operation> cancelled;
    {
        std::lock_guard lock(impl_->gate);
        const auto found = impl_->devices.find(std::string(id));
        if (found == impl_->devices.end() || found->second->operation->generation != generation) return;
        cancelled = found->second->operation;
        impl_->close_locked(cancelled, true);
        impl_->devices.erase(found);
    }
    cancelled->cancellation.cancel();
}

PublicAccount AuthService::microsoft_refresh(std::string_view id, const MsaConfig& config, std::stop_token stop) {
    const auto base = oauth_base(config);
    auto op = impl_->begin(std::string(id));
    ScopeExit cleanup{[&] { impl_->close(op); }};
    std::stop_callback link(stop, [this, op] { impl_->cancel_operation(op); });
    const auto snapshot = impl_->accounts.snapshot(id);
    if (snapshot.account.public_account.type != AccountType::microsoft) throw IdentityError(IdentityErrorCode::invalid_input);
    auto refresh = account_token(impl_->accounts, snapshot.account.refresh_token);
    impl_->checkpoint(op, stop);
    auto response = post_form(*impl_->http, join_url(base, "token"),
        {{"client_id", trim(config.client_id)}, {"grant_type", "refresh_token"},
         {"refresh_token", std::string(refresh.view())}, {"scope", std::string(oauth_scope)}},
        op->cancellation.stop_token(), impl_->options.log, "msa.refresh");
    EraseJson erase{response.body};
    impl_->checkpoint(op, stop);
    if (response.status != 200) {
        if (response.status == 400 || response.status == 401 || response.status == 403) throw IdentityError(IdentityErrorCode::unauthorized, response.status);
        throw IdentityError(IdentityErrorCode::network, response.status);
    }
    auto oauth = oauth_tokens(response.body);
    auto minecraft = microsoft_chain(*impl_->http, oauth.access, impl_->wall_secs(), op->cancellation.stop_token(),
        impl_->options.log, [&] { impl_->checkpoint(op, stop); });
    if (normalize_uuid(snapshot.account.public_account.uuid) != normalize_uuid(minecraft.profile.id)) throw IdentityError(IdentityErrorCode::invalid_response);
    auto updated = impl_->microsoft_account(minecraft, oauth, snapshot);
    auto result = impl_->commit(op, std::move(updated), snapshot, false, stop);
    { std::lock_guard lock(impl_->gate); impl_->xuids[result.id] = minecraft.xuid; }
    return result;
}

SkinLoginOutcome AuthService::skin_login(const std::optional<std::string>& input, std::string_view user,
                                         Secret password, AccountType type, std::stop_token stop) {
    const auto server = auth_server(type, input);
    const auto username = trim(user);
    if (username.empty() || username.size() > 512 || !valid_utf8(username) || password.empty() || password.view().size() > 16384)
        throw IdentityError(IdentityErrorCode::invalid_input);
    auto op = impl_->begin();
    bool keep = false;
    ScopeExit cleanup{[&] { if (!keep) impl_->close(op); }};
    std::stop_callback link(stop, [this, op] { impl_->cancel_operation(op); });
    auto response = ygg_authenticate(*impl_->http, type, server, username, password, op->cancellation.stop_token(), impl_->options.log);
    impl_->checkpoint(op, stop);
    auto session = std::make_shared<Impl::SkinSession>();
    session->operation = op;
    auto& candidate = session->candidate;
    candidate.public_account.type = type;
    candidate.public_account.email = username;
    candidate.public_account.server = server;
    candidate.password = impl_->accounts.seal(password.view());
    password.clear();
    candidate.access_token = std::string(response.access.view());
    candidate.client_token = response.client;
    session->profiles = response.available;
    if (session->profiles.empty() && response.selected) session->profiles.push_back(*response.selected);
    if (session->profiles.empty()) throw IdentityError(IdentityErrorCode::no_profile);
    if (session->profiles.size() > 1) {
        ProfileList selection{"skin_" + random_hex(), session->profiles, op->generation};
        {
            std::lock_guard lock(impl_->gate);
            impl_->check_locked(op, stop);
            op->expires = impl_->now() + impl_->options.skin_session_ttl;
            impl_->skins[selection.session_id] = std::move(session);
            keep = true;
        }
        return {SkinLoginOutcome::Status::select, std::nullopt, std::move(selection)};
    }
    const auto selected = response.selected ? *response.selected : session->profiles[0];
    if (!response.selected) response = ygg_refresh(*impl_->http, type, server, response.access, response.client, selected,
                                                  op->cancellation.stop_token(), impl_->options.log);
    merge_ygg(candidate, response, selected);
    auto account = impl_->commit(op, std::move(candidate), std::nullopt, true, stop);
    return {SkinLoginOutcome::Status::account, std::move(account), std::nullopt};
}

PublicAccount AuthService::skin_select(std::string_view id, std::uint64_t generation,
                                      std::string_view profile_id, std::stop_token stop) {
    std::shared_ptr<Impl::SkinSession> session;
    SkinProfile selected;
    {
        std::lock_guard lock(impl_->gate);
        const auto found = impl_->skins.find(std::string(id));
        if (found == impl_->skins.end()) throw IdentityError(IdentityErrorCode::expired);
        session = found->second;
        if (session->operation->generation != generation) throw IdentityError(IdentityErrorCode::cancelled);
        impl_->check_locked(session->operation, stop);
        if (session->operation->in_flight) throw IdentityError(IdentityErrorCode::busy);
        const auto profile = std::find_if(session->profiles.begin(), session->profiles.end(), [&](const auto& value) { return normalize_uuid(value.id) == normalize_uuid(profile_id); });
        if (profile == session->profiles.end()) throw IdentityError(IdentityErrorCode::invalid_profile);
        selected = *profile;
        session->operation->in_flight = true;
    }
    const auto op = session->operation;
    ScopeExit flight{[&] { impl_->flight_finished(op); }};
    std::stop_callback link(stop, [this, op] { impl_->cancel_operation(op); });
    std::lock_guard lane(session->mutation_lane);
    auto& candidate = session->candidate;
    auto access = account_token(impl_->accounts, candidate.access_token);
    if (!candidate.client_token) throw IdentityError(IdentityErrorCode::missing_credentials);
    auto response = ygg_refresh(*impl_->http, candidate.public_account.type,
        auth_server(candidate.public_account.type, candidate.public_account.server, true), access,
        *candidate.client_token, selected, op->cancellation.stop_token(), impl_->options.log);
    merge_ygg(candidate, response, selected);
    auto result = impl_->commit(op, candidate, session->expected, !session->expected, stop);
    { std::lock_guard lock(impl_->gate); impl_->skins.erase(std::string(id)); }
    return result;
}

ProfileList AuthService::skin_profiles(std::string_view id, std::stop_token stop) {
    auto op = impl_->begin(std::string(id));
    bool keep = false;
    ScopeExit cleanup{[&] { if (!keep) impl_->close(op); }};
    std::stop_callback link(stop, [this, op] { impl_->cancel_operation(op); });
    const auto snapshot = impl_->accounts.snapshot(id);
    const auto& existing = snapshot.account;
    require_skin(existing.public_account);
    const auto server = auth_server(existing.public_account.type, existing.public_account.server, true);
    const auto username = existing.public_account.email ? trim(*existing.public_account.email) : std::string{};
    if (username.empty() || (existing.public_account.type == AccountType::littleskin && username.find('@') == std::string::npos) || !existing.password)
        throw IdentityError(IdentityErrorCode::missing_credentials);
    auto password = impl_->accounts.open(*existing.password);
    auto response = ygg_authenticate(*impl_->http, existing.public_account.type, server, username, password,
                                     op->cancellation.stop_token(), impl_->options.log);
    password.clear();
    impl_->checkpoint(op, stop);
    auto session = std::make_shared<Impl::SkinSession>();
    session->operation = op;
    session->candidate = existing;
    session->candidate.public_account.server = server;
    session->candidate.access_token = std::string(response.access.view());
    session->candidate.client_token = response.client;
    session->expected = snapshot;
    session->original = current_profile(existing.public_account);
    session->profiles = response.available;
    if (session->profiles.empty() && response.selected) session->profiles.push_back(*response.selected);
    if (session->profiles.empty()) throw IdentityError(IdentityErrorCode::no_profile);
    ProfileList selection{"skin_" + random_hex(), session->profiles, op->generation};
    {
        std::lock_guard lock(impl_->gate);
        impl_->check_locked(op, stop);
        op->expires = impl_->now() + impl_->options.skin_session_ttl;
        impl_->skins[selection.session_id] = std::move(session);
        keep = true;
    }
    return selection;
}

void AuthService::skin_cancel(std::string_view id, std::uint64_t generation, std::stop_token stop) {
    std::shared_ptr<Impl::SkinSession> session;
    {
        std::lock_guard lock(impl_->gate);
        const auto found = impl_->skins.find(std::string(id));
        if (found == impl_->skins.end() || found->second->operation->generation != generation) return;
        session = found->second;
        impl_->close_locked(session->operation, true);
        impl_->skins.erase(found);
    }
    session->operation->cancellation.cancel();
    if (!session->expected || !session->original) return;
    // Serializes with a selection already at the server; reauthenticate if its
    // cancelled request rotated the token without returning that rotation.
    std::lock_guard lane(session->mutation_lane);
    try {
        auto op = impl_->begin(session->expected->account.public_account.id);
        ScopeExit cleanup{[&] { impl_->close(op); }};
        std::stop_callback link(stop, [this, op] { impl_->cancel_operation(op); });
        const auto current = impl_->accounts.snapshot(session->expected->account.public_account.id);
        if (current.revision != session->expected->revision) throw IdentityError(IdentityErrorCode::stale_commit);
        auto candidate = session->expected->account;
        const auto server = auth_server(candidate.public_account.type, candidate.public_account.server, true);
        Secret access;
        std::string client;
        if (candidate.password && candidate.public_account.email) {
            auto password = impl_->accounts.open(*candidate.password);
            auto auth = ygg_authenticate(*impl_->http, candidate.public_account.type, server,
                *candidate.public_account.email, password, op->cancellation.stop_token(), impl_->options.log);
            access = std::move(auth.access);
            client = std::move(auth.client);
        } else {
            access = account_token(impl_->accounts, session->candidate.access_token);
            client = session->candidate.client_token.value_or("");
        }
        auto response = ygg_refresh(*impl_->http, candidate.public_account.type, server, access, client,
            session->original, op->cancellation.stop_token(), impl_->options.log);
        merge_ygg(candidate, response, session->original);
        impl_->commit(op, std::move(candidate), session->expected, false, stop);
        safe_log(impl_->options.log, "ygg.cancel_restore", "success");
    } catch (const IdentityError&) { safe_log(impl_->options.log, "ygg.cancel_restore", "restore_failed"); }
}

PublicAccount AuthService::skin_refresh(std::string_view id, std::stop_token stop) {
    auto op = impl_->begin(std::string(id));
    ScopeExit cleanup{[&] { impl_->close(op); }};
    std::stop_callback link(stop, [this, op] { impl_->cancel_operation(op); });
    const auto snapshot = impl_->accounts.snapshot(id);
    auto updated = snapshot.account;
    require_skin(updated.public_account);
    const auto server = auth_server(updated.public_account.type, updated.public_account.server, true);
    const auto selected = current_profile(updated.public_account);
    YggResponse response;
    try {
        auto access = account_token(impl_->accounts, updated.access_token);
        response = ygg_refresh(*impl_->http, updated.public_account.type, server, access,
            updated.client_token.value_or(""), selected, op->cancellation.stop_token(), impl_->options.log);
    } catch (const IdentityError& error) {
        if ((error.code() != IdentityErrorCode::unauthorized && error.code() != IdentityErrorCode::missing_credentials) ||
            !updated.password || !updated.public_account.email) throw;
        impl_->checkpoint(op, stop);
        auto password = impl_->accounts.open(*updated.password);
        auto authenticated = ygg_authenticate(*impl_->http, updated.public_account.type, server,
            *updated.public_account.email, password, op->cancellation.stop_token(), impl_->options.log);
        response = ygg_refresh(*impl_->http, updated.public_account.type, server, authenticated.access,
            authenticated.client, selected, op->cancellation.stop_token(), impl_->options.log);
    }
    merge_ygg(updated, response, selected);
    updated.public_account.server = server;
    return impl_->commit(op, std::move(updated), snapshot, false, stop);
}

bool AuthService::skin_validate(std::string_view id, std::stop_token stop) {
    auto op = impl_->begin(std::string(id));
    ScopeExit cleanup{[&] { impl_->close(op); }};
    std::stop_callback link(stop, [this, op] { impl_->cancel_operation(op); });
    const auto snapshot = impl_->accounts.snapshot(id);
    require_skin(snapshot.account.public_account);
    if (!snapshot.account.access_token || snapshot.account.access_token->empty()) return false;
    auto access = account_token(impl_->accounts, snapshot.account.access_token);
    const auto server = auth_server(snapshot.account.public_account.type, snapshot.account.public_account.server, true);
    Json body{{"accessToken", access.view()}};
    EraseJson erase{body};
    if (snapshot.account.client_token) body["clientToken"] = *snapshot.account.client_token;
    const auto response = post_json(*impl_->http,
        join_url(authentication_base(snapshot.account.public_account.type, server), "/authserver/validate"), body,
        op->cancellation.stop_token(), impl_->options.log, "ygg.validate");
    impl_->checkpoint(op, stop);
    if (response.status == 200 || response.status == 204) return true;
    if (response.status == 401 || response.status == 403) return false;
    ygg_require_success(response);
    return false;
}

void AuthService::skin_invalidate(std::string_view id, std::stop_token stop) {
    auto op = impl_->begin(std::string(id));
    ScopeExit cleanup{[&] { impl_->close(op); }};
    std::stop_callback link(stop, [this, op] { impl_->cancel_operation(op); });
    const auto snapshot = impl_->accounts.snapshot(id);
    auto updated = snapshot.account;
    require_skin(updated.public_account);
    auto access = account_token(impl_->accounts, updated.access_token);
    const auto server = auth_server(updated.public_account.type, updated.public_account.server, true);
    Json body{{"accessToken", access.view()}};
    EraseJson erase{body};
    if (updated.client_token) body["clientToken"] = *updated.client_token;
    auto response = post_json(*impl_->http, join_url(authentication_base(updated.public_account.type, server), "/authserver/invalidate"),
        body, op->cancellation.stop_token(), impl_->options.log, "ygg.invalidate");
    EraseJson erase_response{response.body};
    ygg_require_success(response);
    updated.access_token.reset();
    updated.client_token.reset();
    updated.refresh_token.reset();
    updated.expires_at.reset();
    impl_->commit(op, std::move(updated), snapshot, false, stop);
}

void AuthService::skin_signout(const std::optional<std::string>& input, std::string_view user,
                               Secret password, std::stop_token stop) {
    const auto server = auth_server(AccountType::littleskin, input);
    const auto username = trim(user);
    if (username.empty() || password.empty()) throw IdentityError(IdentityErrorCode::invalid_input);
    auto op = impl_->begin();
    ScopeExit cleanup{[&] { impl_->close(op); }};
    std::stop_callback link(stop, [this, op] { impl_->cancel_operation(op); });
    Json body{{"username", username}, {"password", password.view()}};
    EraseJson erase{body};
    auto response = post_json(*impl_->http, join_url(yggdrasil_base(server), "/authserver/signout"), body,
        op->cancellation.stop_token(), impl_->options.log, "ygg.signout");
    EraseJson erase_response{response.body};
    ygg_require_success(response);
    impl_->checkpoint(op, stop);
}

LaunchIdentity AuthService::prepare_launch(std::string_view id, const std::optional<MsaConfig>& config, std::stop_token stop) {
    check_stop(stop);
    auto snapshot = impl_->accounts.snapshot(id);
    const auto stored_uuid = trim(snapshot.account.public_account.uuid);
    if (!stored_uuid.empty() && !valid_profile_id(stored_uuid))
        throw IdentityError(IdentityErrorCode::invalid_profile);
    if (snapshot.account.public_account.type == AccountType::microsoft) {
        const auto now = impl_->wall_secs();
        const auto skew = static_cast<std::uint64_t>(impl_->options.refresh_skew.count());
        const bool expired = !snapshot.account.expires_at || *snapshot.account.expires_at <= now ||
                             *snapshot.account.expires_at - now <= skew;
        if (expired || !snapshot.account.access_token || snapshot.account.access_token->empty()) {
            if (!config) throw IdentityError(IdentityErrorCode::missing_credentials);
            microsoft_refresh(id, *config, stop);
            snapshot = impl_->accounts.snapshot(id);
        }
    } else if (snapshot.account.public_account.type != AccountType::offline) {
        if (!skin_validate(id, stop)) skin_refresh(id, stop);
        snapshot = impl_->accounts.snapshot(id);
    }
    check_stop(stop);
    const auto& public_account = snapshot.account.public_account;
    const auto launch_uuid = trim(public_account.uuid);
    if (!launch_uuid.empty() && !valid_profile_id(launch_uuid))
        throw IdentityError(IdentityErrorCode::invalid_profile);
    LaunchIdentity result;
    result.account_id = public_account.id;
    result.type = public_account.type;
    result.name = public_account.name;
    result.uuid = normalize_uuid(launch_uuid);
    if (trim(result.name).empty()) throw IdentityError(IdentityErrorCode::no_profile);
    if (result.type == AccountType::offline) {
        if (result.uuid.empty()) result.uuid = normalize_uuid(offline_uuid(trim(result.name)));
        result.access_token = Secret("0");
    } else {
        if (result.uuid.empty()) throw IdentityError(IdentityErrorCode::invalid_profile);
        result.access_token = account_token(impl_->accounts, snapshot.account.access_token);
        if (snapshot.account.client_token) result.client_token.emplace(*snapshot.account.client_token);
        if (result.type == AccountType::microsoft) {
            result.user_type = "msa";
            std::lock_guard lock(impl_->gate);
            const auto xuid = impl_->xuids.find(result.account_id);
            if (xuid != impl_->xuids.end()) result.xuid = xuid->second;
        } else {
            result.user_type = "mojang";
            result.yggdrasil_api = authentication_base(result.type, auth_server(result.type, public_account.server, true));
        }
    }
    if (impl_->accounts.snapshot(id).revision != snapshot.revision) throw IdentityError(IdentityErrorCode::stale_commit);
    check_stop(stop);
    return result;
}

void AuthService::cancel_account_operations(std::string_view id) noexcept {
    std::shared_ptr<Impl::Operation> op;
    {
        std::lock_guard lock(impl_->gate);
        const auto found = impl_->account_operations.find(std::string(id));
        if (found == impl_->account_operations.end()) return;
        op = found->second;
        impl_->close_locked(op, true);
        for (auto it = impl_->skins.begin(); it != impl_->skins.end();) {
            if (it->second->operation == op) it = impl_->skins.erase(it); else ++it;
        }
    }
    op->cancellation.cancel();
}

void AuthService::cancel_all() noexcept {
    if (!impl_) return;
    std::vector<std::shared_ptr<Impl::Operation>> ops;
    {
        std::lock_guard lock(impl_->gate);
        for (const auto& [generation, op] : impl_->live) {
            (void)generation;
            op->cancelled = true;
            op->active = false;
            ops.push_back(op);
        }
        impl_->live.clear();
        impl_->devices.clear();
        impl_->skins.clear();
        impl_->account_operations.clear();
    }
    for (const auto& op : ops) op->cancellation.cancel();
}

void AuthService::purge_expired() {
    impl_->purge();
}

} // namespace nickel::accounts
