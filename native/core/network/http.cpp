#include "core/network/http.hpp"
#include "core/network/control.hpp"
#include "core/network/errors.hpp"
#include "core/network/url_policy.hpp"
#include "core/network/secure_memory.hpp"
#include "core/network/hash.hpp"
#include "core/tasks/secure_fs.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <curl/curl.h>

namespace nickel::transport {
namespace {
using Clock = std::chrono::steady_clock;
using CancelCheck = detail::CancelCheck;
constexpr std::uint64_t max_memory_response = 64ULL * 1024 * 1024;
constexpr std::size_t header_limit = 64 * 1024;
struct TransferFailure : ErrorException {
    bool retryable;
    TransferFailure(std::string message, bool retry = false)
        : ErrorException(Error{"http_failed", std::move(message)}), retryable(retry) {}
};
struct CurlGlobal {
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw TransferFailure("HTTP runtime initialization failed");
        const auto* version = curl_version_info(CURLVERSION_NOW);
        if (!version || !(version->features & CURL_VERSION_SSL))
            throw TransferFailure("HTTP runtime requires TLS support");
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};
void ensure_curl() { static CurlGlobal runtime; }
std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}
std::string trim(std::string_view input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = input.find_last_not_of(" \t\r\n");
    return std::string(input.substr(first, last - first + 1));
}
bool sensitive_header(std::string_view name) {
    const auto key = lower(std::string(name));
    return key == "authorization" || key == "proxy-authorization" || key == "cookie" ||
           key == "x-api-key" || key == "api-key" || key == "x-auth-token";
}
bool redirect_status(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }
bool transient_status(int status) { return status == 408 || status == 425 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504; }
bool retry_code(CURLcode code) {
    return code == CURLE_COULDNT_CONNECT || code == CURLE_OPERATION_TIMEDOUT ||
           code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR ||
           code == CURLE_PARTIAL_FILE || code == CURLE_GOT_NOTHING;
}
std::string method_name(HttpMethod method) {
    switch (method) {
    case HttpMethod::Get: return "GET";
    case HttpMethod::Head: return "HEAD";
    case HttpMethod::Post: return "POST";
    case HttpMethod::Put: return "PUT";
    case HttpMethod::Patch: return "PATCH";
    case HttpMethod::Delete: return "DELETE";
    }
    throw TransferFailure("invalid HTTP method");
}
void check_snapshot(const NetworkSnapshot& snapshot) {
    const auto source_valid = [](SourcePolicy source) {
        return source == SourcePolicy::OfficialAuto || source == SourcePolicy::OfficialFirst || source == SourcePolicy::MirrorFirst;
    };
    const auto timeout_valid = [](std::chrono::milliseconds timeout) {
        return timeout.count() > 0 && timeout <= std::chrono::hours(24);
    };
    if (!source_valid(snapshot.version_list_source) || !source_valid(snapshot.file_download_source) ||
        !source_valid(snapshot.community_api_source) || !timeout_valid(snapshot.connect_timeout) ||
        !timeout_valid(snapshot.idle_timeout) || !timeout_valid(snapshot.total_timeout) ||
        (snapshot.proxy.mode != ProxySnapshot::Mode::Direct && snapshot.proxy.mode != ProxySnapshot::Mode::HttpConnect &&
         snapshot.proxy.mode != ProxySnapshot::Mode::Socks5) || snapshot.user_agent.size() > 8192 ||
        std::any_of(snapshot.user_agent.begin(), snapshot.user_agent.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
        throw TransferFailure("invalid network snapshot");
}
void check_request(const HttpRequest& request, const NetworkSnapshot& snapshot) {
    check_snapshot(snapshot);
    if (!request.max_response_bytes || request.body.size() > 16 * 1024 * 1024 ||
        request.headers.size() > 128 || request.max_redirects > 16 ||
        !request.retry.max_attempts || request.retry.max_attempts > 8 ||
        request.retry.initial_delay.count() < 0 || request.retry.max_delay.count() < 0 ||
        snapshot.connect_timeout.count() <= 0 || snapshot.idle_timeout.count() <= 0 ||
        snapshot.total_timeout.count() <= 0)
        throw TransferFailure("invalid HTTP request bounds");
    std::size_t bytes{};
    std::unordered_set<std::string> seen;
    for (const auto& header : request.headers) {
        bytes += header.name.size() + header.value.size();
        if (header.name.empty() || bytes > header_limit ||
            std::any_of(header.name.begin(), header.name.end(), [](unsigned char c) {
                return !(std::isalnum(c) || c == '-' || c == '_');
            }) || header.value.find('\r') != std::string::npos ||
            header.value.find('\n') != std::string::npos || header.value.find('\0') != std::string::npos)
            throw TransferFailure("invalid HTTP request header");
        const auto key = lower(header.name);
        if (!seen.insert(key).second || key == "host" || key == "connection" || key == "transfer-encoding" ||
            key == "content-length" || key == "proxy-authorization" ||
            (key == "accept-encoding" && lower(header.value) != "identity"))
            throw TransferFailure("unsafe or duplicate HTTP request header");
    }
    if (snapshot.user_agent.find_first_of("\r\n") != std::string::npos)
        throw TransferFailure("invalid user agent");
    if (request.timeout && request.timeout->count() <= 0) throw TransferFailure("invalid HTTP timeout");
}
struct EasyDeleter { void operator()(CURL* value) const { curl_easy_cleanup(value); } };
struct MultiDeleter { void operator()(CURLM* value) const { curl_multi_cleanup(value); } };
struct ListDeleter {
    void operator()(curl_slist* value) const noexcept {
        for (auto* item = value; item; item = item->next)
            if (item->data) detail::erase_bytes(item->data, std::strlen(item->data));
        curl_slist_free_all(value);
    }
};
using Easy = std::unique_ptr<CURL, EasyDeleter>;
using Multi = std::unique_ptr<CURLM, MultiDeleter>;
using CurlList = std::unique_ptr<curl_slist, ListDeleter>;
void append(CurlList& list, std::string value) {
    detail::EraseGuard erased(value);
    auto* next = curl_slist_append(list.get(), value.c_str());
    if (!next) throw TransferFailure("HTTP allocation failed");
    list.release(); list.reset(next);
}
template<class T> void option(CURL* easy, CURLoption key, T value) {
    if (curl_easy_setopt(easy, key, value) != CURLE_OK)
        throw TransferFailure("HTTP option setup failed");
}
long timeout_long(std::chrono::milliseconds duration) {
    if (duration.count() > 24LL * 60 * 60 * 1000) throw TransferFailure("HTTP timeout too large");
    return static_cast<long>(duration.count());
}
struct StreamState {
    const HttpRequest* request{};
    CancelCheck cancelled;
    SharedBudget* budget{};
    ByteSink sink;
    Progress progress;
    HeadValidator validate;
    ResponseHead response;
    detail::Url target;
    bool proxied{};
    std::string proxy_ip;
    std::exception_ptr failure;
    std::size_t header_bytes{};
    bool head_validated{};
    bool discarding{};
    std::uint64_t body_bytes{};
    Clock::time_point started{Clock::now()}, last_activity{started}, sampled{started};
    std::uint64_t sampled_bytes{};

    ~StreamState() { detail::erase(response); detail::erase(target); detail::erase(proxy_ip); }
    void validate_head() {
        if (head_validated) return;
        head_validated = true;
        discarding = redirect_status(response.status) && request->redirect_policy != RedirectPolicy::None;
        const auto encoded = lower(response.header("content-encoding"));
        if (!encoded.empty() && encoded != "identity") throw TransferFailure("encoded transfer is not permitted");
        const auto transfer = lower(response.header("transfer-encoding"));
        if (!transfer.empty() && transfer != "chunked") throw TransferFailure("unsupported HTTP transfer coding");
        const auto length = response.header("content-length");
        if (!length.empty()) {
            if (!transfer.empty()) throw TransferFailure("ambiguous HTTP body framing");
            std::uint64_t value{};
            const auto [end, error] = std::from_chars(length.data(), length.data() + length.size(), value);
            if (error != std::errc{} || end != length.data() + length.size()) throw TransferFailure("invalid HTTP content length");
            response.content_length = value;
        }
        if (validate && !discarding) validate(response);
        const bool carries_body = request->method != HttpMethod::Head && response.status != 304 && response.status != 204;
        if (!discarding && carries_body && response.content_length && *response.content_length > request->max_response_bytes)
            throw TransferFailure("HTTP body exceeds byte limit");
    }
};
std::size_t header_callback(char* bytes, std::size_t size, std::size_t count, void* context) noexcept {
    auto& state = *static_cast<StreamState*>(context);
    const auto length = size * count;
    try {
        detail::check_cancel(state.cancelled);
        state.last_activity = Clock::now();
        if (length > header_limit - state.header_bytes) throw TransferFailure("HTTP headers exceed byte limit");
        state.header_bytes += length;
        std::string_view line(bytes, length);
        if (line.ends_with("\r\n")) line.remove_suffix(2);
        else if (line.ends_with('\n')) line.remove_suffix(1);
        else throw TransferFailure("unterminated HTTP header");
        if (std::any_of(line.begin(), line.end(), [](unsigned char c) {
            return (c < 0x20 && c != '\t') || c == 0x7f;
        })) throw TransferFailure("HTTP header contains control characters or folding");
        if (line.starts_with("HTTP/")) {
            detail::erase(state.response.headers);
            state.response.headers.clear(); state.response.content_length.reset();
            state.head_validated = false;
            const auto space = line.find(' ');
            if (space == std::string_view::npos || line.size() < space + 4) throw TransferFailure("malformed HTTP status");
            int status{};
            const auto value = line.substr(space + 1, 3);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), status);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || status < 100 || status > 599)
                throw TransferFailure("malformed HTTP status");
            state.response.status = status;
        } else if (line.empty()) {
            if (state.response.status >= 200) state.validate_head();
        } else {
            const auto colon = line.find(':');
            if (colon == std::string_view::npos || !colon || std::any_of(line.begin(), line.begin() + colon, [](unsigned char c) {
                return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos);
            })) throw TransferFailure("malformed HTTP header name");
            auto name = lower(std::string(line.substr(0, colon)));
            detail::EraseGuard erased_name(name);
            auto value = trim(line.substr(colon + 1));
            detail::EraseGuard erased_value(value);
            if (state.response.headers.size() >= 128) throw TransferFailure("too many HTTP headers");
            const bool singleton = name == "content-length" || name == "content-range" || name == "location" ||
                name == "etag" || name == "transfer-encoding" || name == "content-encoding" || name == "cache-control" || name == "age";
            if (state.head_validated && singleton) throw TransferFailure("HTTP trailer changes validated response metadata");
            for (const auto& previous : state.response.headers) {
                if (previous.name == name && singleton) throw TransferFailure("ambiguous HTTP header");
            }
            state.response.headers.emplace_back();
            state.response.headers.back().name = std::move(name);
            state.response.headers.back().value = std::move(value);
        }
        return length;
    } catch (...) { state.failure = std::current_exception(); return 0; }
}
std::size_t write_callback(char* bytes, std::size_t size, std::size_t count, void* context) noexcept {
    auto& state = *static_cast<StreamState*>(context);
    const auto length = size * count;
    try {
        detail::check_cancel(state.cancelled);
        state.last_activity = Clock::now();
        state.validate_head();
        state.response.transferred_bytes += length;
        state.budget->throttle(length, state.cancelled);
        state.last_activity = Clock::now();
        if (length > state.request->max_response_bytes - state.body_bytes)
            throw TransferFailure("HTTP body exceeds byte limit");
        state.body_bytes += length;
        if (!state.discarding && state.sink)
            state.sink(std::as_bytes(std::span(bytes, length)));
        const auto now = Clock::now();
        if (state.progress && now - state.sampled >= std::chrono::milliseconds(100)) {
            const auto elapsed = std::chrono::duration<double>(now - state.sampled).count();
            const auto speed = (state.response.transferred_bytes - state.sampled_bytes) / elapsed;
            state.progress({state.response.transferred_bytes, state.discarding ? 0 : state.body_bytes,
                            state.response.content_length, speed, {}});
            state.sampled = now; state.sampled_bytes = state.response.transferred_bytes;
        }
        return length;
    } catch (...) { state.failure = std::current_exception(); return 0; }
}
int progress_callback(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    auto& state = *static_cast<StreamState*>(context);
    try { return state.cancelled && state.cancelled() ? 1 : 0; }
    catch (...) { state.failure = std::current_exception(); return 1; }
}
int prereq_callback(void* context, char* primary, char*, int, int) noexcept {
    auto& state = *static_cast<StreamState*>(context);
    try {
        detail::check_cancel(state.cancelled);
        if (!primary) throw TransferFailure("missing HTTP peer address");
        if (state.proxied) {
            if (primary != state.proxy_ip) throw TransferFailure("proxy peer changed after resolution");
        } else {
            if (std::find(state.target.addresses.begin(), state.target.addresses.end(), primary) == state.target.addresses.end())
                throw TransferFailure("HTTP peer changed after resolution");
        }
        return CURL_PREREQFUNC_OK;
    } catch (...) { state.failure = std::current_exception(); return CURL_PREREQFUNC_ABORT; }
}

