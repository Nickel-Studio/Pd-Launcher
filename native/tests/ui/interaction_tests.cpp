#include "TestServices.hpp"
#include "ui/Bindings.hpp"
#include "ui/Dialogs.hpp"
#include "ui/LaunchError.hpp"
#include "ui/MainWindow.hpp"
#include "ui/Pages.hpp"
#include "ui/ListModel.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QInputMethodEvent>
#include <QPainter>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <memory>

using namespace nickel::ui;
using namespace nickel::ui::tests;

namespace {
QJsonObject failure(const QString& id = "failed-launch-a") {
    return {{"state", "error"}, {"launchId", "current-launch"},
        {"reportTarget", QJsonObject{{"root", "D:/fixture/failed-root"}, {"versionId", "failed-version"}, {"launchId", id}}},
        {"diagnostic", QJsonObject{{"category", "java"}, {"severity", "error"}, {"reason", "Java runtime is not compatible"},
            {"fallbackReason", QJsonValue(QJsonValue::Null)}, {"inputTruncated", false}}}};
}
QJsonObject savedAccounts() {
    return {{"accountType", "microsoft"}, {"activeId", "msa-a"}, {"offlineUsername", "FixturePlayer"},
        {"items", QJsonArray{
            QJsonObject{{"id", "msa-a"}, {"type", "microsoft"}, {"name", "Microsoft fixture"}, {"emailMasked", "a***@example.test"}},
            QJsonObject{{"id", "skin-b"}, {"type", "littleskin"}, {"name", "Skin fixture"}, {"uuid", "00112233445566778899aabbccddeeff"}}}}};
}
}

