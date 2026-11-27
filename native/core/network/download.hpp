#pragma once

#include "core/network/hash.hpp"
#include "core/network/http.hpp"

namespace nickel::transport {
using FileValidator = std::function<void(const std::filesystem::path&, const Cancellation&)>;
struct DownloadRequest {
    std::vector<RequestCandidate> candidates;
    std::filesystem::path destination;
    std::optional<std::uint64_t> expected_size;
    std::vector<Digest> digests;
    std::uint64_t max_bytes{8ULL * 1024 * 1024 * 1024};
    std::string service;
    TrustPolicy trust{TrustPolicy::PublicHttps};
    RetryPolicy retry{3, std::chrono::milliseconds(250), std::chrono::milliseconds(5000)};
    bool require_digest{true};
    bool reuse_verified_existing{true};
    bool resume{true};
    std::uint32_t parallel_ranges{4};
    std::uint64_t range_threshold{16ULL * 1024 * 1024};
    FileValidator validator;
};
struct DownloadReceipt {
    std::filesystem::path destination;
    std::uint64_t size{};
    std::uint64_t transferred_bytes{};
    std::vector<Digest> digests;
    bool reused_existing{};
    bool used_ranges{};
    bool resumed{};
    std::string source_origin;
};
} // namespace nickel::transport
