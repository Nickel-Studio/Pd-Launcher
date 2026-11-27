#include "mock_http.hpp"

#include <future>

namespace identity_test {
using namespace nickel::media;
using nickel::accounts::AccountType;
using nickel::accounts::IdentityError;
using nickel::accounts::IdentityErrorCode;

namespace {
class FixtureDecoder final : public ImageDecoder {
public:
    ImageProbe metadata{64,64,"image/png",1};
    RgbaImage pixels = image(64,64);
    std::atomic<unsigned> probes{0};
    std::atomic<unsigned> decodes{0};
    std::function<void()> pause;
    ImageProbe probe(std::span<const std::uint8_t>) override { ++probes; return metadata; }
    RgbaImage decode(std::span<const std::uint8_t>,const ImageProbe&,const ImageLimits&,std::stop_token stop) override {
        ++decodes;
        if (pause) pause();
        nickel::accounts::detail::check_stop(stop);
        return pixels;
    }
};

std::vector<std::uint8_t> png_bytes() {
    return {0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0,0,0,13,'I','H','D','R'};
}
std::vector<std::uint8_t> webp_bytes() { return {'R','I','F','F',4,0,0,0,'W','E','B','P',1}; }
Json texture_profile(const std::string& url) {
    const auto text = Json{{"textures",{{"SKIN",{{"url",url}}}}}}.dump();
    const auto encoded = nickel::accounts::detail::base64_encode({reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});
    return Json{{"properties",Json::array({{{"name","textures"},{"value",encoded}}})}};
}
SkinRequest microsoft_request(std::string id = "fixture-account") {
    SkinRequest request;
    request.account_id = std::move(id);
    request.source = AccountType::microsoft;
    request.name = "FixturePlayer";
    request.uuid = "069a79f444e94726a5befca90e38aaf5";
    request.output_size = 64;
    return request;
}
void add_skin_download(MockHttp& http, const std::string& url) {
    http.add("GET",url,[](const auto& request,auto) {
        for (const auto& field : request.headers) require(field.first != "Authorization" && field.first != "Cookie", "images no credentials");
        require(!request.sensitive && request.max_response_bytes == 4*1024*1024, "media request bound");
        return IdentityHttpResponse{200,{{"Content-Type","image/png"}},png_bytes()};
    });
}

void test_mojang_cache_and_invalidation() {
    MockHttp http;
    FixtureDecoder decoder;
    fill(decoder.pixels,8,8,8,8,{20,40,60,255});
    ImageLimits limits;
    limits.max_cache_entries = 2;
    limits.max_cache_bytes = 64*64*4*2;
    ImageService images(http,decoder,limits);
    auto request = microsoft_request();
    const std::string hash(64,'a');
    const auto texture = "https://textures.minecraft.net/texture/"+hash;
    auto add = [&] {
        http.add_json("GET","https://sessionserver.mojang.com/session/minecraft/profile/069a79f444e94726a5befca90e38aaf5",200,texture_profile("http://textures.minecraft.net/texture/"+hash));
        add_skin_download(http,texture);
    };
    add();
    auto result = images.head(request);
    require(!result.fallback && !result.cached && pixel(result.head,32,32)[0] == 20, "Mojang trusted PNG decoded");
    require(http.calls == 2 && decoder.decodes == 1, "profile then texture only");
    require(images.head(request).cached && http.calls == 2, "avatar cache hit no network/decode");
    images.invalidate(request.account_id);
    require(images.cache_stats().entries == 0, "account cache invalidation");
    add();
    request.revision = 1;
    require(images.head(request).revision == 1 && decoder.decodes == 2, "revision reload not stale cache");
    for (unsigned i = 0; i < 4; ++i) {
        auto offline = request;
        offline.source = AccountType::offline;
        offline.account_id = "offline-"+std::to_string(i);
        offline.uuid.reset();
        const auto fallback = images.head(offline);
        require(fallback.fallback, "offline builtin fallback");
    }
    const auto stats = images.cache_stats();
    require(stats.entries <= 2 && stats.bytes <= limits.max_cache_bytes, "LRU entries and bytes bounded");
    require(http.calls == 4, "offline zero network");
    images.clear();
    require(images.cache_stats().bytes == 0, "global cache clear");
    http.verify();
}

void test_trust_redirect_and_site_fallback() {
    MockHttp http;
    FixtureDecoder decoder;
    ImageService images(http,decoder);
    auto request = microsoft_request();
    http.add_json("GET","https://sessionserver.mojang.com/session/minecraft/profile/069a79f444e94726a5befca90e38aaf5",200,texture_profile("https://evil.invalid/skin.png?token=secret"));
    require(images.head(request).fallback && http.calls == 1, "profile URL gives no trust");
    images.invalidate(request.account_id);
    const auto texture = "https://textures.minecraft.net/texture/"+std::string(64,'a');
    http.add_json("GET","https://sessionserver.mojang.com/session/minecraft/profile/069a79f444e94726a5befca90e38aaf5",200,texture_profile(texture));
    http.add("GET",texture,[](const auto&,auto) { return IdentityHttpResponse{302,{{"Location","https://evil.invalid/escape"}}, {}}; });
    require(images.head(request).fallback && http.calls == 3, "cross-origin redirect rejected before second request");
    images.invalidate(request.account_id);
    request.trusted_texture_origins = {"https://controlled.invalid"};
    http.add_json("GET","https://sessionserver.mojang.com/session/minecraft/profile/069a79f444e94726a5befca90e38aaf5",200,texture_profile("https://controlled.invalid/first"));
    http.add("GET","https://controlled.invalid/first",[](const auto&,auto) { return IdentityHttpResponse{302,{{"Location","/second"}}, {}}; });
    add_skin_download(http,"https://controlled.invalid/second");
    require(!images.head(request).fallback, "explicit controlled origin with safe redirect");
    auto skin = request;
    skin.account_id = "fixture-site";
    skin.source = AccountType::littleskin;
    skin.server = "https://fixture.invalid/mc/api/yggdrasil/";
    skin.uuid.reset();
    skin.name = "Fixture Player";
    http.add("POST","https://fixture.invalid/mc/api/yggdrasil/api/profiles/minecraft",[](const auto& request,auto) {
        require(Json::parse(body_text(request)) == Json::array({"Fixture Player"}), "skin name lookup direct array");
        return json_response(200,Json::array({profile()}));
    });
    http.add_json("GET","https://fixture.invalid/mc/api/yggdrasil/sessionserver/session/minecraft/profile/069a79f444e94726a5befca90e38aaf5",200,texture_profile("https://evil.invalid/skin.png"));
    http.add("GET","https://fixture.invalid/mc/avatar/player/Fixture%20Player?size=256",[](const auto&,auto) {
        return IdentityHttpResponse{200,{},webp_bytes()};
    });
    decoder.metadata = {32,32,"image/webp",1};
    decoder.pixels = image(32,32,{200,150,100,255});
    const auto avatar = images.head(skin);
    require(!avatar.fallback && pixel(avatar.head,32,32)[0] == 200, "site avatar WebP fallback and minimum crop");
    require(pixel(avatar.head,4,4)[3] == 0, "rendered avatar not atlas hat");
    http.verify();
}

void test_decode_limits_and_cancel_epoch() {
    MockHttp http;
    FixtureDecoder decoder;
    ImageLimits limits;
    limits.max_concurrent_decodes = 1;
    ImageService images(http,decoder,limits);
    EncodedImage encoded{png_bytes(),"image/png",ImageKind::skin};
    decoder.metadata = {65535,65535,"image/png",1};
    expect_error(IdentityErrorCode::image_too_large,[&] { images.decode_bounded(encoded); });
    require(decoder.decodes == 0, "oversize probe rejected before allocation");
    decoder.metadata = {64,64,"image/png",2};
    expect_error(IdentityErrorCode::unsupported_image,[&] { images.decode_bounded(encoded); });
    require(decoder.decodes == 0, "animated image rejected before decode");
    decoder.metadata = {64,64,"image/png",1};
    decoder.pixels = image(32,32);
    expect_error(IdentityErrorCode::unsupported_image,[&] { images.decode_bounded(encoded); });
    decoder.pixels = image(64,64);
    auto svg = encoded;
    svg.bytes = {'<','s','v','g','>','<','/','s','v','g','>'};
    svg.mime = "image/svg+xml";
    const auto probes = decoder.probes.load();
    expect_error(IdentityErrorCode::unsupported_image,[&] { images.decode_bounded(svg); });
    require(decoder.probes == probes, "SVG rejected before Qt plugin");
    auto oversized = encoded;
    oversized.bytes.resize(limits.max_encoded_bytes+1);
    expect_error(IdentityErrorCode::image_too_large,[&] { images.decode_bounded(oversized); });
    PausePoint pause;
    decoder.pause = [&] { pause.pause(); };
    auto first = std::async(std::launch::async,[&] { return images.decode_bounded(encoded); });
    pause.wait();
    std::stop_source cancel;
    auto queued = std::async(std::launch::async,[&] {
        try { images.decode_bounded(encoded,cancel.get_token()); return IdentityErrorCode::invalid_input; }
        catch (const IdentityError& error) { return error.code(); }
    });
    cancel.request_stop();
    require(queued.get() == IdentityErrorCode::cancelled, "queued decode stop wakes wait");
    require(images.cache_stats().active_decodes == 1, "concurrent decode bound");
    pause.release();
    first.get();
    decoder.pause = {};
    require(images.cache_stats().active_decodes == 0, "decode lease released");

    auto request = microsoft_request("fixture-stale-image");
    PausePoint download;
    http.add_json("GET","https://sessionserver.mojang.com/session/minecraft/profile/069a79f444e94726a5befca90e38aaf5",200,texture_profile("https://textures.minecraft.net/texture/"+std::string(64,'b')));
    http.add("GET","https://textures.minecraft.net/texture/"+std::string(64,'b'),[&](const auto&,auto) {
        download.pause();
        return IdentityHttpResponse{200,{},png_bytes()};
    });
    auto stale = std::async(std::launch::async,[&] {
        try { images.head(request); return IdentityErrorCode::invalid_input; }
        catch (const IdentityError& error) { return error.code(); }
    });
    download.wait();
    images.invalidate(request.account_id);
    download.release();
    require(stale.get() == IdentityErrorCode::cancelled && images.cache_stats().entries == 0, "invalidated download never republishes stale head");
    http.verify();
}
}

void run_media_tests() {
    test_mojang_cache_and_invalidation();
    test_trust_redirect_and_site_fallback();
    test_decode_limits_and_cancel_epoch();
    std::cout << "media: trusted texture/redirect/site fallback/byte+pixel budget/concurrent decode/LRU/invalidation/cancel passed\n";
}

} // namespace identity_test
