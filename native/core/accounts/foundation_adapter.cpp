#include "foundation_adapter.hpp"
#include "detail/internal.hpp"
#include "core/config/repository.hpp"
#include "core/config/vault.hpp"

#include <utility>

namespace nickel::accounts {

static_assert(detail::account_document_max_bytes == nickel::accounts_max_bytes);

RepositoryAccountStorage::RepositoryAccountStorage(nickel::JsonRepository& repository)
    : repository_(repository) {}
std::string RepositoryAccountStorage::read_accounts() {
    return repository_.read_text("accounts.json", detail::account_document_max_bytes).value_or("[]");
}
void RepositoryAccountStorage::write_accounts(std::string_view text) {
    repository_.write_text("accounts.json", text, detail::account_document_max_bytes);
}
std::string RepositoryAccountStorage::read_active() {
    return repository_.read_text("active_account.json", 4096).value_or("{\"id\":\"\"}");
}
void RepositoryAccountStorage::write_active(std::string_view text) {
    repository_.write_text("active_account.json", text, 4096);
}
void RepositoryAccountStorage::transaction(const std::function<void()>& callback) {
    transaction(callback, nullptr);
}
void RepositoryAccountStorage::transaction(const std::function<void()>& callback,
                                           const nickel::Cancellation* cancellation) {
    try { repository_.transaction(callback, cancellation); }
    catch (const IdentityError&) { throw; }
    catch (const nickel::ErrorException& error) {
        if (error.error().code == "Conflict") throw IdentityError(IdentityErrorCode::stale_commit);
        if (error.error().code == "Cancelled" || error.error().code == "cancelled")
            throw IdentityError(IdentityErrorCode::cancelled);
        throw IdentityError(IdentityErrorCode::storage);
    }
    catch (...) { throw IdentityError(IdentityErrorCode::storage); }
}

FoundationSecretVault::FoundationSecretVault(nickel::Vault& vault) : vault_(vault) {}
std::string FoundationSecretVault::seal(std::string_view plaintext) { return vault_.seal(plaintext); }
std::string FoundationSecretVault::open(std::string_view ciphertext) { return vault_.open(ciphertext); }

AccountService::AccountService(nickel::JsonRepository& repository, nickel::Vault& vault, IdentityLog log)
    : owned_storage_(std::make_unique<RepositoryAccountStorage>(repository)),
      owned_vault_(std::make_unique<FoundationSecretVault>(vault)),
      impl_(std::make_unique<Impl>(*owned_storage_, *owned_vault_, std::move(log))) {}

} // namespace nickel::accounts
