#include "Pages.hpp"
#include "Dialogs.hpp"
#include "ListModel.hpp"

#include <QJsonArray>
#include <QMenu>
#include <QPointer>
#include <QSignalBlocker>

namespace nickel::ui {
namespace {
// Instance/root browser. Roots and instances are service snapshots; selecting an instance only
// updates navigation state, and destructive actions always require confirmation.
class VersionsPage final : public RoutePage {
public:
    VersionsPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent)
        : RoutePage(theme, bindings, navigation, parent) {
        setObjectName("versionsPage"); auto* row = rowLayout(this, 24);
        auto* side = new Panel(theme, this, SurfaceKind::Fill); side->setObjectName("rootSidebar"); side->setFixedWidth(268); side->setShadow(0); auto* sideColumn = side->column(16, 12);
        sideColumn->addWidget(textLabel(theme, "实例目录", 15));
        roots_ = new ListModel(ListKind::Roots, this); rootView_ = new ListView(theme, ListKind::Roots, side); rootView_->setObjectName("instanceRoots"); rootView_->setSource(roots_); rootView_->setContextMenuPolicy(Qt::CustomContextMenu); sideColumn->addWidget(rootView_, 1);
        auto* add = actionButton(theme, "新增", "plus", ButtonKind::Secondary, side); auto* create = actionButton(theme, "新建目录", "folder", ButtonKind::Ghost, side); auto* import = actionButton(theme, "导入整合包", "bundle", ButtonKind::Ghost, side);
        auto* addRowWidget = new QWidget(side); auto* addRow = rowLayout(addRowWidget, 8); addRow->addWidget(add, 1); addRow->addWidget(create, 1); sideColumn->addWidget(addRowWidget); sideColumn->addWidget(import); row->addWidget(side);
        auto* main = new QWidget(this); auto* column = pageColumn(main, 12); auto* head = new QWidget(main); auto* headRow = rowLayout(head, 12);
        auto* titles = new QWidget(head); auto* titlesColumn = pageColumn(titles, 4); auto* back = actionButton(theme, "返回", "back", ButtonKind::Ghost, titles);
        titlesColumn->addWidget(back, 0, Qt::AlignLeft); titlesColumn->addWidget(textLabel(theme, "Instances", 10, true)); titlesColumn->addWidget(textLabel(theme, "实例与版本", 24));
        currentRoot_ = textLabel(theme, "当前实例目录：未选择", 12, true); titlesColumn->addWidget(currentRoot_); headRow->addWidget(titles, 1);
        auto* download = actionButton(theme, "去下载", "download", ButtonKind::Ghost, head); auto* refresh = actionButton(theme, "重新扫描", "refresh", ButtonKind::Secondary, head); headRow->addWidget(download); headRow->addWidget(refresh); column->addWidget(head);
        auto* hidden = new CheckBox(theme, "显示隐藏实例", main, true); column->addWidget(hidden); error_ = textLabel(theme, {}, 12, true); error_->hide(); column->addWidget(error_);
        state_ = new QStackedWidget(main); instances_ = new ListModel(ListKind::Instances, this); filter_ = new ListFilter(this); view_ = new ListView(theme, ListKind::Instances, state_); view_->setObjectName("instanceGrid"); view_->setSource(instances_, filter_);
        loading_ = new LoadingCard(theme, "正在读取实例目录…", state_); empty_ = emptyNotice(theme, "该实例目录下没有可识别的版本", "把版本放入 versions 文件夹后，点击重新扫描。", state_); state_->addWidget(view_); state_->addWidget(loading_); state_->addWidget(empty_); state_->setCurrentWidget(loading_); column->addWidget(state_, 1); row->addWidget(main, 1);
        connect(back, &QPushButton::clicked, this, [this] { navigation_.navigate(Route::Home); }); connect(download, &QPushButton::clicked, this, [this] { navigation_.navigate(Route::Resources); });
        connect(refresh, &QPushButton::clicked, this, [this] { reloadRoots(); }); connect(hidden, &QCheckBox::toggled, filter_, &ListFilter::setShowHidden);
        connect(rootView_, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) { const QString dir = index.data(ListModel::ObjectRole).toJsonObject().value("dir").toString(); if (!dir.isEmpty()) command(UiAction::ActivateRoot, {{"dir", dir}}, [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else reloadRoots(); }); });
        connect(rootView_, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) { rootMenu(point); });
        connect(add, &QPushButton::clicked, this, [this] { const QString dir = pickDirectory(this, "选择 .minecraft 实例目录"); if (!dir.isEmpty()) rootAction(UiAction::AddRoot, {{"dir", dir}}); });
        connect(create, &QPushButton::clicked, this, [this] { const QString dir = pickDirectory(this, "选择新建 .minecraft 的位置"); if (!dir.isEmpty()) rootAction(UiAction::CreateRoot, {{"parent", dir}}); });
        connect(import, &QPushButton::clicked, this, [this] { importPack(); });
        connect(view_, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) { selectInstance(index.data(ListModel::ObjectRole).toJsonObject()); });
        connect(view_, &QAbstractItemView::activated, this, [this](const QModelIndex& index) { selectInstance(index.data(ListModel::ObjectRole).toJsonObject()); });
        connect(view_->nickelDelegate(), &ListDelegate::actionRequested, this, [this](const QString& action, const QJsonObject& item) { instanceAction(action, item); });
        connect(&bindings.services(), &UiServices::mediaReady, instances_, &ListModel::setImage, Qt::QueuedConnection);
    }
    void enter() override { reloadRoots(); }
    void snapshot(UiTopic topic, const QJsonObject& state) override {
        if (topic != UiTopic::Instances) return;
        if (state.contains("roots")) { invalidate("roots"); applyRoots(state.value("roots").toObject()); }
        if (state.value("root").toString() == activeRoot_ && state.value("items").isArray()) { invalidate("instances"); applyInstances(state); }
    }