ResponseHead one_hop(const HttpRequest& request, const NetworkSnapshot& snapshot,
                      SharedBudget& budget, const CancelCheck& cancelled,
                      ByteSink sink, Progress progress, HeadValidator validate, bool buffer_reserved) {
    detail::check_cancel(cancelled);
    auto permit = budget.acquire_connection(cancelled);
    auto buffer = buffer_reserved ? SharedBudget::Reservation{} : budget.reserve_buffer(128 * 1024, cancelled);
    StreamState state;
    auto target_value = detail::resolve_url(request.url, request.trust, cancelled, snapshot.connect_timeout);
    detail::EraseGuard erased_target(target_value);
    state.target = detail::take(target_value);
    const auto& target = state.target;
    if (!request.allowed_origins.empty() &&
        std::none_of(request.allowed_origins.begin(), request.allowed_origins.end(), [&](const auto& origin) {
            auto parsed = detail::parse_url(origin, request.trust);
            detail::EraseGuard erased(parsed);
            return parsed.origin == target.origin;
        })) throw TransferFailure("HTTP origin is not allowlisted");
    state.request = &request; state.cancelled = cancelled; state.budget = &budget;
    state.sink = std::move(sink); state.progress = std::move(progress); state.validate = std::move(validate);
    state.response.effective_url = target.url; state.response.headers.reserve(128);
    CurlList headers, resolves, connects;
    std::vector<std::byte> ca_bytes;
    detail::EraseGuard erased_ca(ca_bytes);
    std::string proxy_address;
    detail::EraseGuard erased_proxy(proxy_address);
    // Curl borrows lists and the CA blob; destroy handles before their guarded storage.
    Easy easy(curl_easy_init()); Multi multi(curl_multi_init());
    if (!easy || !multi) throw TransferFailure("HTTP handle allocation failed");
    append(resolves, detail::resolve_rule(target));
    for (const auto& header : request.headers) append(headers, header.name + ": " + header.value);
    if (std::none_of(request.headers.begin(), request.headers.end(), [](const auto& h) { return lower(h.name) == "accept-encoding"; }))
        append(headers, "Accept-Encoding: identity");
    append(headers, "Expect:");
    option(easy.get(), CURLOPT_URL, target.url.c_str());
    option(easy.get(), CURLOPT_RESOLVE, resolves.get());
    option(easy.get(), CURLOPT_HTTPHEADER, headers.get());
    option(easy.get(), CURLOPT_USERAGENT, snapshot.user_agent.c_str());
    option(easy.get(), CURLOPT_FOLLOWLOCATION, 0L);
    option(easy.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    option(easy.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    option(easy.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    option(easy.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    option(easy.get(), CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    option(easy.get(), CURLOPT_PROXY_SSL_VERIFYPEER, 1L);
    option(easy.get(), CURLOPT_PROXY_SSL_VERIFYHOST, 2L);
    option(easy.get(), CURLOPT_NOSIGNAL, 1L);
    option(easy.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    option(easy.get(), CURLOPT_CONNECTTIMEOUT_MS, timeout_long(snapshot.connect_timeout));
    option(easy.get(), CURLOPT_TIMEOUT_MS, timeout_long(request.timeout.value_or(snapshot.total_timeout)));
    option(easy.get(), CURLOPT_BUFFERSIZE, 64L * 1024);
    option(easy.get(), CURLOPT_HTTP_CONTENT_DECODING, 0L);
    option(easy.get(), CURLOPT_HTTP_TRANSFER_DECODING, 1L);
    option(easy.get(), CURLOPT_FRESH_CONNECT, 1L);
    option(easy.get(), CURLOPT_FORBID_REUSE, 1L);
    option(easy.get(), CURLOPT_WRITEFUNCTION, &write_callback);
    option(easy.get(), CURLOPT_WRITEDATA, &state);
    option(easy.get(), CURLOPT_HEADERFUNCTION, &header_callback);
    option(easy.get(), CURLOPT_HEADERDATA, &state);
    option(easy.get(), CURLOPT_XFERINFOFUNCTION, &progress_callback);
    option(easy.get(), CURLOPT_XFERINFODATA, &state);
    option(easy.get(), CURLOPT_NOPROGRESS, 0L);
    option(easy.get(), CURLOPT_PREREQFUNCTION, &prereq_callback);
    option(easy.get(), CURLOPT_PREREQDATA, &state);
    if (!snapshot.ca_bundle.empty()) {
        ca_bytes = tasks::detail::read_bounded(snapshot.ca_bundle, 1024 * 1024);
        if (ca_bytes.empty()) throw TransferFailure("empty certificate authority bundle");
        curl_blob blob{ca_bytes.data(), ca_bytes.size(), CURL_BLOB_NOCOPY};
        option(easy.get(), CURLOPT_CAINFO_BLOB, &blob);
        option(easy.get(), CURLOPT_PROXY_CAINFO_BLOB, &blob);
    }
    if (snapshot.proxy.mode == ProxySnapshot::Mode::Direct) {
        option(easy.get(), CURLOPT_PROXY, "");
    } else {
        auto proxy = detail::resolve_url(snapshot.proxy.endpoint,
            snapshot.proxy.allow_loopback_proxy ? TrustPolicy::ControlledLoopback : TrustPolicy::PublicHttps,
            cancelled, snapshot.connect_timeout);
        detail::EraseGuard erased(proxy);
        state.proxied = true; state.proxy_ip = proxy.addresses.front();
        append(resolves, detail::resolve_rule(proxy));
        proxy_address = proxy.origin;
        option(easy.get(), CURLOPT_PROXY, proxy_address.c_str());
        option(easy.get(), CURLOPT_PROXYPORT, static_cast<long>(proxy.port));
        option(easy.get(), CURLOPT_NOPROXY, "");
        option(easy.get(), CURLOPT_PROXYUSERNAME, snapshot.proxy.username.c_str());
        option(easy.get(), CURLOPT_PROXYPASSWORD, snapshot.proxy.password.c_str());
        if (snapshot.proxy.mode == ProxySnapshot::Mode::HttpConnect) {
            option(easy.get(), CURLOPT_PROXYTYPE, proxy.scheme == "https" ? CURLPROXY_HTTPS : CURLPROXY_HTTP);
            option(easy.get(), CURLOPT_HTTPPROXYTUNNEL, 1L);
            // CONNECT uses a prevalidated numeric target, preventing proxy-side DNS rebinding.
            const auto& ip = target.addresses.front();
            append(connects, target.host + ":" + std::to_string(target.port) + ":" +
                (ip.find(':') == std::string::npos ? ip : "[" + ip + "]") + ":" + std::to_string(target.port));
            option(easy.get(), CURLOPT_CONNECT_TO, connects.get());
            option(easy.get(), CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
        } else {
            option(easy.get(), CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
        }
    }
    const auto method = method_name(request.method);
    if (request.method == HttpMethod::Head) option(easy.get(), CURLOPT_NOBODY, 1L);
    else if (request.method != HttpMethod::Get) {
        option(easy.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
        option(easy.get(), CURLOPT_POSTFIELDS, request.body.empty() ? "" : reinterpret_cast<const char*>(request.body.data()));
        option(easy.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    } else if (!request.body.empty()) throw TransferFailure("GET requests cannot carry a body");
    if (curl_multi_add_handle(multi.get(), easy.get()) != CURLM_OK) throw TransferFailure("HTTP multi registration failed");
    bool attached = true;
    auto detach = [&] { if (attached) { curl_multi_remove_handle(multi.get(), easy.get()); attached = false; } };
    try {
        int running{};
        CURLcode result = CURLE_OK;
        for (;;) {
            detail::check_cancel(cancelled);
            if (curl_multi_perform(multi.get(), &running) != CURLM_OK) throw TransferFailure("HTTP multi execution failed");
            if (state.failure) std::rethrow_exception(state.failure);
            if (!running) break;
            if (Clock::now() - state.last_activity > snapshot.idle_timeout) throw TransferFailure("HTTP idle timeout", true);
            int descriptors{};
            if (curl_multi_poll(multi.get(), nullptr, 0, 20, &descriptors) != CURLM_OK)
                throw TransferFailure("HTTP socket poll failed");
        }
        int left{};
        while (auto* message = curl_multi_info_read(multi.get(), &left)) {
            if (message->msg == CURLMSG_DONE) result = message->data.result;
        }
        detach();
        detail::check_cancel(cancelled);
        if (state.failure) std::rethrow_exception(state.failure);
        if (result != CURLE_OK) {
            if (result == CURLE_PEER_FAILED_VERIFICATION || result == CURLE_SSL_CONNECT_ERROR || result == CURLE_SSL_CACERT_BADFILE)
                throw TransferFailure("TLS peer verification or handshake failed");
            throw TransferFailure("HTTP transfer failed: " + std::string(curl_easy_strerror(result)), retry_code(result));
        }
        state.validate_head();
        if (!state.response.status) throw TransferFailure("missing HTTP response status");
        if (request.method != HttpMethod::Head && state.response.status != 304 && state.response.status != 204 &&
            state.response.content_length && state.body_bytes != *state.response.content_length)
            throw TransferFailure("HTTP content length mismatch", true);
        if (state.progress && !state.discarding)
            state.progress({state.response.transferred_bytes, state.body_bytes, state.response.content_length, 0, {}});
        return detail::take(state.response);
    } catch (...) {
        // Cancellation or transfer failure must release the physical socket now,
        // not rely on a later owner destruction after retry/error layers have run.
        detach();
        easy.reset();
        throw;
    }
}
ResponseHead stream_internal(const HttpRequest& input, const NetworkSnapshot& snapshot,
                              SharedBudget& budget, const CancelCheck& cancelled,
                              ByteSink sink, Progress progress, HeadValidator validate,
                              bool buffer_reserved = false) {
    check_request(input, snapshot);
    detail::Erased<HttpRequest> owned(input);
    auto& request = owned.get();
    const auto deadline = Clock::now() + request.timeout.value_or(snapshot.total_timeout);
    auto original = detail::parse_url(request.url, request.trust);
    detail::EraseGuard erased_original(original);
    std::vector<std::string> visited;
    detail::EraseGuard erased_visited(visited);
    visited.reserve(request.max_redirects + 1);
    std::uint64_t transferred{};
    for (std::uint32_t hop = 0;; ++hop) {
        detail::check_cancel(cancelled);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) throw TransferFailure("HTTP redirect deadline exceeded");
        request.timeout = remaining;
        if (std::find(visited.begin(), visited.end(), request.url) != visited.end()) throw TransferFailure("HTTP redirect loop");
        visited.emplace_back(); visited.back() = request.url;
        auto response = one_hop(request, snapshot, budget, cancelled, sink, progress, validate, buffer_reserved);
        detail::EraseGuard erased_response(response);
        transferred += response.transferred_bytes;
        if (!redirect_status(response.status) || request.redirect_policy == RedirectPolicy::None) {
            response.transferred_bytes = transferred;
            return detail::take(response);
        }
        if (hop >= request.max_redirects) throw TransferFailure("HTTP redirect limit exceeded");
        auto location = response.header("location");
        detail::EraseGuard erased_location(location);
        auto redirected = detail::redirect_url(request.url, location);
        detail::EraseGuard erased_redirected(redirected);
        auto next = detail::parse_url(redirected, request.trust);
        detail::EraseGuard erased_next(next);
        auto current = detail::parse_url(request.url, request.trust);
        detail::EraseGuard erased_current(current);
        if (request.redirect_policy == RedirectPolicy::SameOrigin && next.origin != original.origin)
            throw TransferFailure("cross-origin redirect is not permitted");
        if (current.scheme == "https" && next.scheme != "https") throw TransferFailure("TLS redirect downgrade denied");
        if (next.origin != current.origin && std::any_of(request.headers.begin(), request.headers.end(), [](const Header& header) {
            return sensitive_header(header.name);
        })) throw TransferFailure("credential-bearing cross-origin redirect denied");
        if (response.status == 303 || ((response.status == 301 || response.status == 302) && request.method == HttpMethod::Post)) {
            request.method = HttpMethod::Get;
            detail::erase(request.body); request.body.clear();
            detail::erase_headers_if(request.headers, [](const auto& header) { return lower(header.name) == "content-type"; });
        } else if (next.origin != current.origin && !request.body.empty()) {
            throw TransferFailure("cross-origin redirect with request body denied");
        }
        detail::erase(request.url); request.url = detail::take(next.url);
    }
}
std::chrono::milliseconds retry_delay(const HttpRequest& request, std::uint32_t attempt,
                                     const HttpResponse* response) {
    auto delay = request.retry.initial_delay;
    for (std::uint32_t index = 1; index < attempt && delay < request.retry.max_delay; ++index)
        delay = std::min(request.retry.max_delay, delay * 2);
    if (response) {
        const auto value = response->header("retry-after");
        std::uint64_t seconds{};
        if (!value.empty() && std::from_chars(value.data(), value.data() + value.size(), seconds).ec == std::errc{})
            delay = std::max(delay, std::chrono::milliseconds(std::min<std::uint64_t>(seconds, 30) * 1000));
    }
    return std::min(delay, request.retry.max_delay);
}
HttpResponse execute_internal(const HttpRequest& request, const NetworkSnapshot& snapshot,
                               SharedBudget& budget, const CancelCheck& cancelled) {
    check_request(request, snapshot);
    if (request.max_response_bytes > max_memory_response)
        throw TransferFailure("in-memory HTTP response limit exceeds 64 MiB");
    constexpr std::uint64_t overhead = 128 * 1024;
    auto reservation = budget.reserve_buffer(request.max_response_bytes + request.body.size() + overhead, cancelled);
    const bool may_retry = request.method == HttpMethod::Get || request.method == HttpMethod::Head;
    const auto attempts = may_retry ? request.retry.max_attempts : 1U;
    std::uint64_t transferred{};
    for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
        detail::check_cancel(cancelled);
        detail::Erased<HttpResponse> result;
        auto& response = result.get();
        response.body.reserve(static_cast<std::size_t>(request.max_response_bytes));
        try {
            auto head = stream_internal(request, snapshot, budget, cancelled,
                [&](std::span<const std::byte> bytes) { response.body.insert(response.body.end(), bytes.begin(), bytes.end()); },
                {}, {}, true);
            detail::EraseGuard erased_head(head);
            static_cast<ResponseHead&>(response) = detail::take(head);
            transferred += response.transferred_bytes;
            response.transferred_bytes = transferred;
            if (!transient_status(response.status) || attempt + 1 == attempts) return result.take();
            detail::interruptible_wait(retry_delay(request, attempt + 1, &response), cancelled);
        } catch (const TransferFailure& error) {
            if (!error.retryable || attempt + 1 == attempts) throw;
            detail::interruptible_wait(retry_delay(request, attempt + 1, nullptr), cancelled);
        }
    }
    throw TransferFailure("HTTP retries exhausted");
}
std::string cache_key(const HttpRequest& request, const NetworkSnapshot& snapshot) {
    detail::IncrementalHash hash(HashAlgorithm::Sha256);
    const auto field = [&](std::string_view value) {
        std::array<std::byte, 8> size{};
        auto length = static_cast<std::uint64_t>(value.size());
        for (auto& byte : size) { byte = static_cast<std::byte>(length & 0xff); length >>= 8; }
        hash.update(size); hash.update(std::as_bytes(std::span(value.data(), value.size())));
    };
    const auto number = [&](auto value) { field(std::to_string(static_cast<std::int64_t>(value))); };
    field("nickel-http-cache-v2");
    number(request.method); field(request.url); number(request.max_response_bytes);
    number(request.trust); number(request.redirect_policy); number(request.max_redirects);
    number(request.retry.max_attempts); number(request.retry.initial_delay.count()); number(request.retry.max_delay.count());
    number(request.timeout.has_value()); if (request.timeout) number(request.timeout->count());
    number(request.headers.size());
    for (const auto& header : request.headers) { field(lower(header.name)); field(header.value); }
    number(request.allowed_origins.size()); for (const auto& origin : request.allowed_origins) field(origin);
    number(snapshot.version_list_source); number(snapshot.file_download_source); number(snapshot.community_api_source);
    number(snapshot.proxy.mode); field(snapshot.proxy.endpoint);
    // Feed proxy credentials directly into a framed digest; never retain their plaintext in cache keys.
    field(snapshot.proxy.username); field(snapshot.proxy.password); number(snapshot.proxy.allow_loopback_proxy);
    field(tasks::detail::path_utf8(snapshot.ca_bundle)); field(snapshot.user_agent);
    number(snapshot.connect_timeout.count()); number(snapshot.idle_timeout.count()); number(snapshot.total_timeout.count());
    return hash.finish();
}
std::optional<std::chrono::seconds> freshness(const HttpResponse& response, std::chrono::seconds fallback) {
    const auto control = lower(response.header("cache-control"));
    if (control.find("no-store") != std::string::npos || control.find("private") != std::string::npos ||
        !response.header("set-cookie").empty() || !response.header("vary").empty()) return {};
    if (control.find("no-cache") != std::string::npos) return std::chrono::seconds(0);
    auto ttl = fallback;
    const auto at = control.find("max-age=");
    if (at != std::string::npos) {
        const auto value = std::string_view(control).substr(at + 8);
        std::uint64_t seconds{};
        if (std::from_chars(value.data(), value.data() + value.size(), seconds).ec == std::errc{})
            ttl = std::chrono::seconds(std::min<std::uint64_t>(seconds, 86400));
    }
    std::uint64_t age{}; const auto value = response.header("age");
    if (!value.empty() && std::from_chars(value.data(), value.data() + value.size(), age).ec == std::errc{})
        ttl = ttl > std::chrono::seconds(age) ? ttl - std::chrono::seconds(age) : std::chrono::seconds(0);
    return ttl;
}
} // namespace

struct HttpClient::Impl {
    NetworkSnapshot snapshot;
    std::shared_ptr<SharedBudget> budget;
    CacheOptions options;
    mutable std::mutex mutex;
    struct Cached {
        detail::Erased<HttpResponse> response;
        Clock::time_point expires;
        std::uint64_t charge{};
        std::list<std::string>::iterator lru;
    };
    struct Flight {
        std::mutex mutex;
        std::condition_variable changed;
        std::stop_source cancel;
        std::uint32_t subscribers{};
        bool done{};
        detail::Erased<std::optional<HttpResponse>> response;
        std::exception_ptr failure;
        std::jthread worker;
    };
    std::list<std::string> lru;
    std::unordered_map<std::string, Cached> cache;
    std::unordered_map<std::string, std::shared_ptr<Flight>> flights;
    CacheStats stats;
    bool stopping{};
    std::uint64_t generation{};

    void invalidate() {
        if (generation == std::numeric_limits<std::uint64_t>::max()) throw TransferFailure("HTTP cache generation exhausted");
        ++generation;
        cache.clear(); lru.clear(); stats.resident_bytes = 0;
    }

    ~Impl() {
        std::vector<std::shared_ptr<Flight>> pending;
        {
            std::lock_guard lock(mutex); stopping = true;
            for (auto& [key, flight] : flights) { (void)key; flight->cancel.request_stop(); pending.push_back(flight); }
        }
        for (auto& flight : pending) if (flight->worker.joinable()) flight->worker.join();
        detail::erase(snapshot);
    }
    void prune(std::vector<std::shared_ptr<Flight>>& retired) {
        for (auto item = flights.begin(); item != flights.end();) {
            auto flight = item->second;
            bool removable{};
            { std::lock_guard lock(flight->mutex); removable = flight->done && !flight->subscribers; }
            if (removable) { retired.push_back(std::move(flight)); item = flights.erase(item); }
            else ++item;
        }
    }
    void insert(const std::string& key, const HttpResponse& response) {
        const auto ttl = freshness(response, options.freshness);
        if (!ttl || response.status != 200) return;
        std::uint64_t charge = key.size() + response.body.size() + response.effective_url.size();
        for (const auto& h : response.headers) charge += h.name.size() + h.value.size();
        if (charge > options.max_bytes) return;
        detail::Erased<HttpResponse> stored(response);
        if (auto found = cache.find(key); found != cache.end()) {
            stats.resident_bytes -= found->second.charge; lru.erase(found->second.lru); cache.erase(found);
        }
        while (!lru.empty() && charge > options.max_bytes - stats.resident_bytes) {
            auto found = cache.find(lru.back()); stats.resident_bytes -= found->second.charge;
            cache.erase(found); lru.pop_back();
        }
        lru.push_front(key);
        try {
            cache.emplace(key, Cached{std::move(stored), Clock::now() + *ttl, charge, lru.begin()});
            stats.resident_bytes += charge;
        } catch (...) { lru.pop_front(); throw; }
    }
};

std::string ResponseHead::header(std::string_view name) const {
    const auto key = lower(std::string(name));
    for (const auto& h : headers) if (lower(h.name) == key) return h.value;
    return {};
}
std::string HttpResponse::text() const { return {reinterpret_cast<const char*>(body.data()), body.size()}; }
Json HttpResponse::json() const {
    return detail::boundary("invalid_json", "HTTP response is not valid JSON", [&] {
        auto input = text(); detail::EraseGuard erased(input);
        return Json::parse(input);
    });
}
HttpClient::HttpClient(NetworkSnapshot snapshot, std::shared_ptr<SharedBudget> budget, CacheOptions cache) {
    detail::EraseGuard erased_snapshot(snapshot);
    detail::boundary("http_failed", "Cannot construct HTTP client", [&] {
        ensure_curl(); check_snapshot(snapshot);
        if (!budget || !cache.max_inflight || cache.max_inflight > 64 || cache.max_bytes > 128ULL * 1024 * 1024 || cache.freshness.count() < 0)
            throw TransferFailure("invalid HTTP client limits");
        impl_ = std::make_unique<Impl>();
        impl_->snapshot = detail::take(snapshot); impl_->budget = std::move(budget); impl_->options = cache;
    });
}
HttpClient::~HttpClient() = default;
NetworkSnapshot HttpClient::snapshot() const {
    return detail::boundary("http_failed", "Cannot capture network snapshot", [&] {
        std::lock_guard lock(impl_->mutex);
        detail::Erased<NetworkSnapshot> captured(impl_->snapshot);
        return captured.take();
    });
}
void HttpClient::set_snapshot(NetworkSnapshot snapshot) {
    detail::EraseGuard erased_snapshot(snapshot);
    detail::boundary("http_failed", "Cannot update network snapshot", [&] {
        check_snapshot(snapshot);
        std::lock_guard lock(impl_->mutex);
        impl_->invalidate();
        detail::erase(impl_->snapshot); impl_->snapshot = detail::take(snapshot);
    });
}
std::shared_ptr<SharedBudget> HttpClient::budget() const { return impl_->budget; }
HttpResponse HttpClient::perform(const HttpRequest& request, const Cancellation& cancel) {
    return detail::boundary("http_failed", "HTTP request failed", [&] {
        detail::Erased<NetworkSnapshot> captured(this->snapshot());
        return execute_internal(request, captured.get(), *impl_->budget, detail::cancellation_check(cancel));
    });
}
HttpResponse HttpClient::perform(const HttpRequest& request, std::stop_token cancel) {
    return detail::boundary("http_failed", "HTTP request failed", [&] {
        detail::Erased<NetworkSnapshot> captured(this->snapshot());
        return execute_internal(request, captured.get(), *impl_->budget, [cancel] { return cancel.stop_requested(); });
    });
}
HttpResponse HttpClient::execute(const HttpRequest& request, const NetworkSnapshot& snapshot, const Cancellation& cancel) {
    return detail::boundary("http_failed", "HTTP request failed", [&] {
        detail::Erased<NetworkSnapshot> captured(snapshot);
        return execute_internal(request, captured.get(), *impl_->budget, detail::cancellation_check(cancel));
    });
}
HttpResponse HttpClient::get(std::string url, const Cancellation& cancel, std::uint64_t max_bytes) {
    detail::EraseGuard erased_url(url);
    HttpRequest request; detail::EraseGuard erased_request(request);
    request.url = detail::take(url); request.max_response_bytes = max_bytes;
    return perform(request, cancel);
}
ResponseHead HttpClient::stream(const HttpRequest& request, const NetworkSnapshot& snapshot,
                                const Cancellation& cancel, ByteSink sink, Progress progress, HeadValidator validate_head) {
    return detail::boundary("http_failed", "HTTP stream failed", [&] {
        detail::Erased<NetworkSnapshot> captured(snapshot);
        return stream_internal(request, captured.get(), *impl_->budget, detail::cancellation_check(cancel),
                               std::move(sink), std::move(progress), std::move(validate_head));
    });
}
HttpResponse HttpClient::get_cached(const HttpRequest& request, const Cancellation& cancel) {
    return detail::boundary("http_failed", "Cached HTTP request failed", [&]() -> HttpResponse {
        const auto cancelled = detail::cancellation_check(cancel);
        detail::check_cancel(cancelled);
        detail::Erased<NetworkSnapshot> captured;
        std::uint64_t generation{};
        {
            std::lock_guard lock(impl_->mutex);
            detail::copy_sensitive(captured.get(), impl_->snapshot); generation = impl_->generation;
        }
        check_request(request, captured.get());
        if (request.max_response_bytes > max_memory_response) throw TransferFailure("in-memory HTTP response limit exceeds 64 MiB");
        if (request.method != HttpMethod::Get || !request.body.empty() ||
            std::any_of(request.headers.begin(), request.headers.end(), [](const auto& h) { return sensitive_header(h.name); }))
            return execute_internal(request, captured.get(), *impl_->budget, cancelled);
        const auto key = std::to_string(generation) + ":" + cache_key(request, captured.get());
        std::shared_ptr<Impl::Flight> flight;
        for (;;) {
            std::vector<std::shared_ptr<Impl::Flight>> retired;
            std::shared_ptr<Impl::Flight> aborted;
            {
                std::lock_guard lock(impl_->mutex);
                impl_->prune(retired);
                detail::Erased<std::optional<HttpResponse>> stale;
                if (auto cached = impl_->cache.find(key); cached != impl_->cache.end()) {
                    const auto& entity = cached->second.response.get();
                    if (cached->second.expires > Clock::now()) {
                        ++impl_->stats.hits;
                        impl_->lru.splice(impl_->lru.begin(), impl_->lru, cached->second.lru);
                        detail::Erased<HttpResponse> response(entity); response.get().transferred_bytes = 0;
                        return response.take();
                    }
                    if (!entity.header("etag").empty()) { stale.get().emplace(); detail::copy_sensitive(*stale.get(), entity); }
                }
                if (auto existing = impl_->flights.find(key); existing != impl_->flights.end()) {
                    std::lock_guard flight_lock(existing->second->mutex);
                    if (existing->second->cancel.stop_requested() && !existing->second->subscribers) {
                        aborted = existing->second; impl_->flights.erase(existing);
                    } else { flight = existing->second; ++flight->subscribers; ++impl_->stats.coalesced; }
                } else {
                    if (impl_->flights.size() >= impl_->options.max_inflight) throw TransferFailure("cached HTTP inflight limit exceeded");
                    flight = std::make_shared<Impl::Flight>(); flight->subscribers = 1;
                    detail::Erased<HttpRequest> conditional(request);
                    if (stale.get() && std::none_of(conditional.get().headers.begin(), conditional.get().headers.end(), [](const auto& h) { return lower(h.name) == "if-none-match"; })) {
                        if (conditional.get().headers.size() >= 128) throw TransferFailure("conditional HTTP header limit exceeded");
                        conditional.get().headers.emplace_back();
                        conditional.get().headers.back().name = "If-None-Match";
                        conditional.get().headers.back().value = stale.get()->header("etag");
                    }
                    auto* active = flight.get();
                    impl_->flights.emplace(key, flight); ++impl_->stats.misses;
                    try {
                        // Retirement joins outside client/flight locks; snapshot changes never cancel live subscribers.
                        active->worker = std::jthread([this, key, generation, conditional = std::move(conditional),
                                                       snapshot = captured, stale = std::move(stale), active]() mutable {
                            try {
                                detail::Erased<HttpResponse> result(detail::boundary("http_failed", "Cached HTTP transfer failed", [&] {
                                    return execute_internal(conditional.get(), snapshot.get(), *impl_->budget,
                                        [token = active->cancel.get_token()] { return token.stop_requested(); });
                                }));
                                auto& response = result.get();
                                if (response.status == 304) {
                                    if (!stale.get() || !response.body.empty() ||
                                        (!response.header("etag").empty() && response.header("etag") != stale.get()->header("etag")))
                                        throw TransferFailure("conditional HTTP response has no matching cached entity");
                                    auto merged = detail::take(*stale.get());
                                    detail::EraseGuard erased_merged(merged);
                                    merged.transferred_bytes = response.transferred_bytes;
                                    detail::erase_headers_if(merged.headers, [](const auto& h) { return lower(h.name) == "age"; });
                                    for (const auto& header : response.headers) {
                                        const auto name = lower(header.name);
                                        if (name == "content-length" || name == "content-range" || name == "content-encoding" ||
                                            name == "transfer-encoding" || name == "connection") continue;
                                        detail::erase_headers_if(merged.headers, [&](const auto& h) { return lower(h.name) == name; });
                                        if (merged.headers.size() >= 128) throw TransferFailure("merged HTTP header limit exceeded");
                                        merged.headers.emplace_back(); merged.headers.back().name = header.name; merged.headers.back().value = header.value;
                                    }
                                    detail::erase(response); response = detail::take(merged);
                                }
                                {
                                    std::lock_guard client_lock(impl_->mutex);
                                    std::lock_guard flight_lock(active->mutex);
                                    if (!active->cancel.stop_requested() && active->subscribers && !impl_->stopping && generation == impl_->generation)
                                        impl_->insert(key, response);
                                    active->response.get().emplace(std::move(response)); active->done = true;
                                }
                            } catch (...) {
                                std::lock_guard lock(active->mutex); active->failure = std::current_exception(); active->done = true;
                            }
                            active->changed.notify_all();
                        });
                    } catch (...) { impl_->flights.erase(key); throw; }
                }
            }
            retired.clear();
            if (aborted) { if (aborted->worker.joinable()) aborted->worker.join(); detail::check_cancel(cancelled); continue; }
            break;
        }
        std::unique_lock lock(flight->mutex);
        try {
            while (!flight->done) { detail::check_cancel(cancelled); flight->changed.wait_for(lock, std::chrono::milliseconds(20)); }
            detail::check_cancel(cancelled);
            if (flight->failure) std::rethrow_exception(flight->failure);
            detail::Erased<HttpResponse> response(*flight->response.get()); --flight->subscribers;
            return response.take();
        } catch (...) {
            --flight->subscribers;
            if (!flight->subscribers && !flight->done) flight->cancel.request_stop();
            throw;
        }
    });
}
CacheStats HttpClient::cache_stats() const {
    std::lock_guard lock(impl_->mutex);
    auto out = impl_->stats; out.entries = static_cast<std::uint32_t>(impl_->cache.size());
    out.inflight = static_cast<std::uint32_t>(impl_->flights.size());
    return out;
}
void HttpClient::clear_cache() {
    detail::boundary("http_failed", "Cannot clear HTTP cache", [&] {
        std::lock_guard lock(impl_->mutex); impl_->invalidate();
    });
}
HttpResponse HttpClient::fetch_candidates(const std::vector<RequestCandidate>& candidates,
                                          SourceDomain domain, const HttpRequest& options,
                                          const Cancellation& cancel) {
    detail::Erased<NetworkSnapshot> captured(this->snapshot());
    return fetch_candidates(candidates, domain, options, captured.get(), cancel);
}
HttpResponse HttpClient::fetch_candidates(const std::vector<RequestCandidate>& candidates,
                                          SourceDomain domain, const HttpRequest& options,
                                          const NetworkSnapshot& input, const Cancellation& cancel) {
    return detail::boundary("http_failed", "HTTP source selection failed", [&] {
        if (candidates.empty() || candidates.size() > 16) throw TransferFailure("invalid source candidates");
        detail::Erased<NetworkSnapshot> captured(input);
        const auto& snapshot = captured.get();
        check_request(options, snapshot);
        const auto policy = domain == SourceDomain::VersionList ? snapshot.version_list_source :
            domain == SourceDomain::FileDownload ? snapshot.file_download_source : snapshot.community_api_source;
        std::vector<const RequestCandidate*> ordered;
        ordered.reserve(candidates.size()); for (const auto& candidate : candidates) ordered.push_back(&candidate);
        const auto mirror_first = policy == SourcePolicy::MirrorFirst;
        std::stable_sort(ordered.begin(), ordered.end(), [mirror_first](const auto* a, const auto* b) {
            return (a->mirror == mirror_first) > (b->mirror == mirror_first);
        });
        std::exception_ptr last;
        for (const auto* candidate : ordered) {
            detail::check_cancel(detail::cancellation_check(cancel));
            detail::Erased<HttpRequest> owned(options); auto& request = owned.get();
            detail::erase(request.url); request.url = candidate->url;
            if (candidate->mirror && std::any_of(options.headers.begin(), options.headers.end(), [](const auto& h) { return sensitive_header(h.name); }))
                throw TransferFailure("mirror request cannot carry platform credentials");
            request.headers.reserve(request.headers.size() + candidate->headers.size());
            for (const auto& header : candidate->headers) {
                request.headers.emplace_back(); request.headers.back().name = header.name; request.headers.back().value = header.value;
            }
            try {
                detail::Erased<HttpResponse> response(execute_internal(request, snapshot, *impl_->budget, detail::cancellation_check(cancel)));
                if (response.get().status >= 200 && response.get().status < 300) return response.take();
            } catch (...) { last = std::current_exception(); }
        }
        detail::check_cancel(detail::cancellation_check(cancel));
        if (last) std::rethrow_exception(last);
        throw TransferFailure("source candidates returned no successful response");
    });
}
} // namespace nickel::transport
