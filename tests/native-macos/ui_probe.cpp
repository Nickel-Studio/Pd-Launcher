#if !defined(__APPLE__)
#error "A Cocoa toolkit probe is not Windows/Linux cross-runtime evidence."
#endif

#include "ui/Theme.hpp"
#include <QApplication>
#include <QCommandLineParser>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QGlyphRun>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QRawFont>
#include <QSaveFile>
#include <QScreen>
#include <QSvgRenderer>
#include <QStandardPaths>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

QJsonObject font_record(const QFont& font, const QString& sample) {
    QFontMetricsF metrics(font);
    QTextLayout layout(sample, font);
    layout.beginLayout();
    auto line = layout.createLine();
    line.setLineWidth(900);
    layout.endLayout();
    QJsonArray actual_fonts;
    bool invalid_raw_font = false;
    for (const auto& run : layout.glyphRuns()) {
        actual_fonts.append(run.rawFont().familyName());
        invalid_raw_font = invalid_raw_font || !run.rawFont().isValid();
    }
    return QJsonObject{{"requestedFamilies", QJsonArray::fromStringList(font.families())},
                       {"primaryResolvedFamily", QFontInfo(font).family()},
                       {"actualGlyphRunFamilies", actual_fonts},
                       {"sample", sample}, {"advance", metrics.horizontalAdvance(sample)},
                       {"height", metrics.height()}, {"ascent", metrics.ascent()},
                       {"pixelSize", font.pixelSize()}, {"glyphRunInvalid", invalid_raw_font},
                       {"productionFontResourceRegistrationVerified", false}};
}

QJsonObject theme_record() {
    using namespace nickel::ui;
    QJsonArray cases;
    require(accentOptions().size() == 12, "Native theme contract lost one of the 12 accents");
    Theme theme;
    theme.setDeterministicTime(45000);
    for (const auto& option : accentOptions()) {
        for (const auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
            Appearance appearance;
            appearance.mode = mode;
            appearance.accent = option.id;
            appearance.reducedMotion = true;
            appearance.titlebarTint = true;
            theme.setAppearance(appearance);
            const auto& tokens = theme.tokens();
            require(tokens.text.isValid() && tokens.background.isValid() && tokens.accent.isValid(),
                    "An actual native theme produced invalid color tokens");
            require(theme.motionDuration(420) == 0, "Reduced motion did not disable native theme transition");
            cases.append(QJsonObject{{"accent", QString::fromLatin1(option.key)},
                                     {"mode", mode == ThemeMode::Dark ? "dark" : "light"},
                                     {"background", tokens.background.name(QColor::HexArgb)},
                                     {"text", tokens.text.name(QColor::HexArgb)},
                                     {"accentColor", tokens.accent.name(QColor::HexArgb)},
                                     {"textContrast", contrastRatio(tokens.text, tokens.background)}});
        }
    }
    return QJsonObject{{"source", "canonical native/ui/Theme.cpp, not reimplemented"},
                       {"cases", cases}, {"themeCount", 12},
                       {"productionLayoutScreenshotCompared", false}};
}
} // namespace

