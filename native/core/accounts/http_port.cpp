#include "http_port.hpp"
#include "detail/util.hpp"

namespace nickel::accounts {
std::string IdentityHttpResponse::header(std::string_view name) const {
    const auto target = detail::ascii_lower(name);
    for (const auto& [key, value] : headers) if (detail::ascii_lower(key) == target) return value;
    return {};
}
} // namespace nickel::accounts
