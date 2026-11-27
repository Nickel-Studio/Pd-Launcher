#pragma once

#include "accounts.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nickel::transport { class HttpClient; }

namespace nickel::accounts {

struct IdentityHttpRequest {
    std::string method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::uint8_t> body;
    std::size_t max_response_bytes = 1024 * 1024;
    bool sensitive = false;
};
struct IdentityHttpResponse {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::uint8_t> body;
    std::string header(std::string_view name) const;
};

// Production implementation is an adapter to transport HttpClient; no socket code.
// Implementations MUST disable retries/redirects and bound bytes while receiving.
class IdentityHttp {
public:
    virtual ~IdentityHttp() = default;
    virtual IdentityHttpResponse perform(const IdentityHttpRequest& request,
                                          std::stop_token stop) = 0;
};
std::shared_ptr<IdentityHttp> transport_identity_http(transport::HttpClient& http);

} // namespace nickel::accounts
