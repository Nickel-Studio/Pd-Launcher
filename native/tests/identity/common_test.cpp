#include "mock_http.hpp"

#include <future>
#include <type_traits>

namespace identity_test {
namespace {
class CommonDecoder final : public nickel::media::ImageDecoder {
public:
    unsigned probes = 0;
    nickel::media::ImageProbe probe(std::span<const std::uint8_t>) override {
        ++probes;
        return {64, 64, "image/png", 1};
    }
    nickel::media::RgbaImage decode(std::span<const std::uint8_t>, const nickel::media::ImageProbe&,
                                   const nickel::media::ImageLimits&, std::stop_token) override {
        return image(64, 64);
    }
};
}

void run_common_tests() {
    using namespace nickel::accounts;
    static_assert(std::is_base_of_v<nickel::ErrorException, IdentityError>);
    const IdentityError network(IdentityErrorCode::network, 503);
    const nickel::ErrorException& common = network;
    require(common.error().code == "identity.network" && common.error().details == nickel::Json{{"httpStatus", 503}},
            "identity error exposes canonical code/status details");
    require(std::string(common.what()).find("503") == std::string::npos, "canonical message is static and safe");
    const IdentityError cancelled(IdentityErrorCode::cancelled);
    require(cancelled.error().code == "cancelled" && cancelled.error().details.empty(), "common cancellation error contract");

    MemoryStorage storage;
    FixtureVault vault;
    AccountService accounts(storage, vault);
    accounts.add_offline("FixturePlayer", "fixture-offline");
    MockHttp http;
    AuthService auth(accounts, http);
    nickel::Cancellation cancellation;
    auto copy = cancellation;
    copy.cancel();
    require(cancellation.cancelled() && cancellation.stop_token().stop_requested(), "canonical cancellation copies share state");
    expect_error(IdentityErrorCode::cancelled, [&] {
        auth.device_start(MsaConfig{"fixture-client", "consumers", "nickel://auth"}, cancellation);
    });
    expect_error(IdentityErrorCode::cancelled, [&] { auth.prepare_launch("fixture-offline", cancellation); });
    const auto writes = storage.writes;
    expect_error(IdentityErrorCode::cancelled, [&] {
        auth.skin_login("https://fixture.invalid", "fixture@example.invalid", Secret("FIXTURE_PASSWORD"), cancellation);
    });
    require(storage.writes == writes && http.calls == 0, "common pre-cancel never sends credentials or persists");

    CommonDecoder decoder;
    nickel::media::ImageService images(http, decoder);
    nickel::media::SkinRequest request;
    request.account_id = "fixture-offline";
    request.name = "FixturePlayer";
    expect_error(IdentityErrorCode::cancelled, [&] { images.head(request, cancellation); });
    expect_error(IdentityErrorCode::cancelled, [&] { images.fetch_skin(request, cancellation); });
    nickel::media::EncodedImage encoded{{137, 80, 78, 71, 13, 10, 26, 10}, "image/png", nickel::media::ImageKind::skin};
    expect_error(IdentityErrorCode::cancelled, [&] { images.decode_bounded(encoded, cancellation); });
    require(decoder.probes == 0, "common pre-cancel rejects decode before probe");
    http.verify();
    std::cout << "common: canonical ErrorException, Json and linked Cancellation overloads passed\n";
}
} // namespace identity_test
