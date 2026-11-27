#pragma once

#include "accounts.hpp"

namespace nickel { class JsonRepository; class Vault; }

namespace nickel::accounts {

// The supplied owner services must outlive these non-owning adapters.
class RepositoryAccountStorage final : public AccountStorage {
public:
    explicit RepositoryAccountStorage(nickel::JsonRepository& repository);
    std::string read_accounts() override;
    void write_accounts(std::string_view direct_array_json) override;
    std::string read_active() override;
    void write_active(std::string_view id_object_json) override;
    void transaction(const std::function<void()>& callback) override;
    void transaction(const std::function<void()>& callback,
                      const nickel::Cancellation* cancellation) override;
private:
    nickel::JsonRepository& repository_;
};

class FoundationSecretVault final : public SecretVault {
public:
    explicit FoundationSecretVault(nickel::Vault& vault);
    std::string seal(std::string_view plaintext) override;
    std::string open(std::string_view ciphertext) override;
private:
    nickel::Vault& vault_;
};

} // namespace nickel::accounts
