#include "transport_adapter.hpp"
#include "auth.hpp"
#include "../media/images.hpp"

#include <utility>

namespace nickel::accounts {
namespace {
class TransportIdentityHttp final : public IdentityHttp {
    transport::HttpClient& http_;
public:
    explicit TransportIdentityHttp(transport::HttpClient& http) : http_(http) {}
    IdentityHttpResponse perform(const IdentityHttpRequest& request, std::stop_token stop) override {
        detail::check_stop(stop);
        auto translated = detail::identity_transport_request(request);
        struct EraseRequest {
            transport::HttpRequest& value;
            bool sensitive;
            ~EraseRequest() {
                if (!sensitive) return;
                if (!value.body.empty()) detail::erase_bytes(value.body.data(), value.body.size());
                for (auto& header : value.headers) detail::erase_string(header.value);
            }
        } erase_request{translated, request.sensitive};
        transport::HttpResponse response;
        struct EraseResponse {
            transport::HttpResponse& value;
            bool sensitive;
            ~EraseResponse() {
                if (!sensitive) return;
                if (!value.body.empty()) detail::erase_bytes(value.body.data(), value.body.size());
                for (auto& header : value.headers) detail::erase_string(header.value);
            }
        } erase_response{response, request.sensitive};
        try { response = http_.perform(translated, stop); }
        catch (const nickel::ErrorException& error) {
            if (stop.stop_requested() || error.error().code == "cancelled") throw IdentityError(IdentityErrorCode::cancelled);
            throw IdentityError(IdentityErrorCode::network);
        }
        catch (...) { detail::check_stop(stop); throw IdentityError(IdentityErrorCode::network); }
        detail::check_stop(stop);
        if (response.body.size() > request.max_response_bytes || response.status < 100 || response.status > 599)
            throw IdentityError(IdentityErrorCode::invalid_response);
        IdentityHttpResponse result;
        result.status = response.status;
        result.headers.reserve(response.headers.size());
        for (const auto& header : response.headers) result.headers.emplace_back(header.name, header.value);
        result.body.reserve(response.body.size());
        for (const auto byte : response.body) result.body.push_back(std::to_integer<std::uint8_t>(byte));
        return result;
    }
};
} // namespace

std::shared_ptr<IdentityHttp> transport_identity_http(transport::HttpClient& http) {
    return std::make_shared<TransportIdentityHttp>(http);
}
AuthService::AuthService(AccountService& accounts, transport::HttpClient& http, AuthOptions options)
    : AuthService(accounts, transport_identity_http(http), std::move(options)) {}

} // namespace nickel::accounts

namespace nickel::media {
ImageService::ImageService(transport::HttpClient& http, ImageDecoder& decoder, ImageLimits limits, accounts::IdentityLog log)
    : ImageService(accounts::transport_identity_http(http), decoder, limits, std::move(log)) {}
} // namespace nickel::media
