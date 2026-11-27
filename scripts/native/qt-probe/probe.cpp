#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPainter>
#include <QPluginLoader>
#include <QSignalSpy>
#include <QSvgRenderer>
#include <QTest>
#include <QTextDocument>
#include <QTimer>
#include <webp/decode.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void require_visible_pixels(const QImage& actual, const QImage& expected) {
    require(actual.size() == expected.size(), "WebP round-trip dimensions changed");
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const auto left = actual.pixelColor(x, y), right = expected.pixelColor(x, y);
            require(left.alpha() == right.alpha(), "Lossless WebP alpha pixels changed");
            require(left.alpha() == 0 || left == right, "Lossless WebP visible pixels changed");
        }
    }
}

QJsonObject test_webp() {
    require(QImageReader::supportedImageFormats().contains("webp"), "Static WebP format registration failed");
    const QImage reference(QStringLiteral(":/nickel/probe/webp/reference.png"));
    require(!reference.isNull() && reference.size() == QSize(436, 160), "Original PNG reference failed");
    int fixture_visible_differences = 0;
    int partial_alpha_pixels = 0;
    int transparent_pixels = 0;
    int compared_pixels = 0;
    for (const auto* file : {"lossy.webp", "lossless.webp", "opaque.webp"}) {
        const auto path = QStringLiteral(":/nickel/probe/webp/") + QString::fromLatin1(file);
        QImageReader reader(path);
        require(reader.canRead() && reader.size() == reference.size(), "Original WebP image probe failed");
        const auto image = reader.read();
        require(!image.isNull() && image.size() == reference.size(), "Original WebP image decoding failed");
        require(image.hasAlphaChannel() == (QByteArray(file) != "opaque.webp"), "WebP alpha channel changed");

        QFile encoded(path);
        require(encoded.open(QIODevice::ReadOnly), "Original WebP resource could not be read");
        const auto bytes = encoded.readAll();
        QImage direct(image.size(), QImage::Format_RGBA8888);
        require(!direct.isNull(), "WebP C-decoder buffer allocation failed");
        require(WebPDecodeRGBAInto(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                    static_cast<std::size_t>(bytes.size()), direct.bits(),
                    static_cast<std::size_t>(direct.sizeInBytes()), direct.bytesPerLine()) == direct.bits(),
                "Pinned WebP C-decoder failed");
        const auto actual = image.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < actual.height(); ++y) {
            require(std::memcmp(actual.constScanLine(y), direct.constScanLine(y),
                        static_cast<std::size_t>(actual.width()) * 4) == 0,
                    "Qt WebP handler pixels differ from the pinned C decoder");
        }
        compared_pixels += actual.width() * actual.height();
        if (QByteArray(file) == "lossless.webp") {
            for (int y = 0; y < actual.height(); ++y) {
                for (int x = 0; x < actual.width(); ++x) {
                    const auto pixel = actual.pixelColor(x, y);
                    if (pixel.alpha() != 0 && pixel != reference.pixelColor(x, y)) ++fixture_visible_differences;
                }
            }
        }
    }

    // Upstream PNG and pre-encoded lossless WebP are not an identity pair.
    QByteArray lossless;
    QBuffer output(&lossless);
    require(output.open(QIODevice::WriteOnly) && reference.save(&output, "WEBP", 100),
            "Pinned lossless WebP writer failed");
    output.close();
    const auto round_trip = QImage::fromData(lossless, "WEBP");
    require(!round_trip.isNull() && round_trip.hasAlphaChannel(), "Lossless PNG/WebP round-trip decoding failed");
    require_visible_pixels(round_trip, reference);
    for (int y = 0; y < reference.height(); ++y) {
        for (int x = 0; x < reference.width(); ++x) {
            const auto alpha = reference.pixelColor(x, y).alpha();
            transparent_pixels += alpha == 0;
            partial_alpha_pixels += alpha > 0 && alpha < 255;
        }
    }
    require(transparent_pixels > 0 && partial_alpha_pixels > 0, "Original PNG alpha coverage is missing");

    QImageReader animation(QStringLiteral(":/nickel/probe/webp/animation.webp"));
    require(animation.canRead() && animation.supportsAnimation() && animation.size() == QSize(536, 260),
            "Original animated WebP probe failed");
    require(animation.imageCount() == 2 && animation.loopCount() == 2, "Animated WebP frame/loop metadata changed");
    require(animation.backgroundColor() == QColor(128, 128, 128, 128), "Animated WebP background changed");
    const auto first = animation.read();
    require(!first.isNull() && first.size() == QSize(536, 260) && animation.currentImageNumber() == 0
            && animation.currentImageRect() == QRect(0, 0, 436, 160)
            && animation.nextImageDelay() == 1000, "Animated WebP first frame failed");
    const auto second = animation.read();
    require(!second.isNull() && second.size() == first.size() && animation.currentImageNumber() == 1
            && animation.currentImageRect() == QRect(100, 100, 436, 160)
            && animation.nextImageDelay() == 1200 && first != second, "Animated WebP second frame failed");
    require(animation.read().isNull() && !animation.canRead(),
            "Animated WebP did not terminate at the original frame bound");
    return QJsonObject{{"staticImages", 3}, {"cDecoderComparedPixels", compared_pixels},
        {"losslessRoundTripPixels", reference.width() * reference.height()},
        {"pngReferenceVisibleDifferencesFromPreencodedWebp", fixture_visible_differences},
        {"transparentPixels", transparent_pixels}, {"partialAlphaPixels", partial_alpha_pixels},
        {"animationFrames", 2}, {"animationLoops", 2}, {"animationDelaysMs", QStringLiteral("1000,1200")}};
}
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        QLineEdit edit;
        QSignalSpy changes(&edit, &QLineEdit::textChanged);
        edit.setText("native QtTest probe");
        require(changes.count() == 1, "QtTest signal delivery failed");
        QTimer timer;
        timer.setSingleShot(true);
        QSignalSpy ticks(&timer, &QTimer::timeout);
        timer.start(1);
        require(ticks.wait(1000), "Qt Widgets event loop failed");

        QImage source(8, 8, QImage::Format_RGBA8888);
        source.fill(Qt::green);
        QByteArray png;
        QBuffer writer(&png);
        require(writer.open(QIODevice::WriteOnly) && source.save(&writer, "PNG"), "Built-in PNG writer failed");
        QBuffer buffer(&png);
        require(buffer.open(QIODevice::ReadOnly), "PNG buffer failed");
        QImageReader reader(&buffer);
        require(reader.canRead() && reader.size() == source.size(), "Built-in PNG probe failed");
        require(!reader.read().isNull(), "Built-in PNG decoder failed");
        const auto webp_fixtures = test_webp();

        QSvgRenderer svg(QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\"><path d=\"M0 0H8V8H0Z\" fill=\"#00ff00\"/></svg>"));
        require(svg.isValid(), "QtSvg renderer failed");
        QImage canvas(8, 8, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        svg.render(&painter);
        painter.end();
        require(canvas.pixelColor(4, 4) == QColor(Qt::green), "QtSvg path geometry failed");

        QTextDocument markdown;
        const auto features = QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
            | QTextDocument::MarkdownNoHTML;
        markdown.setMarkdown("# Native\n\n<script>blocked()</script>\n\n**Markdown**", features);
        require(markdown.toPlainText().contains("Native") && markdown.toPlainText().contains("Markdown"), "Native Markdown parser failed");
        bool windows = false, offscreen = false, webp = false;
        for (const auto& plugin : QPluginLoader::staticInstances()) {
            const auto name = QByteArray(plugin->metaObject()->className());
            windows = windows || name == "QWindowsIntegrationPlugin";
            offscreen = offscreen || name == "QOffscreenIntegrationPlugin";
            webp = webp || name == "QWebpPlugin";
        }
#ifdef Q_OS_WIN
        require(windows && offscreen, "Windows/offscreen static plugin registration failed");
#endif
        require(webp, "WebP static plugin initializer was not linked");
        const QJsonObject evidence{{"qtVersion", qVersion()}, {"platform", QApplication::platformName()},
            {"widgets", true}, {"qtTest", true}, {"png", true}, {"svg", true}, {"markdownNoHtml", true},
            {"webp", true}, {"webpLosslessPixels", true}, {"webpAlpha", true}, {"webpAnimation", true},
            {"webpStaticPlugin", webp}, {"webpFixtures", webp_fixtures},
            {"windowsStaticPlugin", windows}, {"offscreenStaticPlugin", offscreen}, {"releaseAcceptance", false}};
        std::cout << QJsonDocument(evidence).toJson(QJsonDocument::Compact).constData() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
