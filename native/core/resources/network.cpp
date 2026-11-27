#include "network.hpp"

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

namespace nickel::resources {
namespace {
using Clock = std::chrono::steady_clock;

bool unsafeText(std::string_view text, bool rejectSpace = false) {
    for (const auto byte : text) {
        const auto c = std::uint8_t(byte);
        if (c < 0x20 || c == 0x7f || c == '\\' || (rejectSpace && c == ' ')) return true;
    }
    return !validUtf8(text);
}

unsigned hexDigit(char c) {
    if (c >= '0' && c <= '9') return unsigned(c - '0');
    if (c >= 'a' && c <= 'f') return unsigned(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return unsigned(c - 'A' + 10);
    return 16;
}

std::string percentDecoded(std::string_view raw) {
    std::string result;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '%') { result += raw[i]; continue; }
        if (i + 2 >= raw.size() || hexDigit(raw[i + 1]) >= 16 || hexDigit(raw[i + 2]) >= 16) fail("policy_denied", "Invalid URL escape");
        result += char(hexDigit(raw[i + 1]) * 16 + hexDigit(raw[i + 2]));
        i += 2;
    }
    return result;
}

bool loopbackHost(std::string_view host) { return host == "127.0.0.1" || host == "[::1]"; }

bool publicHost(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.find('.') == std::string_view::npos || host.back() == '.' || host.front() == '[') return false;
    for (const auto suffix : {".localhost", ".local", ".internal", ".lan", ".home", ".invalid", ".test"}) if (host.ends_with(suffix)) return false;
    bool hasLetter = false;
    while (!host.empty()) {
        const auto dot = host.find('.');
        const auto part = host.substr(0, dot);
        if (part.empty() || part.size() > 63 || part.front() == '-' || part.back() == '-') return false;
        for (const auto c : part) {
            if (c >= 'a' && c <= 'z') hasLetter = true;
            else if (!(c >= '0' && c <= '9') && c != '-') return false;
        }
        if (dot == std::string_view::npos) break;
        host.remove_prefix(dot + 1);
    }
    return hasLetter;
}

std::string origin(const ParsedUrl& url) {
    auto result = url.scheme + "://" + url.host;
    if (url.port && !(url.scheme == "https" && *url.port == 443)) result += ':' + std::to_string(*url.port);
    return result;
}

std::string serialize(const ParsedUrl& url) {
    auto result = origin(url) + url.path;
    if (url.hasQuery) result += '?' + url.query;
    if (url.hasFragment) result += '#' + url.fragment;
    return result;
}

std::string normalizePath(std::string_view path) {
    Strings parts;
    const bool trailing = path.ends_with('/');
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.emplace_back(part);
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
    std::string result = "/";
    for (std::size_t i = 0; i < parts.size(); ++i) { if (i) result += '/'; result += parts[i]; }
    if (trailing && result.back() != '/') result += '/';
    return result;
}

std::string cacheKey(const HttpRequest& request, bool cache) {
    Json policy = {request.rules.exactHosts, request.rules.exactOrigins, request.rules.testLoopbackOrigins,
                   request.rules.allowPublicHttps, request.rules.allowQuery, request.rules.allowFragment};
    return Json::array({request.method, request.url, request.headers, request.body, request.maximumBytes,
                        request.timeout.count(), request.followRedirects, request.maximumRedirects,
                        request.validationScope, policy, cache}).dump();
}

struct Directives {
    bool store = true;
    bool mustRevalidate = false;
    std::optional<std::chrono::milliseconds> maximumAge;
    std::chrono::milliseconds age{0};
};

std::optional<std::uint64_t> unsignedValue(std::string_view raw) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == raw.data() + raw.size() ? std::optional{value} : std::nullopt;
}

