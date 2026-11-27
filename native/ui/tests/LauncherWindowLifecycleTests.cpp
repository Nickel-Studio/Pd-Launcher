#include "ui/LauncherWindowLifecycle.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QTest>

using namespace nickel::ui;

namespace {
class LauncherFixture final : public QWidget {
public:
    int acceptedCloses = 0;
    void receive(const LaunchWindowState& state) {
        const auto action = lifecycle_.observe(state);
        (void)applyLauncherWindowAction(*this, action);
    }
protected:
    void closeEvent(QCloseEvent* event) override {
        ++acceptedCloses;
        QWidget::closeEvent(event);
    }
private:
    LauncherWindowLifecycle lifecycle_;
};

LaunchWindowState record(const QString& mode, LaunchWindowPhase phase,
                         bool seen = false, quint64 generation = 1) {
    return {generation, QStringLiteral("launch-%1").arg(generation), mode, phase, seen, false};
}
} // namespace

// Tool-only real-QWidget tests. These do not capture production screenshots or start a game.
class LauncherWindowLifecycleTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QApplication::setQuitOnLastWindowClosed(false); }

    void legacyFiveModes_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("hiddenAfterWindow");
        QTest::addColumn<bool>("minimizedAfterWindow");
        QTest::addColumn<int>("closesAfterWindow");
        QTest::addColumn<bool>("visibleAfterExit");
        QTest::addColumn<bool>("minimizedAfterExit");
        QTest::addColumn<int>("closesAfterExit");
        QTest::newRow("keep") << QStringLiteral("keep") << false << false << 0 << true << false << 0;
        QTest::newRow("hide") << QStringLiteral("hide") << true << false << 0 << true << false << 0;
        QTest::newRow("hide-close") << QStringLiteral("hide-close") << true << false << 0 << false << false << 1;
        QTest::newRow("close") << QStringLiteral("close") << true << false << 1 << false << false << 1;
        QTest::newRow("minimize") << QStringLiteral("minimize") << false << true << 0 << true << true << 0;
    }
    void legacyFiveModes() {
        QFETCH(QString, mode);
        QFETCH(bool, hiddenAfterWindow);
        QFETCH(bool, minimizedAfterWindow);
        QFETCH(int, closesAfterWindow);
        QFETCH(bool, visibleAfterExit);
        QFETCH(bool, minimizedAfterExit);
        QFETCH(int, closesAfterExit);
        LauncherFixture window;
        window.resize(1100, 720);
        window.show();
        QTRY_VERIFY(window.isVisible());
        window.receive(record(mode, LaunchWindowPhase::Preparing));
        QVERIFY(window.isVisible());
        QVERIFY(!window.isMinimized());
        QCOMPARE(window.acceptedCloses, 0);
        window.receive(record(mode, LaunchWindowPhase::Running));
        QVERIFY(window.isVisible());
        QVERIFY(!window.isMinimized());
        window.receive(record(mode, LaunchWindowPhase::Running, true));
        QTRY_COMPARE(window.isVisible(), !hiddenAfterWindow);
        QCOMPARE(window.isMinimized(), minimizedAfterWindow);
        QCOMPARE(window.acceptedCloses, closesAfterWindow);
        window.receive(record(mode, LaunchWindowPhase::Running, true));
        QCOMPARE(window.acceptedCloses, closesAfterWindow);
        auto draining = record(mode, LaunchWindowPhase::Draining, true);
        draining.rootExited = true;
        window.receive(draining);
        QCOMPARE(window.isVisible(), !hiddenAfterWindow);
        QCOMPARE(window.isMinimized(), minimizedAfterWindow);
        QCOMPARE(window.acceptedCloses, closesAfterWindow);
        window.receive(record(mode, LaunchWindowPhase::Finished, true));
        QTRY_COMPARE(window.isVisible(), visibleAfterExit);
        QCOMPARE(window.isMinimized(), minimizedAfterExit);
        QCOMPARE(window.acceptedCloses, closesAfterExit);
        QCOMPARE(window.size(), QSize(1100, 720));
    }

    void cancellationAndPreWindowFailure_data() {
        QTest::addColumn<QString>("mode");
        for (const auto& mode : {"keep", "hide", "hide-close", "close", "minimize"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void cancellationAndPreWindowFailure() {
        QFETCH(QString, mode);
        LauncherFixture window;
        window.show();
        window.receive(record(mode, LaunchWindowPhase::Preparing));
        window.receive(record(mode, LaunchWindowPhase::Finished));
        QVERIFY(window.isVisible());
        QVERIFY(!window.isMinimized());
        QCOMPARE(window.acceptedCloses, 0);
        window.receive(record(mode, LaunchWindowPhase::Running, true));
        QVERIFY(window.isVisible());
        QCOMPARE(window.acceptedCloses, 0);
    }

    void terminalDoesNotReplayWindowReadiness() {
        LauncherFixture window;
        window.show();
        window.receive(record(QStringLiteral("hide-close"), LaunchWindowPhase::Finished, true));
        QVERIFY(window.isVisible());
        QCOMPARE(window.acceptedCloses, 0);
        auto exited = record(QStringLiteral("close"), LaunchWindowPhase::Running, true, 2);
        exited.rootExited = true;
        window.receive(exited);
        QVERIFY(window.isVisible());
        QCOMPARE(window.acceptedCloses, 0);
    }

    void staleGenerationCannotRestoreOrCloseSuccessor() {
        LauncherFixture window;
        window.show();
        window.receive(record(QStringLiteral("hide"), LaunchWindowPhase::Running, true));
        QVERIFY(!window.isVisible());
        window.receive(record(QStringLiteral("hide"), LaunchWindowPhase::Finished, true));
        QVERIFY(window.isVisible());
        window.receive(record(QStringLiteral("hide-close"), LaunchWindowPhase::Running, true, 2));
        QVERIFY(!window.isVisible());
        window.receive(record(QStringLiteral("hide"), LaunchWindowPhase::Finished, true));
        QVERIFY(!window.isVisible());
        QCOMPARE(window.acceptedCloses, 0);
        window.receive(record(QStringLiteral("hide-close"), LaunchWindowPhase::Finished, true, 2));
        QCOMPARE(window.acceptedCloses, 1);
        window.receive(record(QStringLiteral("hide-close"), LaunchWindowPhase::Running, true, 2));
        QCOMPARE(window.acceptedCloses, 1);
    }

    void duplicateReadinessDoesNotHideTrayRestoration() {
        LauncherFixture window;
        window.show();
        const auto running = record(QStringLiteral("hide"), LaunchWindowPhase::Running, true);
        window.receive(running);
        QVERIFY(!window.isVisible());
        window.show();
        window.receive(running);
        QVERIFY(window.isVisible());
        window.receive(record(QStringLiteral("hide"), LaunchWindowPhase::Finished, true));
        QVERIFY(window.isVisible());
    }

    void frozenPolicyAndIdentityRejectConflicts() {
        LauncherFixture window;
        window.show();
        window.receive(record(QStringLiteral("hide"), LaunchWindowPhase::Preparing));
        window.receive(record(QStringLiteral("close"), LaunchWindowPhase::Running, true));
        auto foreign = record(QStringLiteral("hide"), LaunchWindowPhase::Running, true);
        foreign.launchId = QStringLiteral("other-launch");
        window.receive(foreign);
        auto malformed = foreign;
        malformed.generation = 0;
        window.receive(malformed);
        window.receive(record(QStringLiteral("invalid"), LaunchWindowPhase::Running, true, 2));
        QVERIFY(window.isVisible());
        QCOMPARE(window.acceptedCloses, 0);
        window.receive(record(QStringLiteral("hide"), LaunchWindowPhase::Running, true));
        QVERIFY(!window.isVisible());
    }

    void restorePreservesMaximizedState() {
        LauncherFixture window;
        window.showMaximized();
        QTRY_VERIFY(window.isMaximized());
        window.receive(record(QStringLiteral("hide"), LaunchWindowPhase::Running, true));
        QVERIFY(!window.isVisible());
        window.setWindowState(window.windowState() | Qt::WindowMinimized);
        window.receive(record(QStringLiteral("hide"), LaunchWindowPhase::Finished, true));
        QTRY_VERIFY(window.isVisible());
        QVERIFY(window.isMaximized());
        QVERIFY(!window.isMinimized());
    }
};

QTEST_MAIN(LauncherWindowLifecycleTests)
#include "LauncherWindowLifecycleTests.moc"
