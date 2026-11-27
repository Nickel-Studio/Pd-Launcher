#pragma once

#include "http_port.hpp"
#include "detail/util.hpp"
#include "core/network/http.hpp"

namespace nickel::accounts::detail {

// Kept separate from the client call so contract tests require no network runtime.
inline transport::HttpRequest identity_transport_request(const IdentityHttpRequest& request) {
    if (!request.max_response_bytes || request.max_response_bytes > 32 * 1024 * 1024 ||
        request.body.size() > 16 * 1024 * 1024 || request.headers.size() > 128)
        throw IdentityError(IdentityErrorCode::invalid_input);
    transport::HttpRequest result;
    if (request.method == "GET") result.method = transport::HttpMethod::Get;
    else if (request.method == "HEAD") result.method = transport::HttpMethod::Head;
    else if (request.method == "POST") result.method = transport::HttpMethod::Post;
    else if (request.method == "PUT") result.method = transport::HttpMethod::Put;
    else if (request.method == "PATCH") result.method = transport::HttpMethod::Patch;
    else if (request.method == "DELETE") result.method = transport::HttpMethod::Delete;
    else throw IdentityError(IdentityErrorCode::invalid_input);
    const auto url = parse_url(request.url, true);
    result.url = url.str();
    result.trust = url.host == "localhost" || url.host == "127.0.0.1" || url.host == "[::1]"
        ? transport::TrustPolicy::ControlledLoopback : transport::TrustPolicy::PublicHttps;
    result.max_response_bytes = request.max_response_bytes;
    result.redirect_policy = transport::RedirectPolicy::None;
    result.max_redirects = 0;
    result.retry.max_attempts = 1;
    result.headers.reserve(request.headers.size());
    for (const auto& [name, value] : request.headers) result.headers.push_back({name, value});
    result.body.reserve(request.body.size());
    for (const auto byte : request.body) result.body.push_back(static_cast<std::byte>(byte));
    return result;
}

} // namespace nickel::accounts::detail
