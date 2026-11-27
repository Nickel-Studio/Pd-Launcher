#include "mock_http.hpp"
#include "../../core/media/skin_png.hpp"
#include <zlib.h>

#include <limits>

namespace identity_test {
namespace {
using namespace nickel::media;
using nickel::accounts::AccountType;
using nickel::accounts::IdentityErrorCode;

EncodedImage encoded_skin() {
    return {{137,80,78,71,13,10,26,10}, "image/png", ImageKind::skin};
}
class LifecycleDecoder final : public ImageDecoder {
public:
    std::atomic<unsigned> decodes{0};
    std::function<void()> inside_decode;
    ImageProbe probe(std::span<const std::uint8_t>) override { return {64,64,"image/png",1}; }
    RgbaImage decode(std::span<const std::uint8_t>, const ImageProbe&, const ImageLimits&, std::stop_token stop) override {
        ++decodes;
        if (inside_decode) inside_decode();
        nickel::accounts::detail::check_stop(stop);
        return image(64,64,{100,150,200,255});
    }
};
SkinRequest request() {
    SkinRequest value;
    value.account_id = "fixture-media";
    value.source = AccountType::microsoft;
    value.name = "FixturePlayer";
    value.uuid = "069a79f444e94726a5befca90e38aaf5";
    value.output_size = 64;
    return value;
}
void add_fixture_skin(MockHttp& http) {
    const auto url = "https://textures.minecraft.net/texture/" + std::string(64,'a');
    const auto json = Json{{"textures",{{"SKIN",{{"url",url}}}}}}.dump();
    const auto property = nickel::accounts::detail::base64_encode({reinterpret_cast<const std::uint8_t*>(json.data()),json.size()});
    http.add_json("GET", "https://sessionserver.mojang.com/session/minecraft/profile/069a79f444e94726a5befca90e38aaf5", 200,
        Json{{"properties",Json::array({{{"name","textures"},{"value",property}}})}});
    http.add("GET",url,[](const auto&,auto) { return IdentityHttpResponse{200,{},encoded_skin().bytes}; });
}
void test_composition_active_budget() {
    MockHttp http;
    LifecycleDecoder decoder;
    ImageLimits limits;
    limits.max_active_bytes = 64 * 64 * 4 * 2;
    ImageService images(http,decoder,limits);
    add_fixture_skin(http);
    decoder.inside_decode = [&] {
        const auto stats = images.cache_stats();
        require(stats.active_decodes == 1 && stats.active_bytes == 64 * 64 * 4 * 2,
                "active lease includes decoder plane plus head composition output");
    };
    require(!images.head(request()).fallback, "bounded composed fixture head");
    const auto stats = images.cache_stats();
    require(stats.active_decodes == 0 && stats.active_bytes == 0 && stats.active_requests == 0 && stats.tracked_accounts == 0,
            "composition lease and account epoch reclaimed after publication");
    http.verify();
}
void test_preallocation_quota_and_epoch_cleanup() {
    MockHttp http;
    LifecycleDecoder decoder;
    ImageLimits limits;
    limits.max_active_bytes = 64 * 64 * 4 * 2 - 1;
    ImageService images(http,decoder,limits);
    add_fixture_skin(http);
    expect_error(IdentityErrorCode::quota,[&] { images.head(request()); });
    require(decoder.decodes == 0 && images.cache_stats().active_bytes == 0, "source plus output quota rejected before decode");
    for (unsigned i = 0; i < 1000; ++i) images.invalidate("unused-"+std::to_string(i));
    require(images.cache_stats().tracked_accounts == 0, "idle invalidations do not accumulate epoch tombstones");
    auto invalid = request();
    invalid.output_size = 65535;
    expect_error(IdentityErrorCode::image_too_large,[&] { images.head(invalid); });
    http.verify();
}
void test_global_active_wait_and_cancel() {
    MockHttp http;
    LifecycleDecoder decoder;
    ImageLimits limits;
    limits.max_active_bytes = 64 * 64 * 4;
    limits.max_concurrent_decodes = 2;
    ImageService images(http,decoder,limits);
    PausePoint pause;
    decoder.inside_decode = [&] { pause.pause(); };
    const auto encoded = encoded_skin();
    auto first = std::async(std::launch::async,[&] { return images.decode_bounded(encoded); });
    pause.wait();
    nickel::Cancellation cancel;
    auto queued = std::async(std::launch::async,[&] {
        try { images.decode_bounded(encoded,cancel); return false; }
        catch (const nickel::accounts::IdentityError& error) { return error.code() == IdentityErrorCode::cancelled; }
    });
    cancel.cancel();
    require(queued.get() && decoder.decodes == 1, "total active byte budget queues and canonical cancellation wakes waiter");
    pause.release();
    first.get();
    require(images.cache_stats().active_bytes == 0, "total active byte lease released");
}

class WorkingDecoder final : public ImageDecoder {
public:
    static constexpr std::size_t plane = 64 * 64 * 4;
    ImageProbe metadata{64,64,"image/png",1,plane * 3};
    std::atomic<unsigned> decodes{0};
    std::function<void()> inside_decode;
    ImageProbe probe(std::span<const std::uint8_t>) override { return metadata; }
    RgbaImage decode(std::span<const std::uint8_t>, const ImageProbe&, const ImageLimits&, std::stop_token stop) override {
        ++decodes;
        std::vector<std::uint8_t> native(plane,7), converted(plane,9);
        auto result = image(64,64,{100,150,200,255});
        if (inside_decode) inside_decode();
        require(native.front() == 7 && converted.front() == 9, "native and converted fixture planes survive the output copy");
        nickel::accounts::detail::check_stop(stop);
        return result;
    }
};
void test_decoder_working_peak() {
    MockHttp http;
    WorkingDecoder decoder;
    ImageLimits limits;
    limits.max_decoded_bytes = WorkingDecoder::plane;
    limits.max_active_bytes = WorkingDecoder::plane * 4;
    ImageService images(http,decoder,limits);
    add_fixture_skin(http);
    decoder.inside_decode = [&] {
        const auto stats = images.cache_stats();
        require(stats.active_decodes == 1 && stats.active_bytes == WorkingDecoder::plane * 4,
                "all three decoder planes plus composition output are reserved before pixel allocation");
    };
    require(!images.head(request()).fallback, "working peak is separate from the single returned-plane limit");
    const auto stats = images.cache_stats();
    require(stats.active_decodes == 0 && stats.active_bytes == 0 && stats.entries == 1,
            "decoder temporaries and published head release active charges while cache stays separately bounded");
    http.verify();
}
void test_decoder_working_preallocation_and_unwind() {
    MockHttp http;
    WorkingDecoder decoder;
    ImageLimits limits;
    limits.max_active_bytes = WorkingDecoder::plane * 3 - 1;
    ImageService images(http,decoder,limits);
    expect_error(IdentityErrorCode::quota,[&] { images.decode_bounded(encoded_skin()); });
    require(decoder.decodes == 0 && images.cache_stats().active_bytes == 0, "decoder peak quota rejects before all native allocations");
    decoder.metadata.decoded_working_bytes = WorkingDecoder::plane - 1;
    expect_error(IdentityErrorCode::unsupported_image,[&] { images.decode_bounded(encoded_skin()); });
    decoder.metadata.decoded_working_bytes = std::numeric_limits<std::size_t>::max();
    expect_error(IdentityErrorCode::quota,[&] { images.decode_bounded(encoded_skin()); });
    add_fixture_skin(http);
    expect_error(IdentityErrorCode::quota,[&] { images.head(request()); });
    require(decoder.decodes == 0, "too-small or overflowing working hints never enter decode");
    decoder.metadata.decoded_working_bytes = WorkingDecoder::plane * 3;
    limits.max_active_bytes = WorkingDecoder::plane * 3;
    ImageService enough(http,decoder,limits);
    decoder.inside_decode = [] { throw std::runtime_error("FIXTURE_WORKING_FAILURE"); };
    expect_error(IdentityErrorCode::unsupported_image,[&] { enough.decode_bounded(encoded_skin()); });
    const auto stats = enough.cache_stats();
    require(stats.active_bytes == 0 && stats.active_decodes == 0, "multi-plane decoder exception releases the entire working lease");
    http.verify();
}
void test_decoder_working_wait_and_cancel() {
    MockHttp http;
    WorkingDecoder decoder;
    ImageLimits limits;
    limits.max_active_bytes = WorkingDecoder::plane * 3;
    ImageService images(http,decoder,limits);
    PausePoint pause;
    decoder.inside_decode = [&] { pause.pause(); };
    auto first = std::async(std::launch::async,[&] { return images.decode_bounded(encoded_skin()); });
    pause.wait();
    nickel::Cancellation cancellation;
    auto second = std::async(std::launch::async,[&] {
        try { images.decode_bounded(encoded_skin(),cancellation); return false; }
        catch (const nickel::accounts::IdentityError& error) { return error.code() == IdentityErrorCode::cancelled; }
    });
    const auto waiting = second.wait_for(std::chrono::milliseconds(30));
    cancellation.cancel();
    const bool cancelled = second.get();
    pause.release();
    first.get();
    require(waiting == std::future_status::timeout && cancelled && decoder.decodes == 1,
            "multi-plane peak queues despite free decode slots and cancellation wakes without allocation");
    require(images.cache_stats().active_bytes == 0, "cancelled working-set waiter and completed decoder leak no charges");
}

class IconDecoder final : public ImageDecoder {
public:
    static constexpr std::size_t plane = 64 * 32 * 4;
    std::atomic<unsigned> probes{0};
    std::atomic<unsigned> decodes{0};
    std::size_t working_hint = plane * 3;
    std::function<void()> inside_decode;
    static std::uint32_t u32(std::span<const std::uint8_t> bytes) {
        require(bytes.size() >= 4,"fixture PNG integer is bounded");
        return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
               (std::uint32_t{bytes[2]} << 8) | bytes[3];
    }
    ImageProbe probe(std::span<const std::uint8_t> encoded) override {
        ++probes;
        require(encoded.size() >= 33 && u32(encoded.subspan(8,4)) == 13 &&
                std::equal(encoded.begin()+12,encoded.begin()+16,"IHDR"),"fixture metadata-only PNG header probe");
        return {u32(encoded.subspan(16,4)),u32(encoded.subspan(20,4)),"image/png",1,working_hint};
    }
    RgbaImage decode(std::span<const std::uint8_t> encoded, const ImageProbe&, const ImageLimits&, std::stop_token stop) override {
        ++decodes;
        if (inside_decode) inside_decode();
        return decode_skin_png(encoded,stop);
    }
};
void verify_sanitized_icon(const EncodedImage& icon, const RgbaImage& expected) {
    const auto bytes = std::span(icon.bytes);
    require(bytes.size() >= 57 && IconDecoder::u32(bytes.subspan(8,4)) == 13 &&
            std::equal(bytes.begin()+12,bytes.begin()+16,"IHDR") &&
            IconDecoder::u32(bytes.subspan(16,4)) == 32 && IconDecoder::u32(bytes.subspan(20,4)) == 32 &&
            bytes[24] == 8 && bytes[25] == 6 && !bytes[26] && !bytes[27] && !bytes[28],
            "normalized icon is a static 32x32 RGBA8 PNG");
    const auto count = static_cast<std::size_t>(IconDecoder::u32(bytes.subspan(33,4)));
    require(count <= bytes.size()-57 && bytes.size() == count+57 &&
            std::equal(bytes.begin()+37,bytes.begin()+41,"IDAT") &&
            IconDecoder::u32(bytes.subspan(count+45,4)) == 0 &&
            std::equal(bytes.begin()+count+49,bytes.begin()+count+53,"IEND"),
            "icon contains only IHDR, IDAT and IEND with no source metadata or trailing bytes");
    for (const auto& part : {std::pair<std::size_t,std::size_t>{12,17},{37,count+4},{count+49,4}})
        require(static_cast<std::uint32_t>(crc32(0,bytes.data()+part.first,static_cast<uInt>(part.second))) ==
                IconDecoder::u32(bytes.subspan(part.first+part.second,4)),"icon chunk CRC is exact");
    std::vector<std::uint8_t> raw((32*4+1)*32);
    uLongf output = static_cast<uLongf>(raw.size());
    uLong input = static_cast<uLong>(count);
    require(uncompress2(raw.data(),&output,bytes.data()+41,&input) == Z_OK &&
            output == raw.size() && input == count,"icon zlib stream consumes exactly the bounded RGBA scanlines");
    for (std::size_t y = 0; y < 32; ++y)
        require(raw[y*129] == 0 && std::equal(raw.begin()+y*129+1,raw.begin()+(y+1)*129,
                                            expected.pixels.begin()+y*128),"icon scanlines preserve full-image sampled pixels");
}
void test_icon_normalization() {
    MockHttp http;
    IconDecoder decoder;
    auto source = image(64,32);
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 64; ++x)
        fill(source,x,y,1,1,{static_cast<std::uint8_t>(x * 3),static_cast<std::uint8_t>(y * 7),83,129});
    const EncodedImage encoded{encode_skin_png(source),"image/png",ImageKind::skin};
    ImageLimits limits;
    limits.max_decoded_bytes = IconDecoder::plane;
    limits.max_active_bytes = IconDecoder::plane * 3 + 32 * 32 * 4 + icon_png_encoding_working_bytes;
    ImageService images(http,decoder,limits);
    decoder.inside_decode = [&] {
        require(images.cache_stats().active_bytes == limits.max_active_bytes,
                "icon reserves decoder peak, 32x32 output and bounded encoder arena before decoding");
    };
    auto expected = image(32,32);
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x)
        fill(expected,x,y,1,1,pixel(source,x * 2 + 1,y));
    const auto icon = images.normalize_icon(encoded);
    require(decoder.probes == 1 && decoder.decodes == 1 && icon.mime == "image/png" && icon.kind == ImageKind::avatar &&
            icon.bytes == encode_icon_png(expected) && icon.bytes.size() <= icon_png_max_bytes,
            "icon probes and decodes once and scales the complete image without skin crop or avatar margin");
    verify_sanitized_icon(icon,expected);
    require(images.cache_stats().active_bytes == 0 && images.cache_stats().active_decodes == 0 &&
            images.cache_stats().entries == 0 && http.calls == 0,
            "icon publication releases all leases and performs no cache or network side effects");
    expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(icon.bytes); });
    expect_error(IdentityErrorCode::unsupported_image,[&] { encode_skin_png(expected); });
    expect_error(IdentityErrorCode::unsupported_image,[&] { encode_icon_png(image(31,32)); });
    --limits.max_active_bytes;
    ImageService too_small(http,decoder,limits);
    expect_error(IdentityErrorCode::quota,[&] { too_small.normalize_icon(encoded); });
    decoder.working_hint = std::numeric_limits<std::size_t>::max();
    expect_error(IdentityErrorCode::quota,[&] { images.normalize_icon(encoded); });
    decoder.working_hint = IconDecoder::plane-1;
    expect_error(IdentityErrorCode::unsupported_image,[&] { images.normalize_icon(encoded); });
    decoder.working_hint = IconDecoder::plane*3;
    auto invalid = encoded;
    std::fill_n(invalid.bytes.begin()+16,4,std::uint8_t{0});
    expect_error(IdentityErrorCode::image_too_large,[&] { images.normalize_icon(invalid); });
    require(decoder.decodes == 1,"icon dimensions, working overflow and quota reject before pixel allocation");
    nickel::Cancellation cancellation; cancellation.cancel();
    const auto probes_before_cancel = decoder.probes.load();
    expect_error(IdentityErrorCode::cancelled,[&] { images.normalize_icon(encoded,cancellation); });
    require(decoder.probes == probes_before_cancel && decoder.decodes == 1,"pre-cancelled icon does not enter probe or decode");
    nickel::Cancellation during_decode;
    decoder.inside_decode = [&] { during_decode.cancel(); };
    expect_error(IdentityErrorCode::cancelled,[&] { images.normalize_icon(encoded,during_decode); });
    require(images.cache_stats().active_bytes == 0,"icon cancellation after admission releases encoder and pixel reservations");
    decoder.inside_decode = [] { throw std::runtime_error("FIXTURE_ICON_FAILURE"); };
    expect_error(IdentityErrorCode::unsupported_image,[&] { images.normalize_icon(encoded); });
    require(images.cache_stats().active_bytes == 0,"icon decoder exception releases encoder and pixel reservations");
    WorkingDecoder returned_plane;
    ImageLimits output_limits;
    output_limits.max_encoded_bytes = encoded_skin().bytes.size();
    ImageService output_limited(http,returned_plane,output_limits);
    expect_error(IdentityErrorCode::image_too_large,[&] { output_limited.normalize_icon(encoded_skin()); });
    require(returned_plane.decodes == 1 && output_limited.cache_stats().active_bytes == 0 &&
            output_limited.cache_stats().active_decodes == 0,"encoded-output rejection after encoding releases the complete lease");
}
}

void run_media_lifecycle_tests() {
    test_composition_active_budget();
    test_preallocation_quota_and_epoch_cleanup();
    test_global_active_wait_and_cancel();
    test_decoder_working_peak();
    test_decoder_working_preallocation_and_unwind();
    test_decoder_working_wait_and_cancel();
    test_icon_normalization();
    std::cout << "media lifecycle: working-set/output leases, icon32 PNG normalization, preallocation quotas, cancellation and bounded epochs passed\n";
}
} // namespace identity_test
