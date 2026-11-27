#include "ui/DecodeImage.hpp"
#include "ui/NativeMarkdown.hpp"
#include "core/common/error.hpp"
#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QTest>
#include <QTextDocument>
#include <cstring>
#include <limits>
#include <stop_token>
#include <vector>

using namespace nickel;
using namespace nickel::ui;

namespace {
std::vector<std::uint8_t> rasterBytes(const QImage& image, const QByteArray& format = "PNG") {
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, format.constData())) return {};
    const auto* first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
    return {first, first + bytes.size()};
}
std::vector<std::uint8_t> png(const QImage& image) { return rasterBytes(image); }
}
class AdapterTests final : public QObject {
    Q_OBJECT
private slots:
    void markdownPreservesTextAndNoHtml() {
        const auto output = normalizeMarkdownNoIo("# Native heading\n\n**Bold** and `code`\n\n<script>unsafe()</script>\n", {});
        const auto html = QString::fromUtf8(output.data(), qsizetype(output.size()));
        QVERIFY(html.contains("Native heading")); QVERIFY(!html.contains("<script>"));
        QTextDocument document; document.setHtml(html); QVERIFY(document.toPlainText().contains("Bold and code"));
    }
    void markdownRejectsEncodingAndBudgets() {
        QVERIFY_EXCEPTION_THROWN(normalizeMarkdownNoIo(std::string(512 * 1024 + 1, 'x'), {}), ErrorException);
        const std::string incomplete("\xf0\x9f", 2);
        QVERIFY_EXCEPTION_THROWN(normalizeMarkdownNoIo(incomplete, {}), ErrorException);
        const std::string invalid("\xc0\xaf", 2);
        QVERIFY_EXCEPTION_THROWN(normalizeMarkdownNoIo(invalid, {}), ErrorException);
        Cancellation cancel; cancel.cancel();
        QVERIFY_EXCEPTION_THROWN(normalizeMarkdownNoIo("# cancelled", cancel), ErrorException);
    }
    void rasterProbeAndDecodeAreOwnedRgba() {
        QImage source(64, 64, QImage::Format_RGBA8888); source.fill(QColor(20, 40, 60, 180));
        const auto bytes = png(source); QVERIFY(!bytes.empty());
        DecodeImage decoder; const auto expected = decoder.probe(bytes);
        QCOMPARE(expected.width, std::uint32_t(64)); QCOMPARE(expected.height, std::uint32_t(64));
        QCOMPARE(QString::fromStdString(expected.mime), QString("image/png")); QCOMPARE(expected.frames, std::uint32_t(1));
        auto decoded = decoder.decode(bytes, expected, {}, {});
        QCOMPARE(decoded.pixels.size(), std::size_t(64 * 64 * 4));
        const QImage image = imageFromRgba(decoded); QCOMPARE(image, source);
        decoded.pixels[0] = 255; QCOMPARE(image.pixelColor(0, 0).red(), 20);
    }
    void realRasterCodecsReportSimultaneousPixelStorage_data() {
        QTest::addColumn<QByteArray>("format"); QTest::addColumn<int>("sourceFormat"); QTest::addColumn<QString>("mime");
        QTest::newRow("png-rgba8") << QByteArray("PNG") << int(QImage::Format_RGBA8888) << QString("image/png");
        QTest::newRow("png-rgba16") << QByteArray("PNG") << int(QImage::Format_RGBA64) << QString("image/png");
        QTest::newRow("jpeg") << QByteArray("JPEG") << int(QImage::Format_RGB32) << QString("image/jpeg");
        QTest::newRow("webp") << QByteArray("WEBP") << int(QImage::Format_RGBA8888) << QString("image/webp");
    }
    void realRasterCodecsReportSimultaneousPixelStorage() {
        QFETCH(QByteArray, format); QFETCH(int, sourceFormat); QFETCH(QString, mime);
        QVERIFY(QImageReader::supportedImageFormats().contains(format.toLower()));
        QVERIFY(QImageWriter::supportedImageFormats().contains(format.toLower()));
        QImage source(64, 32, QImage::Format(sourceFormat)); source.fill(QColor(20, 40, 60, 180));
        const auto encoded = rasterBytes(source, format); QVERIFY(!encoded.empty());
        DecodeImage decoder; const auto expected = decoder.probe(encoded);
        QCOMPARE(QString::fromStdString(expected.mime), mime);
        QCOMPARE(expected.decoded_working_bytes, std::size_t(64 * 32 * 16));
        QByteArray bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(encoded.data()), qsizetype(encoded.size()));
        QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::ReadOnly)); QImageReader reader(&buffer); reader.setAutoTransform(false);
        const auto metadataFormat = reader.imageFormat(); QImage native = reader.read(); QVERIFY(!native.isNull());
        QCOMPARE(native.size(), source.size()); if (sourceFormat == QImage::Format_RGBA64) QCOMPARE(native.depth(), 64);
        const QImage converted = native.convertToFormat(QImage::Format_RGBA8888); QVERIFY(!converted.isNull());
        std::vector<std::uint8_t> dto(std::size_t(converted.width()) * converted.height() * 4);
        const auto simultaneous = std::size_t(native.sizeInBytes()) + std::size_t(converted.sizeInBytes()) + dto.capacity();
        QVERIFY(simultaneous <= expected.decoded_working_bytes);
        qInfo().noquote() << "Real codec" << format << "metadata format" << int(metadataFormat) << "native depth" << native.depth()
            << "native bytes" << native.sizeInBytes() << "converted bytes" << converted.sizeInBytes()
            << "DTO capacity" << dto.capacity() << "conservative simultaneous plane bytes" << simultaneous
            << "probe working reservation" << expected.decoded_working_bytes;
        media::ImageLimits limits; limits.max_decoded_bytes = dto.size(); limits.max_active_bytes = expected.decoded_working_bytes;
        const auto decoded = decoder.decode(encoded, expected, limits, {});
        QCOMPARE(decoded.width, expected.width); QCOMPARE(decoded.height, expected.height); QCOMPARE(decoded.pixels.size(), dto.size());
        QCOMPARE(decoded.pixels.capacity(), decoded.pixels.size());
        limits.max_active_bytes = expected.decoded_working_bytes - 1;
        QVERIFY_EXCEPTION_THROWN(decoder.decode(encoded, expected, limits, {}), ErrorException);
        auto understated = expected; understated.decoded_working_bytes = 0;
        QVERIFY_EXCEPTION_THROWN(decoder.decode(encoded, understated, {}, {}), ErrorException);
        understated.decoded_working_bytes = dto.size();
        QVERIFY_EXCEPTION_THROWN(decoder.decode(encoded, understated, {}, {}), ErrorException);
    }
    void rasterRejectsUnexpectedProbeAndCancellation() {
        QImage source(64, 64, QImage::Format_RGBA8888); source.fill(Qt::blue); const auto bytes = png(source);
        DecodeImage decoder; auto expected = decoder.probe(bytes); expected.width = 63;
        QVERIFY_EXCEPTION_THROWN(decoder.decode(bytes, expected, {}, {}), ErrorException);
        expected = decoder.probe(bytes); media::ImageLimits limits; limits.max_pixels = 100;
        QVERIFY_EXCEPTION_THROWN(decoder.decode(bytes, expected, limits, {}), ErrorException);
        std::stop_source cancel; cancel.request_stop();
        QVERIFY_EXCEPTION_THROWN(decoder.decode(bytes, expected, {}, cancel.get_token()), ErrorException);
        const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100000\" height=\"100000\"/>";
        const auto view = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(svg.data()), svg.size());
        QVERIFY_EXCEPTION_THROWN(decoder.probe(view), ErrorException);
    }
    void descriptionImageScalesWithoutChangingSmallImages() {
        QImage source(800, 400, QImage::Format_RGBA8888); source.fill(Qt::yellow); const auto bytes = png(source);
        documents::ImageLimits limits; limits.maximumDisplayDimension = 128;
        auto raster = decodeDescriptionImage(bytes, limits, {});
        QCOMPARE(raster.width, unsigned(128)); QCOMPARE(raster.height, unsigned(64));
        QImage image = imageFromRaster(raster); QCOMPARE(image.size(), QSize(128, 64));
        raster.rgba[0] = 0; QCOMPARE(image.pixelColor(0, 0).red(), 255);
        QImage small(16, 8, QImage::Format_RGBA8888); small.fill(Qt::red);
        const auto unchanged = decodeDescriptionImage(png(small), limits, {});
        QCOMPARE(unchanged.width, unsigned(16)); QCOMPARE(unchanged.height, unsigned(8));
    }
    void realDescriptionCodecsScaleHighDepthWithRgbaCacheBudget_data() {
        QTest::addColumn<QByteArray>("format"); QTest::addColumn<int>("sourceFormat");
        QTest::newRow("png8") << QByteArray("PNG") << int(QImage::Format_RGBA8888);
        QTest::newRow("png16") << QByteArray("PNG") << int(QImage::Format_RGBA64);
        QTest::newRow("jpeg") << QByteArray("JPEG") << int(QImage::Format_RGB32);
        QTest::newRow("webp") << QByteArray("WEBP") << int(QImage::Format_RGBA8888);
    }
    void realDescriptionCodecsScaleHighDepthWithRgbaCacheBudget() {
        QFETCH(QByteArray, format); QFETCH(int, sourceFormat);
        QImage source(256, 128, QImage::Format(sourceFormat)); source.fill(QColor(25, 100, 150, 180));
        const auto encoded = rasterBytes(source, format); QVERIFY(!encoded.empty());
        documents::ImageLimits limits; limits.maximumDisplayDimension = 32; limits.maximumCacheBytes = 256 * 128 * 4;
        const auto result = decodeDescriptionImage(encoded, limits, {});
        QCOMPARE(result.width, unsigned(32)); QCOMPARE(result.height, unsigned(16));
        QCOMPARE(result.stride, std::size_t(128)); QCOMPARE(result.rgba.capacity(), std::size_t(32 * 16 * 4));
        QVERIFY(!imageFromRaster(result).isNull());
        qInfo().noquote() << "Description real codec" << format << "input depth" << source.depth()
            << "source geometry" << source.size() << "display" << QSize(int(result.width), int(result.height))
            << "RGBA cache bytes" << limits.maximumCacheBytes << "returned capacity" << result.rgba.capacity();
        limits.maximumCacheBytes -= 1;
        QVERIFY_EXCEPTION_THROWN(decodeDescriptionImage(encoded, limits, {}), ErrorException);
        limits.maximumCacheBytes = 256 * 128 * 4; limits.maximumDisplayDimension = 0;
        QVERIFY_EXCEPTION_THROWN(decodeDescriptionImage(encoded, limits, {}), ErrorException);
    }
    void realRasterCodecsRejectTruncatedAndOversizedInput() {
        DecodeImage decoder;
        for (const auto& format : {QByteArray("PNG"), QByteArray("JPEG"), QByteArray("WEBP")}) {
            QImage source(64, 32, QImage::Format_RGBA8888); source.fill(Qt::blue);
            auto encoded = rasterBytes(source, format); QVERIFY(encoded.size() > 20);
            const auto expected = decoder.probe(encoded); encoded.resize(20);
            QVERIFY_EXCEPTION_THROWN(decoder.decode(encoded, expected, {}, {}), ErrorException);
            QVERIFY_EXCEPTION_THROWN(decodeDescriptionImage(encoded, {}, {}), ErrorException);
        }
        QImage wide(2049, 1, QImage::Format_RGBA8888); wide.fill(Qt::red);
        const auto encoded = png(wide); const auto expected = decoder.probe(encoded);
        QCOMPARE(expected.width, std::uint32_t(2049));
        QVERIFY_EXCEPTION_THROWN(decoder.decode(encoded, expected, {}, {}), ErrorException);
        documents::ImageLimits limits; limits.maximumDimension = 2048;
        QVERIFY_EXCEPTION_THROWN(decodeDescriptionImage(encoded, limits, {}), ErrorException);
        auto excessive = expected; excessive.width = std::uint32_t(std::numeric_limits<int>::max()); excessive.height = excessive.width;
        media::ImageLimits unlimited; unlimited.max_dimension = excessive.width; unlimited.max_pixels = std::numeric_limits<std::uint64_t>::max();
        unlimited.max_decoded_bytes = std::numeric_limits<std::size_t>::max(); unlimited.max_active_bytes = unlimited.max_decoded_bytes;
        QVERIFY_EXCEPTION_THROWN(decoder.decode(encoded, excessive, unlimited, {}), ErrorException);
    }
    void malformedRgbaNeverLeaksBorrowedStorage() {
        media::RgbaImage image{64, 64, {0}}; QVERIFY(imageFromRgba(image).isNull());
        documents::RasterImage raster{32, 32, 4, {0, 0, 0, 0}}; QVERIFY(imageFromRaster(raster).isNull());
    }
    void decodeRejectsZeroOverflowAndDecodedByteBudgets() {
        QImage source(4, 4, QImage::Format_RGBA8888); source.fill(Qt::red); const auto bytes = png(source);
        DecodeImage decoder; auto expected = decoder.probe(bytes);
        expected.width = 0; QVERIFY_EXCEPTION_THROWN(decoder.decode(bytes, expected, {}, {}), ErrorException);
        expected.width = std::numeric_limits<std::uint32_t>::max(); expected.height = 1;
        media::ImageLimits limits; limits.max_dimension = expected.width; limits.max_pixels = std::numeric_limits<std::uint64_t>::max();
        QVERIFY_EXCEPTION_THROWN(decoder.decode(bytes, expected, limits, {}), ErrorException);
        expected = decoder.probe(bytes); limits = {}; limits.max_decoded_bytes = 63;
        QVERIFY_EXCEPTION_THROWN(decoder.decode(bytes, expected, limits, {}), ErrorException);
        const std::vector<std::uint8_t> malformed{0, 1, 2, 3};
        QVERIFY_EXCEPTION_THROWN(decoder.probe(malformed), ErrorException);
        QVERIFY_EXCEPTION_THROWN(decodeDescriptionImage(malformed, {}, {}), ErrorException);
        Cancellation cancelled; cancelled.cancel();
        QVERIFY_EXCEPTION_THROWN(decodeDescriptionImage(bytes, {}, cancelled), ErrorException);
    }
};
QTEST_MAIN(AdapterTests)
#include "adapter_tests.moc"
