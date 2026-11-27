#include "GameForm.hpp"
#include "Dialogs.hpp"
#include "Icons.hpp"
#include "ListModel.hpp"

#include <QIntValidator>
#include <QPainter>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>

namespace nickel::ui {
namespace {
class MemoryGraph final : public QWidget {
public:
    MemoryGraph(Theme& theme, QWidget* parent) : QWidget(parent), theme_(theme) { setMinimumHeight(76); setAccessibleName("内存分配情况"); connect(&theme_, &Theme::changed, this, qOverload<>(&QWidget::update)); }
    void setState(const QJsonObject& state) { state_ = state; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); const auto& tokens = theme_.tokens(); painter.setPen(Qt::NoPen); painter.setBrush(tokens.fill2); painter.drawRoundedRect(QRectF(0, 14, width(), 12), 6, 6);
        if (!state_.value("totalMemoryMb").isDouble() || state_.value("totalMemoryMb").toDouble() <= 0) { painter.setFont(bodyFont(11)); painter.setPen(tokens.faint); painter.drawText(QRectF(0, 40, width(), 20), Qt::AlignLeft, "正在读取本机内存…"); return; }
        const double total = state_.value("totalMemoryMb").toDouble(), available = state_.value("availableMemoryMb").toDouble();
        const double suggestion = state_.value("suggestedMemoryMb").toDouble(); const double used = std::clamp((total - available) / total, 0., 1.);
        painter.setBrush(tokens.fill4); painter.drawRoundedRect(QRectF(0, 14, width() * used, 12), 6, 6); painter.setBrush(tokens.accentText);
        painter.drawRoundedRect(QRectF(width() * used, 14, width() * std::min(suggestion / total, 1. - used), 12), 6, 6);
        painter.setFont(bodyFont(11)); painter.setPen(tokens.muted); painter.drawText(QRectF(0, 40, width(), 20), Qt::AlignLeft, "物理内存 " + formatBytes(total * 1024 * 1024) + " · 可用 " + formatBytes(available * 1024 * 1024));
    }
private:
    Theme& theme_; QJsonObject state_;
};
}
GameForm::GameForm(Theme& theme, bool perVersion, QWidget* parent) : QWidget(parent), theme_(theme), perVersion_(perVersion) {
    auto* column = pageColumn(this, 16);
    auto* launch = new FormGroup(theme, "启动选项", perVersion ? "未覆盖的字段继承全局设置；空文本表示明确清空。" : QString{}, this); column->addWidget(launch);
    if (!perVersion) {
        auto* directory = new QWidget(launch); auto* row = rowLayout(directory, 8); auto* display = new LineEdit(theme, directory); display->setReadOnly(true); display->setPlaceholderText("请在实例页管理目录"); display->setObjectName("gameDir"); row->addWidget(display, 1); launch->addRow("游戏目录", directory);
        fields_.insert("gameDir", {"gameDir", Type::Text, display, nullptr});
    }
    addField(launch, "versionIsolation", "版本隔离", Type::Boolean, {}, "避免不同版本的 Mod、存档与配置互相干扰");
    addField(launch, "windowTitle", "游戏窗口标题", Type::Text); addField(launch, "customInfo", "自定义信息", Type::Text);
    addField(launch, "launcherVisibility", "启动器可见性", Type::Choice, {{"keep", "保持显示"}, {"hide", "启动后隐藏"}, {"hide-close", "隐藏，游戏退出后关闭"}, {"close", "启动后关闭"}, {"minimize", "启动后最小化"}});
    addField(launch, "processPriority", "进程优先级", Type::Choice, {{"low", "低"}, {"normal", "普通"}, {"high", "高"}});
    addField(launch, "windowMode", "窗口模式", Type::Choice, {{"default", "默认"}, {"custom", "自定义大小"}, {"fullscreen", "全屏"}, {"same-as-launcher", "与启动器相同"}, {"maximize", "最大化"}});
    addField(launch, "windowWidth", "窗口宽度", Type::Number); addField(launch, "windowHeight", "窗口高度", Type::Number);
    addField(launch, "javaPath", "Java", Type::Choice, {{"", "自动选择"}});
    auto* javaRefresh = actionButton(theme, "重新扫描 Java", "refresh", ButtonKind::Ghost, launch); launch->body()->addWidget(javaRefresh, 0, Qt::AlignRight); connect(javaRefresh, &QPushButton::clicked, this, &GameForm::javaRefreshRequested);
    auto* memory = new FormGroup(theme, "内存分配", {}, this); column->addWidget(memory);
    addField(memory, "memoryMode", "分配方式", Type::Choice, {{"auto", "自动分配"}, {"custom", "自定义"}});
    addField(memory, "memoryMb", "最大内存 (MiB)", Type::Number, {}, "保留已保存值；超出本机能力时由启动服务报告错误，不在界面静默截断。" );
    memoryInfo_ = textLabel(theme, {}, 11, true); memory->body()->addWidget(memoryInfo_); memoryGraph_ = new MemoryGraph(theme, memory); memory->body()->addWidget(memoryGraph_);
    if (perVersion) {
        auto* server = new FormGroup(theme, "服务器", {}, this); column->addWidget(server);
        addField(server, "serverLogin", "登录方式", Type::Choice, {{"any", "不限"}, {"offline", "离线"}, {"microsoft", "正版"}, {"thirdParty", "第三方皮肤站"}});
        addField(server, "authServer", "认证服务器", Type::Text); addField(server, "serverRegistration", "注册链接", Type::Text);
        addField(server, "serverName", "服务器名称", Type::Text); addField(server, "serverAddress", "自动进入服务器", Type::Text);
    }
    auto* advanced = new FormGroup(theme, "高级选项", {}, this);
    addField(advanced, "jvmArgs", "JVM 参数", Type::Multiline); addField(advanced, "gameArgs", "游戏参数", Type::Multiline);
    addField(advanced, "preLaunchCommand", "启动前执行命令", Type::Multiline);
    addField(advanced, "memoryManagement", "内存管理", Type::Choice, {{"auto-zgc", "自动 ZGC"}, {"generational-zgc", "分代 ZGC"}, {"g1", "G1"}, {"g1-tuned", "优化 G1"}, {"custom", "由 JVM 参数管理"}});
    addField(advanced, "disableWrapper", "禁用 Java Launch Wrapper", Type::Boolean); addField(advanced, "disableLwjglAgent", "禁用 LWJGL Unsafe Agent", Type::Boolean);
    addField(advanced, "highPerformanceGpu", "使用高性能显卡", Type::Boolean);
    if (perVersion) { addField(advanced, "disableModUpdates", "禁止更新 Mod", Type::Boolean); addField(advanced, "disableFileCheck", "关闭文件校验", Type::Boolean); }
    auto* disclosure = new Disclosure(theme, "高级选项", advanced, this); disclosure->setOpen(perVersion); column->addWidget(disclosure);
    auto* skin = new FormGroup(theme, "离线皮肤", {}, this); column->addWidget(skin);
    addField(skin, "offlineSkin", "皮肤来源", Type::Choice, {{"random", "随机"}, {"steve", "Steve"}, {"alex", "Alex"}, {"official", "正版玩家皮肤"}, {"custom", "本地自定义"}});
    addField(skin, "offlineSkinName", "正版玩家名称", Type::Text); addField(skin, "offlineSkinPath", "自定义皮肤路径", Type::Text); addField(skin, "offlineSkinSlim", "纤细手臂", Type::Boolean);
    auto* skinActions = new QWidget(skin); auto* skinRow = rowLayout(skinActions, 8); auto* choose = actionButton(theme, "选择 PNG", "image", ButtonKind::Secondary, skinActions); auto* reset = actionButton(theme, "重置皮肤", "refresh", ButtonKind::Ghost, skinActions); skinRow->addWidget(choose); skinRow->addWidget(reset); skinRow->addStretch(); skin->body()->addWidget(skinActions);
    connect(choose, &QPushButton::clicked, this, [this] { const auto files = pickFiles(this, "选择离线皮肤", "PNG (*.png)"); if (!files.isEmpty()) emit offlineSkinRequested({{"path", files.first()}}); });
    connect(reset, &QPushButton::clicked, this, [this] { emit offlineSkinRequested({{"reset", true}}); });
    column->addStretch(); setAvailable(false);
}
void GameForm::addField(FormGroup* group, const QString& key, const QString& label, Type type, const QList<QPair<QString, QString>>& options, const QString& description) {
    QWidget* editor = nullptr;
    if (type == Type::Choice) {
        auto* combo = new ComboBox(theme_, group); for (const auto& option : options) combo->addItem(option.second, option.first); editor = combo;
        connect(combo, &QComboBox::activated, this, [this, key, combo](int) { edit(key, combo->currentData().toString()); });
    } else if (type == Type::Boolean) {
        auto* check = new CheckBox(theme_, {}, group, true); editor = check; connect(check, &QCheckBox::toggled, this, [this, key](bool value) { edit(key, value); });
    } else if (type == Type::Multiline) {
        auto* text = new TextEdit(theme_, group); text->setMinimumHeight(80); editor = text;
        auto* debounce = new QTimer(text); debounce->setSingleShot(true); debounce->setInterval(400);
        connect(text, &QPlainTextEdit::textChanged, this, [this, debounce] { if (!applying_) debounce->start(); });
        connect(debounce, &QTimer::timeout, this, [this, key, text] { edit(key, text->toPlainText()); });
    } else {
        auto* line = new LineEdit(theme_, group); if (type == Type::Number) line->setValidator(new QIntValidator(1, 2147483647, line)); editor = line;
        connect(line, &QLineEdit::editingFinished, this, [this, key, type, line] { if (type == Type::Number) { bool ok = false; const int value = line->text().toInt(&ok); if (ok && value > 0) edit(key, value); } else edit(key, line->text()); });
    }
    editor->setObjectName(key); editor->setAccessibleName(label); CheckBox* inherit = nullptr; QWidget* control = editor;
    if (perVersion_) {
        auto* host = new QWidget(group); auto* column = pageColumn(host, 6); inherit = new CheckBox(theme_, "继承全局", host); column->addWidget(inherit); column->addWidget(editor); control = host;
        connect(inherit, &QCheckBox::toggled, this, [this, key, editor](bool inherited) {
            if (applying_ || !available_) return; editor->setEnabled(!inherited);
            edit(key, inherited ? (key == "memoryMode" ? QJsonValue("global") : QJsonValue(QJsonValue::Null)) : effective_.value(key));
        });
    }
    fields_.insert(key, {key, type, editor, inherit}); group->addRow(label, control, description, type == Type::Multiline);
}
void GameForm::edit(const QString& key, const QJsonValue& value) {
    if (applying_ || !available_) return; values_.insert(key, value); emit patchRequested({{key, value}});
}
void GameForm::setValues(const QJsonObject& values, const QJsonObject& effective) {
    values_ = values; effective_ = effective.isEmpty() ? values : effective; applying_ = true;
    for (auto it = fields_.begin(); it != fields_.end(); ++it) {
        auto& field = it.value(); const auto original = values.value(field.key);
        const bool inherited = perVersion_ && (original.isNull() || original.isUndefined() || (field.key == "memoryMode" && original.toString() == "global"));
        const QJsonValue value = inherited ? effective_.value(field.key) : original;
        if (field.inherit) { QSignalBlocker block(field.inherit); field.inherit->setChecked(inherited); }
        field.editor->setEnabled(available_ && !inherited);
        QSignalBlocker block(field.editor);
        if (auto* combo = qobject_cast<ComboBox*>(field.editor)) { int index = combo->findData(value.toString()); if (index < 0 && !value.toString().isEmpty()) { combo->addItem(value.toString(), value.toString()); index = combo->count() - 1; } combo->setCurrentIndex(index); }
        else if (auto* check = qobject_cast<CheckBox*>(field.editor)) check->setChecked(value.toBool());
        else if (auto* text = qobject_cast<TextEdit*>(field.editor)) { if (!text->hasFocus()) text->setPlainText(value.toString()); }
        else if (auto* line = qobject_cast<LineEdit*>(field.editor)) { if (!line->hasFocus()) line->setText(field.type == Type::Number ? (value.isDouble() ? QString::number(value.toInteger()) : QString{}) : value.toString()); }
    }
    applying_ = false;
}
void GameForm::setJavaInstallations(const QJsonArray& items) {
    auto* combo = qobject_cast<ComboBox*>(fields_.value("javaPath").editor); if (!combo) return; QSignalBlocker block(combo); const QVariant selected = combo->currentData(); combo->clear(); combo->addItem("自动选择", "");
    for (const auto& value : items) { const auto item = value.toObject(); combo->addItem(item.value("version").toString() + " · " + item.value("vendor").toString() + " · " + item.value("path").toString(), item.value("path").toString()); }
    if (!selected.toString().isEmpty() && combo->findData(selected) < 0) combo->addItem(selected.toString(), selected); combo->setCurrentIndex(combo->findData(selected));
}
void GameForm::setRuntime(const QJsonObject& runtime) {
    runtime_ = runtime; static_cast<MemoryGraph*>(memoryGraph_)->setState(runtime);
    memoryInfo_->setText(runtime.value("suggestedMemoryMb").isDouble() ? "自动分配建议：" + QString::number(runtime.value("suggestedMemoryMb").toInteger()) + " MiB" : "暂未取得自动内存建议");
    const auto capabilities = runtime.value("capabilities").toObject(); if (capabilities.contains("highPerformanceGpu")) fields_.value("highPerformanceGpu").editor->setEnabled(available_ && capabilities.value("highPerformanceGpu").toBool());
}
void GameForm::setAvailable(bool available) { available_ = available; setEnabled(available); if (available) setValues(values_, effective_); }
} // namespace nickel::ui
