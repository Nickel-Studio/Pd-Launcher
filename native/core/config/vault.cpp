#include "core/config/vault.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"
#include "core/common/error.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

namespace nickel {
namespace {
using namespace foundation_detail;
constexpr std::size_t nonce_size = 12;
constexpr std::size_t tag_size = 16;
constexpr std::size_t secret_limit = 1024 * 1024;
constexpr std::string_view prefix = "v1:";

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    return text;
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto remaining = bytes.size() - i;
        const std::uint32_t bits = (static_cast<std::uint32_t>(bytes[i]) << 16) |
            (remaining > 1 ? static_cast<std::uint32_t>(bytes[i + 1]) << 8 : 0) |
            (remaining > 2 ? bytes[i + 2] : 0);
        result.push_back(alphabet[(bits >> 18) & 63]);
        result.push_back(alphabet[(bits >> 12) & 63]);
        result.push_back(remaining > 1 ? alphabet[(bits >> 6) & 63] : '=');
        result.push_back(remaining > 2 ? alphabet[bits & 63] : '=');
    }
    return result;
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
    if (text.empty() || text.size() % 4 != 0 || text.size() > (secret_limit + nonce_size + tag_size + 2) / 3 * 4) {
        fail("CorruptData", "Local credential is damaged; sign in again");
    }
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> result;
    result.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int a = digit(text[i]);
        const int b = digit(text[i + 1]);
        const bool pad_c = text[i + 2] == '=';
        const bool pad_d = text[i + 3] == '=';
        const int c = pad_c ? 0 : digit(text[i + 2]);
        const int d = pad_d ? 0 : digit(text[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (!last && (pad_c || pad_d)) ||
            (pad_c && !pad_d) || (pad_c && (b & 15)) || (!pad_c && pad_d && (c & 3))) {
            fail("CorruptData", "Local credential is damaged; sign in again");
        }
        const auto bits = (static_cast<std::uint32_t>(a) << 18) |
            (static_cast<std::uint32_t>(b) << 12) | (static_cast<std::uint32_t>(c) << 6) | d;
        result.push_back(static_cast<std::uint8_t>(bits >> 16));
        if (!pad_c) result.push_back(static_cast<std::uint8_t>(bits >> 8));
        if (!pad_d) result.push_back(static_cast<std::uint8_t>(bits));
    }
    return result;
}

struct KeyErase {
    VaultKey& key;
    ~KeyErase() { erase_bytes(key); }
};

std::optional<VaultKey> read_key_once(const std::filesystem::path& path) {
    if (path_missing(path)) return std::nullopt;
    auto bytes = read_file_bounded(path, 32);
    struct Erase { std::vector<std::uint8_t>& bytes; ~Erase() { erase_bytes(bytes); } } erase{bytes};
    if (bytes.size() != 32) fail("CorruptData", "Local credential key has an invalid length; sign in again");
    VaultKey key{};
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

std::optional<VaultKey> read_key_with_retry(const std::filesystem::path& path) {
    try { return read_key_once(path); }
    catch (...) {
        const auto original = std::current_exception();
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(15));
            try { if (auto key = read_key_once(path)) return key; } catch (...) {}
        }
        std::rethrow_exception(original);
    }
}

std::optional<VaultKey> retry_existing(const std::filesystem::path& path) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        try { if (auto key = read_key_once(path)) return key; } catch (...) {}
        if (attempt + 1 < 3) std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return std::nullopt;
}

#ifdef _WIN32
class AesGcm {
public:
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::vector<std::uint8_t> object;
    explicit AesGcm(const VaultKey& bytes) {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
            fail("CryptoFailure", "Cannot initialize local credential encryption");
        }
        if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
            fail("CryptoFailure", "Cannot initialize local credential encryption");
        }
        DWORD length = 0, copied = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length),
                              sizeof(length), &copied, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
            fail("CryptoFailure", "Cannot initialize local credential encryption");
        }
        object.resize(length);
        if (BCryptGenerateSymmetricKey(algorithm, &key, object.data(), static_cast<ULONG>(object.size()),
                                       const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0) {
            erase_bytes(object); BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
            fail("CryptoFailure", "Cannot initialize local credential encryption");
        }
    }
    ~AesGcm() {
        if (key) BCryptDestroyKey(key);
        erase_bytes(object);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};
#else
struct AesGcm {
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    AesGcm() { if (!context) fail("CryptoFailure", "Cannot initialize local credential encryption"); }
    ~AesGcm() { EVP_CIPHER_CTX_free(context); }
};
#endif

} // namespace

Vault::Vault(std::filesystem::path config_root) : root_(checked_absolute_path(config_root)) {
    reject_path_links(root_);
}
std::filesystem::path Vault::key_path() const { return root_ / "credentials.key"; }

VaultKey Vault::load_key() const {
    if (auto key = read_key_with_retry(key_path())) return *key;
    fail("NotFound", "Local credential key is missing; sign in again");
}

VaultKey Vault::load_or_create_key() const {
    const auto path = key_path();
    if (auto existing = read_key_with_retry(path)) return *existing;
    DirectoryGuard directory(root_, true);
    if (auto existing = read_key_with_retry(path)) return *existing;
    VaultKey key{};
    KeyErase cleanup_key{key};
    random_bytes(key);
    try {
        if (atomic_create_file(path, key, true)) return key;
        if (auto existing = retry_existing(path)) return *existing;
        fail("AlreadyExists", "Cannot create local credential key; concurrent key is incomplete");
    } catch (const ErrorException& error) {
        if (error.error().code != "Unsupported") {
            const auto original = std::current_exception();
            if (auto existing = retry_existing(path)) return *existing;
            std::rethrow_exception(original);
        }
    }
    // Filesystems without an atomic no-replace rename retain the legacy
    // exclusive-create fallback. A failed writer only removes its own handle.
    try {
        create_file_exclusive(path, key, true);
        directory.verify();
        return key;
    } catch (...) {
        const auto original = std::current_exception();
        if (auto existing = retry_existing(path)) return *existing;
        std::rethrow_exception(original);
    }
}

