#pragma once

#include "core/documents/body.hpp"
#include "core/media/images.hpp"
#include <QImage>

namespace nickel::ui {
// Probe reports all simultaneous native, conversion and returned pixel planes.
class DecodeImage final : public media::ImageDecoder {
public:
    media::ImageProbe probe(std::span<const std::uint8_t> encoded) override;
    media::RgbaImage decode(std::span<const std::uint8_t> encoded, const media::ImageProbe& expected,
                            const media::ImageLimits& limits, std::stop_token stop) override;
};
documents::RasterImage decodeDescriptionImage(std::span<const std::uint8_t> encoded,
    const documents::ImageLimits& limits, const Cancellation& cancellation);
QImage imageFromRgba(const media::RgbaImage& image);
QImage imageFromRaster(const documents::RasterImage& image);
} // namespace nickel::ui
