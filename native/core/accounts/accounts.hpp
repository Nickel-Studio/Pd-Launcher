#pragma once

#include "core/common/error.hpp"
#include "core/common/cancellation.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nickel { class JsonRepository; class Vault; }

namespace nickel::accounts {

enum class AccountType { offline, microsoft, littleskin, yggdrasil };
std::string_view account_type_name(AccountType type) noexcept;
AccountType parse_account_type(std::string_view value);

enum class IdentityErrorCode {
    invalid_input, invalid_server, invalid_response, network, storage, vault,
    missing_account, missing_credentials, unauthorized, declined, expired,
    cancelled, busy, stale_commit, no_entitlement, no_profile, invalid_profile,
    xbox_account_missing, xbox_child_account, xbox_region, xbox_banned,
    xbox_verification, quota, unsupported_image, image_too_large,
    untrusted_download
};
std::string_view identity_error_code_name(IdentityErrorCode code) noexcept;

class IdentityError final : public nickel::ErrorException {
public:
    explicit IdentityError(IdentityErrorCode code, int http_status = 0);
    IdentityErrorCode code() const noexcept;
    int http_status() const noexcept;
private:
    IdentityErrorCode code_;
    int http_status_;
};

struct PublicAccount {
    std::string id;
    AccountType type = AccountType::offline;
    std::string name;
    std::string uuid;
    std::optional<std::string> email;
    std::optional<std::string> server;
    std::optional<std::string> avatar_url;
    bool operator==(const PublicAccount&) const = default;
};

// The codec emits only the seven legacy camelCase public fields.
std::string public_account_json(const PublicAccount& account);
std::string public_accounts_json(const std::vector<PublicAccount>& accounts);
// Vendored Rust FNV identity: dashed lowercase, exact UTF-8 bytes, case-sensitive, no trimming.
// Empty input retains the old result; invalid UTF-8, embedded NUL, or >256 bytes throws invalid_input.
std::string offline_uuid(std::string_view utf8_name);
std::string normalize_uuid(std::string_view uuid);
std::string dash_uuid(std::string_view uuid);
std::string validate_server(const std::optional<std::string>& server,
                            bool stored = false);
std::string yggdrasil_base(std::string_view validated_server);
std::string site_base(std::string_view validated_server);

// Production transactions delegate to foundation's cross-process commit/recovery.
class AccountStorage {
public:
    virtual ~AccountStorage() = default;
    virtual std::string read_accounts() = 0;
    virtual void write_accounts(std::string_view direct_array_json) = 0;
    virtual std::string read_active() = 0;
    virtual void write_active(std::string_view id_object_json) = 0;
    virtual void transaction(const std::function<void()>& callback) { callback(); }
    virtual void transaction(const std::function<void()>& callback,
                              const nickel::Cancellation* cancellation) {
        transaction([&] {
            if (cancellation) cancellation->check();
            callback();
            if (cancellation) cancellation->check();
        });
    }
};

class SecretVault {
public:
    virtual ~SecretVault() = default;
    virtual std::string seal(std::string_view plaintext) = 0;
    virtual std::string open(std::string_view ciphertext) = 0;
};

struct SafeIdentityEvent {
    std::string_view stage;
    std::string_view outcome;
    int http_status = 0;
};
using IdentityLog = std::function<void(const SafeIdentityEvent&)>;

class AuthService;
class AccountService final {
public:
    AccountService(nickel::JsonRepository& repository, nickel::Vault& vault,
                   IdentityLog log = {});
    AccountService(AccountStorage& storage, SecretVault& vault,
                   IdentityLog log = {});
    ~AccountService();
    AccountService(const AccountService&) = delete;
    AccountService& operator=(const AccountService&) = delete;

    std::vector<PublicAccount> list() const;
    std::optional<PublicAccount> find(std::string_view id) const;
    // Preserve valid stored UUID bytes; equivalent formatting is accepted, identity changes rejected.
    // Invalid nonempty stored UUIDs are not silently repaired by an ordinary update or launch.
    PublicAccount upsert_public(PublicAccount account);
    PublicAccount add_offline(std::string_view utf8_name,
                              std::optional<std::string> id = std::nullopt);
    std::vector<PublicAccount> remove(std::string_view id);
    std::string active_id() const;
    std::optional<PublicAccount> active() const;
    void set_active(std::string_view id);
    // Owns account-mutex -> repository lock ordering. The callback may only
    // read/write sibling documents through the SAME repository; no reentry,
    // HTTP, filesystem effects or service configuration before durable return.
    void set_active(std::string_view id,
                     const std::function<void()>& sibling_commit_callback,
                     const nickel::Cancellation& cancellation);

private:
    struct Impl;
    std::unique_ptr<AccountStorage> owned_storage_;
    std::unique_ptr<SecretVault> owned_vault_;
    std::unique_ptr<Impl> impl_;
    friend class AuthService;
};

} // namespace nickel::accounts
