#include "skin_png.hpp"
#include "../accounts/detail/util.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <limits>

namespace nickel::media {
namespace {
using accounts::IdentityError;
using accounts::IdentityErrorCode;
using accounts::detail::check_stop;
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
constexpr std::array<std::array<std::size_t, 4>, 7> adam7{{
    {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
    {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}}};

[[noreturn]] void invalid() { throw IdentityError(IdentityErrorCode::unsupported_image); }
std::uint32_t be32(std::span<const std::uint8_t> bytes) {
    return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
           (std::uint32_t{bytes[2]} << 8) | bytes[3];
}
std::uint16_t be16(std::span<const std::uint8_t> bytes) {
    return static_cast<std::uint16_t>((std::uint16_t{bytes[0]} << 8) | bytes[1]);
}
void append32(Bytes& bytes, std::uint32_t value) {
    for (const unsigned shift : {24U, 16U, 8U, 0U})
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
std::uint32_t crc(std::span<const std::uint8_t> bytes) {
    return static_cast<std::uint32_t>(crc32(0, bytes.data(), static_cast<uInt>(bytes.size())));
}
unsigned channels(std::uint8_t color) {
    switch (color) {
    case 0: case 3: return 1;
    case 2: return 3;
    case 4: return 2;
    case 6: return 4;
    default: invalid();
    }
}
bool valid_depth(std::uint8_t color, std::uint8_t depth) {
    switch (color) {
    case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2: case 4: case 6: return depth == 8 || depth == 16;
    default: return false;
    }
}
std::size_t pass_length(std::size_t length, std::size_t start, std::size_t step) {
    return length <= start ? 0 : (length - start + step - 1) / step;
}
std::uint8_t paeth(std::uint8_t left, std::uint8_t up, std::uint8_t corner) {
    const int p = static_cast<int>(left) + up - corner;
    const int a = std::abs(p - left), b = std::abs(p - up), c = std::abs(p - corner);
    return a <= b && a <= c ? left : b <= c ? up : corner;
}
void unfilter(std::uint8_t filter, Bytes& row, const Bytes& previous, std::size_t bpp) {
    for (std::size_t i = 0; i < row.size(); ++i) {
        const std::uint8_t left = i >= bpp ? row[i - bpp] : std::uint8_t{0};
        const std::uint8_t up = previous[i];
        const std::uint8_t corner = i >= bpp ? previous[i - bpp] : std::uint8_t{0};
        std::uint8_t predictor{};
        switch (filter) {
        case 0: break;
        case 1: predictor = left; break;
        case 2: predictor = up; break;
        case 3: predictor = static_cast<std::uint8_t>((static_cast<unsigned>(left) + up) / 2); break;
        case 4: predictor = paeth(left, up, corner); break;
        default: invalid();
        }
        row[i] = static_cast<std::uint8_t>(static_cast<unsigned>(row[i]) + predictor);
    }
}
std::uint16_t sample(std::span<const std::uint8_t> row, std::size_t index, std::uint8_t depth) {
    if (depth == 16) return be16(row.subspan(index * 2, 2));
    if (depth == 8) return row[index];
    const auto bit = index * depth;
    const auto shift = 8 - depth - bit % 8;
    return static_cast<std::uint16_t>((row[bit / 8] >> shift) & ((1U << depth) - 1));
}
std::array<std::uint8_t, 4> pixel(std::span<const std::uint8_t> row, std::size_t x,
                                 std::uint8_t color, std::uint8_t depth,
                                 const Bytes& palette, const Bytes& transparency) {
    const auto count = channels(color);
    const auto value = [&](std::size_t c) { return sample(row, x * count + c, depth); };
    const auto scale = [depth](std::uint16_t v) {
        return static_cast<std::uint8_t>(std::uint32_t{v} * 255 / ((1U << depth) - 1));
    };
    const auto transparent = [&](std::size_t c) { return be16(std::span(transparency).subspan(c * 2, 2)); };
    switch (color) {
    case 0: {
        const auto gray = value(0);
        const auto alpha = transparency.size() == 2 && gray == transparent(0) ? 0 : 255;
        return {scale(gray), scale(gray), scale(gray), static_cast<std::uint8_t>(alpha)};
    }
    case 2: {
        const auto alpha = transparency.size() == 6 && value(0) == transparent(0) &&
                           value(1) == transparent(1) && value(2) == transparent(2) ? 0 : 255;
        return {scale(value(0)), scale(value(1)), scale(value(2)), static_cast<std::uint8_t>(alpha)};
    }
    case 3: {
        const auto i = static_cast<std::size_t>(value(0));
        if (i >= palette.size() / 3) invalid();
        return {palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2],
                i < transparency.size() ? transparency[i] : std::uint8_t{255}};
    }
    case 4: return {scale(value(0)), scale(value(0)), scale(value(0)), scale(value(1))};
    case 6: return {scale(value(0)), scale(value(1)), scale(value(2)), scale(value(3))};
    default: invalid();
    }
}
void chunk(Bytes& output, std::string_view kind, std::span<const std::uint8_t> data) {
    append32(output, static_cast<std::uint32_t>(data.size()));
    const auto start = output.size();
    output.insert(output.end(), kind.begin(), kind.end());
    output.insert(output.end(), data.begin(), data.end());
    append32(output, crc(std::span(output).subspan(start)));
}
struct DeflateArena {
    static constexpr std::size_t max_bytes = 320 * 1024;
    static_assert(max_bytes % sizeof(std::max_align_t) == 0);
    std::vector<std::max_align_t> storage;
    std::size_t used = 0;
    DeflateArena() : storage(max_bytes / sizeof(std::max_align_t)) {
        std::memset(storage.data(), 0, max_bytes);
    }
    static voidpf allocate(voidpf opaque, uInt items, uInt size) noexcept {
        auto& arena = *static_cast<DeflateArena*>(opaque);
        if (size && items > max_bytes / size) return nullptr;
        const auto bytes = static_cast<std::size_t>(items) * size;
        constexpr auto alignment = alignof(std::max_align_t);
        const auto offset = (arena.used + alignment - 1) / alignment * alignment;
        if (offset > max_bytes || bytes > max_bytes - offset) return nullptr;
        arena.used = offset + bytes;
        return reinterpret_cast<std::uint8_t*>(arena.storage.data()) + offset;
    }
    static void release(voidpf, voidpf) noexcept {}
};
Bytes encode_png(const RgbaImage& image, std::stop_token stop, std::size_t working_limit = 0) {
    check_stop(stop);
    const auto row_bytes = static_cast<std::size_t>(image.width) * 4;
    const auto raw_bytes = (row_bytes + 1) * image.height;
    const auto compressed_bytes = static_cast<std::size_t>(compressBound(static_cast<uLong>(raw_bytes)));
    constexpr std::size_t overhead = 8 + 25 + 12 + 12;
    const auto output_bytes = compressed_bytes + overhead;
    if (working_limit && DeflateArena::max_bytes + raw_bytes + compressed_bytes + output_bytes + 13 > working_limit)
        throw IdentityError(IdentityErrorCode::quota);
    Bytes raw;
    raw.reserve(raw_bytes);
    for (std::size_t y = 0; y < image.height; ++y) {
        check_stop(stop);
        raw.push_back(0);
        raw.insert(raw.end(), image.pixels.begin() + y * row_bytes,
                   image.pixels.begin() + (y + 1) * row_bytes);
    }
    DeflateArena arena;
    z_stream stream{};
    stream.zalloc = &DeflateArena::allocate;
    stream.zfree = &DeflateArena::release;
    stream.opaque = &arena;
    const auto initialized = deflateInit(&stream, Z_BEST_SPEED);
    if (initialized == Z_MEM_ERROR) throw IdentityError(IdentityErrorCode::quota);
    if (initialized != Z_OK) invalid();
    struct DeflateEnd { z_stream& stream; ~DeflateEnd() { deflateEnd(&stream); } } cleanup{stream};
    Bytes compressed(compressed_bytes);
    stream.next_in = raw.data();
    stream.avail_in = static_cast<uInt>(raw.size());
    stream.next_out = compressed.data();
    stream.avail_out = static_cast<uInt>(compressed.size());
    const auto status = deflate(&stream, Z_FINISH);
    if (status == Z_MEM_ERROR) throw IdentityError(IdentityErrorCode::quota);
    if (status != Z_STREAM_END || stream.total_in != raw.size()) invalid();
    compressed.resize(static_cast<std::size_t>(stream.total_out));
    Bytes output, header;
    output.reserve(output_bytes);
    output.insert(output.end(), signature.begin(), signature.end());
    header.reserve(13);
    append32(header, image.width); append32(header, image.height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    chunk(output, "IHDR", header); chunk(output, "IDAT", compressed); chunk(output, "IEND", {});
    check_stop(stop);
    return output;
}
} // namespace

RgbaImage decode_skin_png(std::span<const std::uint8_t> bytes, std::stop_token stop) {
    check_stop(stop);
    if (bytes.size() > skin_png_max_bytes) throw IdentityError(IdentityErrorCode::image_too_large);
    if (bytes.size() < signature.size() || !std::equal(signature.begin(), signature.end(), bytes.begin())) invalid();
    std::size_t position = signature.size();
    std::uint32_t height{};
    std::uint8_t depth{}, color{}, interlace{};
    bool header{}, palette_seen{}, transparency_seen{}, idat_seen{}, idat_ended{}, ended{};
    Bytes palette, transparency, compressed;
    while (position < bytes.size()) {
        check_stop(stop);
        if (bytes.size() - position < 12) invalid();
        const auto length = static_cast<std::size_t>(be32(bytes.subspan(position, 4)));
        if (length > bytes.size() - position - 12) invalid();
        const auto end = position + 12 + length;
        const auto type = bytes.subspan(position + 4, 4);
        const auto data = bytes.subspan(position + 8, length);
        if (!std::all_of(type.begin(), type.end(), [](std::uint8_t c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            }) || (type[2] & 0x20) || crc(bytes.subspan(position + 4, 4 + length)) != be32(bytes.subspan(end - 4, 4))) invalid();
        const std::string_view kind(reinterpret_cast<const char*>(type.data()), type.size());
        if (!header && kind != "IHDR") invalid();
        if (idat_seen && kind != "IDAT") idat_ended = true;
        if (kind == "IHDR") {
            if (header || length != 13 || be32(data.first(4)) != 64) invalid();
            height = be32(data.subspan(4, 4));
            depth = data[8]; color = data[9]; interlace = data[12];
            if ((height != 32 && height != 64) || !valid_depth(color, depth) || data[10] || data[11] || interlace > 1) invalid();
            header = true;
        } else if (kind == "PLTE") {
            if (palette_seen || idat_seen || transparency_seen || data.empty() || data.size() > 768 || data.size() % 3) invalid();
            palette_seen = true;
            palette.assign(data.begin(), data.end());
        } else if (kind == "tRNS") {
            if (transparency_seen || idat_seen) invalid();
            transparency_seen = true;
            transparency.assign(data.begin(), data.end());
        } else if (kind == "IDAT") {
            if (idat_ended) invalid();
            idat_seen = true;
            compressed.insert(compressed.end(), data.begin(), data.end());
        } else if (kind == "IEND") {
            if (length || !idat_seen || end != bytes.size()) invalid();
            ended = true;
        } else if (kind == "acTL" || kind == "fcTL" || kind == "fdAT" || !(type[0] & 0x20)) {
            invalid();
        }
        position = end;
    }
    if (!ended || !header || (color == 3 && (palette.empty() || palette.size() / 3 > (std::size_t{1} << depth))) ||
        ((color == 0 || color == 4) && palette_seen)) invalid();
    if (transparency_seen) {
        const bool valid = color == 0 ? transparency.size() == 2 : color == 2 ? transparency.size() == 6 :
                           color == 3 ? palette_seen && !transparency.empty() && transparency.size() <= palette.size() / 3 : false;
        if (!valid) invalid();
        if (color == 0 || color == 2) {
            for (std::size_t i = 0; i < transparency.size(); i += 2)
                if (be16(std::span(transparency).subspan(i, 2)) >= (1U << depth)) invalid();
        }
    }
    const std::size_t bits = static_cast<std::size_t>(channels(color)) * depth;
    const std::array<std::array<std::size_t, 4>, 1> plain{{{0, 0, 1, 1}}};
    const auto passes = interlace ? std::span<const std::array<std::size_t, 4>>(adam7) :
                                   std::span<const std::array<std::size_t, 4>>(plain);
    std::size_t expected{};
    for (const auto& pass : passes) {
        const auto w = pass_length(64, pass[0], pass[2]);
        const auto h = pass_length(height, pass[1], pass[3]);
        if (w) expected += ((w * bits + 7) / 8 + 1) * h;
    }
    // One extra byte catches expansion beyond the exact scanline budget. A valid
    // pixel count alone is insufficient: require the Adler trailer and EOF too.
    Bytes raw(expected + 1);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) invalid();
    struct InflateEnd { z_stream& stream; ~InflateEnd() { inflateEnd(&stream); } } cleanup{stream};
    stream.next_in = compressed.data();
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = raw.data();
    stream.avail_out = static_cast<uInt>(raw.size());
    const auto status = inflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END || stream.total_out != expected || stream.total_in != compressed.size()) invalid();
    check_stop(stop);
    RgbaImage output{64, height, Bytes(static_cast<std::size_t>(64) * height * 4)};
    std::size_t cursor{};
    for (const auto& pass : passes) {
        const auto w = pass_length(64, pass[0], pass[2]);
        const auto h = pass_length(height, pass[1], pass[3]);
        if (!w || !h) continue;
        const auto stride = (w * bits + 7) / 8;
        Bytes previous(stride);
        for (std::size_t y = 0; y < h; ++y) {
            check_stop(stop);
            Bytes row(raw.begin() + cursor + 1, raw.begin() + cursor + 1 + stride);
            unfilter(raw[cursor], row, previous, (bits + 7) / 8);
            for (std::size_t x = 0; x < w; ++x) {
                const auto rgba = pixel(row, x, color, depth, palette, transparency);
                const auto destination = ((pass[1] + y * pass[3]) * 64 + pass[0] + x * pass[2]) * 4;
                std::copy(rgba.begin(), rgba.end(), output.pixels.begin() + destination);
            }
            previous = std::move(row);
            cursor += stride + 1;
        }
    }
    return output;
}

std::vector<std::uint8_t> encode_skin_png(const RgbaImage& image, std::stop_token stop) {
    check_stop(stop);
    if (image.width != 64 || (image.height != 32 && image.height != 64) ||
        image.pixels.size() != static_cast<std::size_t>(64) * image.height * 4) invalid();
    return encode_png(image, stop);
}

std::vector<std::uint8_t> encode_icon_png(const RgbaImage& image, std::stop_token stop) {
    check_stop(stop);
    if (image.width != 32 || image.height != 32 || image.pixels.size() != 32 * 32 * 4) invalid();
    auto output = encode_png(image, stop, icon_png_encoding_working_bytes);
    if (output.size() > icon_png_max_bytes) throw IdentityError(IdentityErrorCode::image_too_large);
    return output;
}

std::vector<std::uint8_t> adapt_skin_png(std::span<const std::uint8_t> bytes,
                                        bool source_slim, bool target_slim,
                                        bool legacy, std::stop_token stop) {
    auto image = decode_skin_png(bytes, stop);
    image = adapt_skin(image, source_slim, target_slim, legacy);
    return encode_skin_png(image, stop);
}

} // namespace nickel::media
