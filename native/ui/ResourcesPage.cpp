#include "Pages.hpp"
#include "Dialogs.hpp"
#include "ListModel.hpp"
#include "SafeDescription.hpp"

#include <QJsonArray>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace nickel::ui {
namespace {
// Append-only pagination merge keyed by provider+id; the first occurrence wins, so overlapping
// pages from a retried query cannot duplicate rows.
QJsonArray appendItems(QJsonArray previous, const QJsonArray& next) {
    QSet<QString> present;
    for (const auto& value : previous) { const auto item = value.toObject(); present.insert(item.value("provider").toString() + QChar(0) + item.value("id").toString()); }
    for (const auto& value : next) { const auto item = value.toObject(); const QString id = item.value("provider").toString() + QChar(0) + item.value("id").toString(); if (!present.contains(id)) { previous.append(item); present.insert(id); } }
    return previous;
}
// Community search for one resource kind. Every reload invalidates the previous query channel
// before appending the next page, so a slow older reply cannot replace newer results.
class SearchPage final : public RoutePage {
public:
    SearchPage(Theme& theme, Bindings& bindings, Navigation& navigation, const QString& kind, const QString& label, std::function<void(QJsonObject)> open, QWidget* parent)
        : RoutePage(theme, bindings, navigation, parent), kind_(kind), open_(std::move(open)) {
        auto* column = pageColumn(this, 16); auto* head = new QWidget(this); auto* row = rowLayout(head, 8); row->addWidget(textLabel(theme, label, 24)); row->addStretch(); source_ = new ComboBox(theme, head); source_->addItem("双源检索", ""); source_->addItem("Modrinth", "modrinth"); source_->addItem("CurseForge", "curseforge"); row->addWidget(source_); column->addWidget(head);
        search_ = new LineEdit(theme, this); search_->setObjectName("communitySearch-" + kind); search_->setPlaceholderText("搜索 " + label); search_->setAccessibleName("搜索 " + label); column->addWidget(search_);
        auto* filters = new QWidget(this); auto* filterRow = rowLayout(filters, 8); game_ = new LineEdit(theme, filters); game_->setPlaceholderText("Minecraft 版本"); game_->setAccessibleName("Minecraft 版本筛选"); loader_ = new ComboBox(theme, filters); loader_->addItem("全部加载器", ""); for (const QString& id : QStringList{"forge", "neoforge", "fabric", "quilt", "liteloader"}) loader_->addItem(id, id); sort_ = new ComboBox(theme, filters); sort_->addItem("相关性", "relevance"); sort_->addItem("下载量", "downloads"); sort_->addItem("最新更新", "updated"); filterRow->addWidget(game_, 1); filterRow->addWidget(loader_); filterRow->addWidget(sort_); column->addWidget(filters);
        targetLabel_ = textLabel(theme, {}, 11, true); column->addWidget(targetLabel_); issues_ = textLabel(theme, {}, 12, true); issues_->hide(); column->addWidget(issues_);
        model_ = new ListModel(ListKind::Community, this); view_ = new ListView(theme, ListKind::Community, this); view_->setObjectName("communityResults-" + kind); view_->setSource(model_); state_ = new QStackedWidget(this); loading_ = new LoadingCard(theme, "正在检索社区资源…", state_); empty_ = emptyNotice(theme, "没有匹配的资源", "调整关键词、版本或来源后重新检索。", state_); state_->addWidget(view_); state_->addWidget(loading_); state_->addWidget(empty_); state_->setCurrentWidget(loading_); column->addWidget(state_, 1);
        more_ = actionButton(theme, "加载更多", "chevronDown", ButtonKind::Ghost, this); more_->hide(); column->addWidget(more_, 0, Qt::AlignHCenter);
        debounce_.setSingleShot(true); debounce_.setInterval(300); connect(&debounce_, &QTimer::timeout, this, [this] { reload(false); });
        connect(search_, &QLineEdit::textEdited, this, [this] { invalidate("search"); debounce_.start(); }); connect(search_, &QLineEdit::returnPressed, this, [this] { debounce_.stop(); reload(false); });
        connect(game_, &QLineEdit::editingFinished, this, [this] { reload(false); });
        for (auto* combo : {source_, loader_, sort_}) connect(combo, &QComboBox::activated, this, [this](int) { reload(false); });
        connect(more_, &QPushButton::clicked, this, [this] { reload(true); });
        connect(view_, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) { open_(index.data(ListModel::ObjectRole).toJsonObject()); });
        connect(view_, &QAbstractItemView::activated, this, [this](const QModelIndex& index) { open_(index.data(ListModel::ObjectRole).toJsonObject()); });
        connect(&bindings.services(), &UiServices::mediaReady, model_, &ListModel::setImage, Qt::QueuedConnection);
    }
    void setTarget(const InstanceIdentity& target) {
        if (target == target_) return;
        target_ = target; invalidate("search"); loaded_ = false; items_ = {}; model_->setItems({});
        targetLabel_->setText(target.valid() ? "目标实例：" + target.versionId + " · " + target.root : "未选择安装目标；检索与项目正文仍可查看");
        if (isVisible()) reload(false);
    }
    void enter() override { if (!loaded_ || items_.isEmpty()) reload(false); }
    void leave() override {
        debounce_.stop();
        if (queryRequests_.contains("search")) loaded_ = false;
        more_->setBusy(false);
        RoutePage::leave();
    }
private:
    void reload(bool append) {
        const quint64 context = ++context_; const auto target = target_; QJsonObject scope = target.scope(); scope = withFields(scope, {{"kind", kind_}, {"query", search_->text()}, {"provider", source_->currentData().toString()}, {"gameVersion", game_->text().trimmed()}, {"loader", loader_->currentData().toString()}, {"sort", sort_->currentData().toString()}, {"offset", append ? nextOffset_ : 0}, {"limit", 50}});
        if (!append) { items_ = {}; state_->setCurrentWidget(loading_); } more_->setBusy(true); loading_->setError(false);
        query(UiQuery::CommunitySearch, scope, "search", [this, append, context, target](const UiReply& reply) {
            if (context != context_ || !(target == target_)) return; more_->setBusy(false);
            if (reply.error.isError()) { loading_->setError(true); loading_->setLabel(reply.error.message); issues_->setText(reply.error.message); issues_->show(); if (items_.isEmpty()) state_->setCurrentWidget(loading_); report(reply.error); return; }
            const auto result = reply.data.toObject(); items_ = append ? appendItems(items_, result.value("items").toArray()) : result.value("items").toArray();
            view_->preserveSelection([this] { model_->setItems(items_); }); state_->setCurrentWidget(items_.isEmpty() ? empty_ : view_); loaded_ = true; nextOffset_ = result.value("nextOffset").toInt(); more_->setVisible(result.value("hasMore").toBool());
            issues_->setText(joinJsonStrings(result.value("issues"), "\n")); issues_->setVisible(!issues_->text().isEmpty());
        });
    }
    QString kind_; std::function<void(QJsonObject)> open_; InstanceIdentity target_; LineEdit *search_, *game_; ComboBox *source_, *loader_, *sort_; QLabel *targetLabel_, *issues_; ListModel* model_; ListView* view_; QStackedWidget* state_; LoadingCard* loading_; QWidget* empty_; Button* more_; QJsonArray items_; QTimer debounce_; quint64 context_ = 0; int nextOffset_ = 0; bool loaded_ = false;
};
// Vanilla/loader catalog. Installing is a service task; this page only selects versions and
// submits install commands.
class VanillaPage final : public RoutePage {
public:
    VanillaPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent) : RoutePage(theme, bindings, navigation, parent) {
        auto* column = pageColumn(this, 16); stages_ = new QStackedWidget(this); column->addWidget(stages_, 1);
        auto* catalog = new QWidget(stages_); auto* catalogColumn = pageColumn(catalog, 16); catalogColumn->addWidget(textLabel(theme, "原版游戏", 24));
        auto* latest = new QWidget(catalog); auto* latestRow = rowLayout(latest, 16); release_ = actionButton(theme, "最新正式版", "download", ButtonKind::Primary, latest); snapshot_ = actionButton(theme, "最新快照", "download", ButtonKind::Secondary, latest); release_->setMinimumHeight(68); snapshot_->setMinimumHeight(68); release_->setEnabled(false); snapshot_->setEnabled(false); latestRow->addWidget(release_, 1); latestRow->addWidget(snapshot_, 1); catalogColumn->addWidget(latest);
        auto* filters = new QWidget(catalog); auto* filterRow = rowLayout(filters, 8); search_ = new LineEdit(theme, filters); search_->setPlaceholderText("搜索版本"); search_->setAccessibleName("搜索 Minecraft 版本"); type_ = new ComboBox(theme, filters); type_->addItem("全部版本", ""); type_->addItem("正式版", "release"); type_->addItem("快照", "snapshot"); type_->addItem("远古 Beta", "old_beta"); type_->addItem("远古 Alpha", "old_alpha"); filterRow->addWidget(search_, 1); filterRow->addWidget(type_); catalogColumn->addWidget(filters);
        versions_ = new ListModel(ListKind::Vanilla, this); versionFilter_ = new ListFilter(this); view_ = new ListView(theme, ListKind::Vanilla, catalog); view_->setObjectName("vanillaVersions"); view_->setSource(versions_, versionFilter_); catalogColumn->addWidget(view_, 1); stages_->addWidget(catalog);
        auto* install = new QWidget(stages_); auto* installColumn = pageColumn(install, 16); auto* back = actionButton(theme, "返回版本列表", "back", ButtonKind::Ghost, install); installColumn->addWidget(back, 0, Qt::AlignLeft);
        auto* info = new FormGroup(theme, "安装 Minecraft", {}, install); instanceName_ = new LineEdit(theme, info); instanceName_->setObjectName("installInstanceName"); info->addRow("实例名称", instanceName_); versionTitle_ = textLabel(theme, {}, 12, true); info->body()->addWidget(versionTitle_); installColumn->addWidget(info);
        loaderModel_ = new ListModel(ListKind::Loader, this); loaderView_ = new ListView(theme, ListKind::Loader, install); loaderView_->setObjectName("loaderOptions"); loaderView_->setSource(loaderModel_); installColumn->addWidget(loaderView_, 1);
        auto* tools = new QWidget(install); auto* toolsRow = rowLayout(tools, 8); auto* vanilla = actionButton(theme, "不使用加载器", {}, ButtonKind::Ghost, tools); auto* installer = actionButton(theme, "另存加载器安装器", "download", ButtonKind::Secondary, tools); begin_ = actionButton(theme, "开始安装", "download", ButtonKind::Primary, tools); toolsRow->addWidget(vanilla); toolsRow->addStretch(); toolsRow->addWidget(installer); toolsRow->addWidget(begin_); installColumn->addWidget(tools); stages_->addWidget(install);
        loading_ = new LoadingCard(theme, "正在获取原版版本列表…", stages_); stages_->addWidget(loading_); stages_->setCurrentWidget(loading_);
        connect(search_, &QLineEdit::textChanged, versionFilter_, &ListFilter::setSearch); connect(type_, &QComboBox::activated, this, [this](int) { versionFilter_->setKindFilter(type_->currentData().toString()); });
        connect(view_, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) { selectVersion(index.data(ListModel::ObjectRole).toJsonObject()); }); connect(view_, &QAbstractItemView::activated, this, [this](const QModelIndex& index) { selectVersion(index.data(ListModel::ObjectRole).toJsonObject()); });
        connect(release_, &QPushButton::clicked, this, [this] { for (int row = 0; row < versions_->rowCount(); ++row) if (versions_->object(row).value("id").toString() == latestRelease_) { selectVersion(versions_->object(row)); break; } });
        connect(snapshot_, &QPushButton::clicked, this, [this] { for (int row = 0; row < versions_->rowCount(); ++row) if (versions_->object(row).value("id").toString() == latestSnapshot_) { selectVersion(versions_->object(row)); break; } });
        connect(back, &QPushButton::clicked, this, [this, catalog] { stages_->setCurrentWidget(catalog); }); connect(vanilla, &QPushButton::clicked, this, [this] { selectedLoader_ = {}; loaderView_->clearSelection(); });
        connect(loaderView_, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) { const auto option = index.data(ListModel::ObjectRole).toJsonObject(); if (!option.value("compatible").isBool() || option.value("compatible").toBool()) selectedLoader_ = option; });
        connect(begin_, &QPushButton::clicked, this, [this] { this->install(); }); connect(installer, &QPushButton::clicked, this, [this] { const auto option = loaderView_->currentObject(); if (option.isEmpty()) return; const QString path = saveFile(this, "另存加载器安装器", option.value("kind").toString() + "-installer.jar", "Java archive (*.jar)"); if (!path.isEmpty()) command(UiAction::DownloadProjectFile, {{"kind", "loaderInstaller"}, {"minecraftVersion", selectedVersion_.value("id")}, {"loader", option.value("kind")}, {"loaderVersion", option.value("version")}, {"path", path}}); });
    }
    void setRoot(const QString& root) { root_ = root; begin_->setEnabled(!root.isEmpty()); }
    void enter() override { if (versions_->rowCount() == 0) reload(); }
