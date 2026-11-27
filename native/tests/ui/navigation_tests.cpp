#include "TestServices.hpp"
#include "ui/Bindings.hpp"
#include "ui/ListModel.hpp"
#include "ui/MainWindow.hpp"
#include "ui/PageShell.hpp"
#include <QCoreApplication>
#include <QItemSelectionModel>
#include <QPointer>
#include <QTest>

using namespace nickel::ui;
using namespace nickel::ui::tests;

namespace {
class QueryPage final : public RoutePage {
public:
    QueryPage(Theme& theme, Bindings& bindings, Navigation& navigation) : RoutePage(theme, bindings, navigation) {}
    void load(const QString& channel, int& calls) {
        query(UiQuery::Instances, {}, channel, [&calls](const UiReply&) { ++calls; });
    }
};
}
class NavigationTests final : public QObject {
    Q_OBJECT
private slots:
    void bindingsCancelDestroyedOwnerAndUseGlobalIds() {
        TestServices services; services.automatic = false; Bindings first(services), second(services);
        auto* owner = new QObject; int calls = 0;
        const auto a = first.query(UiQuery::Accounts, {}, owner, [&calls](const UiReply&) { ++calls; });
        const auto b = second.query(UiQuery::Accounts, {}, owner, [&calls](const UiReply&) { ++calls; });
        QVERIFY(a != b); delete owner; QVERIFY(services.cancellations.contains(a)); QVERIFY(services.cancellations.contains(b));
        services.deliverQuery(a, {}); services.deliverQuery(b, {}); QTest::qWait(10); QCOMPARE(calls, 0);
    }
    void replacementAndHideCancelQueriesNotCommands() {
        TestServices services; services.automatic = false; Theme theme; Bindings bindings(services); Navigation navigation;
        QueryPage page(theme, bindings, navigation); page.show(); int calls = 0;
        page.load("list", calls); const auto old = services.queries.last().id;
        page.load("list", calls); const auto replacement = services.queries.last().id;
        QVERIFY(services.cancellations.contains(old)); services.deliverQuery(old, {}); QTest::qWait(10); QCOMPARE(calls, 0);
        const auto command = bindings.command(UiAction::InstallVersion, {}, &page);
        page.hide(); QVERIFY(services.cancellations.contains(replacement)); QVERIFY(!services.cancellations.contains(command));
        services.deliverQuery(replacement, {}); QTest::qWait(10); QCOMPARE(calls, 0);
    }
    void multiSelectionAndScrollSurviveReset() {
        Theme theme; ListModel model(ListKind::Mods); ListFilter filter; ListView view(theme, ListKind::Mods);
        QJsonArray rows; for (int i = 0; i < 100; ++i) rows.append(QJsonObject{{"root", "root-a"}, {"id", QString::number(i)}, {"name", "same"}});
        model.setItems(rows); view.setSource(&model, &filter); view.resize(600, 300); view.show(); QTest::qWait(10);
        view.selectionModel()->select(filter.index(10, 0), QItemSelectionModel::Select);
        view.selectionModel()->select(filter.index(20, 0), QItemSelectionModel::Select);
        view.selectionModel()->setCurrentIndex(filter.index(20, 0), QItemSelectionModel::NoUpdate);
        view.verticalScrollBar()->setValue(400); const auto saved = view.viewState();
        QJsonArray reverse; for (int i = 99; i >= 0; --i) reverse.append(rows.at(i));
        view.preserveSelection([&] { model.setItems(reverse); });
        QCOMPARE(view.currentObject().value("id").toString(), QString("20")); QCOMPARE(view.selectedObjects().size(), qsizetype(2));
        QCOMPARE(view.verticalScrollBar()->value(), saved.verticalScroll); QCOMPARE(view.viewState().selectedIds.size(), saved.selectedIds.size());
    }
    void mediaCachesAreBoundedAndScoped() {
        ListModel model(ListKind::Community); QJsonArray rows;
        for (int i = 0; i < 40; ++i) rows.append(QJsonObject{{"provider", "modrinth"}, {"id", QString::number(i)}, {"iconKey", "image-" + QString::number(i)}});
        model.setItems(rows); QImage image(512, 512, QImage::Format_RGBA8888); image.fill(Qt::green);
        model.setImage("unrelated-account-avatar", image); QCOMPARE(model.decodedImageBytes(), qsizetype(0));
        for (int i = 0; i < 40; ++i) model.setImage("image-" + QString::number(i), image);
        QVERIFY(model.decodedImageBytes() <= 8 * 1024 * 1024); QVERIFY(!model.image("image-39").isNull());
        model.setItems({}); QCOMPARE(model.decodedImageBytes(), qsizetype(0));
        QCOMPARE(model.rowCount(), 0);
    }
    void onlyCurrentDetailRetainsWidgets() {
        TestServices services; MainWindow window(services); window.show();
        QTRY_VERIFY(window.findChild<QStackedWidget*>("routeHost")->isEnabled()); window.navigation().navigate(Route::Resources);
        QTest::qWait(10); auto* resources = window.routePage(Route::Resources); QVERIFY(resources);
        DetailFrame first; first.key = "frame-first"; first.provider = "modrinth"; first.projectId = "first"; first.kind = "mod";
        window.navigation().pushDetail(first); QTest::qWait(10);
        auto* input = resources->findChild<LineEdit*>("projectGameFilter"); QVERIFY(input); input->setText("1.20.1");
        QPointer<QWidget> old = resources->findChild<QWidget*>("projectPage"); QVERIFY(old);
        DetailFrame second = first; second.key = "frame-second"; second.projectId = "second";
        window.navigation().pushDetail(second); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(old.isNull()); QCOMPARE(resources->findChildren<QWidget*>("projectPage").size(), qsizetype(1));
        window.navigation().popDetail(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QTest::qWait(10);
        input = resources->findChild<LineEdit*>("projectGameFilter"); QVERIFY(input); QCOMPARE(input->text(), QString("1.20.1"));
        window.navigation().navigate(Route::Settings); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(resources->findChildren<QWidget*>("projectPage").isEmpty()); QCOMPARE(window.navigation().details().size(), qsizetype(1));
        window.navigation().navigate(Route::Resources); QTest::qWait(10);
        QCOMPARE(resources->findChildren<QWidget*>("projectPage").size(), qsizetype(1));
    }
};
QTEST_MAIN(NavigationTests)
#include "navigation_tests.moc"
