#include "DecodeImage.hpp"
#include "core/common/error.hpp"
#include <QBuffer>
#include <QImageReader>
#include <QPixelFormat>
#include <algorithm>
#include <cstring>
#include <limits>

namespace nickel::ui {
namespace {
QByteArray viewBytes(std::span<const std::uint8_t> encoded) {
    if (encoded.empty() || encoded.size() > std::size_t(std::numeric_limits<int>::max()))
        throw ErrorException({"image.encoding", "Image bytes are empty or too large"});
    return QByteArray::fromRawData(reinterpret_cast<const char*>(encoded.data()), qsizetype(encoded.size()));
}
QString mimeForFormat(const QByteArray& format) {
    if (format == "png") return "image/png";
    if (format == "jpg" || format == "jpeg") return "image/jpeg";
    if (format == "webp") return "image/webp";
    if (format == "gif") return "image/gif";
    if (format == "bmp") return "image/bmp";
    if (format == "avif") return "image/avif";
    throw ErrorException({"image.format", "Unsupported raster image decoder"});
}
void checkPixels(std::uint32_t width, std::uint32_t height, std::uint32_t maximumDimension, std::uint64_t maximumPixels) {
    if (width == 0 || height == 0 || width > maximumDimension || height > maximumDimension
        || width > std::uint32_t(std::numeric_limits<int>::max()) || height > std::uint32_t(std::numeric_limits<int>::max())
        || std::uint64_t(width) * height > maximumPixels)
        throw ErrorException({"image.quota", "Image geometry exceeds the canonical pixel budget"});
}
std::size_t planeBytes(std::uint32_t width, std::uint32_t height, std::size_t bytesPerPixel) {
    const auto pixels = std::uint64_t(width) * height;
    const auto maximum = std::uint64_t(std::numeric_limits<qsizetype>::max());
    if (!width || !height || !bytesPerPixel || pixels > maximum / bytesPerPixel
        || pixels > std::numeric_limits<std::size_t>::max() / bytesPerPixel)
        throw ErrorException({"image.quota", "Image pixel storage cannot be represented safely"});
    return std::size_t(pixels) * bytesPerPixel;
}
std::size_t addBytes(std::size_t a, std::size_t b) {
    if (b > std::numeric_limits<std::size_t>::max() - a)
        throw ErrorException({"image.quota", "Simultaneous image storage exceeds the byte range"});
    return a + b;
}
std::size_t nativePixelBytes(const std::string& mime) {
    // PNG may return RGBA64; AVIF can expose 128-bit floating point pixels.
    return mime == "image/avif" ? 16 : 8;
}
std::size_t workingBytes(const media::ImageProbe& probe) {
    const auto rgba = planeBytes(probe.width, probe.height, 4);
    return addBytes(planeBytes(probe.width, probe.height, nativePixelBytes(probe.mime)), addBytes(rgba, rgba));
}
void checkReaderStorage(QImageReader& reader, std::size_t bytesPerPixel) {
    const auto format = reader.imageFormat();
    if (format != QImage::Format_Invalid && QImage::toPixelFormat(format).bitsPerPixel() > bytesPerPixel * 8)
        throw ErrorException({"image.quota", "Native image format exceeds its metadata reservation"});
}
std::size_t checkedImageBytes(const QImage& image, const QSize& expected, std::size_t maximum) {
    if (image.isNull() || image.size() != expected || image.sizeInBytes() <= 0
        || std::uint64_t(image.sizeInBytes()) > maximum)
        throw ErrorException({"image.decode", "Raster image decoding failed or exceeds its reserved plane"});
    return std::size_t(image.sizeInBytes());
}
}
media::ImageProbe DecodeImage::probe(std::span<const std::uint8_t> encoded) {
    QByteArray bytes = viewBytes(encoded); QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer); reader.setAutoDetectImageFormat(true); reader.setDecideFormatFromContent(true); reader.setAutoTransform(false);
    const QByteArray format = reader.format(); const QString mime = mimeForFormat(format); const QSize size = reader.size();
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0)
        throw ErrorException({"image.probe", "Image dimensions cannot be probed without decoding"});
    const int count = reader.imageCount();
    if (count < 1) throw ErrorException({"image.probe", "Image frame count is unavailable"});
    media::ImageProbe result{std::uint32_t(size.width()), std::uint32_t(size.height()), mime.toStdString(), std::uint32_t(count), 0};
    // Canonical PixelLease charges this metadata-only upper bound before read().
    result.decoded_working_bytes = workingBytes(result);
    checkReaderStorage(reader, nativePixelBytes(result.mime));
    return result;
}
media::RgbaImage DecodeImage::decode(std::span<const std::uint8_t> encoded, const media::ImageProbe& expected,
                                   const media::ImageLimits& limits, std::stop_token stop) {
    if (stop.stop_requested()) throw ErrorException({"cancelled", "Image decode cancelled"});
    if (encoded.size() > limits.max_encoded_bytes) throw ErrorException({"image.quota", "Encoded image exceeds its byte budget"});
    checkPixels(expected.width, expected.height, limits.max_dimension, limits.max_pixels);
    const auto rgbaBytes = planeBytes(expected.width, expected.height, 4);
    const auto nativeBytes = planeBytes(expected.width, expected.height, nativePixelBytes(expected.mime));
    const auto requiredWorking = workingBytes(expected);
    if (rgbaBytes > limits.max_decoded_bytes || expected.decoded_working_bytes < requiredWorking
        || expected.decoded_working_bytes > limits.max_active_bytes)
        throw ErrorException({"image.quota", "Image output or simultaneous storage exceeds its reserved budget"});
    if (expected.frames != 1) throw ErrorException({"image.animation", "Skin and avatar decoding accepts one static frame only"});
    const auto actual = probe(encoded);
    if (actual.width != expected.width || actual.height != expected.height || actual.frames != expected.frames || actual.mime != expected.mime
        || actual.decoded_working_bytes != expected.decoded_working_bytes)
        throw ErrorException({"image.changed", "Image probe changed before decoding"});
    QByteArray bytes = viewBytes(encoded); QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer); reader.setDecideFormatFromContent(true); reader.setAutoTransform(false);
    checkReaderStorage(reader, nativePixelBytes(expected.mime));
    if (stop.stop_requested()) throw ErrorException({"cancelled", "Image decode cancelled"});
    QImage native = reader.read(); checkedImageBytes(native, QSize(int(expected.width), int(expected.height)), nativeBytes);
    if (stop.stop_requested()) throw ErrorException({"cancelled", "Image decode cancelled"});
    QImage rgba = native.convertToFormat(QImage::Format_RGBA8888);
    const auto convertedBytes = checkedImageBytes(rgba, native.size(), rgbaBytes);
    if (addBytes(std::size_t(native.sizeInBytes()), addBytes(convertedBytes, rgbaBytes)) > expected.decoded_working_bytes)
        throw ErrorException({"image.quota", "Raster conversion exceeds its simultaneous reservation"});
    native = {}; // Release our native plane before allocating the independently owned DTO.
    media::RgbaImage result{expected.width, expected.height, std::vector<std::uint8_t>(rgbaBytes)};
    if (result.pixels.capacity() != rgbaBytes) throw ErrorException({"image.quota", "RGBA output capacity exceeds its reservation"});
    const auto stride = planeBytes(expected.width, 1, 4);
    for (int row = 0; row < rgba.height(); ++row) {
        if (stop.stop_requested()) throw ErrorException({"cancelled", "Image decode cancelled"});
        std::memcpy(result.pixels.data() + std::size_t(row) * stride, rgba.constScanLine(row), stride);
    }
    return result;
}
documents::RasterImage decodeDescriptionImage(std::span<const std::uint8_t> encoded,
    const documents::ImageLimits& limits, const Cancellation& cancellation) {
    cancellation.check();
    if (encoded.size() > limits.maximumEncodedBytes) throw ErrorException({"image.quota", "Description image exceeds its encoded budget"});
    DecodeImage decoder; const auto probe = decoder.probe(encoded);
    checkPixels(probe.width, probe.height, limits.maximumDimension, limits.maximumPixels);
    if (limits.maximumDisplayDimension == 0 || limits.maximumDisplayDimension > unsigned(std::numeric_limits<int>::max()))
        throw ErrorException({"image.quota", "Description display geometry is invalid"});
    const auto sourceRgbaBytes = planeBytes(probe.width, probe.height, 4);
    if (sourceRgbaBytes > limits.maximumCacheBytes)
        throw ErrorException({"image.quota", "Description source pixels exceed their decode budget"});
    QSize display{int(probe.width), int(probe.height)};
    if (probe.width > limits.maximumDisplayDimension || probe.height > limits.maximumDisplayDimension)
        display.scale(int(limits.maximumDisplayDimension), int(limits.maximumDisplayDimension), Qt::KeepAspectRatio);
    checkPixels(unsigned(display.width()), unsigned(display.height()), limits.maximumDisplayDimension, limits.maximumPixels);
    const auto rgbaBytes = planeBytes(unsigned(display.width()), unsigned(display.height()), 4);
    if (rgbaBytes > limits.maximumCacheBytes)
        throw ErrorException({"image.quota", "Description display pixels exceed their cache budget"});
    const auto nativeBpp = nativePixelBytes(probe.mime);
    const auto sourceNativeBytes = planeBytes(probe.width, probe.height, nativeBpp);
    const auto displayNativeBytes = planeBytes(unsigned(display.width()), unsigned(display.height()), nativeBpp);
    // Generic smooth scaling may retain source, premultiplied source and scaled planes.
    // maximumCacheBytes limits RGBA planes, not a domain-wide concurrent working set.
    const auto sourceWorking = addBytes(sourceNativeBytes, sourceNativeBytes);
    const auto displayWorking = addBytes(addBytes(displayNativeBytes, displayNativeBytes), addBytes(rgbaBytes, rgbaBytes));
    const auto requiredWorking = addBytes(sourceWorking, displayWorking);
    QByteArray bytes = viewBytes(encoded); QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer); reader.setDecideFormatFromContent(true); reader.setAutoTransform(false);
    checkReaderStorage(reader, nativeBpp);
    if (display != QSize(int(probe.width), int(probe.height))) reader.setScaledSize(display);
    cancellation.check(); QImage native = reader.read(); cancellation.check();
    const auto nativeBytes = checkedImageBytes(native, display, displayNativeBytes);
    QImage rgba = native.convertToFormat(QImage::Format_RGBA8888);
    const auto convertedBytes = checkedImageBytes(rgba, display, rgbaBytes);
    if (addBytes(nativeBytes, addBytes(convertedBytes, rgbaBytes)) > requiredWorking)
        throw ErrorException({"image.quota", "Description raster storage exceeds its checked plane bound"});
    native = {};
    const auto stride = planeBytes(unsigned(display.width()), 1, 4);
    documents::RasterImage result{unsigned(display.width()), unsigned(display.height()), stride, resources::Bytes(rgbaBytes)};
    if (result.rgba.capacity() != rgbaBytes) throw ErrorException({"image.quota", "Description RGBA capacity exceeds its reservation"});
    for (int row = 0; row < rgba.height(); ++row) {
        cancellation.check(); std::memcpy(result.rgba.data() + std::size_t(row) * stride, rgba.constScanLine(row), stride);
    }
    return result;
}
QImage imageFromRgba(const media::RgbaImage& image) {
    if (image.width == 0 || image.height == 0 || std::uint64_t(image.width) * image.height * 4 != image.pixels.size()
        || image.width > 8192 || image.height > 8192) return {};
    return QImage(image.pixels.data(), int(image.width), int(image.height), qsizetype(image.width) * 4, QImage::Format_RGBA8888).copy();
}
QImage imageFromRaster(const documents::RasterImage& image) {
    if (image.width == 0 || image.height == 0 || image.width > 4096 || image.height > 4096
        || image.stride < std::size_t(image.width) * 4 || image.stride > std::numeric_limits<std::size_t>::max() / image.height
        || image.rgba.size() != image.stride * image.height) return {};
    return QImage(image.rgba.data(), int(image.width), int(image.height), qsizetype(image.stride), QImage::Format_RGBA8888).copy();
}
} // namespace nickel::ui
