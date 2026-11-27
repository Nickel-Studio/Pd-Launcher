#include "TestServices.hpp"
#include "ui/Bindings.hpp"
#include "ui/Controls.hpp"
#include "ui/ListModel.hpp"
#include "ui/Icons.hpp"
#include "ui/MainWindow.hpp"
#include "ui/SafeDescription.hpp"
#include "ui/TitleBar.hpp"
#include "ui/ProductIdentity.hpp"
#include "ui/StartupGate.hpp"
#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGlyphRun>
#include <QRawFont>
#include <QTextLayout>
#include <QImageReader>
#include <QPluginLoader>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTest>
#include <QWindow>
#include <cmath>
#include <thread>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace nickel::ui;
using namespace nickel::ui::tests;

class NativeUiTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QGuiApplication::setApplicationDisplayName(QStringLiteral("Palladium"));
    }
    void homeGeometry() {
        TestServices services; MainWindow window(services); QSignalSpy ready(&window, &MainWindow::startupReady);
        window.show(); QTRY_COMPARE(ready.count(), 1); QCOMPARE(window.size(), QSize(1100, 720));
        QCOMPARE(window.minimumSize(), QSize(900, 600)); QCOMPARE(window.windowTitle(), QString("Palladium"));
        auto* card = window.findChild<Panel*>("homeCard"); QVERIFY(card);
        QCOMPARE(QRect(card->mapTo(&window, QPoint(0, 0)), card->size()), QRect(28, 80, 400, 612));
        QVERIFY(card->layout()); QCOMPARE(card->layout()->contentsMargins(), QMargins(24, 24, 24, 24));
        auto* title = window.findChild<TitleBar*>("titleBar"); QVERIFY(title); QCOMPARE(title->geometry(), QRect(0, 0, 1100, 52));
        auto* brand = title->findChild<QLabel*>("productBrand"); QVERIFY(brand);
        QCOMPARE(brand->text(), QString("Palladium")); QCOMPARE(brand->geometry(), QRect(20, 0, 132, 52));
        auto* tabs = title->tabs(); QCOMPARE(tabs->count(), 4);
        QCOMPARE(tabs->geometry().center().x(), window.rect().center().x());
        window.resize(900, 600); QTest::qWait(20);
        QCOMPARE(QRect(card->mapTo(&window, QPoint(0, 0)), card->size()), QRect(28, 80, 400, 492));
    }
    void productDisplayNameDrivesWindowChromeAndAbout_data() {
        QTest::addColumn<QString>("displayName"); QTest::addColumn<QString>("expected");
        QTest::newRow("palladium") << QString("Palladium") << QString("Palladium");
        QTest::newRow("pd") << QString("Pd") << QString("Pd");
        QTest::newRow("chinese") << QString::fromUtf8("钯") << QString::fromUtf8("钯");
        QTest::newRow("unset-internal-name") << QString{} << QString("Palladium");
    }
    void productDisplayNameDrivesWindowChromeAndAbout() {
        QFETCH(QString, displayName); QFETCH(QString, expected);
        const auto originalName = QCoreApplication::applicationName();
        const auto originalDisplay = QGuiApplication::applicationDisplayName();
        const auto restore = qScopeGuard([originalName, originalDisplay] {
            QCoreApplication::setApplicationName(originalName); QGuiApplication::setApplicationDisplayName(originalDisplay);
        });
        QCoreApplication::setApplicationName(QStringLiteral("pd-launcher")); QGuiApplication::setApplicationDisplayName(displayName);
        TestServices services; MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled());
        QCOMPARE(productDisplayName(), expected); QCOMPARE(window.windowTitle(), expected);
        QCOMPARE(QCoreApplication::applicationName(), QString("pd-launcher"));
        QCOMPARE(window.objectName(), QString("nickelMainWindow"));
        auto* brand = window.findChild<QLabel*>("productBrand"); QVERIFY(brand); QCOMPARE(brand->text(), expected);
        QCOMPARE(brand->geometry(), QRect(20, 0, 132, 52));
        window.navigation().navigate(Route::More); auto* page = window.routePage(Route::More); QVERIFY(page);
        auto* pager = page->findChild<Pager*>(); QVERIFY(pager); pager->select("about");
        bool identity = false, mark = false;
        for (const auto* label : pager->page("about")->findChildren<QLabel*>()) if (label->text() == expected) identity = true;
        for (const auto* widget : pager->page("about")->findChildren<QWidget*>())
            if (widget->accessibleName() == expected + QString::fromUtf8(" 应用徽标")) { mark = true; QCOMPARE(widget->size(), QSize(64, 64)); }
        QVERIFY(identity); QVERIFY(mark);
    }
    void onboardingUsesVisibleProductName_data() {
        QTest::addColumn<QString>("state");
        QTest::newRow("eula") << QString("eula");
        QTest::newRow("changelog") << QString("changelog");
        QTest::newRow("rejection") << QString("rejection");
    }
    void onboardingUsesVisibleProductName() {
        QFETCH(QString, state); Theme theme; TestServices services; Bindings bindings(services); StartupGate gate(theme, bindings);
        const QJsonObject status{{"needsEula", state == "eula"}, {"needsChangelog", state == "changelog"},
            {"exiting", state == "rejection"}, {"eulaVersion", "fixture-eula"}, {"currentVersion", "1.0.0"}};
        gate.resize(1100, 720); gate.apply(status); gate.show();
        bool product = false;
        for (const auto* label : gate.findChildren<QLabel*>()) if (label->isVisibleTo(&gate)) {
            QVERIFY(!label->text().contains(QStringLiteral("Nickel")));
            if (label->text().contains(QStringLiteral("Palladium"))) product = true;
        }
        QVERIFY(product); QVERIFY(services.commands.isEmpty());
    }
    void productRenamePreservesLegacyAccentKeyAndReportFormat() {
        QCOMPARE(accentFromKey("nickel"), Accent::Nickel); QCOMPARE(accentKey(Accent::Nickel), QString("nickel"));
        const auto& info = accentOptions().at(static_cast<std::size_t>(Accent::Nickel));
        QCOMPARE(QString::fromUtf8(info.label), QString::fromUtf8("钯光")); QCOMPARE(QString(info.color), QString("#8dbcae"));
        QCOMPARE(errorReportFileName(), QString("Pd-error-report.zip"));
    }
    void captureRenderedFrameRejectsHiddenAndWrongThread() {
        TestServices services; MainWindow window(services);
        constexpr qsizetype budget = 64 * 1024 * 1024;
        QVERIFY(window.captureRenderedFrame(budget).isNull());
        window.show(); QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled());
        QImage workerResult;
        std::thread worker([&window, &workerResult] { workerResult = window.captureRenderedFrame(budget); });
        worker.join(); QVERIFY(workerResult.isNull());
        window.hide(); QVERIFY(window.captureRenderedFrame(budget).isNull());
    }
    void captureRenderedFrameRejectsUnboundedBudgets() {
        TestServices services; MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled());
        for (const auto budget : {qsizetype(-1), qsizetype(0), qsizetype(1), qsizetype(64 * 1024 * 1024 + 1)})
            QVERIFY(window.captureRenderedFrame(budget).isNull());
    }
    void captureRenderedFramePreservesOnboardingAndDpi() {
#ifdef Q_OS_WIN
        const bool threadUnaware = qEnvironmentVariable("PD_UI_TEST_THREAD_UNAWARE") == QStringLiteral("1");
        const auto previousCreationDpi = threadUnaware
            ? SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE) : nullptr;
        const auto restoreCreationDpi = qScopeGuard([previousCreationDpi] {
            if (previousCreationDpi) QVERIFY(SetThreadDpiAwarenessContext(previousCreationDpi));
        });
        if (threadUnaware) QVERIFY(previousCreationDpi);
