#pragma once

#include "accounts.hpp"
#include "http_port.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nickel::transport { class HttpClient; }

namespace nickel::accounts {

// Move-only, no implicit conversion or serialization; streaming always redacts.
class Secret final {
public:
    Secret() = default;
    explicit Secret(std::string_view value);
    ~Secret();
    Secret(Secret&& other) noexcept;
    Secret& operator=(Secret&& other) noexcept;
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    std::string_view view() const noexcept;
    bool empty() const noexcept;
    void clear() noexcept;
private:
    std::vector<char> bytes_;
};
std::ostream& operator<<(std::ostream& out, const Secret& secret);

struct MsaConfig {
    std::string client_id;
    std::string authority = "consumers";
    std::string redirect_uri = "nickel://auth";
};
std::optional<MsaConfig> parse_msa_config(std::string_view json_text);

struct DeviceCodeStart {
    std::string session_id;
    std::string user_code;
    std::string verification_uri;
    std::optional<std::string> verification_uri_complete;
    std::chrono::seconds interval{5};
    std::chrono::seconds expires_in{0};
    std::uint64_t generation = 0;
};

enum class DevicePollStatus { pending, slow_down, success };
struct DevicePollResult {
    DevicePollStatus status = DevicePollStatus::pending;
    std::optional<PublicAccount> account;
    std::chrono::milliseconds retry_after{0};
};

struct SkinProfile {
    std::string id;
    std::string name;
    bool operator==(const SkinProfile&) const = default;
};
struct ProfileList {
    std::string session_id;
    std::vector<SkinProfile> profiles;
    std::uint64_t generation = 0;
};
struct SkinLoginOutcome {
    enum class Status { account, select } status = Status::select;
    std::optional<PublicAccount> account;
    std::optional<ProfileList> selection;
};

struct LaunchIdentity {
    std::string account_id;
    AccountType type = AccountType::offline;
    std::string name;
    std::string uuid;
    std::string user_type = "legacy";
    Secret access_token;
    std::optional<Secret> client_token;
    std::optional<std::string> yggdrasil_api;
    std::string xuid;
};

// Inject clocks in controlled tests; callers schedule retries on their worker.
struct AuthClock {
    std::function<std::chrono::steady_clock::time_point()> monotonic;
    std::function<std::chrono::system_clock::time_point()> wall;
};
struct AuthOptions {
    std::size_t max_sessions = 32;
    std::chrono::seconds skin_session_ttl{600};
    std::chrono::seconds refresh_skew{60};
    AuthClock clock;
    IdentityLog log;
};

class AuthService final {
public:
    AuthService(AccountService& accounts, transport::HttpClient& http,
                AuthOptions options = {});
    AuthService(AccountService& accounts, IdentityHttp& controlled_http,
                AuthOptions options = {});
    AuthService(AccountService& accounts, std::shared_ptr<IdentityHttp> http,
                AuthOptions options = {});
    ~AuthService();
    AuthService(const AuthService&) = delete;
    AuthService& operator=(const AuthService&) = delete;

    DeviceCodeStart device_start(const MsaConfig& config,
                                 std::stop_token stop = {});
    DevicePollResult device_poll(std::string_view session_id,
                                 std::uint64_t generation,
                                 std::stop_token stop = {});
    void device_cancel(std::string_view session_id,
                       std::uint64_t generation) noexcept;
    PublicAccount microsoft_refresh(std::string_view account_id,
                                    const MsaConfig& config,
                                    std::stop_token stop = {});

    SkinLoginOutcome skin_login(const std::optional<std::string>& server,
                                std::string_view username, Secret password,
                                AccountType type = AccountType::littleskin,
                                std::stop_token stop = {});
    PublicAccount skin_select(std::string_view session_id,
                              std::uint64_t generation,
                              std::string_view profile_id,
                              std::stop_token stop = {});
    ProfileList skin_profiles(std::string_view account_id,
                              std::stop_token stop = {});
    void skin_cancel(std::string_view session_id, std::uint64_t generation,
                     std::stop_token stop = {});
    PublicAccount skin_refresh(std::string_view account_id,
                               std::stop_token stop = {});
    bool skin_validate(std::string_view account_id,
                       std::stop_token stop = {});
    void skin_invalidate(std::string_view account_id,
                         std::stop_token stop = {});
    void skin_signout(const std::optional<std::string>& server,
                      std::string_view username, Secret password,
                      std::stop_token stop = {});

    LaunchIdentity prepare_launch(std::string_view account_id,
                                  const std::optional<MsaConfig>& config = std::nullopt,
                                  std::stop_token stop = {});
    void cancel_account_operations(std::string_view account_id) noexcept;
    void cancel_all() noexcept;
    void purge_expired();

    DeviceCodeStart device_start(const MsaConfig& config, const nickel::Cancellation& cancellation) {
        return device_start(config, cancellation.stop_token());
    }
    DevicePollResult device_poll(std::string_view id, std::uint64_t generation,
                                const nickel::Cancellation& cancellation) {
        return device_poll(id, generation, cancellation.stop_token());
    }
    PublicAccount microsoft_refresh(std::string_view id, const MsaConfig& config,
                                    const nickel::Cancellation& cancellation) {
        return microsoft_refresh(id, config, cancellation.stop_token());
    }
    SkinLoginOutcome skin_login(const std::optional<std::string>& server,
                                std::string_view username, Secret password,
                                AccountType type, const nickel::Cancellation& cancellation) {
        return skin_login(server, username, std::move(password), type, cancellation.stop_token());
    }
    SkinLoginOutcome skin_login(const std::optional<std::string>& server,
                                std::string_view username, Secret password,
                                const nickel::Cancellation& cancellation) {
        return skin_login(server, username, std::move(password), AccountType::littleskin,
                          cancellation.stop_token());
    }
    PublicAccount skin_select(std::string_view id, std::uint64_t generation,
                              std::string_view profile_id, const nickel::Cancellation& cancellation) {
        return skin_select(id, generation, profile_id, cancellation.stop_token());
    }
    ProfileList skin_profiles(std::string_view id, const nickel::Cancellation& cancellation) {
        return skin_profiles(id, cancellation.stop_token());
    }
    void skin_cancel(std::string_view id, std::uint64_t generation, const nickel::Cancellation& cancellation) {
        skin_cancel(id, generation, cancellation.stop_token());
    }
    PublicAccount skin_refresh(std::string_view id, const nickel::Cancellation& cancellation) {
        return skin_refresh(id, cancellation.stop_token());
    }
    bool skin_validate(std::string_view id, const nickel::Cancellation& cancellation) {
        return skin_validate(id, cancellation.stop_token());
    }
    void skin_invalidate(std::string_view id, const nickel::Cancellation& cancellation) {
        skin_invalidate(id, cancellation.stop_token());
    }
    void skin_signout(const std::optional<std::string>& server, std::string_view username,
                      Secret password, const nickel::Cancellation& cancellation) {
        skin_signout(server, username, std::move(password), cancellation.stop_token());
    }
    LaunchIdentity prepare_launch(std::string_view id, const std::optional<MsaConfig>& config,
                                  const nickel::Cancellation& cancellation) {
        return prepare_launch(id, config, cancellation.stop_token());
    }
    LaunchIdentity prepare_launch(std::string_view id, const nickel::Cancellation& cancellation) {
        return prepare_launch(id, std::nullopt, cancellation.stop_token());
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nickel::accounts
