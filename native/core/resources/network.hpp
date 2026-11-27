#pragma once

#include "types.hpp"

#include <set>

namespace nickel::resources {

struct UrlRules {
    std::set<std::string> exactHosts;
    std::set<std::string> exactOrigins;
    std::set<std::string> testLoopbackOrigins;
    bool allowPublicHttps = false;
    bool allowQuery = true;
    bool allowFragment = false;
};

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::optional<unsigned> port;
    std::string path;
    std::string query;
    std::string fragment;
    bool hasQuery = false;
    bool hasFragment = false;
};

ParsedUrl parseUrl(std::string_view raw);
std::string urlOrigin(std::string_view raw);
std::string checkedUrl(std::string_view raw, const UrlRules& rules);
std::string checkedRawUrl(std::string_view raw, const UrlRules& rules, std::string_view relativeBase = {});
std::string encodeQuery(std::string_view raw);
UrlRules providerApiRules();
UrlRules providerFileRules();
UrlRules descriptionImageRules();
UrlRules externalLinkRules();

struct HttpResponse {
    Bytes bytes;
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string effectiveUrl;
};

struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::map<std::string, std::string> headers;
    Bytes body;
    std::size_t maximumBytes = 8 * 1024 * 1024;
    std::chrono::milliseconds timeout{30000};
    UrlRules rules;
    bool followRedirects = false;
    unsigned maximumRedirects = 3;
    std::string validationScope;
    std::function<void(const HttpResponse&)> validate;
};

// This callback bridges the ONE shared HttpClient; it is not another network client.
// It must enforce DNS/redirect policy and abort the physical transfer on cancellation.
using HttpFetch = std::function<HttpResponse(const HttpRequest&, const Cancellation&)>;

struct CachePolicy {
    std::chrono::milliseconds freshFor{300000};
    std::chrono::milliseconds staleFor{0};
    bool forceRefresh = false;
    bool cache = true;
};

struct CachedResponse {
    std::shared_ptr<const HttpResponse> response;
    Freshness freshness = Freshness::Fresh;
};

struct CacheStats {
    std::size_t bytes = 0;
    std::size_t entries = 0;
    std::size_t inFlight = 0;
    std::uint64_t hits = 0;
    std::uint64_t joins = 0;
    std::uint64_t evictions = 0;
    std::uint64_t physicalRequests = 0;
};

class CachedHttp final {
public:
    explicit CachedHttp(HttpFetch sharedFetch, std::size_t maximumCacheBytes = 32 * 1024 * 1024,
                        unsigned workers = 4, unsigned maximumInFlight = 32);
    ~CachedHttp();
    CachedHttp(const CachedHttp&) = delete;
    CachedHttp& operator=(const CachedHttp&) = delete;
    CachedResponse fetch(HttpRequest request, CachePolicy policy = {}, const Cancellation& cancel = {});
    void clear();
    CacheStats stats() const;
private:
    struct State;
    std::shared_ptr<State> state_;
};

struct ProviderSettings {
    SourcePolicy apiPolicy = SourcePolicy::OfficialFirst;
    SourcePolicy filePolicy = SourcePolicy::OfficialFirst;
    std::string modrinthMirror;
    std::string curseforgeMirror;
    std::string fileMirror;
    std::string curseforgeKey;
    std::string credentialRevision;
    std::set<std::string> testLoopbackOrigins;
};

struct RequestCandidate {
    HttpRequest request;
    bool mirror = false;
};

std::vector<RequestCandidate> apiCandidates(Source source, const std::string& suffix, const ProviderSettings& settings);
std::vector<RequestCandidate> fileCandidates(const std::string& url, const ProviderSettings& settings);
using ResponseValidator = std::function<void(const HttpResponse&)>;
CachedResponse fetchCandidates(CachedHttp& http, std::vector<RequestCandidate> candidates,
                               SourcePolicy policy, CachePolicy cachePolicy, ResponseValidator validate,
                               const Cancellation& cancel = {});
std::string responseHeader(const HttpResponse& response, std::string_view name);

} // namespace nickel::resources