Directives directives(const HttpResponse& response) {
    Directives result;
    auto control = lower(responseHeader(response, "cache-control"));
    bool revalidate = false;
    std::string_view remaining = control;
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        auto item = trim(remaining.substr(0, comma));
        if (item == "no-store") result.store = false;
        if (item == "no-cache") revalidate = true;
        if (item == "must-revalidate") result.mustRevalidate = true;
        if (item.starts_with("max-age=")) {
            auto value = item.substr(8);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            if (const auto seconds = unsignedValue(value)) result.maximumAge = std::chrono::milliseconds(std::min<std::uint64_t>(*seconds, 86400) * 1000);
        }
        if (comma == std::string_view::npos) break;
        remaining.remove_prefix(comma + 1);
    }
    if (revalidate) result.maximumAge = std::chrono::milliseconds(0);
    if (responseHeader(response, "vary") == "*") result.store = false;
    if (const auto age = unsignedValue(responseHeader(response, "age"))) result.age = std::chrono::milliseconds(std::min<std::uint64_t>(*age, 86400 * 365) * 1000);
    return result;
}

std::size_t responseCharge(const HttpResponse& response, const std::string& key) {
    std::size_t charge = response.bytes.capacity() + response.effectiveUrl.capacity() + key.capacity() + 512;
    for (const auto& [name, value] : response.headers) charge += name.capacity() + value.capacity() + 96;
    return charge;
}

bool staleAllowed(const Error& error) {
    return error.code == "network" || error.code == "timeout" || error.code == "http_error";
}

} // namespace

ParsedUrl parseUrl(std::string_view raw) {
    if (raw.empty() || raw.size() > 4096 || trim(raw) != raw || unsafeText(raw, true) || raw.starts_with("//")) fail("policy_denied", "URL rejected by resource policy");
    auto decoded = percentDecoded(raw);
    if (unsafeText(decoded) || decoded.starts_with("//")) fail("policy_denied", "URL contains unsafe escaped characters");
    if (decoded.find('%') != std::string::npos) {
        decoded = percentDecoded(decoded);
        if (unsafeText(decoded) || decoded.starts_with("//")) fail("policy_denied", "URL contains unsafe nested escapes");
    }
    const auto separator = raw.find("://");
    if (separator == std::string_view::npos) fail("policy_denied", "Only absolute HTTP URLs are accepted");
    ParsedUrl result;
    result.scheme = lower(raw.substr(0, separator));
    if (result.scheme != "https" && result.scheme != "http") fail("policy_denied", "Unsupported URL scheme");
    raw.remove_prefix(separator + 3);
    const auto end = raw.find_first_of("/?#");
    auto authority = raw.substr(0, end);
    if (authority.empty() || authority.find('@') != std::string_view::npos || authority.find('%') != std::string_view::npos) fail("policy_denied", "URL credentials or escaped host are forbidden");
    std::string_view port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) fail("policy_denied", "Invalid URL host");
        result.host = lower(authority.substr(0, close + 1));
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') fail("policy_denied", "Invalid URL authority");
            port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.find(':');
        result.host = lower(authority.substr(0, colon));
        if (colon != std::string_view::npos) port = authority.substr(colon + 1);
    }
    if ((!authority.empty() && authority.back() == ':') || result.host.empty()) fail("policy_denied", "Invalid URL authority");
    if (!port.empty()) {
        const auto value = unsignedValue(port);
        if (!value || !*value || *value > 65535) fail("policy_denied", "Invalid URL port");
        result.port = unsigned(*value);
    }
    raw = end == std::string_view::npos ? std::string_view{} : raw.substr(end);
    const auto hash = raw.find('#');
    if (hash != std::string_view::npos) { result.hasFragment = true; result.fragment = std::string(raw.substr(hash + 1)); raw = raw.substr(0, hash); }
    const auto query = raw.find('?');
    if (query != std::string_view::npos) { result.hasQuery = true; result.query = std::string(raw.substr(query + 1)); raw = raw.substr(0, query); }
    result.path = raw.empty() ? "/" : std::string(raw);
    if (result.path.front() != '/') fail("policy_denied", "Invalid URL path");
    return result;
}

