#include "Pages.hpp"
#include "Dialogs.hpp"
#include "ListModel.hpp"
#include "ProductIdentity.hpp"

#include <QCoreApplication>
#include <QGridLayout>
#include <QJsonArray>
#include <QPainter>
#include <QPixmap>
#include <QSignalBlocker>

namespace nickel::ui {
namespace {
const QString projectUrl = QStringLiteral("https://github.com/Nickel-Studio/Pd-Launcher");
class BrandMark final : public QWidget {
public:
    BrandMark(Theme& theme, QWidget* parent) : QWidget(parent), theme_(theme) {
        setObjectName("aboutBrandMark"); setFixedSize(64, 64);
        setAccessibleName(productDisplayName() + " 应用徽标");
        connect(&theme_, &Theme::changed, this, qOverload<>(&QWidget::update));
    }
protected:
    void paintEvent(QPaintEvent*) override {
        const bool dark = theme_.tokens().dark;
        const qreal ratio = devicePixelRatioF();
        if (mark_.isNull() || cachedDark_ != dark || cachedRatio_ != ratio) {
            const QPixmap source(dark ? QStringLiteral(":/nickel/brand/mark-white.png") : QStringLiteral(":/nickel/brand/mark-black.png"));
            mark_ = source.scaled((QSizeF(size()) * ratio).toSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            mark_.setDevicePixelRatio(ratio); cachedDark_ = dark; cachedRatio_ = ratio;
        }
        if (mark_.isNull()) return;
        const QSizeF extent = mark_.deviceIndependentSize();
        const QPointF origin((width() - extent.width()) / 2, (height() - extent.height()) / 2);
        QPainter painter(this); painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(origin, mark_);
    }
private:
    Theme& theme_;
    QPixmap mark_;
    bool cachedDark_ = false;
    qreal cachedRatio_ = 0;
};
// Guide, frontier switches and credits. Frontier toggles persist through app settings; cleanup
// requires a service preview and confirmation, and the page never deletes files itself.
class MorePage final : public RoutePage {
public:
    MorePage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent)
        : RoutePage(theme, bindings, navigation, parent) {
        setObjectName("morePage"); auto* column = pageColumn(this, 12); error_ = textLabel(theme, {}, 12, true); error_->hide(); column->addWidget(error_);
        pager_ = new Pager(theme, "更多", {{"guide", "指南", "globe", {}}, {"frontier", "前沿计划", "spark", {}}, {"about", "关于", "versions", {}}}, this); column->addWidget(pager_, 1);
        buildGuide(); buildFrontier(); buildAbout(); connect(pager_, &Pager::sectionChanged, this, [this](const QString& id) { navigation_.memory(Route::More).section = id; });
    }
    void enter() override {
        query(UiQuery::Settings, {{"section", "app"}}, "settings", [this](const UiReply& reply) { if (reply.error.isError()) reportError(reply.error); else { const auto state = reply.data.toObject(); apply(state.value("values").isObject() ? state.value("values").toObject() : state); } });
    }
    void snapshot(UiTopic topic, const QJsonObject& state) override { if (topic == UiTopic::Settings && state.value("section").toString() != "game") { invalidate("settings"); apply(state.value("values").isObject() ? state.value("values").toObject() : state); } }
private:
    void guideCard(QVBoxLayout* column, const QString& title, const QString& subtitle, const QList<QPair<QString, QString>>& items, const QString& actionText, Route route) {
        auto* group = new FormGroup(theme_, title, subtitle); column->addWidget(group);
        for (const auto& item : items) group->addRow(item.first, textLabel(theme_, item.second, 13, true), {}, false);
        auto* button = actionButton(theme_, actionText, "back", ButtonKind::Ghost, group); group->body()->addWidget(button, 0, Qt::AlignRight); connect(button, &QPushButton::clicked, this, [this, route] { navigation_.navigate(route); });
    }
    void buildGuide() {
        auto* page = new QWidget; auto* column = pageColumn(page, 16);
        guideCard(column, "账号", "三种账号形态共用首页顶部入口，登录与角色操作都在首页卡片内", {{"正版（Microsoft）", "在首页切到正版，使用一次性设备码完成微软授权。"}, {"皮肤站", "服务器地址在设置中确认；有多个角色时在弹窗中选择。"}, {"离线", "在账号与隐私中填写离线用户名，再在首页切到离线。"}, {"切换账号", "在首页账号管理中选择已保存账号。"}}, "回到首页登录", Route::Home);
        guideCard(column, "实例与版本", "目录、版本与启动选择集中在实例页", {{"管理目录", "在左侧新建、添加或导入 .minecraft 目录。"}, {"选择启动实例", "点击整张实例卡选中版本，随后返回首页。"}, {"收藏与更多", "卡片右下角提供收藏、删除实例和版本设置。删除需要确认。"}, {"内存上限", "全局设置提供默认值，版本设置可为当前实例覆盖。"}}, "打开实例管理", Route::Versions);
        guideCard(column, "下载与来源", "游戏与社区资源在资源页；来源与限速在设置中", {{"原版与加载器", "选择 Minecraft 版本与加载器后开始安装。"}, {"社区资源", "资源可安装到当前实例；整合包输入名称后创建独立实例。"}, {"来源策略", "版本列表源、文件下载源与社区资源源互相独立。"}, {"下载很慢", "检查文件下载来源、线程数与速度限制。"}}, "打开资源页", Route::Resources);
        guideCard(column, "外观与排障", "外观与账号入口保持原有分区", {{"明暗与颜色", "明暗模式决定亮暗，颜色主题决定按钮、选中态与焦点环。"}, {"背景", "选择本地图片，调整真实模糊与遮罩浓度。"}, {"启动按钮是灰的", "先选择实例，再登录账号或填写离线用户名。"}, {"邮箱与头像", "隐私模式在账号与隐私中调整，角色与皮肤入口在首页。"}}, "打开设置", Route::Settings);
        column->addStretch(); pager_->addPage("guide", page);
    }
    void buildFrontier() {
        auto* page = new QWidget; auto* column = pageColumn(page, 16); auto* notice = new Panel(theme_, page); notice->column(24, 12)->addWidget(textLabel(theme_, "本页属于前沿计划 (Frontier Project)：功能仍在试验阶段，行为可能在后续版本调整。", 13, true)); column->addWidget(notice);
        auto* library = new FormGroup(theme_, "星月图书馆", {}, page); auto* clean = actionButton(theme_, "清理游戏垃圾", "trash", ButtonKind::Danger, library); library->body()->addWidget(clean, 0, Qt::AlignLeft); column->addWidget(library); connect(clean, &QPushButton::clicked, this, [this] { previewJunk(); });
        auto* ui = new FormGroup(theme_, "用户界面项", {}, page); titleTint_ = new CheckBox(theme_, {}, ui, true); titleTint_->setEnabled(false); ui->addRow("顶栏颜色小改", titleTint_, "让半透明顶栏轻微跟随主题色。" ); column->addWidget(ui);
        auto* experience = new FormGroup(theme_, "操作体验项", "可能改变默认操作习惯的试验性改进", page); naming_ = new CheckBox(theme_, {}, experience, true); naming_->setEnabled(false); experience->addRow("实例取名优化", naming_, "添加孤立 .minecraft 时，用上级文件夹名作为显示名。"); column->addWidget(experience);
        connect(titleTint_, &QCheckBox::toggled, this, [this](bool enabled) { if (!applying_) command(UiAction::PatchSettings, {{"section", "app"}, {"patch", QJsonObject{{"titlebarTint", enabled}}}}); });
        connect(naming_, &QCheckBox::toggled, this, [this](bool enabled) { if (!applying_) command(UiAction::PatchSettings, {{"section", "app"}, {"patch", QJsonObject{{"instanceNamingOptimize", enabled}}}}); }); column->addStretch(); pager_->addPage("frontier", page);
    }
    void creditGrid(QVBoxLayout* column, const QString& title, const QList<QJsonObject>& credits) {
        auto* section = new QWidget; section->setObjectName("aboutCreditsSection");
        auto* sectionColumn = pageColumn(section, 12);
        sectionColumn->addWidget(textLabel(theme_, title, 13, true));
        auto* host = new QWidget(section); auto* grid = new QGridLayout(host);
        grid->setContentsMargins(0, 0, 0, 0); grid->setSpacing(12); int index = 0;
        for (const auto& credit : credits) {
            auto* card = new Panel(theme_, host, SurfaceKind::Fill);
            card->setObjectName("aboutCredit-" + credit.value("id").toString());
            card->setAccessibleName(credit.value("name").toString()); card->setShadow(0);
            auto* row = new QHBoxLayout(card); row->setContentsMargins(16, 12, 16, 12); row->setSpacing(16);
            auto* avatar = new ImageView(theme_, QSize(44, 44), card);
            avatar->setResource("credits/" + credit.value("id").toString() + ".png"); row->addWidget(avatar);
            auto* labels = new QWidget(card); auto* labelsColumn = pageColumn(labels, 0);
            labelsColumn->addWidget(textLabel(theme_, credit.value("name").toString(), 15));
            auto* link = actionButton(theme_, "GitHub 主页", {}, ButtonKind::Ghost, labels);
            link->setFont(bodyFont(12, QFont::DemiBold)); link->setFixedHeight(28);
            labelsColumn->addWidget(link, 0, Qt::AlignLeft); row->addWidget(labels, 1);
            grid->addWidget(card, index / 2, index % 2); ++index;
            connect(link, &QPushButton::clicked, this, [this, url = credit.value("url").toString()] { openExternal(url); });
        }
        sectionColumn->addWidget(host); column->addWidget(section);
    }
    void buildAbout() {
        auto* page = new QWidget; page->setObjectName("aboutPage"); auto* column = pageColumn(page, 24);
        auto* intro = new Panel(theme_, page); intro->setObjectName("aboutIntro");
        auto* introColumn = intro->column(24, 20);
        auto* identity = new QWidget(intro); identity->setObjectName("aboutIdentity");
        auto* row = rowLayout(identity, 20); row->addWidget(new BrandMark(theme_, identity));
        auto* labels = new QWidget(identity); auto* labelsColumn = pageColumn(labels, 8);
        auto* name = textLabel(theme_, productDisplayName(), 31); name->setObjectName("aboutProductName");
        labelsColumn->addWidget(name);
        labelsColumn->addWidget(textLabel(theme_, "跨平台的 Minecraft Java 版启动器。", 13, true)); row->addWidget(labels, 1);
        auto* version = textLabel(theme_, QCoreApplication::applicationVersion().isEmpty() ? "版本信息待载入" : "v" + QCoreApplication::applicationVersion(), 12, true);
        version->setObjectName("aboutVersion"); version->setFont(monoFont(12)); row->addWidget(version, 0, Qt::AlignTop);
        introColumn->addWidget(identity);
        auto* links = new QWidget(intro); auto* linksRow = rowLayout(links, 16);
        auto* project = actionButton(theme_, "项目主页", {}, ButtonKind::Secondary, links);
        project->setObjectName("aboutProjectLink"); project->setFont(bodyFont(12, QFont::DemiBold)); project->setFixedHeight(28);
        linksRow->addWidget(project); linksRow->addStretch();
        connect(project, &QPushButton::clicked, this, [this] { openExternal(projectUrl); });
        for (const auto& link : QList<QPair<QString, QString>>{{"AGPL-3.0-only", projectUrl + "/blob/main/LICENSE"}, {"Artwork terms", projectUrl + "/blob/main/LICENSE-ASSETS"}}) {
            auto* button = actionButton(theme_, link.first, {}, ButtonKind::Ghost, links);
            button->setFont(monoFont(12)); button->setFixedHeight(28); linksRow->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, url = link.second] { openExternal(url); });
        }
        introColumn->addWidget(links); column->addWidget(intro);
        creditGrid(column, "开发者", {{{"id", "itzarona"}, {"name", "ItzArona"}, {"url", "https://github.com/ItzArona"}}, {{"id", "itzplana"}, {"name", "ItzPlana"}, {"url", "https://github.com/ItzPlana"}}});
        creditGrid(column, "特别鸣谢", {{{"id", "shittimchests"}, {"name", "ShittimChests 组织"}, {"url", "https://github.com/ShittimChests"}}});
        column->addStretch(); pager_->addPage("about", page);
    }
    void openExternal(const QString& url) { command(UiAction::OpenExternalUrl, {{"url", url}}, [this](const UiReply& reply) { if (reply.error.isError()) reportError(reply.error); }); }
    void apply(const QJsonObject& state) { applying_ = true; titleTint_->setEnabled(true); naming_->setEnabled(true); QSignalBlocker a(titleTint_), b(naming_); titleTint_->setChecked(state.value("titlebarTint").toBool()); naming_->setChecked(state.value("instanceNamingOptimize").toBool()); applying_ = false; }
    void previewJunk() {
        if (junkBusy_) return; junkBusy_ = true;
        query(UiQuery::JunkPreview, {}, "junk", [this](const UiReply& reply) { junkBusy_ = false; if (reply.error.isError()) { reportError(reply.error); return; } const auto preview = reply.data.toObject(); const QString previewId = preview.value("previewId").toString(); if (previewId.isEmpty()) { reportError({"junk.preview", "垃圾扫描缺少复核标识", {}, true}); return; }
            auto* dialog = new Dialog(theme_, "清理游戏垃圾", this); dialog->setConfirmText("清理"); dialog->body()->addWidget(textLabel(theme_, "找到 " + QString::number(preview.value("fileCount").toInteger()) + " 个日志、崩溃报告或临时文件，约 " + formatBytes(preview.value("totalBytes").toDouble()) + "。")); dialog->body()->addWidget(textLabel(theme_, "不会清理游戏本体、Mod、资源包和存档；服务会在清理前重新验证预览。", 12, true));
            connect(dialog, &Dialog::confirmed, dialog, [this, dialog, previewId] { dialog->setBusy(true); QPointer<Dialog> owner(dialog); command(UiAction::CleanJunk, {{"previewId", previewId}}, [this, owner](const UiReply& outcome) { if (!owner) return; if (outcome.error.isError()) owner->showError(outcome.error); else { owner->accept(); error_->setText("清理完成，腾出了 " + formatBytes(outcome.data.toObject().value("freedBytes").toDouble())); error_->show(); } }); }); dialog->open();
        });
    }
    void reportError(const UiError& error) { error_->setText(error.message); error_->show(); report(error); }
    Pager* pager_; QLabel* error_; CheckBox *titleTint_, *naming_; bool applying_ = false, junkBusy_ = false;
};
}
RoutePage* createMorePage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent) { return new MorePage(theme, bindings, navigation, parent); }
} // namespace nickel::ui
