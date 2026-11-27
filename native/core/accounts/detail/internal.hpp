#pragma once

#include "../auth.hpp"
#include "util.hpp"

#include <mutex>
#include <unordered_map>

namespace nickel::accounts::detail {

inline constexpr std::size_t account_document_max_bytes = 8 * 1024 * 1024;
inline constexpr std::size_t account_document_max_entries = 4096;

struct StoredAccount {
    PublicAccount public_account;
    std::optional<std::string> access_token;
    std::optional<std::string> client_token;
    std::optional<std::string> refresh_token;
    std::optional<std::uint64_t> expires_at;
    std::optional<std::string> password;
    ~StoredAccount();
    StoredAccount() = default;
    StoredAccount(const StoredAccount&) = default;
    StoredAccount& operator=(const StoredAccount&) = default;
    StoredAccount(StoredAccount&&) noexcept = default;
    StoredAccount& operator=(StoredAccount&&) noexcept = default;
};

struct AccountSnapshot {
    StoredAccount account;
    std::uint64_t revision = 0;
};

Json account_json(const PublicAccount& account);
Json stored_json(const StoredAccount& account);
std::vector<StoredAccount> parse_accounts(std::string_view text);
std::string stored_accounts_json(const std::vector<StoredAccount>& accounts);

struct AccountState {
    AccountStorage& storage;
    SecretVault& vault;
    IdentityLog log;
    mutable std::mutex mutex;
    mutable std::unordered_map<std::string, std::uint64_t> revisions;
    mutable std::unordered_map<std::string, std::array<std::uint8_t, 16>> fingerprints;
    mutable std::uint64_t sequence = 0;

    AccountState(AccountStorage& storage_, SecretVault& vault_, IdentityLog log_);
    void observe_locked(const std::vector<StoredAccount>& accounts) const;
    std::vector<StoredAccount> load_locked() const;
    std::vector<StoredAccount> update_locked(const std::function<void(std::vector<StoredAccount>&)>& mutator);
    AccountSnapshot snapshot(std::string_view id) const;
    PublicAccount commit(StoredAccount incoming,
                         const std::optional<AccountSnapshot>& expected,
                         bool deduplicate);
    std::string seal(std::string_view plaintext);
    Secret open(std::string_view ciphertext);
};

} // namespace nickel::accounts::detail

namespace nickel::accounts {
struct AccountService::Impl final : detail::AccountState {
    using detail::AccountState::AccountState;
};
} // namespace nickel::accounts
