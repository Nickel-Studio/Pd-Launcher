#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::transport {

enum class HashAlgorithm { Sha1, Sha256, Sha512, Md5 };
struct Digest {
    HashAlgorithm algorithm{HashAlgorithm::Sha256};
    std::string hex;
    bool operator==(const Digest&) const = default;
};

namespace detail {
class IncrementalHash {
public:
    explicit IncrementalHash(HashAlgorithm algorithm);
    ~IncrementalHash();
    IncrementalHash(IncrementalHash&&) noexcept;
    IncrementalHash& operator=(IncrementalHash&&) noexcept;
    IncrementalHash(const IncrementalHash&) = delete;
    IncrementalHash& operator=(const IncrementalHash&) = delete;
    void update(std::span<const std::byte> bytes);
    std::string finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void validate_digests(const std::vector<Digest>& digests);
std::vector<Digest> hash_file(const std::filesystem::path& path,
                              const std::vector<HashAlgorithm>& algorithms,
                              const std::function<bool()>& cancelled = {});
bool digest_equal(std::string_view expected, std::string_view actual) noexcept;
} // namespace detail
} // namespace nickel::transport