private:
    void reload() { query(UiQuery::VanillaCatalog, {}, "catalog", [this](const UiReply& reply) { if (reply.error.isError()) { loading_->setError(true); loading_->setLabel(reply.error.message); report(reply.error); return; } const auto state = reply.data.toObject(); versions_->setItems(state.value("items").toArray()); latestRelease_ = state.value("latestRelease").toString(); latestSnapshot_ = state.value("latestSnapshot").toString(); release_->setSubtitle(latestRelease_); snapshot_->setSubtitle(latestSnapshot_); release_->setEnabled(!latestRelease_.isEmpty()); snapshot_->setEnabled(!latestSnapshot_.isEmpty()); stages_->setCurrentIndex(0); }); }
    void selectVersion(const QJsonObject& version) {
        if (version.value("id").toString().isEmpty()) return; selectedVersion_ = version; selectedLoader_ = {}; instanceName_->setText(version.value("id").toString()); versionTitle_->setText("Minecraft " + version.value("id").toString() + " · " + root_); loaderModel_->setItems({}); stages_->setCurrentIndex(1);
        const QString id = version.value("id").toString(); query(UiQuery::LoaderCatalog, {{"minecraftVersion", id}}, "loaders", [this, id](const UiReply& reply) { if (selectedVersion_.value("id").toString() != id) return; if (reply.error.isError()) report(reply.error); else loaderModel_->setItems(reply.data.toObject().value("items").toArray()); });
    }
    void install() {
        if (root_.isEmpty() || instanceName_->text().trimmed().isEmpty() || installing_) return;
        installing_ = true; begin_->setBusy(true); QJsonObject args{{"root", root_}, {"minecraftVersion", selectedVersion_.value("id")}, {"instanceName", instanceName_->text().trimmed()}, {"loader", selectedLoader_.value("kind")}, {"loaderVersion", selectedLoader_.value("version")}, {"operationId", QUuid::createUuid().toString(QUuid::WithoutBraces)}};
        command(UiAction::InstallVersion, args, [this](const UiReply& reply) { installing_ = false; begin_->setBusy(false); if (reply.error.isError()) report(reply.error); });
    }
    QStackedWidget* stages_; LoadingCard* loading_; Button *release_, *snapshot_, *begin_; LineEdit *search_, *instanceName_; ComboBox* type_; QLabel* versionTitle_; ListModel *versions_, *loaderModel_; ListFilter* versionFilter_; ListView *view_, *loaderView_; QJsonObject selectedVersion_, selectedLoader_; QString latestRelease_, latestSnapshot_, root_; bool installing_ = false;
};
// Project detail: safe description, versions and dependencies. The detail stack in Navigation owns
// the back target; remember() stores scroll/filter state before popDetail().
class ProjectPage final : public RoutePage {
public:
    ProjectPage(Theme& theme, Bindings& bindings, Navigation& navigation, DetailFrame frame,
                InstanceIdentity target, std::function<void(QJsonObject, QJsonObject)> dependency, QWidget* parent)
        : RoutePage(theme, bindings, navigation, parent), frame_(std::move(frame)), target_(std::move(target)), dependency_(std::move(dependency)) {
        setObjectName("projectPage");
        auto* column = pageColumn(this, 16);
        auto* back = actionButton(theme, navigation.details().size() > 1 ? "返回上级资源" : "返回检索", "back", ButtonKind::Ghost, this);
        column->addWidget(back, 0, Qt::AlignLeft);
        connect(back, &QPushButton::clicked, this, [this] { remember(); navigation_.popDetail(); });
        auto* info = new Panel(theme, this); auto* infoColumn = info->column(20, 12);
        auto* head = new QWidget(info); auto* headRow = rowLayout(head, 16);
        icon_ = new ImageView(theme, QSize(64, 64), head); icon_->setFallbackIcon("resources"); headRow->addWidget(icon_);
        title_ = textLabel(theme, frame_.projectId, 24); headRow->addWidget(title_, 1);
        auto* external = actionButton(theme, "项目主页", "globe", ButtonKind::Ghost, head); headRow->addWidget(external); infoColumn->addWidget(head);
        description_ = textLabel(theme, {}, 13, true); infoColumn->addWidget(description_);
        body_ = new SafeDescriptionBrowser(theme, bindings.services(), info); body_->setObjectName("projectDescription");
        body_->setMinimumHeight(280); infoColumn->addWidget(body_); column->addWidget(info, 1);
        auto* versions = new Panel(theme, this); auto* versionsColumn = versions->column(20, 12);
        auto* filters = new QWidget(versions); auto* filterRow = rowLayout(filters, 8);
        game_ = new LineEdit(theme, filters); game_->setObjectName("projectGameFilter"); game_->setPlaceholderText("Minecraft 版本");
        game_->setAccessibleName("Minecraft 版本筛选"); game_->setText(frame_.filters.value("gameVersion").toString());
        loader_ = new ComboBox(theme, filters); loader_->setObjectName("projectLoaderFilter"); loader_->setAccessibleName("加载器筛选");
        loader_->addItem("全部加载器", "");
        for (const QString& id : QStringList{"forge", "neoforge", "fabric", "quilt", "liteloader"}) loader_->addItem(id, id);
        loader_->setCurrentIndex(std::max(0, loader_->findData(frame_.filters.value("loader").toString())));
        filterRow->addWidget(game_, 1); filterRow->addWidget(loader_); versionsColumn->addWidget(filters);
        model_ = new ListModel(ListKind::ProjectVersions, this); view_ = new ListView(theme, ListKind::ProjectVersions, versions);
        view_->setObjectName("projectFiles"); view_->setSource(model_); view_->setMinimumHeight(140); versionsColumn->addWidget(view_, 1);
        auto* tools = new QWidget(versions); auto* toolsRow = rowLayout(tools, 8);
        auto* dependencies = actionButton(theme, "必需依赖", "versions", ButtonKind::Ghost, tools);
        auto* save = actionButton(theme, "另存文件", "download", ButtonKind::Secondary, tools);
        install_ = actionButton(theme, frame_.kind == "modpack" ? "安装整合包" : "安装到当前实例", "download", ButtonKind::Primary, tools);
        toolsRow->addWidget(dependencies); toolsRow->addStretch(); toolsRow->addWidget(save); toolsRow->addWidget(install_); versionsColumn->addWidget(tools);
        more_ = actionButton(theme, "加载更多版本", "chevronDown", ButtonKind::Ghost, versions); more_->hide();
        versionsColumn->addWidget(more_, 0, Qt::AlignHCenter); column->addWidget(versions, 1);
        dependencyModel_ = new ListModel(ListKind::Community, this); dependencyView_ = new ListView(theme, ListKind::Community, this);
        dependencyView_->setObjectName("requiredDependencies"); dependencyView_->setSource(dependencyModel_);
        dependencyView_->setMinimumHeight(120); dependencyView_->hide(); column->addWidget(dependencyView_, 1);
        error_ = textLabel(theme, {}, 12, true); error_->hide(); column->addWidget(error_);
        connect(external, &QPushButton::clicked, this, [this] { command(UiAction::OpenExternalUrl, withFields(scope(), {{"projectHomepage", true}})); });
        connect(body_, &SafeDescriptionBrowser::externalLinkRequested, this, [this](const QString& documentKey, const QString& opaqueKey) {
            command(UiAction::OpenExternalUrl, {{"documentKey", documentKey}, {"opaqueKey", opaqueKey}});
        });
        connect(game_, &QLineEdit::editingFinished, this, [this] { reloadVersions(false); });
        connect(loader_, &QComboBox::activated, this, [this](int) { reloadVersions(false); });
        connect(more_, &QPushButton::clicked, this, [this] { reloadVersions(true); });
        connect(dependencies, &QPushButton::clicked, this, [this] { loadDependencies(); });
        connect(save, &QPushButton::clicked, this, [this] { installFile(true); });
        connect(install_, &QPushButton::clicked, this, [this] { installFile(false); });
        connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
            invalidate("dependencies"); dependencyModel_->setItems({}); dependencyView_->hide();
        });
        connect(dependencyView_, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) {
            remember(); dependency_(index.data(ListModel::ObjectRole).toJsonObject(), view_->currentObject());
        });
        connect(dependencyView_, &QAbstractItemView::activated, this, [this](const QModelIndex& index) {
            remember(); dependency_(index.data(ListModel::ObjectRole).toJsonObject(), view_->currentObject());
        });
        connect(&bindings.services(), &UiServices::mediaReady, this, [this](const QString& key, const QImage& image) {
            if (!key.isEmpty() && key == project_.value("iconKey").toString()) icon_->setImage(image);
        }, Qt::QueuedConnection);
        install_->setEnabled(target_.valid() || frame_.kind == "modpack");
    }
    void enter() override {
        query(UiQuery::ProjectDetail, scope(), "project", [this](const UiReply& reply) {
            if (reply.error.isError()) { reportError(reply.error); return; }
            project_ = reply.data.toObject(); title_->setText(project_.value("title").toString());
            description_->setText(project_.value("description").toString()); body_->setBody(project_.value("body").toObject());
            QTimer::singleShot(0, body_, [this] { body_->verticalScrollBar()->setValue(frame_.descriptionScroll); });
        });
        reloadVersions(false);
    }
    void leave() override { remember(); RoutePage::leave(); body_->releaseBody(); }
    void remember() {
        if (auto* frame = navigation_.detail(frame_.key)) {
            frame->filters = {{"gameVersion", game_->text().trimmed()}, {"loader", loader_->currentData().toString()}};
            frame->descriptionScroll = body_->verticalScrollBar()->value();
            frame->versionsScroll = view_->verticalScrollBar()->value();
            frame->selectedVersionId = view_->currentObject().value("id").toString();
            frame->openGroups = dependencyView_->isHidden() ? QStringList{} : QStringList{"dependencies"};
        }
    }
