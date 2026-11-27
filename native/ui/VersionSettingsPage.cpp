#include "Pages.hpp"
#include "Dialogs.hpp"
#include "GameForm.hpp"
#include "ListModel.hpp"

#include <QJsonArray>
#include <QPointer>
#include <QSignalBlocker>
#include <QTreeWidget>

namespace nickel::ui {
namespace {
// Per-instance overview/settings/mods/export page. Every mutation is a service command scoped by
// root+versionId; the page never writes instance files and drops replies for a replaced target.
class VersionSettingsPage final : public RoutePage {
public:
    VersionSettingsPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent)
        : RoutePage(theme, bindings, navigation, parent) {
        setObjectName("versionSettingsPage"); auto* column = pageColumn(this, 12);
        auto* back = actionButton(theme, "返回实例列表", "detail/back", ButtonKind::Ghost, this); column->addWidget(back, 0, Qt::AlignLeft); connect(back, &QPushButton::clicked, this, [this] { navigation_.backToVersions(); });
        error_ = textLabel(theme, {}, 12, true); error_->hide(); column->addWidget(error_);
        pager_ = new Pager(theme, "版本设置", {{"overview", "概览", "account/layers", {}}, {"settings", "设置", "account/sliders", {}}, {"mods", "Mod 管理", "mods", {}}, {"export", "导出", "bundle", {}}, {"installed", "已安装资源", "folderOpen", {}}}, this); column->addWidget(pager_, 1);
        buildOverview(); game_ = new GameForm(theme, true); pager_->addPage("settings", game_); buildMods(); buildExport(); buildInstalled();
        connect(pager_, &Pager::sectionChanged, this, [this](const QString& section) { navigation_.memory(Route::VersionSettings).section = section; loadSection(section); });
        connect(game_, &GameForm::patchRequested, this, [this](const QJsonObject& patch) { scopedCommand(UiAction::PatchVersionSettings, {{"patch", patch}}); });
        connect(game_, &GameForm::javaRefreshRequested, this, [this] { loadJava(); });
        connect(game_, &GameForm::offlineSkinRequested, this, [this](const QJsonObject& args) { scopedCommand(args.value("reset").toBool() ? UiAction::ResetOfflineSkin : UiAction::UpdateOfflineSkin, args); });
        connect(&navigation, &Navigation::versionTargetChanged, this, [this] { enter(); });
        connect(&bindings.services(), &UiServices::mediaReady, mods_, &ListModel::setImage, Qt::QueuedConnection);
        connect(&bindings.services(), &UiServices::mediaReady, resources_, &ListModel::setImage, Qt::QueuedConnection);
        pager_->setEnabled(false);
    }
    void enter() override {
        const auto target = navigation_.versionTarget(); if (!target.valid()) { showError({"instance.missing", "尚未选择实例", {}, false}); pager_->setEnabled(false); return; }
        if (!(target == target_)) { target_ = target; detail_ = {}; mods_->setItems({}); resources_->setItems({}); tree_->clear(); game_->setAvailable(false); }
        const auto scope = target_.scope(); query(UiQuery::VersionDetail, scope, "detail", [this, target](const UiReply& reply) { if (!(target == target_)) return; if (reply.error.isError()) showError(reply.error); else { detail_ = reply.data.toObject(); if (!matchesInstance(detail_, target_)) { showError({"instance.stale", "版本信息目标不一致", {}, true}); return; } applyOverview(); pager_->setEnabled(true); loadSection(pager_->selected()); } });
        loadJava(); query(UiQuery::RuntimeInformation, scope, "runtime", [this, target](const UiReply& reply) { if (target == target_ && !reply.error.isError()) game_->setRuntime(reply.data.toObject()); });
    }
    void snapshot(UiTopic topic, const QJsonObject& state) override {
        if (!matchesInstance(state, target_)) return;
        if (topic == UiTopic::Version) { if (state.value("detail").isObject()) { invalidate("detail"); detail_ = state.value("detail").toObject(); applyOverview(); } if (state.value("settings").isObject()) { invalidate("settings"); applySettings(state.value("settings").toObject()); } }
        if (topic == UiTopic::Mods) { invalidate("mods"); applyList(state, mods_, modView_); }
        if (topic == UiTopic::Resources) { invalidate("installed"); applyList(state, resources_, resourceView_); }
    }
private:
    void buildOverview() {
        auto* page = new QWidget; auto* column = pageColumn(page, 16); auto* info = new Panel(theme_, page); auto* infoColumn = info->column(20, 12); title_ = textLabel(theme_, "正在读取版本信息…", 24); metadata_ = textLabel(theme_, {}, 12, true); infoColumn->addWidget(title_); infoColumn->addWidget(metadata_); column->addWidget(info);
        auto* personal = new FormGroup(theme_, "个性化", "只在启动器里生效的展示信息", page); column->addWidget(personal);
        icon_ = new ComboBox(theme_, personal); const QList<QPair<QString, QString>> icons{{"", "自动选择"}, {"grass", "原版"}, {"command_block", "快照"}, {"cobblestone", "旧版"}, {"anvil", "Forge"}, {"neoforge", "NeoForge"}, {"fabric", "Fabric"}, {"grass_path", "OptiFine"}, {"egg", "LiteLoader"}, {"gold_block", "愚人节"}, {"redstone_block", "未识别"}, {"redstone_lamp_on", "红石灯（开）"}, {"redstone_lamp_off", "红石灯（关）"}};
        for (const auto& option : icons) icon_->addItem(option.second, option.first); personal->addRow("图标", icon_);
        auto* customIcon = actionButton(theme_, "选择自定义图标", "image", ButtonKind::Ghost, personal); personal->body()->addWidget(customIcon, 0, Qt::AlignRight);
        category_ = new ComboBox(theme_, personal); category_->addItem("普通", "normal"); category_->addItem("常用", "favorite"); category_->addItem("隐藏", "hidden"); personal->addRow("分类", category_);
        name_ = new LineEdit(theme_, personal); name_->setObjectName("instanceDisplayName"); personal->addRow("显示名称", name_);
        description_ = new TextEdit(theme_, personal); description_->setMinimumHeight(72); personal->addRow("描述", description_, {}, true);
        favorite_ = new CheckBox(theme_, "收藏此实例", personal); personal->body()->addWidget(favorite_);
        auto* save = actionButton(theme_, "保存展示信息", "check", ButtonKind::Primary, personal); personal->body()->addWidget(save, 0, Qt::AlignRight);
        connect(save, &QPushButton::clicked, this, [this] { scopedCommand(UiAction::RenameInstance, {{"patch", QJsonObject{{"name", name_->text()}, {"description", description_->toPlainText()}, {"category", category_->currentData().toString()}, {"favorite", favorite_->isChecked()}}}}); });
        connect(icon_, &QComboBox::activated, this, [this](int) { scopedCommand(UiAction::SetInstanceIcon, {{"icon", icon_->currentData().toString().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(icon_->currentData().toString())}}); });
        connect(customIcon, &QPushButton::clicked, this, [this] { const auto files = pickFiles(this, "选择实例图标", "图片 (*.png *.jpg *.jpeg *.webp)"); if (!files.isEmpty()) scopedCommand(UiAction::SetInstanceIcon, {{"path", files.first()}}); });
        auto* shortcuts = new FormGroup(theme_, "快捷方式", "在系统文件管理器中打开对应目录", page); column->addWidget(shortcuts);
        for (const auto& shortcut : QList<QPair<QString, QString>>{{"instance", "实例目录"}, {"mods", "Mods"}, {"resourcepacks", "资源包"}, {"shaderpacks", "光影"}, {"saves", "存档"}, {"logs", "日志"}}) { auto* button = actionButton(theme_, shortcut.second, "folder", ButtonKind::Ghost, shortcuts); shortcuts->body()->addWidget(button); connect(button, &QPushButton::clicked, this, [this, key = shortcut.first] { scopedCommand(UiAction::OpenPath, {{"location", key}}); }); }
        auto* advanced = new FormGroup(theme_, "高级管理", {}, page); column->addWidget(advanced); auto* reset = actionButton(theme_, "重置版本设置", "refresh", ButtonKind::Secondary, advanced); auto* erase = actionButton(theme_, "删除版本", "trash", ButtonKind::Danger, advanced); advanced->body()->addWidget(reset); advanced->body()->addWidget(erase);
        connect(reset, &QPushButton::clicked, this, [this] { const auto target = target_; confirmAction(theme_, this, "重置版本设置", "清除当前实例的覆盖字段，恢复继承全局设置？", [this, target] { if (target == target_) scopedCommand(UiAction::ResetVersionSettings, {}); }); });
        connect(erase, &QPushButton::clicked, this, [this] { const auto target = target_; confirmAction(theme_, this, "删除版本", "删除实例目录内全部文件？\n" + detail_.value("path").toString(), [this, target] { command(UiAction::DeleteInstance, target.scope(), [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else navigation_.backToVersions(); }); }); });
        column->addStretch(); pager_->addPage("overview", page);
    }
    void buildMods() {
        auto* page = new QWidget; auto* column = pageColumn(page, 12); auto* head = new QWidget(page); auto* row = rowLayout(head, 8); row->addWidget(textLabel(theme_, "Mod 管理", 19)); row->addStretch(); auto* browse = actionButton(theme_, "下载 Mod", "download", ButtonKind::Primary, head); auto* scan = actionButton(theme_, "重新扫描", "refresh", ButtonKind::Secondary, head); row->addWidget(browse); row->addWidget(scan); column->addWidget(head);
        auto* search = new LineEdit(theme_, page); search->setObjectName("modSearch"); search->setPlaceholderText("搜索 Mod"); column->addWidget(search);
        mods_ = new ListModel(ListKind::Mods, this); modFilter_ = new ListFilter(this); modView_ = new ListView(theme_, ListKind::Mods, page); modView_->setObjectName("modsList"); modView_->setSource(mods_, modFilter_); column->addWidget(modView_, 1);
        auto* tools = new QWidget(page); auto* toolRow = rowLayout(tools, 6); auto* selectAll = actionButton(theme_, "全选", {}, ButtonKind::Ghost, tools); toolRow->addWidget(selectAll);
        const QList<QPair<QString, UiAction>> actions{{"启用", UiAction::SetModEnabled}, {"禁用", UiAction::SetModEnabled}, {"检查更新", UiAction::CheckModUpdates}, {"更新", UiAction::UpdateMods}, {"删除", UiAction::DeleteMods}, {"导出", UiAction::ExportMods}};
        for (const auto& operation : actions) { auto* button = actionButton(theme_, operation.first, {}, operation.second == UiAction::DeleteMods ? ButtonKind::Danger : ButtonKind::Ghost, tools); toolRow->addWidget(button); connect(button, &QPushButton::clicked, this, [this, operation] { modBatch(operation.first, operation.second); }); }
        auto* cancel = actionButton(theme_, "取消选择", "close", ButtonKind::Ghost, tools); toolRow->addWidget(cancel); connect(cancel, &QPushButton::clicked, mods_, &ListModel::clearChecked); column->addWidget(tools);
        connect(selectAll, &QPushButton::clicked, this, [this] { for (int row = 0; row < modFilter_->rowCount(); ++row) mods_->setChecked(modFilter_->index(row, 0).data(ListModel::IdentityRole).toString(), true); });
        connect(search, &QLineEdit::textChanged, modFilter_, &ListFilter::setSearch); connect(browse, &QPushButton::clicked, this, [this] { navigation_.openResourceMods(target_); }); connect(scan, &QPushButton::clicked, this, [this] { loadSection("mods"); });
        connect(modView_->nickelDelegate(), &ListDelegate::actionRequested, this, [this](const QString& action, const QJsonObject& item) {
            if (action == "toggle") scopedCommand(UiAction::SetModEnabled, {{"ids", QJsonArray{item.value("id")}}, {"enabled", !item.value("enabled").toBool()}});
            else if (action == "delete") { const auto target = target_; confirmAction(theme_, this, "删除 Mod", "删除「" + item.value("name").toString() + "」？", [this, target, item] { command(UiAction::DeleteMods, withFields(target.scope(), {{"ids", QJsonArray{item.value("id")}}}), [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else loadSection("mods"); }); }); }
            else if (action == "rename") promptText(theme_, this, "重命名 Mod", "文件名称", item.value("fileName").toString(), [this, item](const QString& name, Dialog* dialog) { QPointer<Dialog> owner(dialog); scopedCommand(UiAction::RenameMod, {{"id", item.value("id")}, {"name", name}}, [owner](const UiReply& reply) { if (!owner) return; if (reply.error.isError()) owner->showError(reply.error); else owner->accept(); }); });
        }); pager_->addPage("mods", page, false);
    }
    void buildInstalled() {
        auto* page = new QWidget; auto* column = pageColumn(page, 12); auto* head = new QWidget(page); auto* row = rowLayout(head, 8); row->addWidget(textLabel(theme_, "已安装", 19)); row->addStretch(); auto* scan = actionButton(theme_, "重新扫描", "refresh", ButtonKind::Secondary, head); auto* import = actionButton(theme_, "导入资源", "plus", ButtonKind::Primary, head); row->addWidget(scan); row->addWidget(import); column->addWidget(head);
        resources_ = new ListModel(ListKind::Resources, this); resourceView_ = new ListView(theme_, ListKind::Resources, page); resourceView_->setSource(resources_); column->addWidget(resourceView_, 1);
        auto* erase = actionButton(theme_, "删除选择的资源", "trash", ButtonKind::Danger, page); column->addWidget(erase, 0, Qt::AlignRight);
        connect(scan, &QPushButton::clicked, this, [this] { loadSection("installed"); });
        connect(import, &QPushButton::clicked, this, [this] { const auto files = pickFiles(this, "导入本地资源", "资源 (*.jar *.zip)"); QJsonArray paths; for (const auto& file : files) paths.append(file); if (!paths.isEmpty()) scopedCommand(UiAction::ImportResources, {{"paths", paths}}); });
        connect(erase, &QPushButton::clicked, this, [this] { const auto ids = selectedIds(resources_, resourceView_); if (ids.isEmpty()) return; const auto target = target_; confirmAction(theme_, this, "删除资源", "删除选中的 " + QString::number(ids.size()) + " 项资源？", [this, target, ids] { command(UiAction::DeleteResource, withFields(target.scope(), {{"ids", ids}}), [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else loadSection("installed"); }); }); }); pager_->addPage("installed", page, false);
    }
    void buildExport() {
        auto* page = new QWidget; auto* column = pageColumn(page, 16); auto* info = new FormGroup(theme_, "整合包信息", {}, page); exportName_ = new LineEdit(theme_, info); exportVersion_ = new LineEdit(theme_, info); info->addRow("名称", exportName_); info->addRow("版本", exportVersion_); column->addWidget(info);
        auto* format = new FormGroup(theme_, "导出格式", {}, page); exportFormat_ = new ComboBox(theme_, format); exportFormat_->addItem("Modrinth 整合包 (.mrpack)", "mrpack"); exportFormat_->addItem("便携整合包 (.zip)", "portable"); format->addRow("格式", exportFormat_);
        includeLauncher_ = new CheckBox(theme_, "包含启动器", format); includePersonal_ = new CheckBox(theme_, "包含个性化信息", format); format->body()->addWidget(includeLauncher_); format->body()->addWidget(includePersonal_); column->addWidget(format);
        tree_ = new QTreeWidget(page); tree_->setObjectName("exportTree"); tree_->setHeaderHidden(true); tree_->setFrameShape(QFrame::NoFrame); tree_->setPalette(theme_.palette()); tree_->setFont(bodyFont()); tree_->setItemDelegate(new PopupDelegate(theme_, tree_)); tree_->setVerticalScrollBar(new ScrollBar(theme_, Qt::Vertical, tree_)); tree_->setMinimumHeight(220); column->addWidget(tree_, 1);
        auto* exportButton = actionButton(theme_, "导出整合包", "download", ButtonKind::Primary, page); column->addWidget(exportButton, 0, Qt::AlignRight);
        connect(exportFormat_, &QComboBox::activated, this, [this](int) { const bool portable = exportFormat_->currentData().toString() == "portable"; includeLauncher_->setEnabled(portable); includePersonal_->setEnabled(portable); });
        connect(exportButton, &QPushButton::clicked, this, [this] { const bool portable = exportFormat_->currentData().toString() == "portable"; const QString path = saveFile(this, "导出整合包", exportName_->text() + (portable ? ".zip" : ".mrpack"), portable ? "ZIP (*.zip)" : "Modrinth (*.mrpack)"); if (path.isEmpty()) return; QJsonArray files; collect(tree_->invisibleRootItem(), files); scopedCommand(UiAction::ExportPack, {{"path", path}, {"name", exportName_->text()}, {"version", exportVersion_->text()}, {"format", exportFormat_->currentData().toString()}, {"files", files}, {"includeLauncher", portable && includeLauncher_->isChecked()}, {"includePersonalization", portable && includePersonal_->isChecked()}}); }); pager_->addPage("export", page, false);
    }
    static void collect(QTreeWidgetItem* item, QJsonArray& result) { for (int row = 0; row < item->childCount(); ++row) { auto* child = item->child(row); if (child->childCount()) collect(child, result); else if (child->checkState(0) == Qt::Checked) result.append(child->data(0, Qt::UserRole).toString()); } }
    void addTree(const QJsonArray& items, QTreeWidgetItem* parent) { for (const auto& value : items) { const auto object = value.toObject(); auto* item = new QTreeWidgetItem(parent, {object.value("name").toString()}); item->setData(0, Qt::UserRole, object.value("path")); item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate); item->setCheckState(0, object.value("checked").toBool() ? Qt::Checked : Qt::Unchecked); addTree(object.value("children").toArray(), item); } }
    QJsonArray selectedIds(ListModel* model, ListView* view) const { QJsonArray ids; const auto checked = model->checked(); if (!checked.isEmpty()) { for (int row = 0; row < model->rowCount(); ++row) if (checked.contains(model->identity(row))) ids.append(model->object(row).value("id")); } else for (const auto& value : view->selectedObjects()) ids.append(value.toObject().value("id")); return ids; }
    void modBatch(const QString& label, UiAction action) {
        QJsonArray ids = selectedIds(mods_, modView_); if (action != UiAction::CheckModUpdates && ids.isEmpty()) return; QJsonObject fields{{"ids", ids}};
        if (action == UiAction::SetModEnabled) fields.insert("enabled", label == "启用");
        if (action == UiAction::ExportMods) { const QString path = saveFile(this, "导出 Mod", "Mods.zip", "ZIP (*.zip)"); if (path.isEmpty()) return; fields.insert("path", path); }
        if (action == UiAction::DeleteMods) { const auto target = target_; confirmAction(theme_, this, "删除 Mod", "删除选中的 " + QString::number(ids.size()) + " 个 Mod？", [this, target, fields] { command(UiAction::DeleteMods, withFields(target.scope(), fields), [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else loadSection("mods"); }); }); }
        else scopedCommand(action, fields);
    }
    void applyOverview() {
        title_->setText(detail_.value("name").toString().isEmpty() ? target_.versionId : detail_.value("name").toString()); metadata_->setText(detail_.value("minecraftVersion").toString() + " · " + detail_.value("loader").toString() + " · " + target_.root); pager_->setTitle(title_->text());
        QSignalBlocker a(name_), b(description_), c(category_), d(favorite_), e(icon_); if (!name_->hasFocus()) name_->setText(title_->text()); if (!description_->hasFocus()) description_->setPlainText(detail_.value("description").toString()); category_->setCurrentIndex(category_->findData(detail_.value("category").toString())); favorite_->setChecked(detail_.value("favorite").toBool()); icon_->setCurrentIndex(icon_->findData(detail_.value("iconKey").toString())); if (exportName_->text().isEmpty()) exportName_->setText(title_->text());
    }
    void applySettings(const QJsonObject& state) { game_->setValues(state.value("values").toObject(), state.value("effective").toObject()); game_->setAvailable(true); }
    void applyList(const QJsonObject& state, ListModel* model, ListView* view) { QJsonArray rows; for (const auto& value : state.value("items").toArray()) rows.append(withFields(value.toObject(), target_.scope())); view->preserveSelection([model, rows] { model->setItems(rows); }); if (!state.value("issues").toArray().isEmpty()) error_->setText(joinJsonStrings(state.value("issues"), "\n")); }
    void loadJava() { query(UiQuery::JavaInstallations, target_.scope(), "java", [this](const UiReply& reply) { if (!reply.error.isError()) game_->setJavaInstallations(reply.data.isArray() ? reply.data.toArray() : reply.data.toObject().value("items").toArray()); else showError(reply.error); }); }
    void loadSection(const QString& section) {
        if (!target_.valid() || detail_.isEmpty()) return; const auto target = target_;
        if (section == "settings") query(UiQuery::VersionSettings, target.scope(), "settings", [this, target](const UiReply& reply) { if (!(target == target_)) return; if (reply.error.isError()) showError(reply.error); else applySettings(reply.data.toObject()); });
        if (section == "mods" || section == "installed") query(section == "mods" ? UiQuery::Mods : UiQuery::InstalledResources, target.scope(), section, [this, target, section](const UiReply& reply) { if (!(target == target_)) return; if (reply.error.isError()) showError(reply.error); else if (matchesInstance(reply.data.toObject(), target)) applyList(reply.data.toObject(), section == "mods" ? mods_ : resources_, section == "mods" ? modView_ : resourceView_); });
        if (section == "export") query(UiQuery::ExportTree, target.scope(), "export", [this, target](const UiReply& reply) { if (!(target == target_)) return; if (reply.error.isError()) showError(reply.error); else { tree_->clear(); addTree(reply.data.toObject().value("items").toArray(), tree_->invisibleRootItem()); } });
    }
    void scopedCommand(UiAction action, const QJsonObject& fields, Bindings::Callback callback = {}) { if (!target_.valid()) return; const auto target = target_; command(action, withFields(target.scope(), fields), [this, target, callback = std::move(callback)](const UiReply& reply) { if (callback) callback(reply); else report(reply.error); if (target == target_ && !reply.error.isError()) loadSection(pager_->selected()); }); }
    void showError(const UiError& error) { error_->setText(error.message); error_->show(); report(error); }
    Pager* pager_; GameForm* game_; InstanceIdentity target_; QJsonObject detail_; QLabel *title_, *metadata_, *error_; LineEdit *name_, *exportName_, *exportVersion_; TextEdit* description_; ComboBox *category_, *icon_, *exportFormat_; CheckBox *favorite_, *includeLauncher_, *includePersonal_; QTreeWidget* tree_; ListModel *mods_, *resources_; ListFilter* modFilter_; ListView *modView_, *resourceView_;
};
}
RoutePage* createVersionSettingsPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent) { return new VersionSettingsPage(theme, bindings, navigation, parent); }
} // namespace nickel::ui