std::string urlOrigin(std::string_view raw) { return origin(parseUrl(raw)); }

std::string checkedUrl(std::string_view raw, const UrlRules& rules) {
    const auto url = parseUrl(raw);
    if ((!rules.allowQuery && url.hasQuery) || (!rules.allowFragment && url.hasFragment)) fail("policy_denied", "URL query or fragment rejected");
    const auto canonicalOrigin = origin(url);
    if (url.scheme == "http" && url.port && loopbackHost(url.host) && rules.testLoopbackOrigins.contains(canonicalOrigin)) return serialize(url);
    if (url.scheme != "https" || (url.port && *url.port != 443) || !publicHost(url.host)) fail("policy_denied", "Non-public HTTPS resource rejected");
    if (!rules.allowPublicHttps && !rules.exactHosts.contains(url.host) && !rules.exactOrigins.contains(canonicalOrigin)) fail("policy_denied", "Resource origin is not approved");
    return serialize(url);
}

std::string checkedRawUrl(std::string_view raw, const UrlRules& rules, std::string_view base) {
    if (raw.empty() || raw.size() > 4096 || trim(raw) != raw || unsafeText(raw, true) || raw.starts_with("//")) fail("policy_denied", "Unsafe relative resource URL");
    const auto firstDelimiter = raw.find_first_of("/?#");
    const auto colon = raw.find(':');
    if (colon != std::string_view::npos && (firstDelimiter == std::string_view::npos || colon < firstDelimiter)) return checkedUrl(raw, rules);
    if (base.empty()) fail("policy_denied", "Relative resource has no approved remote base");
    auto baseRules = externalLinkRules();
    baseRules.testLoopbackOrigins = rules.testLoopbackOrigins;
    auto url = parseUrl(checkedUrl(base, baseRules));
    std::string_view path = raw;
    const auto hash = path.find('#');
    url.hasFragment = hash != std::string_view::npos;
    url.fragment = url.hasFragment ? std::string(path.substr(hash + 1)) : std::string{};
    if (hash != std::string_view::npos) path = path.substr(0, hash);
    const auto query = path.find('?');
    url.hasQuery = query != std::string_view::npos;
    url.query = url.hasQuery ? std::string(path.substr(query + 1)) : std::string{};
    if (query != std::string_view::npos) path = path.substr(0, query);
    if (!path.empty()) {
        if (path.front() == '/') url.path = normalizePath(path);
        else url.path = normalizePath(url.path.substr(0, url.path.rfind('/') + 1) + std::string(path));
    }
    return checkedUrl(serialize(url), rules);
}

