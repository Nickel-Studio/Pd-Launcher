#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace nickel {

using VaultKey = std::array<std::uint8_t, 32>;
using VaultNonce = std::array<std::uint8_t, 12>;

class Vault {
public:
    explicit Vault(std::filesystem::path config_root);
    std::filesystem::path key_path() const;
    std::string seal(std::string_view plaintext) const;
    std::string open(std::string_view sealed) const;
    VaultKey load_key() const;
    VaultKey load_or_create_key() const;

    static std::string seal_with_key(const VaultKey& key, std::string_view plaintext);
    // Explicit nonce is for deterministic interoperability fixtures only.
    static std::string seal_with_nonce(const VaultKey& key, const VaultNonce& nonce,
                                       std::string_view plaintext);
    static std::string open_with_key(const VaultKey& key, std::string_view sealed);

private:
    std::filesystem::path root_;
};

} // namespace nickel
