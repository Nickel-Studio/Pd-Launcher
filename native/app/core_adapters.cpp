#include "core_adapters.hpp"
#include "BuildInfo.hpp"

#include "core/common/error.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <utility>

namespace nickel::app {
namespace {

transport::SourcePolicy policy(const Json& settings, const char* key) {
    const auto found = settings.find(key);
    if (found == settings.end() || !found->is_string()) return transport::SourcePolicy::OfficialAuto;
    const auto value = resources::lower(resources::trim(found->get<std::string>()));
    if (value == "official_auto") return transport::SourcePolicy::OfficialAuto;
    if (value == "official_first") return transport::SourcePolicy::OfficialFirst;
    if (value == "mirror_first") return transport::SourcePolicy::MirrorFirst;
    throw ErrorException({"invalid_settings", "The download source policy is invalid", Json::object()});
}

resources::SourcePolicy community_policy(transport::SourcePolicy value) {
    switch (value) {
    case transport::SourcePolicy::OfficialAuto: return resources::SourcePolicy::Fastest;
    case transport::SourcePolicy::OfficialFirst: return resources::SourcePolicy::OfficialFirst;
    case transport::SourcePolicy::MirrorFirst: return resources::SourcePolicy::MirrorFirst;
    }
    throw ErrorException({"invalid_settings", "The community source policy is invalid", Json::object()});
}

std::uint64_t unsigned_value(const Json& settings, const char* key, std::uint64_t fallback) {
    const auto found = settings.find(key);
    if (found == settings.end()) return fallback;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    if (found->is_number_integer()) {
        const auto value = found->get<std::int64_t>();
        if (value >= 0) return static_cast<std::uint64_t>(value);
    }
    return fallback;
}

transport::HttpMethod method(std::string_view value) {
    if (value == "GET") return transport::HttpMethod::Get;
    if (value == "HEAD") return transport::HttpMethod::Head;
    if (value == "POST") return transport::HttpMethod::Post;
    if (value == "PUT") return transport::HttpMethod::Put;
    if (value == "PATCH") return transport::HttpMethod::Patch;
    if (value == "DELETE") return transport::HttpMethod::Delete;
    resources::fail("policy_denied", "Unsupported resource HTTP method");
}

bool sensitive_header(std::string_view name) {
    const auto value = resources::lower(name);
    return value == "authorization" || value == "proxy-authorization" || value == "cookie"
        || value == "x-api-key";
}

} // namespace

NetworkConfiguration network_configuration(const Json& settings) {
    if (!settings.is_object()) throw ErrorException({"invalid_settings", "Settings must be an object", Json::object()});
    NetworkConfiguration output;
    output.snapshot.user_agent = std::string(build::display_name) + "/" + std::string(build::version)
        + " (" + std::string(build::application_id) + ")";
    output.snapshot.version_list_source = policy(settings, "versionListSource");
    output.snapshot.file_download_source = policy(settings, "fileDownloadSource");
    output.snapshot.community_api_source = policy(settings, "communityApiSource");
    output.limits.connections = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(unsigned_value(settings, "maxDownloadThreads", 10), 1, 64));
    output.limits.bytes_per_second = std::min<std::uint64_t>(unsigned_value(settings, "downloadLimitBps", 0), 128ULL * 1024 * 1024);
    output.community.apiPolicy = community_policy(output.snapshot.community_api_source);
    output.community.filePolicy = community_policy(output.snapshot.file_download_source);
    // Legacy production mirror bases are fixed; settings fields do not grant new remote trust.
    output.community.modrinthMirror = "https://mod.mcimirror.top/modrinth/v2/";
    output.community.curseforgeMirror = "https://mod.mcimirror.top/curseforge/v1/";
    output.community.fileMirror = "https://mod.mcimirror.top/";
    return output;
}

resources::HttpFetch community_http(std::shared_ptr<transport::HttpClient> client) {
    if (!client) throw std::invalid_argument("The shared HTTP client is required");
    return [client = std::move(client)](const resources::HttpRequest& original, const Cancellation& cancel) {
        const auto deadline = std::chrono::steady_clock::now() + original.timeout;
        auto current = original;
        const auto snapshot = client->snapshot();
        std::set<std::string> visited;
        for (unsigned hop = 0;; ++hop) {
            cancel.check();
            current.url = resources::checkedUrl(current.url, current.rules);
            if (!visited.insert(current.url).second) resources::fail("policy_denied", "Resource redirect loop");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) resources::fail("timeout", "Resource request timed out");
            transport::HttpRequest request;
            request.method = method(current.method);
            request.url = current.url;
            request.max_response_bytes = current.maximumBytes;
            request.redirect_policy = transport::RedirectPolicy::None;
            request.timeout = remaining;
            const auto origin = resources::urlOrigin(current.url);
            if (current.rules.testLoopbackOrigins.contains(origin)) request.trust = transport::TrustPolicy::ControlledLoopback;
            for (const auto& [key, value] : current.headers) request.headers.push_back({key, value});
            request.body.reserve(current.body.size());
            for (const auto value : current.body) request.body.push_back(static_cast<std::byte>(value));
            const auto response = client->execute(request, snapshot, cancel);
            resources::HttpResponse output;
            output.status = response.status;
            output.effectiveUrl = response.effective_url;
            output.bytes.reserve(response.body.size());
            for (const auto value : response.body) output.bytes.push_back(std::to_integer<std::uint8_t>(value));
            for (const auto& header : response.headers) output.headers[resources::lower(header.name)] = header.value;
            const bool redirect = response.status == 301 || response.status == 302 || response.status == 303
                || response.status == 307 || response.status == 308;
            if (!redirect || !current.followRedirects) return output;
            if (hop >= current.maximumRedirects) resources::fail("policy_denied", "Resource redirect bound exceeded");
            const auto next = resources::checkedRawUrl(response.header("location"), current.rules, current.url);
            if (resources::urlOrigin(next) != origin) {
                for (const auto& [key, value] : current.headers) {
                    (void)value;
                    if (sensitive_header(key)) resources::fail("policy_denied", "Credential-bearing redirect denied");
                }
                if (!current.body.empty()) resources::fail("policy_denied", "Cross-origin resource body redirect denied");
            }
            if (response.status == 303 || ((response.status == 301 || response.status == 302) && current.method == "POST")) {
                current.method = "GET";
                current.body.clear();
                std::erase_if(current.headers, [](const auto& header) { return resources::lower(header.first) == "content-type"; });
            }
            current.url = next;
        }
    };
}

} // namespace nickel::app
