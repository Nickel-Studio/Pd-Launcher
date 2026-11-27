#pragma once

#include "core/network/http.hpp"
#include <functional>

namespace nickel::transport::detail {
struct Url {
    std::string url;
    std::string scheme;
    std::string host;
    std::uint16_t port{};
    std::string origin;
    std::vector<std::string> addresses;
    bool loopback{};
};
Url parse_url(std::string_view raw, TrustPolicy trust);
Url resolve_url(std::string_view raw, TrustPolicy trust,
                const std::function<bool()>& cancelled,
                std::chrono::milliseconds timeout);
std::string redirect_url(std::string_view base, std::string_view location);
bool public_address(std::string_view text) noexcept;
bool loopback_address(std::string_view text) noexcept;
std::string resolve_rule(const Url& url);
std::string proxy_connection(const Url& target, const ProxySnapshot& proxy,
                             const std::function<bool()>& cancelled,
                             std::chrono::milliseconds timeout);
} // namespace nickel::transport::detail