#endif
        TestServices services; services.holdFinish = true; MainWindow window(services);
        QSignalSpy ready(&window, &MainWindow::startupReady); window.show();
        QTRY_VERIFY(!services.commands.isEmpty());
        const auto commandCount = services.commands.size();
        const auto platform = QGuiApplication::platformName();
        if (platform == QStringLiteral("offscreen") || platform.startsWith(QStringLiteral("minimal"))) {
            QVERIFY(window.captureRenderedFrame(64 * 1024 * 1024).isNull());
        } else {
            QTRY_VERIFY(window.windowHandle() && window.windowHandle()->isExposed());
            const qreal ratio = window.devicePixelRatioF();
            const auto widthPixels = qsizetype(std::ceil(window.width() * ratio));
            const auto heightPixels = qsizetype(std::ceil(window.height() * ratio));
            const qsizetype planeBytes = widthPixels * heightPixels * 4;
            QVERIFY(window.captureRenderedFrame(planeBytes).isNull());
            const auto image = window.captureRenderedFrame(planeBytes * 2);
            QVERIFY(!image.isNull()); QCOMPARE(image.size(), (QSizeF(window.size()) * ratio).toSize());
            QCOMPARE(image.devicePixelRatio(), ratio); QVERIFY(image.sizeInBytes() <= planeBytes);
#ifdef Q_OS_WIN
            const auto hwnd = reinterpret_cast<HWND>(window.winId());
            const auto windowContext = GetWindowDpiAwarenessContext(hwnd);
            RECT localClient{}; QVERIFY(GetClientRect(hwnd, &localClient));
            POINT physicalOrigin{localClient.left, localClient.top};
            POINT physicalEnd{localClient.right, localClient.bottom};
            QVERIFY(ClientToScreen(hwnd, &physicalOrigin)); QVERIFY(ClientToScreen(hwnd, &physicalEnd));
            QVERIFY(LogicalToPhysicalPointForPerMonitorDPI(hwnd, &physicalOrigin));
            QVERIFY(LogicalToPhysicalPointForPerMonitorDPI(hwnd, &physicalEnd));
            const auto previousObserverDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            QVERIFY(previousObserverDpi);
            auto restoreObserverDpi = qScopeGuard([previousObserverDpi] {
                QVERIFY(SetThreadDpiAwarenessContext(previousObserverDpi));
            });
            RECT observerClient{}; QVERIFY(GetClientRect(hwnd, &observerClient));
            qInfo().noquote() << "DPI_RESEARCH_NOT_PRODUCTION"
                << "threadUnaware=" << threadUnaware
                << "windowAwareness=" << int(GetAwarenessFromDpiAwarenessContext(windowContext))
                << "windowUnaware=" << bool(AreDpiAwarenessContextsEqual(windowContext, DPI_AWARENESS_CONTEXT_UNAWARE))
                << "windowsDpi=" << GetDpiForWindow(hwnd) << "observerSystemDpi=" << GetDpiForSystem()
                << "logical=" << window.size() << "qtWidgetDpr=" << ratio
                << "qtWindowDpr=" << window.windowHandle()->devicePixelRatio()
                << "imageDpr=" << image.devicePixelRatio() << "imagePixels=" << image.size()
                << "observerClient=" << QSize(observerClient.right - observerClient.left, observerClient.bottom - observerClient.top)
                << "convertedPhysicalClient=" << QSize(physicalEnd.x - physicalOrigin.x, physicalEnd.y - physicalOrigin.y);
            const bool observerRestored = SetThreadDpiAwarenessContext(previousObserverDpi) != nullptr;
            restoreObserverDpi.dismiss(); QVERIFY(observerRestored);
#endif
            bool painted = false;
            for (int y = 0; y < image.height() && !painted; ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const auto color = image.pixelColor(x, y);
                    if (color.alpha() && (color.red() || color.green() || color.blue())) { painted = true; break; }
                }
            QVERIFY(painted);
        }
        QCOMPARE(services.commands.size(), commandCount); QCOMPARE(ready.count(), 0);
        QVERIFY(!window.findChild<QStackedWidget*>("routeHost")->isEnabled());
    }
    void sevenRoutesAndReturnOrigin() {
        TestServices services; MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled());
        for (const auto route : {Route::Home, Route::Resources, Route::Settings, Route::More, Route::Versions, Route::DownloadDetail}) {
            window.navigation().navigate(route); QVERIFY(window.routePage(route));
        }
        window.navigation().openVersionSettings({"fixture-root", "1.20.1"}); QVERIFY(window.routePage(Route::VersionSettings));
        QCOMPARE(window.navigation().tabIndex(), -1);
        window.navigation().navigate(Route::DownloadDetail); window.navigation().returnFromDownloads();
        QCOMPARE(window.navigation().route(), Route::VersionSettings);
    }
    void onboardingHoldsInputUntilCommit() {
        TestServices services; services.holdFinish = true; MainWindow window(services); QSignalSpy ready(&window, &MainWindow::startupReady);
        window.show(); QTRY_VERIFY(!services.commands.isEmpty());
        QCOMPARE(ready.count(), 0); QVERIFY(!window.findChild<QStackedWidget*>("routeHost")->isEnabled());
        const auto command = services.commands.last(); QCOMPARE(command.operation, UiAction::FinishOnboarding);
        services.deliverCommand(command.id); QTRY_COMPARE(ready.count(), 1);
        QVERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled());
    }
    void staleDestroyedOwnerIsIgnored() {
        TestServices services; services.automatic = false; Bindings bindings(services); int deliveries = 0;
        auto* owner = new QObject; const auto id = bindings.query(UiQuery::Instances, {}, owner, [&deliveries](const UiReply&) { ++deliveries; });
        delete owner; services.deliverQuery(id, {}); QTest::qWait(20); QCOMPARE(deliveries, 0);
    }
    void nativeEditorClipboardAndSemantics() {
        Theme theme; LineEdit input(theme); input.setAccessibleName("Native text input"); input.show(); input.setFocus();
        QTest::keyClicks(&input, "Nickel"); QCOMPARE(input.text(), QString("Nickel")); input.selectAll(); input.copy();
        QCOMPARE(QApplication::clipboard()->text(), QString("Nickel")); input.clear(); input.paste(); QCOMPARE(input.text(), QString("Nickel"));
        auto* accessible = QAccessible::queryAccessibleInterface(&input); QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::EditableText); QCOMPARE(accessible->text(QAccessible::Name), QString("Native text input"));
    }
    void allAccentsContrast() {
        QCOMPARE(accentOptions().size(), std::size_t(12));
        for (const auto mode : {ThemeMode::Dark, ThemeMode::Light}) {
            for (const auto& option : accentOptions()) {
                qInfo() << "Accent mode/id:" << int(mode) << int(option.id);
                Appearance appearance; appearance.mode = mode; appearance.accent = option.id;
                const auto tokens = makeThemeTokens(appearance, true);
                const double contrast = contrastRatio(tokens.accentText, tokens.surface4);
                qInfo() << "Accent contrast:" << contrast;
                QVERIFY(contrast >= 4.79);
                QCOMPARE(accentFromKey(accentKey(option.id)), option.id);
            }
        }
    }
    void fullListIdentity() {
        ListModel model(ListKind::Instances); QJsonArray rows;
        for (int i = 0; i < 10000; ++i) rows.append(QJsonObject{{"root", "fixture-root"}, {"id", QString::number(i)}, {"displayName", "duplicate name"}});
        model.setItems(rows); QCOMPARE(model.rowCount(), 10000); QVERIFY(model.identity(0) != model.identity(9999));
    }
    void staticPluginAndImageFormatEvidence() {
        QStringList keys;
        for (const auto& plugin : QPluginLoader::staticPlugins()) {
            const auto metadata = plugin.metaData();
            qInfo().noquote() << "Static plugin:" << metadata.value("className").toString();
            for (const auto& key : metadata.value("MetaData").toObject().value("Keys").toArray()) keys.append(key.toString());
        }
        const auto formats = QImageReader::supportedImageFormats();
        qInfo().noquote() << "Platform:" << QGuiApplication::platformName() << "Image formats:" << formats;
        QVERIFY(formats.contains("png")); QVERIFY(formats.contains("jpeg"));
#ifdef QT_STATIC
        QVERIFY(keys.contains("offscreen"));
#ifdef Q_OS_WIN
        QVERIFY(keys.contains("windows"));
#endif
#endif
    }
    void resolvedTextFontsContainRepresentativeGlyphs() {
        const auto fonts = QList<QFont>{bodyFont(14), displayFont(21, QFont::Bold), monoFont(12)};
        const QString sample = QString::fromUtf8("Palladium Pd 钯 钯光 Nickel 1.20.1 中文启动账号设置");
        qInfo().noquote() << "Platform:" << QGuiApplication::platformName() << "font directory:" << qEnvironmentVariable("QT_QPA_FONTDIR");
        QVERIFY(QFile::exists(":/nickel/brand/fonts/nickel-text-subset.ttf"));
        for (const auto& font : fonts) {
            qInfo().noquote() << "Requested:" << font.families() << "Resolved primary:" << QFontInfo(font).family();
            const auto primary = QRawFont::fromFont(font); QVERIFY(primary.isValid());
            QTextLayout layout(sample, font); layout.beginLayout(); auto line = layout.createLine(); QVERIFY(line.isValid()); line.setLineWidth(1200); layout.endLayout();
            const auto runs = layout.glyphRuns(0, -1, QTextLayout::RetrieveGlyphIndexes); QVERIFY(!runs.isEmpty()); qsizetype glyphCount = 0;
            for (const auto& run : runs) {
                const auto raw = run.rawFont(); QVERIFY(raw.isValid());
                const auto glyphs = run.glyphIndexes(); glyphCount += glyphs.size();
                qInfo().noquote() << "Glyph run:" << raw.familyName() << "glyphs:" << glyphs.size();
                for (const auto glyph : glyphs) QVERIFY2(glyph != 0, "Actual fallback layout contains a missing glyph");
            }
            QVERIFY(glyphCount >= sample.size());
        }
    }
    void legacyResourceAliasesAndNearestNeighbourPixels() {
        for (const auto& key : {"grass", "defaultredstone", "defaultmod", "optifabric", "skins/steve-head.png", "skins/alex-head.png"})
            QVERIFY2(!functionalImage(key).isNull(), key);
        QCOMPARE(functionalImage("defaultredstone"), functionalImage("icons/redstone_block.png"));
        QCOMPARE(functionalImage("optifabric"), functionalImage("blocks/OptiFabric.png"));
        QVERIFY(isPixelArtResource("defaultredstone")); QVERIFY(isPixelArtResource(":/nickel/functional/skins/steve-head.png"));
        QVERIFY(functionalImage("file:///C:/unapproved.png").isNull()); QVERIFY(functionalImage("../outside.png").isNull());
        QImage source(2, 1, QImage::Format_ARGB32); source.setPixelColor(0, 0, Qt::black); source.setPixelColor(1, 0, Qt::white);
        QImage target(20, 10, QImage::Format_ARGB32); target.fill(Qt::transparent);
        { QPainter painter(&target); paintContainedImage(painter, target.rect(), source, true); }
        for (int x = 0; x < target.width(); ++x) QCOMPARE(target.pixelColor(x, 5), QColor(x < 10 ? Qt::black : Qt::white));
    }
    void fixtureScreenshots() {
        const QString output = qEnvironmentVariable("NICKEL_UI_SCREENSHOTS");
        if (output.isEmpty()) QSKIP("Set NICKEL_UI_SCREENSHOTS to a test-owned directory to capture native fixtures.");
        for (const bool populated : {false, true}) {
            const QString directory = output + (populated ? QString{} : QStringLiteral("/empty"));
            QVERIFY(QDir().mkpath(directory)); TestServices services;
            if (populated) {
                services.accounts = {{"accountType", "microsoft"}, {"activeId", "fixture-microsoft"}, {"offlineUsername", "FixturePlayer"},
                    {"items", QJsonArray{
                        QJsonObject{{"id", "fixture-microsoft"}, {"type", "microsoft"}, {"name", "FixturePlayer"}, {"emailMasked", "f***@example.invalid"}, {"avatarKey", "skins/steve-head.png"}},
                        QJsonObject{{"id", "fixture-littleskin"}, {"type", "littleskin"}, {"name", "FixtureSkin"}, {"emailMasked", "s***@example.invalid"}, {"avatarKey", "skins/alex-head.png"}}
                    }}};
                services.queryReplies.insert(UiQuery::VanillaCatalog, {{"latestRelease", "1.21.1"}, {"latestSnapshot", "24w33a"},
                    {"items", QJsonArray{
                        QJsonObject{{"id", "1.21.1"}, {"type", "release"}, {"releaseTime", "2024-08-08"}, {"iconKey", "grass"}},
                        QJsonObject{{"id", "24w33a"}, {"type", "snapshot"}, {"releaseTime", "2024-08-15"}, {"iconKey", "command_block"}},
                        QJsonObject{{"id", "1.20.1"}, {"type", "release"}, {"releaseTime", "2023-06-12"}, {"iconKey", "grass"}}
                    }}});
                services.queryReplies.insert(UiQuery::Instances, {{"items", QJsonArray{
                    QJsonObject{{"id", "1.20.1"}, {"displayName", "Native fixture"}, {"minecraftVersion", "1.20.1"}, {"loader", "Fabric"}, {"iconKey", "fabric"}, {"category", "normal"}, {"hasJson", true}, {"favorite", true}},
                    QJsonObject{{"id", "1.21.1"}, {"displayName", "Vanilla fixture"}, {"minecraftVersion", "1.21.1"}, {"loader", "Vanilla"}, {"iconKey", "grass"}, {"category", "normal"}, {"hasJson", true}},
                    QJsonObject{{"id", "incomplete-fixture"}, {"displayName", "Incomplete fixture"}, {"minecraftVersion", "1.20.1"}, {"iconKey", "defaultredstone"}, {"category", "normal"}, {"hasJson", false}}
                }}});
                services.queryReplies.insert(UiQuery::VersionDetail, {{"name", "Native fixture"}, {"minecraftVersion", "1.20.1"}, {"loader", "Fabric"},
                    {"category", "normal"}, {"iconKey", ""}, {"favorite", true}, {"description", "Deterministic visual fixture; not a production instance."}});
                services.downloads = {{"ready", true}, {"bubbleDismissed", true},
                    {"overview", QJsonObject{{"percent", 42.5}, {"speedBps", 2097152}, {"remainingFiles", 3}}},
                    {"items", QJsonArray{
                        QJsonObject{{"id", "fixture-running"}, {"title", "Minecraft 1.20.1"}, {"status", "running"}, {"percent", 42.5},
                            {"stages", QJsonArray{QJsonObject{{"name", "Metadata"}, {"completed", true}}, QJsonObject{{"name", "Libraries and assets"}, {"completed", false}}}}},
                        QJsonObject{{"id", "fixture-queued"}, {"title", "Queued resource fixture"}, {"status", "queued"}, {"queuePosition", 2}},
                        QJsonObject{{"id", "fixture-failed"}, {"title", "Failed download fixture"}, {"status", "error"}, {"message", "Controlled fixture failure; no network operation."},
                            {"stages", QJsonArray{QJsonObject{{"name", "Download file"}, {"completed", false}}}}}
                    }}};
            }
            MainWindow window(services); window.show();
            QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled());
            for (const auto route : {Route::Home, Route::Resources, Route::Settings, Route::More, Route::Versions, Route::DownloadDetail}) {
                window.navigation().navigate(route);
                if (populated && (route == Route::Resources || route == Route::Versions || route == Route::DownloadDetail)) {
                    const char* name = route == Route::Resources ? "vanillaVersions" : route == Route::Versions ? "instanceGrid" : "downloadTasks";
                    auto* view = window.findChild<ListView*>(name); QVERIFY(view); QTRY_COMPARE(view->model()->rowCount(), 3);
                }
                QTest::qWait(40);
                const QPixmap capture = window.grab(); QCOMPARE(capture.size(), QSize(1100, 720));
                QVERIFY(capture.save(directory + "/native-fixture-" + routeKey(route) + ".png"));
            }
            window.navigation().openVersionSettings({"fixture-root", "1.20.1"});
            auto* name = window.findChild<LineEdit*>("instanceDisplayName"); QVERIFY(name); QTRY_COMPARE(name->text(), QString("Native fixture"));
            QTest::qWait(40); const QPixmap capture = window.grab(); QCOMPARE(capture.size(), QSize(1100, 720));
            QVERIFY(capture.save(directory + "/native-fixture-versionSettings.png"));
        }
    }
};
QTEST_MAIN(NativeUiTests)
#include "ui_tests.moc"
