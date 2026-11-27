#include "TestServices.hpp"
#include "ui/BackgroundView.hpp"
#include "ui/Controls.hpp"
#include "ui/MainWindow.hpp"
#include "ui/ThemeArtwork.hpp"
#include <QGraphicsDropShadowEffect>
#include <QSignalSpy>
#include <QSvgRenderer>
#include <QTest>
#include <QXmlStreamReader>
#include <limits>

using namespace nickel::ui;
using namespace nickel::ui::tests;

class EffectsTests final : public QObject {
    Q_OBJECT
private slots:
    void blurChangesPixelsWithoutChangingTheSource() {
        QImage source(128, 64, QImage::Format_ARGB32_Premultiplied); source.fill(Qt::black);
        for (int y = 0; y < source.height(); ++y) for (int x = 64; x < source.width(); ++x) source.setPixelColor(x, y, Qt::white);
        const QImage original = source; const auto key = source.cacheKey();
        QCOMPARE(blurRasterImage(source, 0), source);
        const QImage blurred = blurRasterImage(source, 12); QVERIFY(!blurred.isNull()); QCOMPARE(blurred.size(), source.size());
        QVERIFY(blurred.pixelColor(60, 32).red() > 0); QVERIFY(blurred.pixelColor(68, 32).red() < 255);
        QCOMPARE(source, original); QCOMPARE(source.cacheKey(), key);
        QVERIFY(blurRasterImage(source, -1).isNull());
        QVERIFY(blurRasterImage(source, std::numeric_limits<qreal>::quiet_NaN()).isNull());
    }
    void opacityIsImageOpacityAndBlurIsCached() {
        Theme theme; Appearance appearance; appearance.mode = ThemeMode::Dark; appearance.reducedMotion = true;
        appearance.backgroundOpacity = 0; theme.setAppearance(appearance);
        BackgroundView background(theme); background.resize(320, 180); background.show();
        QImage image(64, 64, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::red);
        QVERIFY(background.setImage(image)); QVERIFY(!background.frame().isNull());
        const auto withoutImage = background.frame().pixelColor(background.frame().width() / 2, background.frame().height() / 2);
        const qint64 cached = background.imageLayer().cacheKey();
        appearance.backgroundOpacity = 1; theme.setAppearance(appearance);
        const auto withImage = background.frame().pixelColor(background.frame().width() / 2, background.frame().height() / 2);
        QVERIFY(withImage.red() > withoutImage.red()); QCOMPARE(background.imageLayer().cacheKey(), cached);
        background.refresh(); QCOMPARE(background.imageLayer().cacheKey(), cached);
        appearance.backgroundBlur = 20; theme.setAppearance(appearance); QVERIFY(background.imageLayer().cacheKey() != cached);
        QVERIFY(background.testAttribute(Qt::WA_TransparentForMouseEvents)); QCOMPARE(background.focusPolicy(), Qt::NoFocus);
        background.resize(8192, 8192); background.refresh();
        QVERIFY(qint64(background.frame().width()) * background.frame().height() <= 4 * 1024 * 1024);
    }
    void artworkSvgIsNativeAndOriginalGeometry() {
        for (const auto& option : accentOptions()) {
            Appearance appearance; appearance.accent = option.id; appearance.mode = ThemeMode::Dark;
            const auto tokens = makeThemeTokens(appearance, true);
            for (const auto placement : {ArtworkPlacement::Background, ArtworkPlacement::Titlebar}) {
                const QByteArray svg = themeArtworkSvg(appearance, tokens, placement);
                if (option.id < Accent::Nickel) { QVERIFY(svg.isEmpty()); continue; }
                QVERIFY(!svg.isEmpty()); QSvgRenderer renderer(svg); QVERIFY(renderer.isValid());
                QXmlStreamReader xml(svg); while (!xml.atEnd()) xml.readNext(); QVERIFY(!xml.hasError());
                QVERIFY(!svg.contains("<image")); QVERIFY(!svg.contains("<script")); QVERIFY(!svg.contains("href="));
                QCOMPARE(renderer.viewBoxF().size(), placement == ArtworkPlacement::Background ? QSizeF(1200, 800) : QSizeF(240, 64));
            }
        }
        Appearance appearance; appearance.accent = Accent::Nickel;
        QVERIFY(themeArtworkSvg(appearance, makeThemeTokens(appearance, true), ArtworkPlacement::Titlebar).contains("M24 44 52 14 86 24"));
        appearance.accent = Accent::Custom; appearance.custom.decoration = Decoration::None;
        QVERIFY(themeArtworkSvg(appearance, makeThemeTokens(appearance, true), ArtworkPlacement::Background).isEmpty());
    }
    void selectingUserBackgroundSuppressesArtworkWhilePending() {
        Theme theme; Appearance appearance; appearance.mode = ThemeMode::Dark; appearance.accent = Accent::Nickel; appearance.reducedMotion = true;
        theme.setAppearance(appearance); BackgroundView background(theme); background.resize(600, 400); background.show(); background.refresh();
        const QImage artwork = background.frame();
        background.setUserBackgroundSelected(true); QVERIFY(background.frame() != artwork);
        background.setUserBackgroundSelected(false); QCOMPARE(background.frame(), artwork);
    }
    void backdropMapsSiblingCoordinatesAndRejectsOtherWindows() {
        Theme theme; Appearance appearance; appearance.mode = ThemeMode::Dark; appearance.backgroundOpacity = 1; appearance.reducedMotion = true;
        theme.setAppearance(appearance); QWidget window; window.resize(320, 180);
        BackgroundView background(theme, &window); background.setGeometry(window.rect());
        QImage source(64, 32, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x) source.setPixelColor(x, y, QColor(x * 4, 30, 220 - x * 3));
        QVERIFY(background.setImage(source)); window.show(); background.refresh();
        QWidget sibling(&window); sibling.setGeometry(57, 41, 93, 62);
        QImage full(window.size(), QImage::Format_ARGB32_Premultiplied); full.fill(Qt::transparent);
        { QPainter painter(&full); background.paintBackdrop(painter, window); }
        QImage sampled(sibling.size(), QImage::Format_ARGB32_Premultiplied); sampled.fill(Qt::transparent);
        { QPainter painter(&sampled); QVERIFY(paintWindowBackdrop(painter, sibling)); }
        QCOMPARE(sampled, full.copy(sibling.geometry()));
        QWidget foreign; foreign.resize(sibling.size()); QImage rejected(foreign.size(), QImage::Format_ARGB32_Premultiplied); rejected.fill(Qt::transparent);
        const QImage unchanged = rejected;
        { QPainter painter(&rejected); background.paintBackdrop(painter, foreign); QVERIFY(!paintWindowBackdrop(painter, foreign)); }
        QCOMPARE(rejected, unchanged);
    }
    void shadowLevelSurvivesAuthoritativeSettingsUpdates() {
        TestServices services; MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled());
        auto* card = window.findChild<Panel*>("homeCard"); QVERIFY(card);
        auto settings = services.settings; settings.insert("shadows", true); settings.insert("windowOpacity", .7); window.applySettings(settings);
        auto* shadow = qobject_cast<QGraphicsDropShadowEffect*>(card->graphicsEffect()); QVERIFY(shadow);
        QCOMPARE(shadow->blurRadius(), qreal(40)); QCOMPARE(shadow->offset(), QPointF(0, 16));
        QCOMPARE(window.theme().appearance().windowOpacity, .7);
        window.applySettings(settings); QCOMPARE(card->graphicsEffect(), shadow);
        settings.insert("shadows", false); window.applySettings(settings); QVERIFY(!card->graphicsEffect());
    }
};
QTEST_MAIN(EffectsTests)
#include "effects_tests.moc"
