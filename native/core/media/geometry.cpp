#include "images.hpp"
#include "builtin_heads.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace nickel::media {
namespace {
using accounts::IdentityError;
using accounts::IdentityErrorCode;

void validate_pixels(const RgbaImage& image, const ImageLimits& limits) {
    const auto pixels = static_cast<std::uint64_t>(image.width) * image.height;
    if (image.width == 0 || image.height == 0 || image.width > limits.max_dimension ||
        image.height > limits.max_dimension || pixels > limits.max_pixels ||
        pixels > limits.max_decoded_bytes / 4 || pixels > std::numeric_limits<std::size_t>::max() / 4)
        throw IdentityError(IdentityErrorCode::image_too_large);
    if (image.pixels.size() != static_cast<std::size_t>(pixels) * 4)
        throw IdentityError(IdentityErrorCode::unsupported_image);
}

RgbaImage transparent(std::uint32_t width, std::uint32_t height, const ImageLimits& limits) {
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (!width || !height || width > limits.max_dimension || height > limits.max_dimension ||
        pixels > limits.max_pixels || pixels > limits.max_decoded_bytes / 4 ||
        pixels > std::numeric_limits<std::size_t>::max() / 4)
        throw IdentityError(IdentityErrorCode::image_too_large);
    return {width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(pixels) * 4, 0)};
}

void over(const std::uint8_t* source, std::uint8_t* target) {
    const std::uint32_t alpha = source[3];
    const std::uint32_t bottom = static_cast<std::uint32_t>(target[3]) * (255 - alpha);
    const std::uint32_t total = alpha * 255 + bottom;
    if (!total) return;
    for (unsigned c = 0; c < 3; ++c) {
        target[c] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(source[c]) * alpha * 255 +
                                             static_cast<std::uint32_t>(target[c]) * bottom + total / 2) / total);
    }
    target[3] = static_cast<std::uint8_t>((total + 127) / 255);
}

void draw(const RgbaImage& source, RgbaImage& target, const PixelRect& from, const PixelRect& to) {
    if (from.x < 0 || from.y < 0 || from.width <= 0 || from.height <= 0 ||
        from.x + from.width > source.width || from.y + from.height > source.height ||
        to.width <= 0 || to.height <= 0) throw IdentityError(IdentityErrorCode::unsupported_image);
    for (std::uint32_t y = 0; y < target.height; ++y) {
        const double center_y = y + 0.5;
        if (center_y < to.y || center_y >= to.y + to.height) continue;
        const auto sy = static_cast<std::uint32_t>(from.y + std::floor((center_y - to.y) * from.height / to.height));
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const double center_x = x + 0.5;
            if (center_x < to.x || center_x >= to.x + to.width) continue;
            const auto sx = static_cast<std::uint32_t>(from.x + std::floor((center_x - to.x) * from.width / to.width));
            over(&source.pixels[(static_cast<std::size_t>(sy) * source.width + sx) * 4],
                 &target.pixels[(static_cast<std::size_t>(y) * target.width + x) * 4]);
        }
    }
}

bool opaque(const RgbaImage& image, const PixelRect& rect) {
    for (std::uint32_t y = static_cast<std::uint32_t>(rect.y); y < rect.y + rect.height; ++y)
        for (std::uint32_t x = static_cast<std::uint32_t>(rect.x); x < rect.x + rect.width; ++x)
            if (image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4 + 3]) return true;
    return false;
}

void mirror(const RgbaImage& source, RgbaImage& target, std::uint32_t sx, std::uint32_t sy,
            std::uint32_t dx, std::uint32_t dy, std::uint32_t width, std::uint32_t height) {
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        const auto from = (static_cast<std::size_t>(sy + y) * source.width + sx + width - 1 - x) * 4;
        const auto to = (static_cast<std::size_t>(dy + y) * target.width + dx + x) * 4;
        std::copy_n(source.pixels.begin() + from, 4, target.pixels.begin() + to);
    }
}

