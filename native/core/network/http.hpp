#pragma once

#include "core/network/budget.hpp"
#include "core/common/cancellation.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace nickel { class Cancellation; }

namespace nickel::transport {
using Json = nlohmann::json;

enum class HttpMethod { Get, Head, Post, Put, Patch, Delete };
enum class RedirectPolicy { None, SameOrigin, PublicHttps };
enum class TrustPolicy { PublicHttps, ControlledLoopback };
enum class SourcePolicy { OfficialAuto, OfficialFirst, MirrorFirst };
enum class SourceDomain { VersionList, FileDownload, CommunityApi };
struct Header {
    std::string name;
    std::string value;
    bool operator==(const Header&) const = default;
};
using Headers = std::vector<Header>;
struct RetryPolicy {
    std::uint32_t max_attempts{1};
    std::chrono::milliseconds initial_delay{250};
    std::chrono::milliseconds max_delay{5000};
};
struct ProxySnapshot {
    enum class Mode { Direct, HttpConnect, Socks5 };
    Mode mode{Mode::Direct};
    std::string endpoint;
    std::string username;
    std::string password;
    bool allow_loopback_proxy{};
    bool operator==(const ProxySnapshot&) const = default;
};
struct NetworkSnapshot {
    SourcePolicy version_list_source{SourcePolicy::OfficialAuto};
    SourcePolicy file_download_source{SourcePolicy::OfficialAuto};
    SourcePolicy community_api_source{SourcePolicy::OfficialAuto};
    ProxySnapshot proxy;
    std::filesystem::path ca_bundle;
    std::string user_agent{"Nickel/native"};
    std::chrono::milliseconds connect_timeout{10000};
    std::chrono::milliseconds idle_timeout{30000};
    std::chrono::milliseconds total_timeout{120000};
};
struct RequestCandidate {
    std::string url;
    bool mirror{};
    Headers headers;
};
struct HttpRequest {
    HttpMethod method{HttpMethod::Get};
    std::string url;
    Headers headers;
    std::vector<std::byte> body;
    std::uint64_t max_response_bytes{8ULL * 1024 * 1024};
    RedirectPolicy redirect_policy{RedirectPolicy::PublicHttps};
    TrustPolicy trust{TrustPolicy::PublicHttps};
    RetryPolicy retry;
    std::uint32_t max_redirects{8};
    std::vector<std::string> allowed_origins;
    std::optional<std::chrono::milliseconds> timeout;
};
struct ResponseHead {
    int status{};
    Headers headers;
    std::string effective_url;
    std::optional<std::uint64_t> content_length;
    std::uint64_t transferred_bytes{};
    std::string header(std::string_view name) const;
};
struct HttpResponse : ResponseHead {
    std::vector<std::byte> body;
    std::string text() const;
    Json json() const;
};
struct TransferProgress {
    std::uint64_t transferred_bytes{};
    std::uint64_t completed_bytes{};
    std::optional<std::uint64_t> total_bytes;
    double speed_bytes_per_second{};
    std::optional<std::chrono::milliseconds> remaining;
};
using Progress = std::function<void(const TransferProgress&)>;
using ByteSink = std::function<void(std::span<const std::byte>)>;
using HeadValidator = std::function<void(const ResponseHead&)>;

struct CacheOptions {
    std::uint64_t max_bytes{32ULL * 1024 * 1024};
    std::chrono::seconds freshness{60};
    std::uint32_t max_inflight{64};
};
struct CacheStats {
    std::uint64_t resident_bytes{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t coalesced{};
    std::uint32_t entries{};
    std::uint32_t inflight{};
};
struct DownloadRequest;
struct DownloadReceipt;

// Typed DTOs are returned directly; failures use the frozen common error contract.
class HttpClient {
public:
    explicit HttpClient(NetworkSnapshot snapshot = {},
                        std::shared_ptr<SharedBudget> budget = SharedBudget::process(),
                        CacheOptions cache = {});
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    NetworkSnapshot snapshot() const;
    // Apply only after settings persistence; in-flight operations retain their captured snapshot.
    void set_snapshot(NetworkSnapshot snapshot);
    std::shared_ptr<SharedBudget> budget() const;
    HttpResponse perform(const HttpRequest& request, const Cancellation& cancel);
    HttpResponse perform(const HttpRequest& request, std::stop_token cancel = {});
    HttpResponse execute(const HttpRequest& request, const NetworkSnapshot& snapshot,
                         const Cancellation& cancel);
    HttpResponse get(std::string url, const Cancellation& cancel,
                     std::uint64_t max_bytes = 8ULL * 1024 * 1024);
    HttpResponse get_cached(const HttpRequest& request, const Cancellation& cancel);
    HttpResponse fetch_candidates(const std::vector<RequestCandidate>& candidates,
                                  SourceDomain domain, const HttpRequest& options,
                                  const Cancellation& cancel);
    HttpResponse fetch_candidates(const std::vector<RequestCandidate>& candidates,
                                  SourceDomain domain, const HttpRequest& options,
                                  const NetworkSnapshot& snapshot, const Cancellation& cancel);
    ResponseHead stream(const HttpRequest& request, const NetworkSnapshot& snapshot,
                        const Cancellation& cancel, ByteSink sink,
                        Progress progress = {}, HeadValidator validate_head = {});
    DownloadReceipt download(const DownloadRequest& request, const Cancellation& cancel,
                             Progress progress = {});
    DownloadReceipt download(const DownloadRequest& request, const NetworkSnapshot& snapshot,
                             const Cancellation& cancel, Progress progress = {});
    CacheStats cache_stats() const;
    void clear_cache();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::string safe_origin(std::string_view url);
void validate_url(std::string_view url, TrustPolicy trust = TrustPolicy::PublicHttps);
} // namespace nickel::transport

namespace nickel { namespace network = transport; }
