#pragma once

#include "images.hpp"

namespace nickel::media {

inline constexpr std::size_t skin_png_max_bytes = 1024 * 1024;
inline constexpr std::size_t icon_png_max_bytes = 8 * 1024;
// Bounded zlib arena, scanlines, compressed data and sanitized PNG publication.
inline constexpr std::size_t icon_png_encoding_working_bytes = 336 * 1024;

// Full CRC/zlib validation; exactly 64x32 or 64x64, static, unpremultiplied RGBA.
RgbaImage decode_skin_png(std::span<const std::uint8_t> bytes,
                          std::stop_token stop = {});
std::vector<std::uint8_t> encode_skin_png(const RgbaImage& image,
                                         std::stop_token stop = {});
// Uses the same PNG encoder, but accepts only a complete 32x32 RGBA icon.
std::vector<std::uint8_t> encode_icon_png(const RgbaImage& image,
                                         std::stop_token stop = {});
std::vector<std::uint8_t> adapt_skin_png(std::span<const std::uint8_t> bytes,
                                        bool source_slim, bool target_slim,
                                        bool legacy, std::stop_token stop = {});

} // namespace nickel::media
