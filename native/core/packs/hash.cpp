#include "internal.hpp"
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

namespace nickel::packs {
std::string hash_file(const fs::path& path, std::string_view algorithm, std::stop_token cancel) {
    using namespace detail;
    io::check_cancelled(cancel);
    NativeFile file(path, NativeFile::Mode::read);
    std::array<unsigned char, copy_buffer_bytes> buffer{};
    std::vector<unsigned char> digest;
#ifdef _WIN32
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<unsigned char> object;
        ~Hash() {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } state;
    const auto name = algorithm == "sha1" ? BCRYPT_SHA1_ALGORITHM : algorithm == "sha512" ? BCRYPT_SHA512_ALGORITHM
        : algorithm == "sha256" ? BCRYPT_SHA256_ALGORITHM : nullptr;
    if (!name) throw Error("unsupported_hash", "Unsupported pack digest");
    if (BCryptOpenAlgorithmProvider(&state.algorithm, name, nullptr, 0) != 0) throw Error("hash", "Cannot open pack hash algorithm");
    DWORD length = 0, written = 0;
    if (BCryptGetProperty(state.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &written, 0) != 0)
        throw Error("hash", "Cannot inspect hash object");
    state.object.resize(length);
    if (BCryptCreateHash(state.algorithm, &state.hash, state.object.data(), length, nullptr, 0, 0) != 0)
        throw Error("hash", "Cannot create pack hash");
    digest.resize(algorithm == "sha1" ? 20 : algorithm == "sha256" ? 32 : 64);
#else
    const auto method = algorithm == "sha1" ? EVP_sha1() : algorithm == "sha512" ? EVP_sha512()
        : algorithm == "sha256" ? EVP_sha256() : nullptr;
    if (!method) throw Error("unsupported_hash", "Unsupported pack digest");
    const auto state = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!state || EVP_DigestInit_ex(state.get(), method, nullptr) != 1) throw Error("hash", "Cannot create pack hash");
    digest.resize(static_cast<std::size_t>(EVP_MD_size(method)));
#endif
    const auto size = file.size();
    std::uint64_t offset = 0;
    while (offset < size) {
        io::check_cancelled(cancel);
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
        if (file.read_at(offset, buffer.data(), count) != count) throw Error("source_changed", "Digest source changed");
#ifdef _WIN32
        if (BCryptHashData(state.hash, buffer.data(), static_cast<ULONG>(count), 0) != 0) throw Error("hash", "Cannot hash pack bytes");
#else
        if (EVP_DigestUpdate(state.get(), buffer.data(), count) != 1) throw Error("hash", "Cannot hash pack bytes");
#endif
        offset += count;
    }
    file.unchanged();
    io::check_cancelled(cancel);
#ifdef _WIN32
    if (BCryptFinishHash(state.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) throw Error("hash", "Cannot finish pack digest");
#else
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(state.get(), digest.data(), &length) != 1 || length != digest.size()) throw Error("hash", "Cannot finish pack digest");
#endif
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
} // namespace nickel::packs
