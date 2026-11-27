#include "fixtures.hpp"
#include "../../core/accounts/transport_adapter.hpp"

#include <array>

int main() {
    using namespace nickel::accounts;
    using namespace identity_test;
    try {
        IdentityHttpRequest input;
        input.method = "POST";
        input.url = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
        input.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Authorization", "Bearer FIXTURE_SECRET"}};
        input.body = {0, 1, 127, 128, 255};
        input.max_response_bytes = 1024 * 1024;
        input.sensitive = true;
        const auto output = detail::identity_transport_request(input);
        require(output.method == nickel::transport::HttpMethod::Post && output.url == input.url, "HTTP adapter method/URL exact");
        require(output.max_response_bytes == input.max_response_bytes && output.redirect_policy == nickel::transport::RedirectPolicy::None &&
                output.max_redirects == 0 && output.retry.max_attempts == 1, "HTTP adapter disables transport redirect/retry and preserves receive quota");
        require(output.trust == nickel::transport::TrustPolicy::PublicHttps && output.allowed_origins.empty(), "public authentication never grants private origin trust");
        require(output.headers[1] == nickel::transport::Header{"Authorization", "Bearer FIXTURE_SECRET"} && output.body.size() == input.body.size(),
                "HTTP adapter preserves header and raw body bytes without JSON coercion");
        for (std::size_t i = 0; i < input.body.size(); ++i)
            require(std::to_integer<std::uint8_t>(output.body[i]) == input.body[i], "unsigned body byte parity");
        const std::array<std::pair<const char*, nickel::transport::HttpMethod>, 6> methods{{
            {"GET", nickel::transport::HttpMethod::Get}, {"HEAD", nickel::transport::HttpMethod::Head},
            {"POST", nickel::transport::HttpMethod::Post}, {"PUT", nickel::transport::HttpMethod::Put},
            {"PATCH", nickel::transport::HttpMethod::Patch}, {"DELETE", nickel::transport::HttpMethod::Delete}}};
        for (const auto& [name, expected] : methods) {
            input.method = name;
            require(detail::identity_transport_request(input).method == expected, "all native HTTP method mappings");
        }
        input.method = "GET";
        input.url = "http://127.0.0.1:43123/fixture";
        input.headers.clear();
        input.body.clear();
        require(detail::identity_transport_request(input).trust == nickel::transport::TrustPolicy::ControlledLoopback,
                "explicit controlled loopback uses actual transport loopback policy");
        input.url = "https://fixture.invalid/skin.png";
        require(detail::identity_transport_request(input).headers.empty(), "image request adds no Authorization/Cookie");
        input.method = "CONNECT";
        expect_error(IdentityErrorCode::invalid_input, [&] { detail::identity_transport_request(input); });
        input.method = "GET";
        input.max_response_bytes = 0;
        expect_error(IdentityErrorCode::invalid_input, [&] { detail::identity_transport_request(input); });
        std::cout << "transport request contract: actual owner DTO, methods, byte quota, no automatic retries/redirects, explicit loopback passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "transport request contract failure: " << error.what() << '\n';
        return 1;
    }
}
