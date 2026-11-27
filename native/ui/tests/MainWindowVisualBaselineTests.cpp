#include "ui/MainWindow.hpp"
#include "tests/ui/TestServices.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>
#include <thread>

using namespace nickel::ui;
using namespace nickel::ui::tests;

namespace {
QRect measuredRect(const QJsonObject& state, const char* key) {
    const auto rect = state.value(QLatin1String(key)).toObject();
    return {rect.value("x").toInt(), rect.value("y").toInt(),
            rect.value("width").toInt(), rect.value("height").toInt()};
}
} // namespace

// Tool-only geometry/state assertions: no screenshots, DPI simulation or production-frame claims.
class MainWindowVisualBaselineTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QApplication::setQuitOnLastWindowClosed(false); }
    void originalGeometryAndMeasuredRoute_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QSize>("size");
        QTest::newRow("dark-default") << QStringLiteral("dark") << QSize(1100, 720);
        QTest::newRow("light-default") << QStringLiteral("light") << QSize(1100, 720);
        QTest::newRow("dark-minimum") << QStringLiteral("dark") << QSize(900, 600);
        QTest::newRow("light-minimum") << QStringLiteral("light") << QSize(900, 600);
    }
    void originalGeometryAndMeasuredRoute() {
        QFETCH(QString, mode);
        QFETCH(QSize, size);
        TestServices services; services.settings.insert("themeMode", mode);
        MainWindow window(services); window.resize(size);
        QSignalSpy ready(&window, &MainWindow::startupReady); window.show();
        QTRY_COMPARE(ready.size(), 1);
        auto state = window.observedVisualState();
        QCOMPARE(state.value("route").toString(), QStringLiteral("home"));
        QCOMPARE(state.value("effectiveThemeMode").toString(), mode);
        QCOMPARE(state.value("accent").toString(), QStringLiteral("mono"));
        QCOMPARE(measuredRect(state, "window"), QRect(QPoint{}, size));
        QCOMPARE(measuredRect(state, "titlebar"), QRect(0, 0, size.width(), 52));
        QCOMPARE(measuredRect(state, "routeHost"), QRect(20, 72, size.width() - 40, size.height() - 92));
        QCOMPARE(measuredRect(state, "homeCard"), QRect(28, 80, 400, size.height() - 108));
        QVERIFY(state.value("startupReady").toBool());
        QVERIFY(state.value("visible").toBool());
        QCOMPARE(state.value("widgetDpr").toDouble(), window.devicePixelRatioF());
        window.navigation().navigate(Route::More);
        auto* more = window.routePage(Route::More); QVERIFY(more);
        auto* pager = more->findChild<Pager*>(); QVERIFY(pager);
        pager->select("about");
        QTRY_VERIFY(pager->page("about")->isVisible());
        state = window.observedVisualState();
        QCOMPARE(state.value("route").toString(), QStringLiteral("more"));
        QCOMPARE(state.value("section").toString(), QStringLiteral("about"));
        QCOMPARE(state.value("pageObjectName").toString(), QStringLiteral("morePage"));
        QCOMPARE(measuredRect(state, "pagerSidebar").x(), 20);
        QCOMPARE(measuredRect(state, "pagerSidebar").width(), 208);
        QCOMPARE(measuredRect(state, "pagerContent").topLeft(), QPoint(248, 72));
        QCOMPARE(measuredRect(state, "aboutMark").size(), QSize(64, 64));
        QCOMPARE(state.value("aboutNamePixelSize").toInt(), 31);
        const auto colors = state.value("colors").toObject();
        QCOMPARE(colors.value("background").toString(), mode == QStringLiteral("dark") ? QStringLiteral("#0e1011") : QStringLiteral("#eceef0"));
        QCOMPARE(colors.value("surface").toString(), mode == QStringLiteral("dark") ? QStringLiteral("#1a1d1e") : QStringLiteral("#ffffff"));
        auto* about = pager->page("about");
        auto* column = qobject_cast<QVBoxLayout*>(about->layout()); QVERIFY(column); QCOMPARE(column->spacing(), 24);
        auto* intro = about->findChild<QWidget*>("aboutIntro"); QVERIFY(intro);
        auto* introColumn = qobject_cast<QVBoxLayout*>(intro->layout()); QVERIFY(introColumn);
        QCOMPARE(introColumn->spacing(), 20); QCOMPARE(introColumn->contentsMargins(), QMargins(24, 24, 24, 24));
        auto* identity = about->findChild<QWidget*>("aboutIdentity"); QVERIFY(identity);
        auto* row = qobject_cast<QHBoxLayout*>(identity->layout()); QVERIFY(row); QCOMPARE(row->spacing(), 20);
        auto* version = about->findChild<QLabel*>("aboutVersion"); QVERIFY(version); QCOMPARE(version->font().pixelSize(), 12);
        QCOMPARE(window.minimumSize(), QSize(900, 600)); QCOMPARE(window.size(), size);
        window.navigation().navigate(Route::Home);
        QCOMPARE(window.observedVisualState().value("route").toString(), QStringLiteral("home"));
    }
    void measuredStateRejectsForeignThread() {
        TestServices services; MainWindow window(services);
        QJsonObject observed;
        std::thread worker([&] { observed = window.observedVisualState(); }); worker.join();
        QVERIFY(observed.isEmpty());
        QVERIFY(!window.observedVisualState().isEmpty());
    }
};

QTEST_MAIN(MainWindowVisualBaselineTests)
#include "MainWindowVisualBaselineTests.moc"