std::string encodeQuery(std::string_view raw) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const auto byte : raw) {
        const auto c = std::uint8_t(byte);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += char(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}

UrlRules providerApiRules() { UrlRules rules; rules.exactHosts = {"api.modrinth.com", "api.curseforge.com"}; return rules; }
UrlRules providerFileRules() { UrlRules rules; rules.exactHosts = {"cdn.modrinth.com", "edge.forgecdn.net", "media.forgecdn.net", "mediafilez.forgecdn.net"}; return rules; }
UrlRules descriptionImageRules() {
    auto rules = providerFileRules();
    for (const auto host : {"raw.githubusercontent.com", "user-images.githubusercontent.com", "github.com", "images.weserv.nl", "i.imgur.com"}) rules.exactHosts.insert(host);
    return rules;
}
UrlRules externalLinkRules() { UrlRules rules; rules.allowPublicHttps = true; rules.allowFragment = true; return rules; }

std::string responseHeader(const HttpResponse& response, std::string_view name) {
    for (const auto& [key, value] : response.headers) if (lower(key) == lower(name)) return value;
    return {};
}

struct CachedHttp::State {
    struct Entry {
        std::shared_ptr<const HttpResponse> response;
        Clock::time_point at;
        Directives policy;
        std::size_t charge = 0;
        std::uint64_t used = 0;
    };
    struct Flight {
        HttpRequest request;
        CachePolicy policy;
        Cancellation physical;
        std::optional<Entry> previous;
        std::shared_ptr<const HttpResponse> response;
        std::optional<Error> error;
        Freshness freshness = Freshness::Fresh;
        std::size_t subscribers = 0;
        std::uint64_t generation = 0;
        bool done = false;
        std::condition_variable_any changed;
    };
    HttpFetch fetch;
    std::size_t maximumBytes;
    unsigned maximumFlights;
    mutable std::mutex mutex;
    std::condition_variable available;
    std::map<std::string, Entry> cache;
    std::map<std::string, std::shared_ptr<Flight>> flights;
    std::deque<std::pair<std::string, std::shared_ptr<Flight>>> queue;
    std::vector<std::jthread> workers;
    CacheStats counters;
    std::uint64_t tick = 0;
    std::uint64_t generation = 0;
    bool stopping = false;

    State(HttpFetch sharedFetch, std::size_t maximum, unsigned count, unsigned maxFlights)
        : fetch(std::move(sharedFetch)), maximumBytes(std::min<std::size_t>(maximum, 128 * 1024 * 1024)), maximumFlights(std::clamp(maxFlights, 1U, 64U)) {
        if (!fetch) fail("invalid_input", "Shared HTTP callback is required");
        workers.reserve(std::clamp(count, 1U, 8U));
        try { for (unsigned i = 0; i < std::clamp(count, 1U, 8U); ++i) workers.emplace_back([this] { run(); }); }
        catch (...) { shutdown(); throw; }
    }

    ~State() { shutdown(); }

    void shutdown() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            cancelFlights();
        }
        available.notify_all();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    }

    void cancelFlights() {
        for (auto& [key, flight] : flights) {
            (void)key;
            flight->physical.cancel();
            flight->error = Error{"cancelled", "Cached request cancelled"};
            flight->done = true;
            flight->changed.notify_all();
        }
        flights.clear();
        queue.clear();
    }

    void eraseCache(const std::string& key) {
        const auto found = cache.find(key);
        if (found != cache.end()) { counters.bytes -= found->second.charge; cache.erase(found); }
    }

    void store(const std::string& key, const std::shared_ptr<const HttpResponse>& response) {
        const auto policy = directives(*response);
        eraseCache(key);
        const auto charge = responseCharge(*response, key);
        if (!policy.store || charge > maximumBytes) return;
        while (charge > maximumBytes - counters.bytes && !cache.empty()) {
            const auto victim = std::min_element(cache.begin(), cache.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            counters.bytes -= victim->second.charge;
            cache.erase(victim);
            ++counters.evictions;
        }
        cache.emplace(key, Entry{response, Clock::now(), policy, charge, ++tick});
        counters.bytes += charge;
    }

    void run() {
        for (;;) {
            std::string key;
            std::shared_ptr<Flight> flight;
            {
                std::unique_lock lock(mutex);
                available.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping) return;
                key = std::move(queue.front().first);
                flight = std::move(queue.front().second);
                queue.pop_front();
                if (flight->done || flight->physical.cancelled()) continue;
                ++counters.physicalRequests;
            }
            std::shared_ptr<const HttpResponse> response;
            std::optional<Error> error;
            auto freshness = Freshness::Fresh;
            try {
                auto request = flight->request;
                if (flight->previous && flight->policy.cache && request.method == "GET") {
                    const auto etag = responseHeader(*flight->previous->response, "etag");
                    const auto modified = responseHeader(*flight->previous->response, "last-modified");
                    if (!etag.empty()) request.headers.insert_or_assign("if-none-match", etag);
                    else if (!modified.empty()) request.headers.insert_or_assign("if-modified-since", modified);
                }
                auto result = fetch(request, flight->physical);
                flight->physical.check();
                const auto effective = result.effectiveUrl.empty() ? request.url : checkedUrl(result.effectiveUrl, request.rules);
                if (!request.followRedirects && effective != request.url) fail("policy_denied", "Unexpected resource redirect");
                if (result.status == 304) {
                    if (!flight->previous) fail("invalid_response", "Unexpected cache revalidation");
                    auto previous = *flight->previous->response;
                    for (const auto& [name, value] : result.headers) previous.headers.insert_or_assign(lower(name), value);
                    previous.headers.erase("age");
                    result = std::move(previous);
                    freshness = Freshness::Revalidated;
                }
                if (result.status < 200 || result.status >= 300) fail(result.status == 404 ? "not_found" : "http_error", "Provider HTTP request failed", {{"httpStatus", result.status}});
                if (result.bytes.size() > request.maximumBytes) fail("too_large", "Response exceeded resource byte budget");
                if (request.validate) request.validate(result);
                response = std::make_shared<const HttpResponse>(std::move(result));
            } catch (const ErrorException& failure) { error = failure.error(); }
              catch (const std::exception&) { error = Error{"network", "Shared HTTP request failed"}; }
              catch (...) { error = Error{"network", "Shared HTTP request failed"}; }
            {
                std::lock_guard lock(mutex);
                if (flight->done) continue;
                if (response && flight->subscribers && flight->generation == generation && flight->policy.cache && flight->request.method == "GET") store(key, response);
                flight->response = std::move(response);
                flight->error = std::move(error);
                flight->freshness = freshness;
                flight->done = true;
                const auto active = flights.find(key);
                if (active != flights.end() && active->second == flight) flights.erase(active);
                flight->changed.notify_all();
            }
        }
    }
};