private:
    void reloadRoots() {
        query(UiQuery::InstanceRoots, {}, "roots", [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else applyRoots(reply.data.toObject()); });
    }
    void applyRoots(const QJsonObject& state) {
        activeRoot_ = state.value("active").toString(); currentRoot_->setText("当前实例目录：" + activeRoot_); rootView_->preserveSelection([this, state] { roots_->setItems(state.value("items").toArray()); });
        const auto index = roots_->find(activeRoot_); if (index.isValid()) rootView_->setCurrentIndex(index);
        if (activeRoot_.isEmpty()) { state_->setCurrentWidget(empty_); return; } reloadInstances();
    }
    void reloadInstances() {
        const QString root = activeRoot_; loading_->setError(false); loading_->setLabel("正在扫描版本实例…"); if (instances_->rowCount() == 0) state_->setCurrentWidget(loading_);
        query(UiQuery::Instances, {{"root", root}}, "instances", [this, root](const UiReply& reply) { if (root != activeRoot_) return; if (reply.error.isError()) showError(reply.error); else if (reply.data.toObject().value("root").toString() == root) applyInstances(reply.data.toObject()); });
    }
    void applyInstances(const QJsonObject& state) {
        QJsonArray items; for (const auto& value : state.value("items").toArray()) items.append(withFields(value.toObject(), {{"root", activeRoot_}}));
        view_->preserveSelection([this, items] { instances_->setItems(items); }); state_->setCurrentWidget(items.isEmpty() ? empty_ : view_); error_->hide();
    }
    void showError(const UiError& error) { error_->setText(error.message); error_->show(); if (instances_->rowCount() == 0) { loading_->setError(true); loading_->setLabel(error.message); state_->setCurrentWidget(loading_); } report(error); }
    void rootAction(UiAction action, const QJsonObject& args) { command(action, args, [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else reloadRoots(); }); }
    void rootMenu(const QPoint& point) {
        const auto item = rootView_->indexAt(point).data(ListModel::ObjectRole).toJsonObject(); const QString dir = item.value("dir").toString(); if (dir.isEmpty()) return;
        QMenu menu(this); menu.setPalette(theme_.palette()); menu.setFont(bodyFont());
        auto* open = menu.addAction("打开目录"); auto* rename = menu.addAction("重命名"); auto* remove = menu.addAction("从列表移除"); auto* erase = menu.addAction("删除磁盘目录");
        auto* chosen = menu.exec(rootView_->viewport()->mapToGlobal(point));
        if (chosen == open) command(UiAction::OpenPath, {{"path", dir}});
        if (chosen == rename) promptText(theme_, this, "重命名实例目录", "显示名称", item.value("label").toString(), [this, dir](const QString& text, Dialog* dialog) { QPointer<Dialog> owner(dialog); dialog->setBusy(true); command(UiAction::RenameRoot, {{"dir", dir}, {"label", text}}, [this, owner](const UiReply& reply) { if (!owner) return; if (reply.error.isError()) owner->showError(reply.error); else { owner->accept(); reloadRoots(); } }); });
        if (chosen == remove) confirmAction(theme_, this, "移除实例目录", "只从列表移除，不删除任何文件。\n" + dir, [this, dir] { rootAction(UiAction::RemoveRoot, {{"dir", dir}}); });
        if (chosen == erase) confirmAction(theme_, this, "删除磁盘目录", "删除目录及所有实例、存档和文件？\n" + dir, [this, dir] { rootAction(UiAction::DeleteRoot, {{"dir", dir}}); });
    }
    void selectInstance(const QJsonObject& item) {
        if (selecting_ || !item.value("hasJson").toBool()) return; const InstanceIdentity identity{item.value("root").toString(), item.value("id").toString()}; if (!identity.valid()) return;
        selecting_ = true; command(UiAction::SelectInstance, identity.scope(), [this](const UiReply& reply) { selecting_ = false; if (reply.error.isError()) showError(reply.error); else navigation_.navigate(Route::Home); });
    }
    void instanceAction(const QString& action, const QJsonObject& item) {
        const InstanceIdentity identity{item.value("root").toString(), item.value("id").toString()}; if (!identity.valid()) return;
        if (action == "settings") { navigation_.openVersionSettings(identity); return; }
        if (action == "favorite") command(UiAction::SetInstanceFavorite, withFields(identity.scope(), {{"favorite", !item.value("favorite").toBool()}}), [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else reloadInstances(); });
        if (action == "delete") confirmAction(theme_, this, "删除实例", "删除「" + item.value("displayName").toString() + "」及目录内全部文件？\n" + item.value("path").toString(), [this, identity] { command(UiAction::DeleteInstance, identity.scope(), [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else reloadInstances(); }); });
        if (action == "rename") promptText(theme_, this, "重命名实例", "显示名称", item.value("displayName").toString(), [this, identity](const QString& text, Dialog* dialog) { QPointer<Dialog> owner(dialog); command(UiAction::RenameInstance, withFields(identity.scope(), {{"patch", QJsonObject{{"name", text}}}}), [this, owner](const UiReply& reply) { if (!owner) return; if (reply.error.isError()) owner->showError(reply.error); else { owner->accept(); reloadInstances(); } }); });
    }
    void importPack() {
        if (activeRoot_.isEmpty()) return; const auto files = pickFiles(this, "导入整合包", "整合包 (*.mrpack *.zip)"); if (files.isEmpty()) return;
        const QString path = files.first(), root = activeRoot_;
        promptText(theme_, this, "导入整合包", "新实例名称", {}, [this, path, root](const QString& name, Dialog* dialog) { dialog->accept(); command(UiAction::ImportPack, {{"path", path}, {"root", root}, {"instanceName", name}}); });
    }
    ListModel *roots_, *instances_; ListFilter* filter_; ListView *rootView_, *view_; QStackedWidget* state_; LoadingCard* loading_; QWidget* empty_; QLabel *error_, *currentRoot_; QString activeRoot_; bool selecting_ = false;
};
}
RoutePage* createVersionsPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent) { return new VersionsPage(theme, bindings, navigation, parent); }
} // namespace nickel::ui
