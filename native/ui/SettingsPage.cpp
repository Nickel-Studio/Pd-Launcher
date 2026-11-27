#include "Pages.hpp"
#include "Dialogs.hpp"
#include "GameForm.hpp"
#include "ListModel.hpp"

#include <QButtonGroup>
#include <QGridLayout>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <cmath>

namespace nickel::ui {
namespace {
// App-level settings. Edits accumulate as a debounced patch of changed keys and are applied by the
// service; widgets are refreshed only from authoritative snapshots, never from local drafts.
class SettingsPage final : public RoutePage {
public:
    SettingsPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent)
        : RoutePage(theme, bindings, navigation, parent) {
        setObjectName("settingsPage"); auto* column = pageColumn(this, 12); error_ = textLabel(theme, {}, 12, true); error_->hide(); column->addWidget(error_);
        pager_ = new Pager(theme, "设置", {{"appearance", "外观", "palette", {}}, {"custom-theme", "自定义", "palette", {}}, {"account", "账号与隐私", "shield", {}}, {"download", "下载", "download", {}}, {"game", "游戏", "play", {}}}, this); column->addWidget(pager_, 1);
        buildAppearance(); buildCustom(); buildAccount(); buildDownload(); game_ = new GameForm(theme, false); pager_->addPage("game", game_);
        pager_->setItemVisible("custom-theme", false); pager_->setEnabled(false);
        connect(pager_, &Pager::sectionChanged, this, [this](const QString& id) { navigation_.memory(Route::Settings).section = id; if (id == "game") reloadGame(); });
        connect(game_, &GameForm::patchRequested, this, [this](const QJsonObject& patch) { command(UiAction::PatchSettings, {{"section", "game"}, {"patch", patch}}, [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); }); });
        connect(game_, &GameForm::javaRefreshRequested, this, [this] { reloadJava(); });
        connect(game_, &GameForm::offlineSkinRequested, this, [this](const QJsonObject& args) { command(args.value("reset").toBool() ? UiAction::ResetOfflineSkin : UiAction::UpdateOfflineSkin, args, [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else reloadGame(); }); });
        connect(&bindings.services(), &UiServices::mediaReady, this, [this](const QString& key, const QImage& image) { if (key == settings_.value("backgroundKey").toString()) background_->setImage(image); }, Qt::QueuedConnection);
        saveTimer_.setSingleShot(true); saveTimer_.setInterval(180); connect(&saveTimer_, &QTimer::timeout, this, [this] { saveDraft(); });
    }
    void enter() override { reload(); if (pager_->selected() == "game") reloadGame(); }
    void leave() override { saveTimer_.stop(); if (!pending_.isEmpty()) saveDraft(); RoutePage::leave(); }
    void snapshot(UiTopic topic, const QJsonObject& state) override {
        if (topic != UiTopic::Settings) return;
        if (state.value("section").toString() == "game") { game_->setAvailable(true); game_->setValues(state.value("values").toObject()); }
        else { invalidate("settings"); apply(state.value("values").isObject() ? state.value("values").toObject() : state); }
    }
private:
    LineEdit* textField(FormGroup* group, const QString& key, const QString& label, const QString& description = {}) {
        auto* field = new LineEdit(theme_, group); field->setObjectName(key); editors_.insert(key, field); group->addRow(label, field, description, true);
        connect(field, &QLineEdit::editingFinished, this, [this, key, field] { patch({{key, field->text()}}); }); return field;
    }
    ComboBox* choice(FormGroup* group, const QString& key, const QString& label, const QList<QPair<QString, QString>>& options) {
        auto* field = new ComboBox(theme_, group); for (const auto& option : options) field->addItem(option.second, option.first); field->setObjectName(key); editors_.insert(key, field); group->addRow(label, field);
        connect(field, &QComboBox::activated, this, [this, key, field](int) { patch({{key, field->currentData().toString()}}); }); return field;
    }
    CheckBox* toggle(FormGroup* group, const QString& key, const QString& label, const QString& description = {}) {
        auto* field = new CheckBox(theme_, {}, group, true); field->setObjectName(key); editors_.insert(key, field); group->addRow(label, field, description);
        connect(field, &QCheckBox::toggled, this, [this, key](bool checked) { patch({{key, checked}}); }); return field;
    }
    Slider* slider(FormGroup* group, const QString& key, const QString& label, int min, int max, double scale = 1) {
        auto* host = new QWidget(group); auto* column = pageColumn(host, 2); auto* field = new Slider(theme_, host); field->setRange(min, max); field->setObjectName(key); field->setAccessibleName(label); auto* value = textLabel(theme_, {}, 11, true, host);
        column->addWidget(field); column->addWidget(value); editors_.insert(key, field); scales_.insert(key, scale); group->addRow(label, host);
        connect(field, &QSlider::valueChanged, value, [value, scale](int n) { value->setText(QString::number(n / scale, 'g', 5)); });
        connect(field, &QSlider::valueChanged, this, [this, key, scale](int n) { patch({{key, n / scale}}); }); return field;
    }
    void buildAppearance() {
        auto* page = new QWidget; auto* column = pageColumn(page, 16); auto* group = new FormGroup(theme_, "外观", "颜色主题与明暗模式独立选择", page); column->addWidget(group);
        choice(group, "themeMode", "明暗模式", {{"system", "跟随系统"}, {"light", "亮色"}, {"dark", "暗色"}});
        auto* accents = new QWidget(group); auto* grid = new QGridLayout(accents); grid->setContentsMargins(0, 0, 0, 0); grid->setHorizontalSpacing(8); grid->setVerticalSpacing(8); auto* exclusive = new QButtonGroup(accents);
        int index = 0; for (const auto& accent : accentOptions()) { auto* button = new RadioButton(theme_, QString::fromUtf8(accent.label), accents); button->setSwatch(QColor(accent.color)); button->setObjectName("accent-" + QString::fromLatin1(accent.key)); exclusive->addButton(button); accents_.insert(QString::fromLatin1(accent.key), button); grid->addWidget(button, index / 3, index % 3); ++index; connect(button, &QRadioButton::clicked, this, [this, key = QString::fromLatin1(accent.key)] { patch({{"accent", key}}); }); }
        group->addRow("颜色主题", accents, {}, true);
        auto* custom = actionButton(theme_, "调整自定义主题", "palette", ButtonKind::Ghost, group); group->body()->addWidget(custom, 0, Qt::AlignRight); connect(custom, &QPushButton::clicked, this, [this] { patch({{"accent", "custom"}}); pager_->setItemVisible("custom-theme", true); pager_->select("custom-theme"); });
        auto* bg = new QWidget(group); auto* bgRow = rowLayout(bg, 12); background_ = new ImageView(theme_, QSize(160, 90), bg); background_->setObjectName("backgroundPreview"); background_->setFallbackIcon("image"); bgRow->addWidget(background_); auto* bgTools = new QWidget(bg); auto* bgColumn = pageColumn(bgTools, 8); auto* pick = actionButton(theme_, "选择图片", "image", ButtonKind::Primary, bgTools); auto* clear = actionButton(theme_, "清除背景", {}, ButtonKind::Secondary, bgTools); bgColumn->addWidget(pick); bgColumn->addWidget(clear); bgRow->addWidget(bgTools, 1); group->addRow("背景图片", bg, "用户背景不会叠加主题背景装饰", true);
        connect(pick, &QPushButton::clicked, this, [this] { const auto files = pickFiles(this, "选择背景图片", "图片 (*.png *.jpg *.jpeg *.webp *.bmp)"); if (!files.isEmpty()) patch({{"backgroundImage", files.first()}}); });
        connect(clear, &QPushButton::clicked, this, [this] { patch({{"backgroundImage", QJsonValue(QJsonValue::Null)}}); background_->setImage({}); });
        slider(group, "backgroundBlur", "背景模糊度", 0, 40); slider(group, "backgroundOpacity", "背景遮罩浓度", 0, 100, 100);
        column->addStretch(); pager_->addPage("appearance", page);
    }
    void buildCustom() {
        auto* page = new QWidget; auto* column = pageColumn(page, 16); auto* group = new FormGroup(theme_, "自定义主题", {}, page); column->addWidget(group);
        for (const auto& spec : QList<QPair<QString, QString>>{{"hue", "色相"}, {"saturation", "饱和度"}, {"lightness", "明度"}, {"titlebarHueShift", "顶栏色相偏移"}, {"decorationOpacity", "装饰强度"}}) {
            auto* field = new Slider(theme_, group); field->setObjectName("custom-" + spec.first); field->setRange(spec.first == "titlebarHueShift" ? -180 : 0, spec.first == "hue" ? 360 : spec.first == "titlebarHueShift" ? 180 : 100); custom_.insert(spec.first, field); group->addRow(spec.second, field);
            connect(field, &QSlider::valueChanged, this, [this, key = spec.first](int value) {
                patch({{"customTheme", QJsonObject{{key, key == "decorationOpacity" ? value / 100. : value}}}});
            });
        }
        decoration_ = new ComboBox(theme_, group); for (const auto& option : QList<QPair<QString, QString>>{{"none", "无装饰"}, {"lattice", "晶格"}, {"ribbons", "流带"}, {"orbits", "环轨"}, {"folds", "折纸"}}) decoration_->addItem(option.second, option.first); group->addRow("原创装饰", decoration_);
        connect(decoration_, &QComboBox::activated, this, [this](int) { patch({{"customTheme", QJsonObject{{"decoration", decoration_->currentData().toString()}}}}); });
        auto* reset = actionButton(theme_, "重置自定义参数", "refresh", ButtonKind::Secondary, group); group->body()->addWidget(reset, 0, Qt::AlignRight); connect(reset, &QPushButton::clicked, this, [this] { patch({{"customTheme", QJsonObject{{"hue", 174}, {"saturation", 39}, {"lightness", 44}, {"titlebarHueShift", 0}, {"decoration", "lattice"}, {"decorationOpacity", .35}}}}); apply(settings_); });
        column->addStretch(); pager_->addPage("custom-theme", page);
    }
    void buildAccount() {
        auto* page = new QWidget; auto* column = pageColumn(page, 16); auto* group = new FormGroup(theme_, "账号与隐私", "登录凭据与本机隐私偏好", page); column->addWidget(group);
        toggle(group, "privacyMode", "隐私模式", "隐藏邮箱等敏感信息，仅本机可见");
        auto* verification = new CheckBox(theme_, {}, group, true); verification->setObjectName("verifyMicrosoftSsl");
        verification->setChecked(true); verification->setEnabled(false);
        group->addRow("在正版登录时进行 SSL 证书验证", verification, "始终验证服务器证书；此版本不支持关闭验证。");
        textField(group, "offlineUsername", "离线用户名", "离线进入游戏的昵称（最多 16 个字符）")->setMaxLength(16);
        textField(group, "littleskinServer", "皮肤站服务器", "第三方皮肤站地址，默认 LittleSkin"); column->addStretch(); pager_->addPage("account", page);
    }
    void buildDownload() {
        auto* page = new QWidget; auto* column = pageColumn(page, 16); auto* group = new FormGroup(theme_, "下载", "下载来源、并发与速度限制", page); column->addWidget(group);
        const QList<QPair<QString, QString>> sources{{"mirror_first", "尽量使用镜像源"}, {"official_auto", "自动选择"}, {"official_first", "尽量使用官方源"}};
        choice(group, "versionListSource", "版本列表源", sources); choice(group, "fileDownloadSource", "文件下载源", sources); slider(group, "maxDownloadThreads", "最大线程数", 1, 64);
        auto* host = new QWidget(group); auto* speedColumn = pageColumn(host, 2); speed_ = new Slider(theme_, host); speed_->setObjectName("downloadLimitBps"); speed_->setRange(0, 37); speedLabel_ = textLabel(theme_, {}, 11, true, host); speedColumn->addWidget(speed_); speedColumn->addWidget(speedLabel_); group->addRow("速度限制", host);
        connect(speed_, &QSlider::valueChanged, this, [this](int step) { const double bps = step >= 37 ? 0 : std::round((1024. * 1024 / 4) * std::pow(2., step / 4.)); speedLabel_->setText(bps == 0 ? "不限速" : formatBytes(bps) + "/s"); patch({{"downloadLimitBps", bps}}); });
        choice(group, "communityApiSource", "社区资源源", sources); textField(group, "modrinthApiMirror", "Modrinth 镜像"); textField(group, "curseforgeApiMirror", "CurseForge 镜像"); textField(group, "communityFileMirror", "资源文件镜像"); column->addStretch(); pager_->addPage("download", page);
    }
    static QJsonObject mergeDraft(QJsonObject base, const QJsonObject& fields) {
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            if (it.key() == "customTheme" && it.value().isObject())
                base.insert(it.key(), withFields(base.value(it.key()).toObject(), it.value().toObject()));
            else base.insert(it.key(), it.value());
        }
        return base;
    }
    void patch(const QJsonObject& patch) {
        if (applying_ || !ready_) return;
        settings_ = mergeDraft(settings_, patch); pending_ = mergeDraft(pending_, patch); saveTimer_.start();
        pager_->setItemVisible("custom-theme", settings_.value("accent").toString() == "custom");
    }
    void saveDraft() {
        if (saving_ || pending_.isEmpty()) return; const QJsonObject patch = pending_; pending_ = {}; saving_ = true;
        command(UiAction::PatchSettings, {{"section", "app"}, {"patch", patch}}, [this, patch](const UiReply& reply) {
            saving_ = false;
            if (reply.error.isError()) { pending_ = mergeDraft(patch, pending_); showError(reply.error); }
            else { error_->hide(); if (!pending_.isEmpty()) saveDraft(); else if (isVisible()) reload(); }
        });
    }
    void reload() { query(UiQuery::Settings, {{"section", "app"}}, "settings", [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else { const auto state = reply.data.toObject(); apply(state.value("values").isObject() ? state.value("values").toObject() : state); } }); }
    void apply(const QJsonObject& state) {
        settings_ = mergeDraft(state, pending_); ready_ = true; applying_ = true; pager_->setEnabled(true);
        for (auto it = editors_.begin(); it != editors_.end(); ++it) { QSignalBlocker block(it.value()); const auto value = settings_.value(it.key());
            if (auto* combo = qobject_cast<ComboBox*>(it.value())) combo->setCurrentIndex(combo->findData(value.toString()));
            else if (auto* check = qobject_cast<CheckBox*>(it.value())) check->setChecked(value.toBool());
            else if (auto* slider = qobject_cast<Slider*>(it.value())) slider->setValue(int(std::lround(value.toDouble() * scales_.value(it.key(), 1))));
            else if (auto* text = qobject_cast<LineEdit*>(it.value())) { if (!text->hasFocus()) text->setText(value.toString()); }
        }
        for (auto it = accents_.begin(); it != accents_.end(); ++it) { QSignalBlocker block(it.value()); it.value()->setChecked(it.key() == settings_.value("accent").toString()); }
        const auto custom = settings_.value("customTheme").toObject(); for (auto it = custom_.begin(); it != custom_.end(); ++it) { QSignalBlocker block(it.value()); it.value()->setValue(int(std::lround(custom.value(it.key()).toDouble() * (it.key() == "decorationOpacity" ? 100 : 1)))); }
        { QSignalBlocker block(decoration_); decoration_->setCurrentIndex(decoration_->findData(custom.value("decoration").toString())); }
        custom_.value("titlebarHueShift")->setEnabled(settings_.value("titlebarTint").toBool()); custom_.value("decorationOpacity")->setEnabled(custom.value("decoration").toString() != "none");
        const double bps = settings_.value("downloadLimitBps").toDouble(); { QSignalBlocker block(speed_); speed_->setValue(bps == 0 ? 37 : std::clamp(int(std::lround(4 * std::log2(bps / (1024. * 1024 / 4)))), 0, 36)); } speedLabel_->setText(bps == 0 ? "不限速" : formatBytes(bps) + "/s");
        pager_->setItemVisible("custom-theme", settings_.value("accent").toString() == "custom"); applying_ = false;
    }
    void reloadGame() {
        query(UiQuery::Settings, {{"section", "game"}}, "game", [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else { const auto state = reply.data.toObject(); game_->setValues(state.value("values").isObject() ? state.value("values").toObject() : state); game_->setAvailable(true); } });
        reloadJava(); query(UiQuery::RuntimeInformation, {}, "runtime", [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else game_->setRuntime(reply.data.toObject()); });
    }
    void reloadJava() { query(UiQuery::JavaInstallations, {}, "java", [this](const UiReply& reply) { if (reply.error.isError()) showError(reply.error); else game_->setJavaInstallations(reply.data.isArray() ? reply.data.toArray() : reply.data.toObject().value("items").toArray()); }); }
    void showError(const UiError& error) { error_->setText(error.message); error_->show(); report(error); }
    Pager* pager_; QLabel* error_; GameForm* game_; QHash<QString, QWidget*> editors_; QHash<QString, double> scales_; QHash<QString, RadioButton*> accents_; QHash<QString, Slider*> custom_;
    ComboBox* decoration_; Slider* speed_; QLabel* speedLabel_; ImageView* background_; QJsonObject settings_, pending_; QTimer saveTimer_; bool applying_ = false, ready_ = false, saving_ = false;
};
}
RoutePage* createSettingsPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent) { return new SettingsPage(theme, bindings, navigation, parent); }
} // namespace nickel::ui