CachedHttp::CachedHttp(HttpFetch sharedFetch, std::size_t maximumBytes, unsigned workers, unsigned maximumInFlight)
    : state_(std::make_shared<State>(std::move(sharedFetch), maximumBytes, workers, maximumInFlight)) {}
CachedHttp::~CachedHttp() { state_->shutdown(); }

CachedResponse CachedHttp::fetch(HttpRequest request, CachePolicy policy, const Cancellation& cancel) {
    cancel.check();
    request.url = checkedUrl(request.url, request.rules);
    if (!request.maximumBytes || request.maximumBytes > 8 * 1024 * 1024 || request.body.size() > 256 * 1024 ||
        request.timeout.count() <= 0 || request.timeout.count() > 300000 || request.maximumRedirects > 8 ||
        (request.method != "GET" && request.method != "POST") || policy.freshFor.count() < 0 || policy.freshFor.count() > 86400000 ||
        policy.staleFor.count() < 0 || policy.staleFor.count() > 86400000 || request.validationScope.size() > 1024 ||
        (request.validate && request.validationScope.empty()) || request.headers.size() > 64)
        fail("invalid_input", "Invalid bounded resource HTTP request");
    std::size_t headerBytes = 0;
    std::map<std::string, std::string> headers;
    for (const auto& [name, value] : request.headers) {
        headerBytes += name.size() + value.size();
        if (name.empty() || unsafeText(name, true) || unsafeText(value)) fail("invalid_input", "Invalid request header");
        headers.insert_or_assign(lower(name), value);
    }
    if (headerBytes > 16384) fail("too_large", "HTTP headers exceed budget");
    request.headers = std::move(headers);
    const auto key = cacheKey(request, policy.cache);
    auto state = state_;
    std::shared_ptr<State::Flight> flight;
    std::optional<State::Entry> previous;
    std::unique_lock lock(state->mutex);
    if (state->stopping) fail("cancelled", "Cache service stopped");
    if (policy.cache) {
        const auto found = state->cache.find(key);
        if (found != state->cache.end()) {
            found->second.used = ++state->tick;
            previous = found->second;
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - previous->at) + previous->policy.age;
            const auto fresh = std::min(policy.freshFor, previous->policy.maximumAge.value_or(policy.freshFor));
            if (!policy.forceRefresh && age < fresh) { ++state->counters.hits; return {previous->response, Freshness::Fresh}; }
        }
    }
    const auto active = state->flights.find(key);
    if (active != state->flights.end() && !active->second->physical.cancelled()) {
        flight = active->second;
        if (flight->subscribers >= 1024) fail("busy", "Request subscriber budget exceeded");
        ++state->counters.joins;
    } else {
        if (state->flights.size() >= state->maximumFlights) fail("busy", "Resource request queue budget exceeded");
        flight = std::make_shared<State::Flight>();
        flight->request = std::move(request);
        flight->policy = policy;
        flight->previous = previous;
        flight->generation = state->generation;
        state->flights.insert_or_assign(key, flight);
        state->queue.emplace_back(key, flight);
        state->available.notify_one();
    }
    ++flight->subscribers;
    flight->changed.wait(lock, cancel.stop_token(), [&] { return flight->done; });
    --flight->subscribers;
    if (cancel.cancelled()) {
        if (!flight->subscribers && !flight->done) {
            flight->physical.cancel();
            std::erase_if(state->queue, [&](const auto& queued) { return queued.second == flight; });
            const auto current = state->flights.find(key);
            if (current != state->flights.end() && current->second == flight) state->flights.erase(current);
        }
        fail("cancelled", "Resource request cancelled");
    }
    if (flight->error) {
        if (previous && !previous->policy.mustRevalidate && !policy.forceRefresh && policy.staleFor.count() > 0 && staleAllowed(*flight->error)) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - previous->at) + previous->policy.age;
            const auto fresh = std::min(policy.freshFor, previous->policy.maximumAge.value_or(policy.freshFor));
            if (age < fresh + policy.staleFor) return {previous->response, Freshness::Stale};
        }
        throw ErrorException(*flight->error);
    }
    return {flight->response, flight->freshness};
}

