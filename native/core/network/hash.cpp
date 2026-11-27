#include "core/network/hash.hpp"
#include "core/tasks/secure_fs.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

namespace nickel::transport::detail {
namespace {
std::size_t digest_size(HashAlgorithm algorithm) {
    switch (algorithm) {
    case HashAlgorithm::Sha1: return 20;
    case HashAlgorithm::Sha256: return 32;
    case HashAlgorithm::Sha512: return 64;
    case HashAlgorithm::Md5: return 16;
    }
    throw std::runtime_error("invalid digest algorithm");
}
#ifdef _WIN32
const wchar_t* algorithm_name(HashAlgorithm algorithm) {
    switch (algorithm) {
    case HashAlgorithm::Sha1: return BCRYPT_SHA1_ALGORITHM;
    case HashAlgorithm::Sha256: return BCRYPT_SHA256_ALGORITHM;
    case HashAlgorithm::Sha512: return BCRYPT_SHA512_ALGORITHM;
    case HashAlgorithm::Md5: return BCRYPT_MD5_ALGORITHM;
    }
    throw std::runtime_error("invalid digest algorithm");
}
#else
const EVP_MD* algorithm_type(HashAlgorithm algorithm) {
    switch (algorithm) {
    case HashAlgorithm::Sha1: return EVP_sha1();
    case HashAlgorithm::Sha256: return EVP_sha256();
    case HashAlgorithm::Sha512: return EVP_sha512();
    case HashAlgorithm::Md5: return EVP_md5();
    }
    throw std::runtime_error("invalid digest algorithm");
}
#endif
} // namespace

struct IncrementalHash::Impl {
    HashAlgorithm algorithm;
    bool finished{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE provider{};
    BCRYPT_HASH_HANDLE hash{};
    std::vector<unsigned char> object;
    ~Impl() {
        if (hash) BCryptDestroyHash(hash);
        if (provider) BCryptCloseAlgorithmProvider(provider, 0);
    }
#else
    EVP_MD_CTX* hash{};
    ~Impl() { if (hash) EVP_MD_CTX_free(hash); }
#endif
};

IncrementalHash::IncrementalHash(HashAlgorithm algorithm) : impl_(std::make_unique<Impl>()) {
    impl_->algorithm = algorithm;
#ifdef _WIN32
    if (BCryptOpenAlgorithmProvider(&impl_->provider, algorithm_name(algorithm), nullptr, 0) < 0)
        throw std::runtime_error("CNG hash provider failed");
    DWORD object_size{}, received{};
    if (BCryptGetProperty(impl_->provider, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                          &received, 0) < 0) throw std::runtime_error("CNG hash property failed");
    impl_->object.resize(object_size);
    if (BCryptCreateHash(impl_->provider, &impl_->hash, impl_->object.data(), object_size,
                         nullptr, 0, 0) < 0) throw std::runtime_error("CNG hash creation failed");
#else
    impl_->hash = EVP_MD_CTX_new();
    if (!impl_->hash || EVP_DigestInit_ex(impl_->hash, algorithm_type(algorithm), nullptr) != 1)
        throw std::runtime_error("hash creation failed");
#endif
}
IncrementalHash::~IncrementalHash() = default;
IncrementalHash::IncrementalHash(IncrementalHash&&) noexcept = default;
IncrementalHash& IncrementalHash::operator=(IncrementalHash&&) noexcept = default;
void IncrementalHash::update(std::span<const std::byte> bytes) {
    if (impl_->finished) throw std::runtime_error("digest already finalized");
#ifdef _WIN32
    while (!bytes.empty()) {
        const auto count = static_cast<ULONG>(std::min<std::size_t>(bytes.size(), std::numeric_limits<ULONG>::max()));
        if (BCryptHashData(impl_->hash,
                           reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
                           count, 0) < 0) throw std::runtime_error("CNG hash update failed");
        bytes = bytes.subspan(count);
    }
#else
    if (EVP_DigestUpdate(impl_->hash, bytes.data(), bytes.size()) != 1)
        throw std::runtime_error("hash update failed");
#endif
}
std::string IncrementalHash::finish() {
    if (impl_->finished) throw std::runtime_error("digest already finalized");
    std::array<unsigned char, 64> bytes{};
    const auto count = digest_size(impl_->algorithm);
#ifdef _WIN32
    if (BCryptFinishHash(impl_->hash, bytes.data(), static_cast<ULONG>(count), 0) < 0)
        throw std::runtime_error("CNG hash finalization failed");
#else
    unsigned int actual{};
    if (EVP_DigestFinal_ex(impl_->hash, bytes.data(), &actual) != 1 || actual != count)
        throw std::runtime_error("hash finalization failed");
#endif
    impl_->finished = true;
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t at = 0; at < count; ++at) { out += hex[bytes[at] >> 4]; out += hex[bytes[at] & 15]; }
    return out;
}
void validate_digests(const std::vector<Digest>& digests) {
    std::unordered_set<int> seen;
    for (const auto& digest : digests) {
        if (!seen.insert(static_cast<int>(digest.algorithm)).second ||
            digest.hex.size() != 2 * digest_size(digest.algorithm) ||
            !std::all_of(digest.hex.begin(), digest.hex.end(), [](char value) {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                       (value >= 'A' && value <= 'F');
            })) throw std::runtime_error("malformed or duplicate artifact digest");
    }
}
bool digest_equal(std::string_view expected, std::string_view actual) noexcept {
    if (expected.size() != actual.size()) return false;
    unsigned int difference{};
    const auto lower = [](unsigned char code) {
        return code >= 'A' && code <= 'F' ? static_cast<unsigned char>(code - 'A' + 'a') : code;
    };
    for (std::size_t at = 0; at < expected.size(); ++at)
        difference |= lower(static_cast<unsigned char>(expected[at])) ^
                      lower(static_cast<unsigned char>(actual[at]));
    return difference == 0;
}
std::vector<Digest> hash_file(const std::filesystem::path& path,
                              const std::vector<HashAlgorithm>& algorithms,
                              const std::function<bool()>& cancelled) {
    auto file = tasks::detail::SecureFile::open_read(path);
    std::vector<IncrementalHash> hashes;
    for (auto algorithm : algorithms) hashes.emplace_back(algorithm);
    std::array<std::byte, 64 * 1024> buffer;
    for (;;) {
        if (cancelled && cancelled()) throw std::runtime_error("operation cancelled");
        const auto count = file.read(buffer);
        if (!count) break;
        for (auto& hash : hashes) hash.update(std::span(buffer).first(count));
    }
    std::vector<Digest> out;
    for (std::size_t at = 0; at < hashes.size(); ++at) out.push_back({algorithms[at], hashes[at].finish()});
    return out;
}
} // namespace nickel::transport::detail