std::array<PixelRect, 6> arm_faces(unsigned x, unsigned y, unsigned width) {
    return {{{static_cast<double>(x + 4), static_cast<double>(y), static_cast<double>(width), 4},
             {static_cast<double>(x + 4 + width), static_cast<double>(y), static_cast<double>(width), 4},
             {static_cast<double>(x), static_cast<double>(y + 4), 4, 12},
             {static_cast<double>(x + 4), static_cast<double>(y + 4), static_cast<double>(width), 12},
             {static_cast<double>(x + 4 + width), static_cast<double>(y + 4), 4, 12},
             {static_cast<double>(x + 8 + width), static_cast<double>(y + 4), static_cast<double>(width), 12}}};
}
}

HeadGeometry head_geometry(std::uint32_t width, std::uint32_t height,
                           std::uint32_t size, ImageKind kind) {
    if (!width || !height || !size) throw IdentityError(IdentityErrorCode::unsupported_image);
    HeadGeometry geometry;
    const double face = size * 48.0 / 64.0;
    geometry.face_destination = {(size - face) / 2, (size - face) / 2, face, face};
    if (kind == ImageKind::skin && width >= 64 && height >= 32) {
        const auto unit = std::max<std::uint32_t>(1, width / 64);
        if (height < 16 * unit) throw IdentityError(IdentityErrorCode::unsupported_image);
        geometry.face_source = {8.0 * unit, 8.0 * unit, 8.0 * unit, 8.0 * unit};
        // A high-res legacy sheet remains legacy: compare height to the scaled atlas.
        if (height >= 64 * unit) {
            const double hat = size * 56.0 / 64.0;
            geometry.hat_source = PixelRect{40.0 * unit, 8.0 * unit, 8.0 * unit, 8.0 * unit};
            geometry.hat_destination = PixelRect{(size - hat) / 2, (size - hat) / 2, hat, hat};
        }
    } else {
        const double edge = std::min(width, height);
        geometry.face_source = {0, 0, edge, edge};
    }
    return geometry;
}

RgbaImage compose_head(const RgbaImage& image, std::uint32_t size, ImageKind kind,
                       const ImageLimits& limits) {
    validate_pixels(image, limits);
    auto output = transparent(size, size, limits);
    const auto geometry = head_geometry(image.width, image.height, size, kind);
    draw(image, output, geometry.face_source, geometry.face_destination);
    if (geometry.hat_source && geometry.hat_destination && opaque(image, *geometry.hat_source))
        draw(image, output, *geometry.hat_source, *geometry.hat_destination);
    return output;
}

RgbaImage resize_image(const RgbaImage& image, std::uint32_t width,
                       std::uint32_t height, const ImageLimits& limits) {
    validate_pixels(image, limits);
    auto output = transparent(width, height, limits);
    draw(image, output, {0, 0, static_cast<double>(image.width), static_cast<double>(image.height)},
         {0, 0, static_cast<double>(width), static_cast<double>(height)});
    return output;
}

RgbaImage builtin_head_asset(bool alex, const ImageLimits& limits) {
    const auto width = alex ? detail::alex_head_width : detail::steve_head_width;
    const auto height = alex ? detail::alex_head_height : detail::steve_head_height;
    auto asset = transparent(width, height, limits);
    const auto runs = alex ? std::span<const detail::BuiltinHeadRun>(detail::alex_head_runs)
                           : std::span<const detail::BuiltinHeadRun>(detail::steve_head_runs);
    std::size_t offset = 0;
    for (const auto& run : runs) {
        if (run.count > (asset.pixels.size() - offset) / 4)
            throw IdentityError(IdentityErrorCode::unsupported_image);
        for (std::uint32_t i = 0; i < run.count; ++i) {
            std::copy(run.rgba.begin(), run.rgba.end(), asset.pixels.begin() + offset);
            offset += 4;
        }
    }
    if (offset != asset.pixels.size()) throw IdentityError(IdentityErrorCode::unsupported_image);
    return asset;
}