std::string Vault::seal(std::string_view plaintext) const {
    auto key = load_or_create_key();
    KeyErase cleanup{key};
    return seal_with_key(key, plaintext);
}
std::string Vault::open(std::string_view sealed) const {
    auto key = load_key();
    KeyErase cleanup{key};
    return open_with_key(key, sealed);
}
std::string Vault::seal_with_key(const VaultKey& key, std::string_view plaintext) {
    VaultNonce nonce{};
    random_bytes(nonce);
    return seal_with_nonce(key, nonce, plaintext);
}

std::string Vault::seal_with_nonce(const VaultKey& key, const VaultNonce& nonce, std::string_view plaintext) {
    if (plaintext.size() > secret_limit) fail("LimitExceeded", "Local credential exceeds its size limit");
    if (!valid_utf8(plaintext)) fail("InvalidArgument", "Local credential is not valid UTF-8");
    std::vector<std::uint8_t> payload(nonce_size + plaintext.size() + tag_size);
    std::copy(nonce.begin(), nonce.end(), payload.begin());
    auto* encrypted = payload.data() + nonce_size;
    auto* tag = encrypted + plaintext.size();
#ifdef _WIN32
    AesGcm cipher(key);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data()); info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbTag = tag; info.cbTag = tag_size;
    ULONG written = 0;
    std::uint8_t empty = 0;
    const auto status = BCryptEncrypt(cipher.key,
        plaintext.empty() ? &empty : reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())),
        static_cast<ULONG>(plaintext.size()), &info, nullptr, 0,
        plaintext.empty() ? &empty : encrypted, static_cast<ULONG>(plaintext.size()), &written, 0);
    if (status < 0 || written != plaintext.size()) fail("CryptoFailure", "Cannot encrypt local credential");
#else
    AesGcm cipher;
    int written = 0, tail = 0;
    if (EVP_EncryptInit_ex(cipher.context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_SET_IVLEN, nonce_size, nullptr) != 1 ||
        EVP_EncryptInit_ex(cipher.context, nullptr, nullptr, key.data(), nonce.data()) != 1 ||
        EVP_EncryptUpdate(cipher.context, encrypted, &written,
            reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size())) != 1 ||
        EVP_EncryptFinal_ex(cipher.context, encrypted + written, &tail) != 1 ||
        static_cast<std::size_t>(written + tail) != plaintext.size() ||
        EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_GET_TAG, tag_size, tag) != 1) {
        fail("CryptoFailure", "Cannot encrypt local credential");
    }
#endif
    return std::string(prefix) + base64_encode(payload);
}

std::string Vault::open_with_key(const VaultKey& key, std::string_view sealed) {
    sealed = trim(sealed);
    if (!sealed.starts_with(prefix)) fail("Unsupported", "Local credential format is unknown; sign in again");
    const auto payload = base64_decode(sealed.substr(prefix.size()));
    if (payload.size() < nonce_size + tag_size) fail("CorruptData", "Local credential is damaged; sign in again");
    const auto size = payload.size() - nonce_size - tag_size;
    if (size > secret_limit) fail("LimitExceeded", "Local credential exceeds its size limit");
    std::vector<std::uint8_t> plaintext(size + 1);
    struct PlainErase {
        std::vector<std::uint8_t>& bytes;
        ~PlainErase() { erase_bytes(bytes); }
    } cleanup{plaintext};
#ifdef _WIN32
    AesGcm cipher(key);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(payload.data()); info.cbNonce = nonce_size;
    info.pbTag = const_cast<PUCHAR>(payload.data() + nonce_size + size); info.cbTag = tag_size;
    ULONG written = 0;
    const auto status = BCryptDecrypt(cipher.key, const_cast<PUCHAR>(payload.data() + nonce_size),
        static_cast<ULONG>(size), &info, nullptr, 0, plaintext.data(), static_cast<ULONG>(size), &written, 0);
    if (status < 0 || written != size) fail("AuthenticationFailed", "Local credential expired or key does not match; sign in again");
#else
    AesGcm cipher;
    int written = 0, tail = 0;
    if (EVP_DecryptInit_ex(cipher.context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_SET_IVLEN, nonce_size, nullptr) != 1 ||
        EVP_DecryptInit_ex(cipher.context, nullptr, nullptr, key.data(), payload.data()) != 1 ||
        EVP_DecryptUpdate(cipher.context, plaintext.data(), &written, payload.data() + nonce_size, static_cast<int>(size)) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_SET_TAG, tag_size,
            const_cast<unsigned char*>(payload.data() + nonce_size + size)) != 1 ||
        EVP_DecryptFinal_ex(cipher.context, plaintext.data() + written, &tail) != 1 ||
        static_cast<std::size_t>(written + tail) != size) {
        fail("AuthenticationFailed", "Local credential expired or key does not match; sign in again");
    }
#endif
    std::string result(reinterpret_cast<const char*>(plaintext.data()), size);
    if (!valid_utf8(result)) fail("CorruptData", "Local credential content cannot be decoded; sign in again");
    return result;
}

} // namespace nickel
