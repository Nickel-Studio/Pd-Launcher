#include "core/config/detail.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace nickel::foundation_detail {

void random_bytes(std::span<std::uint8_t> output) {
    while (!output.empty()) {
        const auto length = std::min<std::size_t>(output.size(), 1024 * 1024);
#ifdef _WIN32
        if (BCryptGenRandom(nullptr, output.data(), static_cast<ULONG>(length), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            fail("CryptoFailure", "System random number generator failed");
        }
#else
        if (RAND_bytes(output.data(), static_cast<int>(length)) != 1) fail("CryptoFailure", "System random number generator failed");
#endif
        output = output.subspan(length);
    }
}

void erase_bytes(std::span<std::uint8_t> bytes) noexcept {
#ifdef _WIN32
    if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
#else
    if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
#endif
}

struct Sha256::Impl {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~Impl() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
#else
    EVP_MD_CTX* hash = nullptr;
    ~Impl() { if (hash) EVP_MD_CTX_free(hash); }
#endif
    bool finished = false;
};
Sha256::Sha256() : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    if (BCryptOpenAlgorithmProvider(&impl_->algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(impl_->algorithm, &impl_->hash, nullptr, 0, nullptr, 0, 0) < 0) {
        fail("CryptoFailure", "Cannot initialize integrity check");
    }
#else
    impl_->hash = EVP_MD_CTX_new();
    if (!impl_->hash || EVP_DigestInit_ex(impl_->hash, EVP_sha256(), nullptr) != 1) {
        fail("CryptoFailure", "Cannot initialize integrity check");
    }
#endif
}
Sha256::~Sha256() = default;
void Sha256::update(std::span<const std::uint8_t> bytes) {
    if (impl_->finished) fail("InvalidArgument", "Integrity check is already complete");
    while (!bytes.empty()) {
        const auto size = std::min<std::size_t>(bytes.size(), 1024 * 1024);
#ifdef _WIN32
        if (BCryptHashData(impl_->hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(size), 0) < 0) {
            fail("CryptoFailure", "Cannot compute integrity check");
        }
#else
        if (EVP_DigestUpdate(impl_->hash, bytes.data(), size) != 1) fail("CryptoFailure", "Cannot compute integrity check");
#endif
        bytes = bytes.subspan(size);
    }
}
std::string Sha256::finish() {
    if (impl_->finished) fail("InvalidArgument", "Integrity check is already complete");
    std::array<std::uint8_t, 32> digest{};
#ifdef _WIN32
    if (BCryptFinishHash(impl_->hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        fail("CryptoFailure", "Cannot complete integrity check");
    }
#else
    unsigned length = 0;
    if (EVP_DigestFinal_ex(impl_->hash, digest.data(), &length) != 1 || length != digest.size()) {
        fail("CryptoFailure", "Cannot complete integrity check");
    }
#endif
    impl_->finished = true;
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : digest) { result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]); }
    return result;
}
std::string sha256_text(std::string_view text) {
    Sha256 hash;
    hash.update({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    return hash.finish();
}

} // namespace nickel::foundation_detail