RgbaImage builtin_head(bool alex, std::uint32_t size, const ImageLimits& limits) {
    return compose_head(builtin_head_asset(alex, limits), size, ImageKind::avatar, limits);
}

RgbaImage adapt_skin(const RgbaImage& image, bool source_slim, bool target_slim,
                     bool legacy, const ImageLimits& limits) {
    validate_pixels(image, limits);
    if (image.width < 64 || image.width % 64 || (image.height != image.width && image.height * 2 != image.width))
        throw IdentityError(IdentityErrorCode::unsupported_image);
    const unsigned unit = image.width / 64;
    if (image.height == image.width / 2 && source_slim) throw IdentityError(IdentityErrorCode::invalid_input);
    auto modern = transparent(image.width, image.width, limits);
    std::copy(image.pixels.begin(), image.pixels.end(), modern.pixels.begin());
    if (image.height == image.width / 2) {
        constexpr unsigned faces[][6] = {
            {4,16,20,48,4,4},{8,16,24,48,4,4},{8,20,16,52,4,12},{4,20,20,52,4,12},
            {0,20,24,52,4,12},{12,20,28,52,4,12},{44,16,36,48,4,4},{48,16,40,48,4,4},
            {48,20,32,52,4,12},{44,20,36,52,4,12},{40,20,40,52,4,12},{52,20,44,52,4,12}
        };
        for (const auto& face : faces) mirror(image, modern, face[0]*unit, face[1]*unit,
                                             face[2]*unit, face[3]*unit, face[4]*unit, face[5]*unit);
    }
    const bool destination_slim = target_slim && !legacy;
    if (source_slim != destination_slim) {
        auto output = modern;
        for (const auto origin : {std::array<unsigned,2>{40,16}, {40,32}, {32,48}, {48,48}}) {
            for (unsigned y = origin[1]*unit; y < (origin[1]+16)*unit; ++y)
                std::fill_n(output.pixels.begin() + (static_cast<std::size_t>(y)*output.width + origin[0]*unit)*4, 16*unit*4, std::uint8_t{0});
            const auto from = arm_faces(origin[0], origin[1], source_slim ? 3 : 4);
            const auto to = arm_faces(origin[0], origin[1], destination_slim ? 3 : 4);
            for (unsigned i = 0; i < from.size(); ++i) {
                const auto sw = static_cast<unsigned>(from[i].width)*unit;
                const auto tw = static_cast<unsigned>(to[i].width)*unit;
                for (unsigned y = 0; y < static_cast<unsigned>(from[i].height)*unit; ++y) for (unsigned x = 0; x < tw; ++x) {
                    const auto sx = static_cast<unsigned>(from[i].x)*unit + std::min(sw-1, (2*x+1)*sw/(2*tw));
                    const auto sy = static_cast<unsigned>(from[i].y)*unit + y;
                    const auto tx = static_cast<unsigned>(to[i].x)*unit + x;
                    const auto ty = static_cast<unsigned>(to[i].y)*unit + y;
                    std::copy_n(modern.pixels.begin() + (static_cast<std::size_t>(sy)*modern.width+sx)*4, 4,
                                output.pixels.begin() + (static_cast<std::size_t>(ty)*output.width+tx)*4);
                }
            }
        }
        modern = std::move(output);
    }
    if (legacy) {
        constexpr unsigned overlays[][6] = {{0,32,0,16,16,16},{16,32,16,16,24,16},{40,32,40,16,16,16}};
        for (const auto& box : overlays) for (unsigned y = 0; y < box[5]*unit; ++y) for (unsigned x = 0; x < box[4]*unit; ++x) {
            over(&modern.pixels[(static_cast<std::size_t>(box[1]*unit+y)*modern.width+box[0]*unit+x)*4],
                 &modern.pixels[(static_cast<std::size_t>(box[3]*unit+y)*modern.width+box[2]*unit+x)*4]);
        }
        modern.height /= 2;
        modern.pixels.resize(static_cast<std::size_t>(modern.width)*modern.height*4);
    }
    return modern;
}

} // namespace nickel::media