class InteractionTests final : public QObject {
    Q_OBJECT
private slots:
    void failureIdentityUsesCanonicalReasonAndFailedScope() {
        const auto state = failure(); const QString key = launchFailureKey(state); QVERIFY(!key.isEmpty());
        auto current = state; current.insert("launchId", "another-current-launch");
        current.insert("selectedInstance", QJsonObject{{"root", "another-root"}, {"versionId", "another-version"}});
        QCOMPARE(launchFailureKey(current), key);
        auto diagnostic = state.value("diagnostic").toObject(); diagnostic.insert("reason", "A different reason");
        current.insert("diagnostic", diagnostic); QVERIFY(launchFailureKey(current) != key);
        QVERIFY(launchFailureKey(failure("failed-launch-b")) != key); QVERIFY(launchFailureKey({}).isEmpty());
    }
    void failureActionsRetainFailedInstanceAndReleaseAfterDismissal() {
        Theme theme; TestServices services; services.automatic = false; Bindings bindings(services); QWidget host;
        QPointer<Dialog> dialog = showLaunchErrorDialog(theme, bindings, failure(), &host); QVERIFY(dialog);
        auto* reason = dialog->findChild<QLabel*>("launchFailureReason"); QVERIFY(reason);
        QCOMPARE(reason->text(), QString("Java runtime is not compatible")); QCOMPARE(reason->textFormat(), Qt::PlainText);
        auto* logs = dialog->findChild<Button*>("openFailedLogs"); QVERIFY(logs); QVERIFY(logs->isEnabled()); logs->click();
        QCOMPARE(services.commands.size(), qsizetype(1)); const auto open = services.commands.first();
        QCOMPARE(open.operation, UiAction::OpenPath); QCOMPARE(open.arguments.value("location").toString(), QString("logs"));
        QCOMPARE(open.arguments.value("root").toString(), QString("D:/fixture/failed-root"));
        QCOMPARE(open.arguments.value("versionId").toString(), QString("failed-version"));
        QCOMPARE(open.arguments.value("launchId").toString(), QString("failed-launch-a"));
        services.deliverCommand(open.id); QTest::qWait(10); dialog->accept();
        QCOMPARE(services.commands.last().operation, UiAction::ClearLaunchError);
        QCOMPARE(services.commands.last().arguments.value("launchId").toString(), QString("failed-launch-a"));
        const auto clearId = services.commands.last().id;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(dialog.isNull());
        QVERIFY(!services.cancellations.contains(clearId)); services.deliverCommand(clearId); QTest::qWait(10);
    }
    void missingFailedTargetDisablesDirectoryAndReport() {
        Theme theme; TestServices services; Bindings bindings(services); QWidget host; auto state = failure(); state.remove("reportTarget");
        QPointer<Dialog> dialog = showLaunchErrorDialog(theme, bindings, state, &host); QVERIFY(dialog);
        QVERIFY(!dialog->findChild<Button*>("openFailedLogs")->isEnabled());
        QVERIFY(!dialog->findChild<Button*>("exportLaunchReport")->isEnabled());
        dialog->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(dialog.isNull());
        QVERIFY(services.commands.isEmpty());
    }
    void busyDialogCannotBeClosedByEscapeOrWindowClose() {
        Theme theme; QPointer<Dialog> dialog = new Dialog(theme, "Busy test"); dialog->open(); dialog->setBusy(true);
        QSignalSpy dismissed(dialog, &QDialog::finished); dialog->reject(); QVERIFY(!dialog->close());
        QTest::keyClick(dialog, Qt::Key_Escape); QCOMPARE(dismissed.count(), 0); QVERIFY(dialog->isVisible());
        dialog->setBusy(false); dialog->reject(); QCOMPARE(dismissed.count(), 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(dialog.isNull());
    }
    void homeFailureIsNotDuplicatedAndSurvivesRouteHide() {
        TestServices services; MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled()); services.automatic = false;
        auto* home = window.routePage(Route::Home); QVERIFY(home); const auto state = failure();
        home->snapshot(UiTopic::Launch, state); QTRY_VERIFY(home->findChild<Dialog*>("launchErrorDialog"));
        QPointer<Dialog> dialog = home->findChild<Dialog*>("launchErrorDialog"); home->snapshot(UiTopic::Launch, state);
        QCOMPARE(home->findChildren<Dialog*>("launchErrorDialog").size(), qsizetype(1));
        home->snapshot(UiTopic::Instances, {{"selectedInstance", QJsonObject{{"root", "new-root"}, {"versionId", "new-version"}}}});
        window.navigation().navigate(Route::Settings); QVERIFY(!dialog->isVisible());
        window.navigation().navigate(Route::Home); QTRY_VERIFY(dialog->isVisible());
        QCOMPARE(dialog->property("failedRoot").toString(), QString("D:/fixture/failed-root")); dialog->accept();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(dialog.isNull());
        home->snapshot(UiTopic::Launch, state); QTest::qWait(10); QVERIFY(!home->findChild<Dialog*>("launchErrorDialog"));
        home->snapshot(UiTopic::Launch, failure("failed-launch-b")); QTRY_VERIFY(home->findChild<Dialog*>("launchErrorDialog"));
        home->findChild<Dialog*>("launchErrorDialog")->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void reasonFallsBackAndTruncatesWholeCodepoints() {
        Theme theme; TestServices services; Bindings bindings(services); QWidget host;
        auto state = failure(); auto diagnostic = state.value("diagnostic").toObject();
        diagnostic.insert("reason", " \n "); diagnostic.insert("fallbackReason", "Fallback reason\nFull details stay in the report");
        state.insert("diagnostic", diagnostic);
        QPointer<Dialog> dialog = showLaunchErrorDialog(theme, bindings, state, &host); QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QLabel*>("launchFailureReason")->text(), QString("Fallback reason"));
        dialog->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        std::u32string codepoints(250, U'\U0001f642'); diagnostic.insert("reason", QString::fromUcs4(codepoints.data(), qsizetype(codepoints.size())));
        state.insert("diagnostic", diagnostic); dialog = showLaunchErrorDialog(theme, bindings, state, &host); QVERIFY(dialog);
        const auto reason = dialog->findChild<QLabel*>("launchFailureReason")->text().toStdU32String();
        QCOMPARE(reason.size(), std::size_t(240)); QCOMPARE(reason[236], U'\U0001f642'); QCOMPARE(reason.substr(237), std::u32string(U"..."));
        dialog->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void newerFailureReplacesOldDialogWithoutClearingNewScope() {
        TestServices services; MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled()); services.automatic = false;
        auto* home = window.routePage(Route::Home); home->snapshot(UiTopic::Launch, failure());
        QTRY_VERIFY(home->findChild<Dialog*>("launchErrorDialog"));
        QPointer<Dialog> old = home->findChild<Dialog*>("launchErrorDialog"); const auto before = services.commands.size();
        home->snapshot(UiTopic::Launch, failure("failed-launch-b"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(old.isNull());
        auto* current = home->findChild<Dialog*>("launchErrorDialog"); QVERIFY(current);
        QCOMPARE(current->property("failedLaunchId").toString(), QString("failed-launch-b")); QCOMPARE(services.commands.size(), before);
        home->snapshot(UiTopic::Launch, {{"state", "idle"}});
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(!home->findChild<Dialog*>("launchErrorDialog"));
        QCOMPARE(services.commands.size(), before);
    }
    void pendingFailedActionSurvivesNewLaunchAndRouteHide() {
        TestServices services; MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled()); services.automatic = false;
        auto* home = window.routePage(Route::Home); home->snapshot(UiTopic::Launch, failure());
        QTRY_VERIFY(home->findChild<Dialog*>("launchErrorDialog"));
        QPointer<Dialog> old = home->findChild<Dialog*>("launchErrorDialog"); old->findChild<Button*>("openFailedLogs")->click();
        const auto open = services.commands.last(); QCOMPARE(open.operation, UiAction::OpenPath); QVERIFY(old->isBusy());
        home->snapshot(UiTopic::Launch, failure("failed-launch-b"));
        window.navigation().navigate(Route::Settings); QVERIFY(old); QVERIFY(!old->isVisible());
        QVERIFY(!services.cancellations.contains(open.id));
        services.deliverCommand(open.id); QTRY_VERIFY(old.isNull());
        QCOMPARE(services.commands.last().id, open.id); window.navigation().navigate(Route::Home);
        QTRY_VERIFY(home->findChild<Dialog*>("launchErrorDialog"));
        QCOMPARE(home->findChild<Dialog*>("launchErrorDialog")->property("failedLaunchId").toString(), QString("failed-launch-b"));
        home->findChild<Dialog*>("launchErrorDialog")->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void homeRunningCannotStopAndOnlyPreparationCanCancel() {
        TestServices services; MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled()); services.automatic = false;
        auto* home = window.routePage(Route::Home); auto* launch = home->findChild<Button*>("launchGame");
        auto* cancel = home->findChild<Button*>("cancelLaunch"); QVERIFY(launch); QVERIFY(cancel);
        const auto before = services.commands.size();
        home->snapshot(UiTopic::Launch, {{"state", "running"}, {"launchId", "running-a"}, {"cancelRequested", false}});
        QCOMPARE(launch->text(), QString::fromUtf8("游戏运行中")); QVERIFY(!launch->isEnabled()); QVERIFY(!cancel->isEnabled());
        launch->click(); cancel->click(); QCOMPARE(services.commands.size(), before);
        home->snapshot(UiTopic::Launch, {{"state", "preparing"}, {"launchId", "prepare-b"}, {"stage", "Checking libraries"}, {"percent", 25}, {"cancelRequested", false}});
        QVERIFY(!launch->isEnabled()); QVERIFY(cancel->isEnabled());
        cancel->click(); QCOMPARE(services.commands.size(), before + 1); QCOMPARE(services.commands.last().operation, UiAction::CancelLaunch);
        QCOMPARE(services.commands.last().arguments, QJsonObject({{"launchId", "prepare-b"}})); QVERIFY(!cancel->isEnabled());
        cancel->click(); QCOMPARE(services.commands.size(), before + 1);
        services.deliverCommand(services.commands.last().id); QTest::qWait(10); QVERIFY(!cancel->isEnabled());
        home->snapshot(UiTopic::Launch, {{"state", "cancelled"}, {"launchId", "prepare-b"}}); QVERIFY(launch->isEnabled());
        home->snapshot(UiTopic::Launch, {{"state", "preparing"}}); QVERIFY(!cancel->isEnabled());
    }
    void launchReplyCannotRegressNewerRunningTopicOrSubmitTwice() {
        TestServices services; MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled()); services.automatic = false;
        auto* home = window.routePage(Route::Home); auto* launch = home->findChild<Button*>("launchGame"); QVERIFY(launch->isEnabled());
        const auto before = services.commands.size(); launch->click(); QCOMPARE(services.commands.size(), before + 1);
        const auto pending = services.commands.last(); QCOMPARE(pending.operation, UiAction::LaunchGame);
        QCOMPARE(pending.arguments, QJsonObject({{"root", "fixture-root"}, {"versionId", "1.20.1"}}));
        home->snapshot(UiTopic::Accounts, services.accounts); QVERIFY(!launch->isEnabled()); launch->click(); QCOMPARE(services.commands.size(), before + 1);
        home->snapshot(UiTopic::Launch, {{"state", "running"}, {"launchId", "generated-a"}});
        services.deliverCommand(pending.id, {{"state", "preparing"}, {"launchId", "generated-a"}}); QTest::qWait(10);
        QVERIFY(!launch->isEnabled()); QCOMPARE(launch->text(), QString::fromUtf8("游戏运行中"));
        home->snapshot(UiTopic::Launch, {{"state", "exited"}, {"launchId", "generated-a"}}); QVERIFY(launch->isEnabled());
        launch->click(); QCOMPARE(services.commands.size(), before + 2); const auto second = services.commands.last();
        services.deliverCommand(second.id, {{"state", "preparing"}, {"launchId", "generated-b"}}); QTest::qWait(10);
        QVERIFY(home->findChild<Button*>("cancelLaunch")->isEnabled()); QVERIFY(!launch->isEnabled());
    }
    void staleBootstrapCannotReplaceNewerLaunchTopic() {
        TestServices services; services.automatic = false; Theme theme; Bindings bindings(services); Navigation navigation;
        std::unique_ptr<RoutePage> home(createHomePage(theme, bindings, navigation)); home->show(); home->enter();
        const auto pending = services.queries.last(); QCOMPARE(pending.operation, UiQuery::Bootstrap);
        home->snapshot(UiTopic::Accounts, services.accounts); home->snapshot(UiTopic::Instances, {{"selectedInstance", services.selected}});
        home->snapshot(UiTopic::Launch, {{"state", "running"}, {"launchId", "newer-launch"}});
        services.deliverQuery(pending.id, {{"selectedInstance", services.selected}, {"launch", QJsonObject{{"state", "idle"}}}}); QTest::qWait(10);
        auto* launch = home->findChild<Button*>("launchGame"); QVERIFY(!launch->isEnabled()); QCOMPARE(launch->text(), QString::fromUtf8("游戏运行中"));
    }
    void homeAvatarToolsRevealOnlyForCharacterHoverOrKeyboardFocus_data() {
        QTest::addColumn<bool>("reducedMotion");
        QTest::newRow("reduced-motion") << true;
        QTest::newRow("animated") << false;
    }
    void homeAvatarToolsRevealOnlyForCharacterHoverOrKeyboardFocus() {
        QFETCH(bool, reducedMotion);
        TestServices services; services.accounts = savedAccounts(); services.settings.insert("reducedMotion", reducedMotion);
        MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled());
        auto* home = window.routePage(Route::Home); auto* character = home->findChild<QWidget*>("homeCharacter"); QVERIFY(character);
        auto* edit = home->findChild<Button*>("editAccountCharacter"); auto* change = home->findChild<Button*>("switchSavedAccount");
        QVERIFY(edit); QVERIFY(change); QCOMPARE(edit->size(), QSize(28, 28)); QCOMPARE(change->size(), QSize(28, 28));
        auto* editEffect = qobject_cast<QGraphicsOpacityEffect*>(edit->graphicsEffect());
        auto* changeEffect = qobject_cast<QGraphicsOpacityEffect*>(change->graphicsEffect()); QVERIFY(editEffect); QVERIFY(changeEffect);
        window.findChild<Button*>("chooseVersion")->setFocus(Qt::OtherFocusReason);
        QEvent leave(QEvent::Leave); QCoreApplication::sendEvent(character, &leave);
        QTRY_COMPARE(editEffect->opacity(), qreal(0)); QCOMPARE(changeEffect->opacity(), qreal(0));
        const QRect avatar = home->findChild<ImageView*>("homeAvatar")->geometry();
        const QPointF center = character->rect().center();
        QEnterEvent enter(center, center, character->mapToGlobal(center.toPoint())); QCoreApplication::sendEvent(character, &enter);
        if (!reducedMotion) QCOMPARE(editEffect->opacity(), qreal(0));
        QTRY_COMPARE(editEffect->opacity(), qreal(1)); QCOMPARE(changeEffect->opacity(), qreal(1));
        QCOMPARE(home->findChild<ImageView*>("homeAvatar")->geometry(), avatar);
        QCoreApplication::sendEvent(character, &leave); edit->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(edit->hasFocus()); QTRY_COMPARE(editEffect->opacity(), qreal(1)); QCOMPARE(changeEffect->opacity(), qreal(1));
        change->setFocus(Qt::BacktabFocusReason); QTRY_VERIFY(change->hasFocus()); QTRY_COMPARE(editEffect->opacity(), qreal(1));
        window.findChild<Button*>("chooseVersion")->setFocus(Qt::OtherFocusReason); edit->setFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(edit->hasFocus()); QTRY_COMPARE(editEffect->opacity(), qreal(0)); QCOMPARE(changeEffect->opacity(), qreal(0));
        QCoreApplication::sendEvent(character, &enter);
        auto appearance = window.theme().appearance(); appearance.reducedMotion = true; window.theme().setAppearance(appearance);
        QCOMPARE(editEffect->opacity(), qreal(1)); QCOMPARE(changeEffect->opacity(), qreal(1));
        home->snapshot(UiTopic::Accounts, {{"accountType", "offline"}, {"offlineUsername", "FixturePlayer"}, {"items", QJsonArray{}}});
        QVERIFY(!edit->isVisible()); QVERIFY(!change->isVisible()); QCOMPARE(edit->size(), QSize(28, 28));
        QCOMPARE(editEffect->opacity(), qreal(0)); QCOMPARE(changeEffect->opacity(), qreal(0));
    }
    void accountRowsExposeLegacyLabels_data() {
        QTest::addColumn<QJsonObject>("item"); QTest::addColumn<QString>("name"); QTest::addColumn<QString>("description");
        QTest::newRow("microsoft-masked")
            << QJsonObject{{"id", "a"}, {"type", "microsoft"}, {"name", "Player"}, {"emailMasked", "a***@example.test"}, {"selected", true}, {"accountSwitchRow", true}}
            << QString("Player") << QString::fromUtf8("正版账号\na***@example.test\n当前\n删除 Player");
        QTest::newRow("microsoft-fallback")
            << QJsonObject{{"id", "a"}, {"type", "microsoft"}, {"name", "Player"}, {"accountSwitchRow", true}}
            << QString("Player") << QString::fromUtf8("正版账号\nMicrosoft 账户\n删除 Player");
        QTest::newRow("skin-server")
            << QJsonObject{{"id", "b"}, {"type", "littleskin"}, {"name", "Skin"}, {"server", "https://skin.example.test"}, {"accountSwitchRow", true}}
            << QString("Skin") << QString::fromUtf8("皮肤站\nhttps://skin.example.test\n删除 Skin");
        QTest::newRow("skin-fallback")
            << QJsonObject{{"id", "b"}, {"type", "littleskin"}, {"name", "Skin"}, {"accountSwitchRow", true}}
            << QString("Skin") << QString::fromUtf8("皮肤站\nLittleSkin\n删除 Skin");
        QTest::newRow("offline-unnamed")
            << QJsonObject{{"id", "c"}, {"type", "offline"}, {"accountSwitchRow", true}}
            << QString::fromUtf8("未命名") << QString::fromUtf8("离线\n离线 · 仅本地\n删除 未命名");
        QTest::newRow("unknown-type")
            << QJsonObject{{"id", "d"}, {"type", "future"}, {"name", "Future"}, {"accountSwitchRow", true}}
            << QString("Future") << QString::fromUtf8("账号\n\n删除 Future");
    }
    void accountRowsExposeLegacyLabels() {
        QFETCH(QJsonObject, item); QFETCH(QString, name); QFETCH(QString, description);
        ListModel model(ListKind::Accounts); model.setItems(QJsonArray{item}); const auto row = model.index(0, 0);
        QCOMPARE(row.data(Qt::DisplayRole).toString(), name); QCOMPARE(row.data(Qt::AccessibleTextRole).toString(), name);
        QCOMPARE(row.data(Qt::ToolTipRole).toString(), description); QCOMPARE(row.data(Qt::AccessibleDescriptionRole).toString(), description);
        QCOMPARE(row.data(ListModel::IdentityRole).toString(), item.value("id").toString());
        QCOMPARE(row.data(ListModel::ObjectRole).toJsonObject(), item);
    }
    void accountSwitcherPaintsLabelsCapsuleAndThirtyFourPixelAvatar_data() {
        QTest::addColumn<bool>("dark"); QTest::newRow("dark") << true; QTest::newRow("light") << false;
    }
    void accountSwitcherPaintsLabelsCapsuleAndThirtyFourPixelAvatar() {
        QFETCH(bool, dark); Theme theme; auto appearance = theme.appearance(); appearance.mode = dark ? ThemeMode::Dark : ThemeMode::Light;
        theme.setAppearance(appearance); ListModel model(ListKind::Accounts); ListDelegate delegate(theme, ListKind::Accounts);
        const QJsonObject item{{"id", "a"}, {"type", "littleskin"}, {"name", "Fixture"}, {"avatarKey", "fixture-avatar"}, {"accountSwitchRow", true}};
        const auto render = [&](bool active) {
            auto row = item; row.insert("selected", active); model.setItems(QJsonArray{row});
            QImage avatar(8, 8, QImage::Format_ARGB32); avatar.fill(QColor(255, 0, 220)); model.setImage("fixture-avatar", avatar);
            QImage image(480, 70, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::black);
            QStyleOptionViewItem option; option.rect = image.rect(); option.state = QStyle::State_Enabled;
            { QPainter painter(&image); delegate.paint(&painter, option, model.index(0, 0)); }
            return image;
        };
        const auto pixelsNear = [](const QImage& image, const QRect& rect, const QColor& color) {
            int count = 0; const auto clipped = rect.intersected(image.rect());
            for (int y = clipped.top(); y <= clipped.bottom(); ++y) for (int x = clipped.left(); x <= clipped.right(); ++x) {
                const auto pixel = image.pixelColor(x, y);
                if (qAbs(pixel.red() - color.red()) < 24 && qAbs(pixel.green() - color.green()) < 24 && qAbs(pixel.blue() - color.blue()) < 24) ++count;
            }
            return count;
        };
        const auto inactive = render(false); const auto active = render(true);
        const QRect deletion = delegate.accountDeleteRect(active.rect()); QCOMPARE(deletion.height(), 28); QVERIFY(active.rect().contains(deletion));
        const int currentWidth = QFontMetrics(bodyFont(12)).horizontalAdvance(QString::fromUtf8("当前"));
        const QRect current(deletion.left() - 8 - currentWidth, 2, currentWidth, 64);
        QVERIFY(pixelsNear(active, current, theme.tokens().accentText) > 0);
        QCOMPARE(pixelsNear(inactive, current, theme.tokens().accentText), 0);
        QVERIFY(pixelsNear(active, deletion.adjusted(35, 0, -13, 0), theme.tokens().muted) > 0);
        const QRect badge(64, 38, QFontMetrics(bodyFont(12)).horizontalAdvance(QString::fromUtf8("皮肤站")) + 12, 18);
        QVERIFY(pixelsNear(active, badge.adjusted(6, 0, -6, 0), theme.tokens().muted) > 0);
        QVERIFY(active.pixelColor(67, 47) != active.pixelColor(61, 47));
        QRect avatarBounds;
        for (int y = 0; y < active.height(); ++y) for (int x = 0; x < active.width(); ++x)
            if (active.pixelColor(x, y) == QColor(255, 0, 220)) avatarBounds = avatarBounds.united(QRect(x, y, 1, 1));
        QCOMPARE(avatarBounds, QRect(18, 17, 34, 34));
        QStyleOptionViewItem option; QCOMPARE(delegate.sizeHint(option, model.index(0, 0)).height(), 70);
    }
    void accountDeleteDragAndModelResetCannotActivateOrDeleteAnotherRow() {
        Theme theme; ListModel model(ListKind::Accounts); ListView list(theme, ListKind::Accounts); list.setSource(&model);
        const QJsonArray rows{QJsonObject{{"id", "a"}, {"name", "A"}, {"type", "microsoft"}, {"accountSwitchRow", true}},
            QJsonObject{{"id", "b"}, {"name", "B"}, {"type", "littleskin"}, {"accountSwitchRow", true}}};
        model.setItems(rows); list.resize(480, 180); list.show(); QTest::qWait(10);
        QSignalSpy clicked(&list, &QListView::clicked); QSignalSpy activated(&list, &QListView::activated);
        QSignalSpy actions(list.nickelDelegate(), &ListDelegate::actionRequested);
        const auto deletePoint = [&](int row) { return list.nickelDelegate()->accountDeleteRect(list.visualRect(model.index(row, 0))).center(); };
        const auto rowPoint = [&](int row) { return list.visualRect(model.index(row, 0)).center(); };
        QTest::mousePress(list.viewport(), Qt::LeftButton, Qt::NoModifier, deletePoint(0));
        QTest::mouseRelease(list.viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(0));
        QCOMPARE(clicked.count(), 0); QCOMPARE(activated.count(), 0); QCOMPARE(actions.count(), 0);
        QTest::mousePress(list.viewport(), Qt::LeftButton, Qt::NoModifier, deletePoint(0));
        QTest::mouseRelease(list.viewport(), Qt::LeftButton, Qt::NoModifier, deletePoint(1));
        QCOMPARE(clicked.count(), 0); QCOMPARE(activated.count(), 0); QCOMPARE(actions.count(), 0);
        QTest::mousePress(list.viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(0));
        QTest::mouseRelease(list.viewport(), Qt::LeftButton, Qt::NoModifier, deletePoint(0));
        QCOMPARE(clicked.count(), 0); QCOMPARE(activated.count(), 0); QCOMPARE(actions.count(), 0);
        QTest::mousePress(list.viewport(), Qt::LeftButton, Qt::NoModifier, deletePoint(0));
        model.setItems(QJsonArray{rows.at(1), rows.at(0)}); list.doItemsLayout();
        QTest::mouseRelease(list.viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(0));
        QCOMPARE(clicked.count(), 0); QCOMPARE(activated.count(), 0); QCOMPARE(actions.count(), 0);
        QTest::mousePress(list.viewport(), Qt::LeftButton, Qt::NoModifier, deletePoint(0));
        model.setItems(rows); list.doItemsLayout();
        QTest::mouseRelease(list.viewport(), Qt::LeftButton, Qt::NoModifier, deletePoint(0));
        QCOMPARE(clicked.count(), 0); QCOMPARE(activated.count(), 0); QCOMPARE(actions.count(), 0);
        QTest::mouseClick(list.viewport(), Qt::RightButton, Qt::NoModifier, deletePoint(0)); QCOMPARE(actions.count(), 0);
        QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, deletePoint(0)); QCOMPARE(actions.count(), 1);
        QCOMPARE(actions.first().at(0).toString(), QString("delete")); QCOMPARE(actions.first().at(1).toJsonObject().value("id").toString(), QString("a"));
        QCOMPARE(clicked.count(), 0); QCOMPARE(activated.count(), 0);
        QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(1)); QCOMPARE(clicked.count(), 1);
        list.setCurrentIndex(model.index(1, 0)); QTest::keyClick(&list, Qt::Key_Return); QCOMPARE(activated.count(), 1);
    }

    void accountSwitcherCurrentRowIsNoopAndOtherTypeSwitchesOnce() {
        TestServices services; services.accounts = savedAccounts(); MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled()); services.automatic = false;
        auto* home = window.routePage(Route::Home); auto* trigger = home->findChild<Button*>("switchSavedAccount"); QVERIFY(trigger);
        trigger->click(); QPointer<Dialog> dialog = home->findChild<Dialog*>("accountSwitcherDialog"); QVERIFY(dialog);
        QCOMPARE(dialog->width(), 480); QVERIFY(dialog->confirmButton()->isHidden());
        auto* list = dialog->findChild<ListView*>("savedAccounts"); QVERIFY(list); QTest::qWait(10);
        QCOMPARE(list->model()->rowCount(), 2); QCOMPARE(list->model()->index(0, 0).data(ListModel::ObjectRole).toJsonObject().value("id").toString(), QString("skin-b"));
        const auto before = services.commands.size(); const auto current = list->model()->index(1, 0);
        QVERIFY(current.data(Qt::AccessibleDescriptionRole).toString().contains(QString::fromUtf8("当前")));
        QVERIFY(list->model()->index(0, 0).data(Qt::AccessibleDescriptionRole).toString().contains(QString::fromUtf8("皮肤站")));
        const QString output = qEnvironmentVariable("NICKEL_UI_SCREENSHOTS");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output + "/states"));
            QVERIFY(dialog->grab().save(output + "/states/native-fixture-accountSwitcher.png"));
        }
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualRect(current).center());
        QCOMPARE(services.commands.size(), before); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(dialog.isNull());
        trigger->click(); dialog = home->findChild<Dialog*>("accountSwitcherDialog"); QVERIFY(dialog); list = dialog->findChild<ListView*>("savedAccounts"); QTest::qWait(10);
        const auto target = list->model()->index(0, 0); QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualRect(target).center());
        QCOMPARE(services.commands.size(), before + 1); const auto change = services.commands.last();
        QCOMPARE(change.operation, UiAction::SetActiveAccount); QCOMPARE(change.arguments, QJsonObject({{"id", "skin-b"}, {"type", "littleskin"}})); QVERIFY(dialog->isBusy());
        auto state = services.accounts; state.insert("activeId", "skin-b"); state.insert("accountType", "littleskin");
        home->snapshot(UiTopic::Accounts, state); QVERIFY(!home->findChild<Button*>("launchGame")->isEnabled());
        services.deliverCommand(change.id); QTest::qWait(10); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(dialog.isNull());
        home->snapshot(UiTopic::Accounts, state); QVERIFY(home->findChild<Button*>("launchGame")->isEnabled());
        QCOMPARE(home->findChild<TabStrip*>("accountTypes")->currentIndex(), 1);
    }
    void accountDeletionNeverActivatesTheRowAndBusySurvivesHide() {
        TestServices services; services.accounts = savedAccounts(); MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled()); services.automatic = false;
        auto* home = window.routePage(Route::Home); home->findChild<Button*>("switchSavedAccount")->click();
        QPointer<Dialog> dialog = home->findChild<Dialog*>("accountSwitcherDialog"); QVERIFY(dialog);
        auto* list = dialog->findChild<ListView*>("savedAccounts"); QTest::qWait(10); const auto target = list->model()->index(0, 0); const QRect row = list->visualRect(target);
        const QRect deletionRect = list->nickelDelegate()->accountDeleteRect(row); QCOMPARE(deletionRect.height(), 28);
        const auto before = services.commands.size(); QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, deletionRect.center());
        QCOMPARE(services.commands.size(), before + 1); const auto deletion = services.commands.last();
        QCOMPARE(deletion.operation, UiAction::RemoveAccount); QCOMPARE(deletion.arguments, QJsonObject({{"id", "skin-b"}})); QVERIFY(dialog->isBusy());
        dialog->reject(); QVERIFY(dialog->isVisible()); window.navigation().navigate(Route::Settings); QVERIFY(dialog); QVERIFY(!dialog->isVisible());
        QVERIFY(!services.cancellations.contains(deletion.id)); services.deliverCommand(deletion.id); QTest::qWait(10); QVERIFY(dialog); QVERIFY(!dialog->isBusy());
        auto state = services.accounts; state.insert("items", QJsonArray{services.accounts.value("items").toArray().first()});
        home->snapshot(UiTopic::Accounts, state); emit services.stateChanged(UiTopic::Accounts, state); QTest::qWait(10);
        QCOMPARE(list->model()->rowCount(), 1); QCOMPARE(services.commands.last().operation, UiAction::RemoveAccount);
        dialog->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void loginOtherClearsActiveSelectionButPreservesAccountType() {
        TestServices services; services.accounts = savedAccounts(); MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled()); services.automatic = false;
        auto* home = window.routePage(Route::Home); home->findChild<Button*>("switchSavedAccount")->click();
        QPointer<Dialog> dialog = home->findChild<Dialog*>("accountSwitcherDialog"); QVERIFY(dialog);
        const auto before = services.commands.size(); dialog->findChild<Button*>("loginOtherAccount")->click(); QCOMPARE(services.commands.size(), before + 1);
        const auto clear = services.commands.last(); QCOMPARE(clear.operation, UiAction::SetActiveAccount);
        QCOMPARE(clear.arguments, QJsonObject({{"id", ""}, {"type", "microsoft"}}));
        services.deliverCommand(clear.id); QTest::qWait(10); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(dialog.isNull());
        auto state = services.accounts; state.insert("activeId", ""); home->snapshot(UiTopic::Accounts, state);
        QVERIFY(!home->findChild<Button*>("launchGame")->isEnabled()); QCOMPARE(home->findChild<TabStrip*>("accountTypes")->currentIndex(), 0);
    }
    void accountTypePendingCannotBeReleasedByEarlyTopicAndRefreshTopicUnlocks() {
        TestServices services; MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled()); services.automatic = false;
        auto* home = window.routePage(Route::Home); auto* tabs = home->findChild<TabStrip*>("accountTypes"); auto* launch = home->findChild<Button*>("launchGame");
        tabs->setCurrentIndex(0); const auto change = services.commands.last(); QCOMPARE(change.operation, UiAction::SetAccountType);
        home->snapshot(UiTopic::Accounts, services.accounts); QVERIFY(!launch->isEnabled()); QVERIFY(!tabs->isEnabled());
        services.deliverCommand(change.id); QTest::qWait(10); const auto refresh = services.queries.last(); QCOMPARE(refresh.operation, UiQuery::Accounts);
        home->snapshot(UiTopic::Accounts, services.accounts); QVERIFY(launch->isEnabled()); QVERIFY(tabs->isEnabled());
        auto stale = savedAccounts(); services.deliverQuery(refresh.id, stale); QTest::qWait(10); QCOMPARE(tabs->currentIndex(), 2);
    }
    void settingsDraftAndRetryContainOnlyChangedNestedKeys() {
        Theme theme; TestServices services; services.settings.insert("accent", "custom");
        services.settings.insert("customTheme", QJsonObject{{"hue", 174}, {"saturation", 39}, {"lightness", 44},
            {"decoration", "lattice"}, {"decorationOpacity", .35}, {"unknownFutureValue", QJsonValue(qint64(9007199254740993LL))}});
        Bindings bindings(services); Navigation navigation; std::unique_ptr<RoutePage> page(createSettingsPage(theme, bindings, navigation));
        page->show(); page->enter(); auto* hue = page->findChild<Slider*>("custom-hue");
        auto* saturation = page->findChild<Slider*>("custom-saturation"); QVERIFY(hue); QVERIFY(saturation);
        QTRY_COMPARE(hue->value(), 174); services.automatic = false; hue->setValue(80); saturation->setValue(45);
        QTRY_COMPARE(services.commands.size(), qsizetype(1));
        const auto first = services.commands.first(); QCOMPARE(first.operation, UiAction::PatchSettings);
        const auto initial = first.arguments.value("patch").toObject().value("customTheme").toObject();
        QCOMPARE(initial.size(), 2); QCOMPARE(initial.value("hue").toInt(), 80); QCOMPARE(initial.value("saturation").toInt(), 45);
        QVERIFY(!initial.contains("unknownFutureValue")); hue->setValue(100);
        emit services.commandFinished({first.id, {}, {"save.failed", "Save failed", {}, true}});
        QTRY_COMPARE(services.commands.size(), qsizetype(2));
        const auto retry = services.commands.last(); const auto nested = retry.arguments.value("patch").toObject().value("customTheme").toObject();
        QCOMPARE(nested.size(), 2); QCOMPARE(nested.value("hue").toInt(), 100); QCOMPARE(nested.value("saturation").toInt(), 45);
        QVERIFY(!nested.contains("unknownFutureValue")); const auto queries = services.queries.size();
        page->leave(); page->hide(); services.deliverCommand(retry.id); QTest::qWait(20);
        QCOMPARE(services.queries.size(), queries);
    }
    void nativeEditorAcceptsImeCommitAndSelectionReplacement() {
        Theme theme; LineEdit input(theme); input.show(); input.setFocus(); input.setText("before text"); input.setSelection(0, 6);
        QInputMethodEvent commit; commit.setCommitString(QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87"));
        QCoreApplication::sendEvent(&input, &commit); QCOMPARE(input.text(), QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87 text"));
        QVERIFY(input.testAttribute(Qt::WA_InputMethodEnabled));
    }
};
QTEST_MAIN(InteractionTests)
#include "interaction_tests.moc"