void CachedHttp::clear() {
    std::lock_guard lock(state_->mutex);
    state_->cache.clear();
    state_->counters.bytes = 0;
    ++state_->generation;
    state_->cancelFlights();
}

CacheStats CachedHttp::stats() const {
    std::lock_guard lock(state_->mutex);
    auto result = state_->counters;
    result.entries = state_->cache.size();
    result.inFlight = state_->flights.size();
    return result;
}

std::vector<RequestCandidate> apiCandidates(Source source, const std::string& suffix, const ProviderSettings& settings) {
    if (suffix.empty() || suffix.front() == '/' || suffix.find('#') != std::string::npos || suffix.find("://") != std::string::npos || unsafeText(suffix, true)) fail("invalid_input", "Unsafe provider API suffix");
    HttpRequest official;
    official.url = (source == Source::Modrinth ? "https://api.modrinth.com/v2/" : "https://api.curseforge.com/v1/") + suffix;
    official.rules = providerApiRules();
    official.headers = {{"accept", "application/json"}, {"accept-encoding", "identity"}};
    if (source == Source::CurseForge && !settings.curseforgeKey.empty()) {
        if (settings.curseforgeKey.size() > 4096 || unsafeText(settings.curseforgeKey)) fail("invalid_input", "Invalid provider credential");
        official.headers.emplace("x-api-key", settings.curseforgeKey);
    }
    official.url = checkedUrl(official.url, official.rules);
    std::vector<RequestCandidate> result{{official, false}};
    const auto& mirror = source == Source::Modrinth ? settings.modrinthMirror : settings.curseforgeMirror;
    if (!mirror.empty()) {
        UrlRules rules;
        rules.exactOrigins.insert(urlOrigin(mirror));
        rules.testLoopbackOrigins = settings.testLoopbackOrigins;
        rules.allowQuery = false;
        auto base = checkedUrl(mirror, rules);
        if (base.back() != '/') base += '/';
        rules.allowQuery = true;
        auto request = official;
        request.headers.erase("x-api-key");
        request.url = checkedUrl(base + suffix, rules);
        request.rules = std::move(rules);
        result.push_back({std::move(request), true});
    }
    return result;
}