private:
    QJsonObject scope() const {
        return withFields(target_.scope(), {{"provider", frame_.provider}, {"projectId", frame_.projectId}, {"kind", frame_.kind}});
    }
    void reportError(const UiError& error) { error_->setText(error.message); error_->show(); report(error); }
    void reloadVersions(bool append) {
        more_->setBusy(true);
        const QJsonObject filters{{"gameVersion", game_->text().trimmed()}, {"loader", loader_->currentData().toString()}};
        query(UiQuery::ProjectVersions, withFields(scope(), withFields(filters, {{"offset", append ? nextOffset_ : 0}, {"limit", 100}, {"dependency", frame_.dependency}})), "versions", [this, append](const UiReply& reply) {
            more_->setBusy(false);
            if (reply.error.isError()) { reportError(reply.error); return; }
            const auto result = reply.data.toObject();
            items_ = append ? appendItems(items_, result.value("items").toArray()) : result.value("items").toArray();
            view_->preserveSelection([this] { model_->setItems(items_); });
            if (!restoredVersions_) {
                for (int row = 0; row < model_->rowCount(); ++row)
                    if (model_->object(row).value("id").toString() == frame_.selectedVersionId) { view_->setCurrentIndex(model_->index(row)); break; }
                view_->verticalScrollBar()->setValue(frame_.versionsScroll); restoredVersions_ = true;
                if (frame_.openGroups.contains("dependencies")) loadDependencies();
            }
            more_->setVisible(result.value("hasMore").toBool()); nextOffset_ = result.value("nextOffset").toInt();
        });
    }
    void loadDependencies() {
        const auto version = view_->currentObject(); if (version.isEmpty()) return;
        const QString id = version.value("id").toString();
        query(UiQuery::RequiredDependencies, withFields(scope(), {{"projectVersionId", id}}), "dependencies", [this, id](const UiReply& reply) {
            if (view_->currentObject().value("id").toString() != id) return;
            if (reply.error.isError()) reportError(reply.error);
            else { dependencyModel_->setItems(reply.data.toObject().value("items").toArray()); dependencyView_->setVisible(dependencyModel_->rowCount() > 0); }
        });
    }
    void installFile(bool saveAs) {
        const auto version = view_->currentObject(); if (version.isEmpty() || submitting_) return;
        QJsonObject args = withFields(scope(), {{"projectVersionId", version.value("id")}, {"dependency", frame_.dependency}});
        if (saveAs) {
            QString fileName;
            for (const auto& file : version.value("files").toArray()) {
                const auto object = file.toObject(); if (fileName.isEmpty() || object.value("primary").toBool()) fileName = object.value("name").toString();
            }
            const QString path = saveFile(this, "另存资源文件", fileName, "所有文件 (*)");
            if (!path.isEmpty()) command(UiAction::DownloadProjectFile, withFields(args, {{"path", path}}));
            return;
        }
        if (frame_.kind == "modpack") {
            promptText(theme_, this, "安装整合包", "新实例名称", title_->text(), [this, args](const QString& name, Dialog* dialog) {
                dialog->accept(); submit(withFields(args, {{"instanceName", name}}));
            });
        } else if (target_.valid()) submit(args);
    }
    void submit(const QJsonObject& args) {
        if (submitting_) return; submitting_ = true; install_->setBusy(true);
        command(UiAction::InstallProject, args, [this](const UiReply& reply) {
            submitting_ = false; install_->setBusy(false); if (reply.error.isError()) reportError(reply.error);
        });
    }
    DetailFrame frame_; InstanceIdentity target_; std::function<void(QJsonObject, QJsonObject)> dependency_;
    QJsonObject project_; QJsonArray items_; QLabel *title_, *description_, *error_; ImageView* icon_;
    SafeDescriptionBrowser* body_; LineEdit* game_; ComboBox* loader_; ListModel *model_, *dependencyModel_;
    ListView *view_, *dependencyView_; Button *more_, *install_; int nextOffset_ = 0;
    bool submitting_ = false, restoredVersions_ = false;
};
// Download-center host: swaps between the vanilla page and per-kind search pages and mirrors the
// navigation detail stack. It does not cache project data itself.
class ResourcesPage final : public RoutePage {
public:
    ResourcesPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent)
        : RoutePage(theme, bindings, navigation, parent) {
        setObjectName("resourcesPage"); auto* column = pageColumn(this, 0); stack_ = new QStackedWidget(this); column->addWidget(stack_);
        pager_ = new Pager(theme, "下载中心", {{"vanilla", "原版游戏", "play", "原版游戏"}, {"mod", "Mod", "mods", "社区资源"},
            {"modpack", "整合包", "bundle", "社区资源"}, {"datapack", "数据包", "resources", "社区资源"},
            {"resourcepack", "资源包", "pack", "社区资源"}, {"shader", "光影包", "shader", "社区资源"}}, stack_);
        stack_->addWidget(pager_); vanilla_ = new VanillaPage(theme, bindings, navigation, pager_); pager_->addPage("vanilla", vanilla_, false);
        for (const auto& entry : QList<QPair<QString, QString>>{{"mod", "Mod"}, {"modpack", "整合包"}, {"datapack", "数据包"}, {"resourcepack", "资源包"}, {"shader", "光影包"}}) {
            auto* search = new SearchPage(theme, bindings, navigation, entry.first, entry.second, [this](QJsonObject hit) { openProject(hit, {}); }, pager_);
            searches_.insert(entry.first, search); pager_->addPage(entry.first, search, false);
            connect(search, &RoutePage::errorReported, this, &RoutePage::errorReported);
        }
        connect(vanilla_, &RoutePage::errorReported, this, &RoutePage::errorReported);
        connect(pager_, &Pager::sectionChanged, this, [this](const QString& id) {
            navigation_.memory(Route::Resources).section = id;
            if (active_ && !navigation_.currentDetail()) activateSection(id);
        });
        connect(&navigation, &Navigation::detailChanged, this, [this] { synchronizeDetails(); });
        connect(&navigation, &Navigation::resourceTargetChanged, this, [this] { setTarget(navigation_.resourceTarget()); pager_->select("mod"); });
    }
    ~ResourcesPage() override { suspendDetail(); }
    void enter() override {
        active_ = true;
        query(UiQuery::Bootstrap, {}, "target", [this](const UiReply& reply) {
            if (reply.error.isError()) { report(reply.error); return; }
            const auto state = reply.data.toObject(); const auto selected = state.value("selectedInstance").toObject();
            setTarget(navigation_.resourceTarget().valid() ? navigation_.resourceTarget()
                : InstanceIdentity{selected.value("root").toString(), selected.value("versionId").toString()});
            vanilla_->setRoot(state.value("roots").toObject().value("active").toString());
        });
        const QString section = navigation_.memory(Route::Resources).section;
        if (!section.isEmpty()) pager_->select(section);
        synchronizeDetails();
    }
    void leave() override {
        active_ = false; RoutePage::leave();
        if (activeSection_) activeSection_->leave(); activeSection_ = nullptr;
        suspendDetail();
    }
    void snapshot(UiTopic topic, const QJsonObject& state) override {
        if (topic == UiTopic::Instances && state.contains("selectedInstance") && !navigation_.resourceTarget().valid()) {
            const auto selected = state.value("selectedInstance").toObject(); setTarget({selected.value("root").toString(), selected.value("versionId").toString()});
        }
    }
