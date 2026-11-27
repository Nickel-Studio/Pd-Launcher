#include "mock_http.hpp"

namespace identity_test {
namespace {
using namespace nickel::media;
using nickel::accounts::AccountType;
using nickel::accounts::IdentityError;
using nickel::accounts::IdentityErrorCode;

EncodedImage png() { return {{137,80,78,71,13,10,26,10}, "image/png", ImageKind::skin}; }
class PublicationDecoder final : public ImageDecoder {
public:
    std::atomic<unsigned> decodes{0};
    std::function<void()> inside_decode;
    ImageProbe probe(std::span<const std::uint8_t>) override { return {64,64,"image/png",1}; }
    RgbaImage decode(std::span<const std::uint8_t>, const ImageProbe&, const ImageLimits&, std::stop_token stop) override {
        ++decodes;
        if (inside_decode) inside_decode();
        nickel::accounts::detail::check_stop(stop);
        return image(64,64,{120,150,180,255});
    }
};
SkinRequest skin_request(std::string account_id = "fixture-publication") {
    SkinRequest request;
    request.account_id = std::move(account_id);
    request.source = AccountType::microsoft;
    request.name = "FixturePlayer";
    request.uuid = "069a79f444e94726a5befca90e38aaf5";
    request.output_size = 64;
    return request;
}
void add_skin(MockHttp& http, std::string mime = "image/png") {
    const auto url = "https://textures.minecraft.net/texture/" + std::string(64,'e');
    const auto text = Json{{"textures",{{"SKIN",{{"url",url}}}}}}.dump();
    const auto property = nickel::accounts::detail::base64_encode({reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});
    http.add_json("GET","https://sessionserver.mojang.com/session/minecraft/profile/069a79f444e94726a5befca90e38aaf5",200,
                  Json{{"properties",Json::array({{{"name","textures"},{"value",property}}})}});
    http.add("GET",url,[mime = std::move(mime)](const auto&,auto) { return IdentityHttpResponse{200,{{"Content-Type",mime}},png().bytes}; });
}
void test_output_retained_until_publication() {
    MockHttp http;
    PublicationDecoder decoder;
    ImageLimits limits;
    limits.max_active_bytes = 64 * 64 * 4 * 2;
    PausePoint ready;
    std::atomic<unsigned> publications{0};
    ImageService images(http,decoder,limits,[&](const auto& event) {
        if (event.stage == "media.head" && event.outcome == "ready" && ++publications == 1) ready.pause();
    });
    add_skin(http);
    add_skin(http);
    auto first = std::async(std::launch::async,[&] { return images.head(skin_request("first")); });
    ready.wait();
    const auto retained = images.cache_stats();
    nickel::Cancellation cancellation;
    auto second = std::async(std::launch::async,[&] {
        try { images.head(skin_request("second"),cancellation); return false; }
        catch (const IdentityError& error) { return error.code() == IdentityErrorCode::cancelled; }
    });
    const auto queued = second.wait_for(std::chrono::milliseconds(40));
    cancellation.cancel();
    const auto cancelled = second.get();
    ready.release();
    first.get();
    require(retained.active_decodes == 0 && retained.active_bytes == 64 * 64 * 4 && retained.active_requests == 1,
            "finished decoder releases slot but keeps unreturned output active");
    require(queued == std::future_status::timeout && cancelled && decoder.decodes == 1,
            "retained output counts against next source+output reservation and queued cancel");
    const auto final = images.cache_stats();
    require(final.active_bytes == 0 && final.active_requests == 0 && final.tracked_accounts == 0,
            "publication and cancelled wait reclaim every lease/epoch");
    http.verify();
}
void test_cancel_ready_never_caches() {
    MockHttp http;
    PublicationDecoder decoder;
    nickel::Cancellation cancellation;
    ImageService images(http,decoder,{},[&](const auto& event) {
        if (event.stage == "media.head" && event.outcome == "ready") cancellation.cancel();
    });
    add_skin(http);
    expect_error(IdentityErrorCode::cancelled,[&] { images.head(skin_request(),cancellation); });
    const auto stats = images.cache_stats();
    require(stats.entries == 0 && stats.bytes == 0 && stats.active_bytes == 0 && stats.active_requests == 0,
            "caller cancellation between composition and publication never seeds cache");
    http.verify();
}
void test_clear_reentry_and_new_epoch() {
    MockHttp http;
    PublicationDecoder decoder;
    ImageService images(http,decoder);
    PausePoint pause;
    std::atomic<bool> reentered{false};
    http.add("GET","https://sessionserver.mojang.com/session/minecraft/profile/069a79f444e94726a5befca90e38aaf5",
             [&](const auto&,std::stop_token stop) {
        std::stop_callback callback(stop,[&] { images.cache_stats(); reentered = true; });
        pause.pause();
        return json_response(200,Json::object());
    });
    auto stale = std::async(std::launch::async,[&] {
        try { images.head(skin_request()); return false; }
        catch (const IdentityError& error) { return error.code() == IdentityErrorCode::cancelled; }
    });
    pause.wait();
    images.clear();
    auto fresh = skin_request();
    fresh.source = AccountType::offline;
    fresh.revision = 1;
    const auto result = images.head(fresh);
    pause.release();
    const auto cancelled = stale.get();
    const auto stats = images.cache_stats();
    require(reentered && cancelled && result.fallback && result.revision == 1 && stats.entries == 1 && stats.tracked_accounts == 0,
            "clear invokes callbacks outside mutex and old finish never erases new account epoch/publication");
    http.verify();
}
void test_input_metadata_mime_and_exception_bounds() {
    MockHttp http;
    PublicationDecoder decoder;
    ImageLimits limits;
    limits.max_cache_bytes = 64 * 64 * 4;
    ImageService images(http,decoder,limits);
    auto request = skin_request();
    request.source = AccountType::offline;
    require(images.head(request).fallback && images.cache_stats().entries == 0,
            "cache quota counts key/id/entry metadata as well as pixels");
    request.name.assign(257,'a');
    expect_error(IdentityErrorCode::invalid_input,[&] { images.head(request); });
    request = skin_request();
    request.source = AccountType::offline;
    request.name = std::string(1,static_cast<char>(0xff));
    expect_error(IdentityErrorCode::invalid_input,[&] { images.head(request); });
    request = skin_request();
    request.trusted_texture_origins.assign(17,"https://fixture.invalid");
    expect_error(IdentityErrorCode::invalid_input,[&] { images.fetch_skin(request); });
    request = skin_request();
    request.source = AccountType::littleskin;
    request.name.clear();
    request.server = "https://fixture.invalid/../evil";
    expect_error(IdentityErrorCode::invalid_server,[&] { images.head(request); });
    add_skin(http,"text/html");
    require(images.head(skin_request()).fallback && decoder.decodes == 0, "declared HTML MIME never enters image decoder even with PNG signature");
    decoder.inside_decode = [] { throw std::runtime_error("FIXTURE_DECODER_SECRET_MUST_NOT_ESCAPE"); };
    expect_error(IdentityErrorCode::unsupported_image,[&] { images.decode_bounded(png()); });
    const auto stats = images.cache_stats();
    require(stats.active_bytes == 0 && stats.active_decodes == 0 && stats.tracked_accounts == 0,
            "decoder exception unwinds active reservation without exposing fixture exception");
    http.verify();
}
} // namespace

void run_media_publication_tests() {
    test_output_retained_until_publication();
    test_cancel_ready_never_caches();
    test_clear_reentry_and_new_epoch();
    test_input_metadata_mime_and_exception_bounds();
    std::cout << "media publication: retained-output budget, cancel-before-cache, reentrant clear/new epoch, cache metadata/input/MIME/exception bounds passed\n";
}
} // namespace identity_test