std::vector<RequestCandidate> fileCandidates(const std::string& raw, const ProviderSettings& settings) {
    auto rules = providerFileRules();
    rules.testLoopbackOrigins = settings.testLoopbackOrigins;
    HttpRequest official;
    official.url = checkedUrl(raw, rules);
    official.rules = rules;
    official.maximumBytes = 512 * 1024 * 1024;
    official.headers = {{"accept-encoding", "identity"}};
    std::vector<RequestCandidate> result{{official, false}};
    if (!settings.fileMirror.empty() && rules.exactHosts.contains(parseUrl(official.url).host)) {
        UrlRules mirrorRules;
        mirrorRules.exactOrigins.insert(urlOrigin(settings.fileMirror));
        mirrorRules.testLoopbackOrigins = settings.testLoopbackOrigins;
        mirrorRules.allowQuery = false;
        auto base = checkedUrl(settings.fileMirror, mirrorRules);
        if (base.back() != '/') base += '/';
        const auto url = parseUrl(official.url);
        auto suffix = url.path.substr(1);
        if (url.hasQuery) suffix += '?' + url.query;
        mirrorRules.allowQuery = true;
        auto request = official;
        request.url = checkedUrl(base + suffix, mirrorRules);
        request.rules = std::move(mirrorRules);
        result.push_back({std::move(request), true});
    }
    return result;
}

CachedResponse fetchCandidates(CachedHttp& http, std::vector<RequestCandidate> candidates, SourcePolicy policy,
                               CachePolicy cachePolicy, ResponseValidator validate, const Cancellation& cancel) {
    cancel.check();
    std::erase_if(candidates, [&](const auto& item) { return (policy == SourcePolicy::OfficialOnly && item.mirror) || (policy == SourcePolicy::MirrorOnly && !item.mirror); });
    if (policy == SourcePolicy::MirrorFirst) std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.mirror && !b.mirror; });
    if (candidates.empty()) fail("not_found", "No approved provider candidate is available");
    for (auto& item : candidates) {
        const auto previous = item.request.validate;
        if (validate) item.request.validate = [previous, validate](const auto& response) { if (previous) previous(response); validate(response); };
    }
    if (policy != SourcePolicy::Fastest || candidates.size() < 2) {
        std::optional<Error> last;
        for (auto& item : candidates) {
            cancel.check();
            try { return http.fetch(std::move(item.request), cachePolicy, cancel); }
            catch (const ErrorException& error) { if (error.error().code == "cancelled") throw; last = error.error(); }
        }
        throw ErrorException(*last);
    }
    if (candidates.size() > 2) fail("too_large", "Provider candidate budget exceeded");
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<CachedResponse> winner;
    std::optional<Error> last;
    unsigned completed = 0;
    Cancellation stops[2];
    std::stop_callback parentStopped(cancel.stop_token(), [&] { stops[0].cancel(); stops[1].cancel(); changed.notify_all(); });
    std::vector<std::jthread> workers;
    for (unsigned i = 0; i < candidates.size(); ++i) workers.emplace_back([&, i] {
        try {
            auto response = http.fetch(candidates[i].request, cachePolicy, stops[i]);
            std::lock_guard lock(mutex);
            if (!winner) { winner = std::move(response); stops[1 - i].cancel(); }
        } catch (const ErrorException& error) { std::lock_guard lock(mutex); last = error.error(); }
          catch (...) { std::lock_guard lock(mutex); last = Error{"network", "Provider candidate failed"}; }
        { std::lock_guard lock(mutex); ++completed; }
        changed.notify_all();
    });
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return winner || completed == candidates.size() || cancel.cancelled(); });
    }
    for (auto& worker : workers) worker.join();
    cancel.check();
    if (winner) return *winner;
    throw ErrorException(last.value_or(Error{"network", "No provider candidate succeeded"}));
}

} // namespace nickel::resources