int main(int argc, char** argv) {
    try {
        QStandardPaths::setTestModeEnabled(true);
        QApplication application(argc, argv);
        application.setApplicationName("native-macos-delivery-probe-not-product");
        application.setOrganizationName("Nickel delivery synthetic probe");
        QCommandLineParser parser;
        parser.addHelpOption();
        parser.addOption({"output", "Owned output JSON path", "output"});
        parser.process(application);
        require(parser.isSet("output"), "An explicit owned output path is required");
        require(QString::fromLatin1(qVersion()) == QStringLiteral("6.8.3"), "Runtime Qt version is not exactly 6.8.3");
        require(QGuiApplication::platformName() == QStringLiteral("cocoa"), "Native Cocoa plugin required, not offscreen/minimal");
        QSvgRenderer svg(QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\"><path d=\"M1 1h14v14H1z\"/></svg>"));
        require(svg.isValid(), "Exact QtSvg runtime could not parse a bounded synthetic image");
        QWidget window;
        window.setWindowTitle("Native toolkit probe - not production layout evidence");
        window.resize(1100, 720);
        auto* layout = new QVBoxLayout(&window);
        auto* label = new QLabel(QString::fromUtf8("Native Cocoa / Chinese \xe9\x95\x8d\xe5\x90\xaf\xe5\x8a\xa8\xe5\x99\xa8 / emoji \xf0\x9f\x8e\xae"), &window);
        label->setFont(nickel::ui::bodyFont());
        layout->addWidget(label);
        window.show();
        int status = 1;
        QTimer::singleShot(500, &application, [&] {
            try {
                QJsonArray screens;
                for (auto* screen : QGuiApplication::screens()) {
                    screens.append(QJsonObject{{"dpr", screen->devicePixelRatio()},
                                               {"logicalDpi", screen->logicalDotsPerInch()},
                                               {"physicalDpi", screen->physicalDotsPerInch()},
                                               {"width", screen->geometry().width()},
                                               {"height", screen->geometry().height()}});
                }
                const auto capture = window.grab().toImage();
                const qreal dpr = window.devicePixelRatioF();
                require(!capture.isNull() && dpr > 0 &&
                            std::abs(capture.width() / capture.devicePixelRatio() - window.width()) <= 1,
                        "Native high-DPI capture did not preserve logical window width");
                QJsonArray formats;
                const auto supported = QImageReader::supportedImageFormats();
                for (const auto& format : supported) formats.append(QString::fromLatin1(format));
                for (const QByteArray required : {QByteArray("png"), QByteArray("jpeg"), QByteArray("gif"),
                                                 QByteArray("svg"), QByteArray("webp")}) {
                    require(supported.contains(required), "A required minimal deployed image codec is absent");
                }
                const auto sample = QString::fromUtf8("Nickel \xe9\x95\x8d\xe5\x90\xaf\xe5\x8a\xa8\xe5\x99\xa8 / path \xe7\x94\xa8\xe6\x88\xb7 / \xf0\x9f\x8e\xae");
                QJsonObject evidence{{"schema", 1}, {"kind", "real-apple-native-cocoa-toolkit-probe"},
                                     {"executedOnApple", true}, {"qtVersion", QString::fromLatin1(qVersion())},
                                     {"platformPlugin", QGuiApplication::platformName()},
                                     {"qtScaleFactorInput", qEnvironmentVariable("QT_SCALE_FACTOR")},
                                     {"windowDpr", dpr}, {"screens", screens}, {"imageFormats", formats},
                                     {"avifAdvertised", supported.contains(QByteArray("avif"))},
                                     {"theme", theme_record()},
                                     {"bodyFont", font_record(nickel::ui::bodyFont(), sample)},
                                     {"displayFont", font_record(nickel::ui::displayFont(), sample)},
                                     {"monoFont", font_record(nickel::ui::monoFont(), sample)},
                                     {"physicalMultiMonitorDpiTested", false},
                                     {"nativeImeAndAccessibilityTested", false},
                                     {"productionLayoutAndFunctionalAssetsCompared", false},
                                     {"actualProductionApplication", false}, {"productDelivered", false}};
                QSaveFile output(parser.value("output"));
                require(output.open(QIODevice::WriteOnly), "Owned evidence file cannot be opened");
                const auto bytes = QJsonDocument(evidence).toJson(QJsonDocument::Indented);
                require(output.write(bytes) == bytes.size() && output.commit(), "Owned evidence write failed");
                std::cout << "EXECUTED native Cocoa toolkit/actual Theme/metrics probe; product and visual parity unverified\n";
                status = 0;
            } catch (const std::exception& error) {
                std::cerr << "FAIL Cocoa toolkit probe: " << error.what() << '\n';
            }
            application.exit(status);
        });
        return application.exec();
    } catch (const std::exception& error) {
        std::cerr << "FAIL Cocoa toolkit probe: " << error.what() << '\n';
        return 1;
    }
}
