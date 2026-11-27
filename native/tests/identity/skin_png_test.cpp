#include "fixtures.hpp"
#include "../../core/media/skin_png.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <span>

namespace identity_test {
namespace {
using namespace nickel::media;
using nickel::accounts::IdentityErrorCode;
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t,8> png_signature{137,80,78,71,13,10,26,10};
void u32(Bytes& target, std::uint32_t value) {
    for (const unsigned shift : {24U,16U,8U,0U}) target.push_back(static_cast<std::uint8_t>(value >> shift));
}
std::uint32_t u32(std::span<const std::uint8_t> value) {
    return (std::uint32_t{value[0]} << 24) | (std::uint32_t{value[1]} << 16) |
           (std::uint32_t{value[2]} << 8) | value[3];
}
void append_chunk(Bytes& target, std::string_view kind, std::span<const std::uint8_t> data) {
    u32(target,static_cast<std::uint32_t>(data.size()));
    const auto start = target.size();
    target.insert(target.end(),kind.begin(),kind.end());
    target.insert(target.end(),data.begin(),data.end());
    u32(target,static_cast<std::uint32_t>(crc32(0,target.data()+start,static_cast<uInt>(target.size()-start))));
}
Bytes compressed(std::span<const std::uint8_t> raw) {
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    Bytes output(size);
    require(compress2(output.data(),&size,raw.data(),static_cast<uLong>(raw.size()),Z_BEST_SPEED) == Z_OK,"fixture zlib encoding");
    output.resize(size);
    return output;
}
Bytes make_png(std::uint8_t color, std::uint8_t depth, std::uint8_t interlace,
               std::span<const std::uint8_t> raw, const Bytes& palette = {}, const Bytes& transparency = {}) {
    Bytes output(png_signature.begin(),png_signature.end()), header;
    u32(header,64); u32(header,32);
    header.insert(header.end(),{depth,color,0,0,interlace});
    append_chunk(output,"IHDR",header);
    if (!palette.empty()) append_chunk(output,"PLTE",palette);
    if (!transparency.empty()) append_chunk(output,"tRNS",transparency);
    append_chunk(output,"IDAT",compressed(raw));
    append_chunk(output,"IEND",{});
    return output;
}
Bytes replace_chunk(const Bytes& input, std::string_view kind, const std::function<void(Bytes&)>& edit) {
    Bytes output(png_signature.begin(),png_signature.end());
    bool found{};
    for (std::size_t cursor = 8; cursor < input.size();) {
        const auto length = static_cast<std::size_t>(u32(std::span(input).subspan(cursor,4)));
        const std::string_view type(reinterpret_cast<const char*>(input.data()+cursor+4),4);
        Bytes data(input.begin()+cursor+8,input.begin()+cursor+8+length);
        if (type == kind) { edit(data); found = true; }
        append_chunk(output,type,data);
        cursor += length+12;
    }
    require(found,"fixture chunk edit found");
    return output;
}
void test_roundtrip_and_uv() {
    for (const unsigned height : {32U,64U}) {
        auto source = image(64,height);
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < 64; ++x)
            fill(source,x,y,1,1,{static_cast<std::uint8_t>(x*3),static_cast<std::uint8_t>(y*3),77,static_cast<std::uint8_t>((x+y)*2)});
        const auto png = encode_skin_png(source);
        require(png.size() < skin_png_max_bytes && decode_skin_png(png) == source,"static unpremultiplied PNG exact roundtrip");
        const auto modern = decode_skin_png(adapt_skin_png(png,false,false,false));
        require(modern.height == 64 && pixel(modern,8,8) == pixel(source,8,8),"skin adaptation preserves head pixels without replacing UUID");
        if (height == 32) {
            require(pixel(modern,20,52) == pixel(source,7,20),"legacy left leg UV is mirrored from the right leg");
            require(pixel(modern,36,52) == pixel(source,47,20),"legacy left arm UV is mirrored from the right arm");
            expect_error(IdentityErrorCode::invalid_input,[&] { adapt_skin_png(png,true,false,false); });
        }
        const auto legacy = decode_skin_png(adapt_skin_png(png,false,false,true));
        require(legacy.height == 32,"old resource-pack target emits a 64x32 sheet");
    }
    auto wide = image(64,64);
    fill(wide,44,20,1,12,{10,0,0,255}); fill(wide,45,20,1,12,{20,0,0,255});
    fill(wide,46,20,1,12,{30,0,0,255}); fill(wide,47,20,1,12,{40,0,0,255});
    const auto narrow = decode_skin_png(adapt_skin_png(encode_skin_png(wide),false,true,false));
    require(pixel(narrow,44,20)[0] == 10 && pixel(narrow,45,20)[0] == 30 && pixel(narrow,46,20)[0] == 40,
            "wide-to-slim arm UV uses pixel-center sampling, not a head crop");
    const auto expanded = decode_skin_png(adapt_skin_png(encode_skin_png(narrow),true,false,false));
    require(pixel(expanded,44,20)[0] == 10 && pixel(expanded,45,20)[0] == 30 &&
            pixel(expanded,46,20)[0] == 30 && pixel(expanded,47,20)[0] == 40,"slim-to-wide UV expands each arm face");
}
void test_palette_gray_and_16_bit() {
    Bytes indexed;
    for (unsigned y = 0; y < 32; ++y) { indexed.push_back(0); indexed.insert(indexed.end(),8,0x55); }
    const auto palette = make_png(3,1,0,indexed,{10,20,30,40,50,60},{255,31});
    const auto decoded = decode_skin_png(palette);
    require(pixel(decoded,0,0) == std::array<std::uint8_t,4>{10,20,30,255} &&
            pixel(decoded,1,0) == std::array<std::uint8_t,4>{40,50,60,31},"packed palette plus tRNS alpha decoded exactly");
    expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(make_png(3,1,0,indexed,{10,20,30})); });
    const auto too_many = make_png(3,1,0,indexed,{1,2,3,4,5,6,7,8,9});
    expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(too_many); });
    const auto gray = decode_skin_png(make_png(0,1,0,indexed,{}, {0,0}));
    require(pixel(gray,0,0) == std::array<std::uint8_t,4>{0,0,0,0} && pixel(gray,1,0)[0] == 255,"grayscale packed samples and transparent gray");
    Bytes rgba16;
    for (unsigned y = 0; y < 32; ++y) {
        rgba16.push_back(0);
        for (unsigned x = 0; x < 64; ++x) rgba16.insert(rgba16.end(),{0xff,0xff,0x80,0x80,0,0,0x40,0x40});
    }
    require(pixel(decode_skin_png(make_png(6,16,0,rgba16)),0,0) == std::array<std::uint8_t,4>{255,128,0,64},"16-bit RGBA samples scale without premultiplying");
}
void test_interlace_and_filters() {
    const auto source = image(64,32,{17,29,43,71});
    constexpr std::array<std::array<unsigned,4>,7> passes{{{0,0,8,8},{4,0,8,8},{0,4,4,8},{2,0,4,4},{0,2,2,4},{1,0,2,2},{0,1,1,2}}};
    Bytes raw;
    for (const auto& pass : passes) for (unsigned y = pass[1]; y < 32; y += pass[3]) {
        raw.push_back(0);
        for (unsigned x = pass[0]; x < 64; x += pass[2]) {
            const auto color = pixel(source,x,y); raw.insert(raw.end(),color.begin(),color.end());
        }
    }
    require(decode_skin_png(make_png(6,8,1,raw)) == source,"Adam7 pass dimensions and placement decode exactly");
    for (const std::uint8_t filter : {std::uint8_t{0},std::uint8_t{1},std::uint8_t{2},std::uint8_t{3},std::uint8_t{4}}) {
        Bytes scanlines;
        for (unsigned y = 0; y < 32; ++y) {
            scanlines.push_back(filter);
            for (unsigned i = 0; i < 64*4; ++i) {
                const auto value = source.pixels[y*64*4+i];
                const unsigned left = i >= 4 ? source.pixels[y*64*4+i-4] : 0;
                const unsigned up = y ? source.pixels[(y-1)*64*4+i] : 0;
                unsigned predictor{};
                if (filter == 1) predictor = left;
                else if (filter == 2) predictor = up;
                else if (filter == 3) predictor = (left+up)/2;
                else if (filter == 4) predictor = y && i >= 4 ? value : i >= 4 ? left : up;
                scanlines.push_back(static_cast<std::uint8_t>(static_cast<unsigned>(value)-predictor));
            }
        }
        require(decode_skin_png(make_png(6,8,0,scanlines)) == source,"all five PNG filters reconstruct bounded scanlines");
    }
}
void test_strict_stream_and_chunks() {
    const auto source = encode_skin_png(image(64,32,{1,2,3,4}));
    auto corrupt = source; corrupt[corrupt.size()/2] ^= 1;
    expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(corrupt); });
    for (std::size_t length = 0; length < source.size(); ++length)
        expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(std::span(source).first(length)); });
    auto trailing = source; trailing.push_back(0);
    expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(trailing); });
    const auto zlib_trailer = replace_chunk(source,"IDAT",[](Bytes& value) { value.pop_back(); });
    expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(zlib_trailer); });
    const auto extra_zlib = replace_chunk(source,"IDAT",[](Bytes& value) { value.push_back(0); });
    expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(extra_zlib); });
    const auto huge_geometry = replace_chunk(source,"IHDR",[](Bytes& value) { value[0] = 0x7f; });
    expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(huge_geometry); });
    Bytes bad_filter(32*(64*4+1),0); bad_filter[0] = 5;
    expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(make_png(6,8,0,bad_filter)); });
    Bytes too_much(32*(64*4+1)+1,0);
    expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(make_png(6,8,0,too_much)); });
    Bytes animated(png_signature.begin(),png_signature.end());
    const auto first_length = static_cast<std::size_t>(u32(std::span(source).subspan(8,4)))+12;
    animated.insert(animated.end(),source.begin()+8,source.begin()+8+first_length);
    append_chunk(animated,"acTL",Bytes(8,0));
    animated.insert(animated.end(),source.begin()+8+first_length,source.end());
    expect_error(IdentityErrorCode::unsupported_image,[&] { decode_skin_png(animated); });
    expect_error(IdentityErrorCode::image_too_large,[&] { decode_skin_png(Bytes(skin_png_max_bytes+1,0)); });
    nickel::Cancellation cancelled; cancelled.cancel();
    expect_error(IdentityErrorCode::cancelled,[&] { decode_skin_png(source,cancelled.stop_token()); });
    expect_error(IdentityErrorCode::cancelled,[&] { encode_skin_png(image(64,64),cancelled.stop_token()); });
}
}
void run_skin_png_tests() {
    test_roundtrip_and_uv();
    test_palette_gray_and_16_bit();
    test_interlace_and_filters();
    test_strict_stream_and_chunks();
    std::cout << "skin PNG: complete static decode, CRC/zlib bounds, formats, filters and UV parity passed\n";
}
} // namespace identity_test