private:
    void setTarget(const InstanceIdentity& target) {
        if (target == target_) return;
        suspendDetail(); target_ = target;
        for (auto* search : searches_) search->setTarget(target);
        if (active_) synchronizeDetails();
    }
    void activateSection(const QString& section) {
        RoutePage* next = section == "vanilla" ? static_cast<RoutePage*>(vanilla_) : searches_.value(section);
        if (activeSection_ && activeSection_ != next) activeSection_->leave();
        activeSection_ = next;
        if (activeSection_) activeSection_->enter();
    }
    void openProject(const QJsonObject& hit, const QJsonObject& dependency) {
        if (detailPage_) detailPage_->remember();
        DetailFrame frame; frame.key = QUuid::createUuid().toString(QUuid::WithoutBraces);
        frame.provider = hit.value("provider").toString(); frame.projectId = hit.value("projectId").toString();
        if (frame.projectId.isEmpty()) frame.projectId = hit.value("id").toString();
        frame.kind = hit.value("kind").toString(); if (frame.kind.isEmpty()) frame.kind = pager_->selected();
        frame.dependency = dependency; navigation_.pushDetail(std::move(frame));
    }
    void suspendDetail() {
        if (!detailPage_) return;
        if (auto* frame = navigation_.detail(detailKey_)) frame->scroll = detailHost_->verticalScrollBar()->value();
        auto* host = detailHost_; auto* page = detailPage_;
        detailPage_ = nullptr; detailHost_ = nullptr; detailKey_.clear();
        page->leave(); stack_->removeWidget(host); host->hide();
        // A navigation signal may originate inside this host's clicked/activated handler.
        host->deleteLater();
    }
    void synchronizeDetails() {
        if (!active_) { suspendDetail(); return; }
        const auto* current = navigation_.currentDetail();
        if (current && detailPage_ && detailKey_ == current->key) return;
        suspendDetail();
        current = navigation_.currentDetail();
        if (!current) { stack_->setCurrentWidget(pager_); activateSection(pager_->selected()); return; }
        if (activeSection_) activeSection_->leave(); activeSection_ = nullptr;
        const DetailFrame frame = *current;
        detailKey_ = frame.key;
        detailPage_ = new ProjectPage(theme_, bindings_, navigation_, frame, target_, [this](QJsonObject hit, QJsonObject parentVersion) {
            const auto required = hit.value("requiredVersion").toObject();
            openProject(hit.value("project").isObject() ? hit.value("project").toObject() : hit,
                {{"parentVersion", parentVersion}, {"requiredVersion", required}});
        }, stack_);
        detailHost_ = new ScrollArea(theme_, stack_); detailHost_->setObjectName("projectDetailHost"); detailHost_->setWidget(detailPage_);
        stack_->addWidget(detailHost_); stack_->setCurrentWidget(detailHost_);
        connect(detailPage_, &RoutePage::errorReported, this, &RoutePage::errorReported);
        if (frame.scroll > 0) {
            auto* bar = detailHost_->verticalScrollBar();
            connect(bar, &QScrollBar::rangeChanged, detailHost_, [bar, desired = frame.scroll, restored = false](int, int maximum) mutable {
                if (!restored && maximum >= desired) { bar->setValue(desired); restored = true; }
            });
        }
        detailPage_->enter();
    }
    QStackedWidget* stack_; Pager* pager_; VanillaPage* vanilla_; QHash<QString, SearchPage*> searches_;
    ProjectPage* detailPage_ = nullptr; ScrollArea* detailHost_ = nullptr; QString detailKey_;
    RoutePage* activeSection_ = nullptr; InstanceIdentity target_; bool active_ = false;
};
}
RoutePage* createResourcesPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent) { return new ResourcesPage(theme, bindings, navigation, parent); }
} // namespace nickel::ui
