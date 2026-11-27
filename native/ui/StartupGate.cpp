#include "StartupGate.hpp"
#include "ProductIdentity.hpp"
#include <QJsonArray>
#include <QKeyEvent>
#include <QPainter>
#include <algorithm>

namespace nickel::ui {
StartupGate::StartupGate(Theme& theme, Bindings& bindings, QWidget* parent)
    : QWidget(parent), theme_(theme), bindings_(bindings), panel_(new Panel(theme, this)),
      layout_(panel_->column(24, 16)), title_(textLabel(theme, {}, 19, false, panel_)),
      body_(new QWidget(panel_)), error_(textLabel(theme, {}, 12, true, panel_)), footer_(new QWidget(panel_)) {
    setObjectName("startupGate"); setAccessibleName("首次启动确认"); setFocusPolicy(Qt::StrongFocus);
    panel_->setShadow(3); layout_->addWidget(title_); layout_->addWidget(body_, 1);
    layout_->addWidget(error_); layout_->addWidget(footer_); error_->hide();
    rejectionTimer_.setInterval(100);
    connect(&rejectionTimer_, &QTimer::timeout, this, [this] {
        if (rejectionClock_.elapsed() >= 5000) {
            rejectionTimer_.stop(); emit rejectionFinished();
        } else render();
    });
    connect(&theme_, &Theme::changed, this, qOverload<>(&QWidget::update));
    render();
}
void StartupGate::start() {
    if (busy_ || finished_) return;
    busy_ = true; failure_ = {}; const auto generation = ++generation_;
    bindings_.query(UiQuery::Onboarding, {}, this, [this, generation](const UiReply& reply) {
        if (generation != generation_) return;
        busy_ = false; failure_ = reply.error;
        if (!reply.error.isError()) apply(reply.data.toObject()); else render();
    });
    render();
}
void StartupGate::apply(const QJsonObject& status) {
    if (finished_ || rejectionClock_.isValid()) return;
    if (!status.value("needsEula").isBool() || !status.value("needsChangelog").isBool()
        || !status.value("exiting").isBool() || status.value("eulaVersion").toString().isEmpty()
        || status.value("currentVersion").toString().isEmpty()) {
        status_ = {};
        failure_ = {"onboarding.invalid", "首次启动记录无效，请重试。", {}, true}; render(); return;
    }
    status_ = status; failure_ = {};
    if (status.value("exiting").toBool()) { rejectionClock_.start(); rejectionTimer_.start(); }
    if (!status.value("needsEula").toBool() && !status.value("needsChangelog").toBool()
        && !status.value("exiting").toBool() && !finishing_ && !busy_) perform(UiAction::FinishOnboarding, {});
    else render();
}
void StartupGate::perform(UiAction action, const QJsonObject& arguments) {
    if (busy_ || finished_) return;
    busy_ = true; finishing_ = action == UiAction::FinishOnboarding; failure_ = {};
    const auto generation = ++generation_;
    const auto eulaVersion = status_.value("eulaVersion").toString();
    const auto currentVersion = status_.value("currentVersion").toString();
    bindings_.command(action, arguments, this, [this, generation, action, eulaVersion, currentVersion](const UiReply& reply) {
        if (generation != generation_) return;
        busy_ = false; finishing_ = false; failure_ = reply.error;
        if (reply.error.isError()) { render(); return; }
        if (action == UiAction::FinishOnboarding) {
            if (status_.isEmpty() || rejectionClock_.isValid()
                || status_.value("needsEula").toBool() || status_.value("needsChangelog").toBool()
                || status_.value("eulaVersion").toString() != eulaVersion
                || status_.value("currentVersion").toString() != currentVersion) {
                failure_ = {"onboarding.changed", "首次启动状态发生变化，请重新确认。", {}, true}; render(); return;
            }
            finished_ = true; hide(); emit ready(); return;
        }
        if (action == UiAction::RejectEula) return;
        apply(reply.data.toObject());
    });
    render();
}
void StartupGate::render() {
    const bool restoreAgreement = body_->findChild<CheckBox*>("eulaAgreement")
        && body_->findChild<CheckBox*>("eulaAgreement")->isChecked();
    layout_->removeWidget(body_); layout_->removeWidget(footer_);
    body_->hide(); footer_->hide(); body_->deleteLater(); footer_->deleteLater();
    body_ = new QWidget(panel_); footer_ = new QWidget(panel_);
    layout_->insertWidget(1, body_, 1); layout_->addWidget(footer_);
    auto* body = pageColumn(body_, 12); auto* actions = rowLayout(footer_, 8); actions->addStretch();
    const bool exiting = rejectionClock_.isValid();
    const bool eula = !status_.isEmpty() && status_.value("needsEula").toBool();
    const bool changes = !status_.isEmpty() && status_.value("needsChangelog").toBool();
    title_->setText(exiting ? "无法继续提供服务" : eula ? "开始之前，请确认 Minecraft EULA"
        : changes ? productDisplayName() + " " + status_.value("currentVersion").toString() + " 更新说明" : "正在准备首次启动");
    if (exiting) {
        body->addWidget(textLabel(theme_, "如果不同意 Minecraft 的 EULA，" + productDisplayName() + " 无法提供服务。"));
        auto* countdown = actionButton(theme_, "正在退出…" + QString::number(std::max(0, 5 - int(rejectionClock_.elapsed() / 1000))), {}, ButtonKind::Secondary, footer_);
        countdown->setEnabled(false); actions->addWidget(countdown);
    } else if (eula) {
        body->addWidget(textLabel(theme_, "使用 Minecraft 前，请阅读并同意其最终用户许可协议。" + productDisplayName() + " 是非官方启动器，不代表 Mojang 或 Microsoft。"));
        auto* summary = new FormGroup(theme_, "MINECRAFT · EULA", "请合法获取游戏，遵守内容分发、Mod 使用和社区安全相关条款。未成年人应在监护人的帮助下阅读。", body_);
        const QString url = status_.value("eulaUrl").toString();
        auto* read = actionButton(theme_, "阅读 Minecraft EULA 完整协议", "globe", ButtonKind::Outline, summary);
        summary->body()->addWidget(read); summary->body()->addWidget(textLabel(theme_, url, 11, true)); body->addWidget(summary);
        connect(read, &QPushButton::clicked, this, [this, url] { bindings_.command(UiAction::OpenExternalUrl, {{"url", url}}, this, [this](const UiReply& reply) { failure_ = reply.error; error_->setText(failure_.message); error_->setVisible(failure_.isError()); }); });
        auto* checked = new CheckBox(theme_, "我已阅读、理解并同意 Minecraft EULA，愿意签署此声明。", body_);
        checked->setObjectName("eulaAgreement"); checked->setChecked(restoreAgreement); checked->setEnabled(!busy_); body->addWidget(checked);
        auto* reject = actionButton(theme_, "拒绝", {}, ButtonKind::Danger, footer_);
        reject->setObjectName("rejectEula");
        auto* accept = actionButton(theme_, "同意并签署", {}, ButtonKind::Primary, footer_);
        accept->setObjectName("acceptEula"); accept->setEnabled(restoreAgreement && !busy_); reject->setEnabled(!busy_);
        actions->addWidget(reject); actions->addWidget(accept);
        connect(checked, &QCheckBox::toggled, accept, [this, accept](bool checkedValue) { accept->setEnabled(checkedValue && !busy_); });
        const QString version = status_.value("eulaVersion").toString();
        connect(accept, &QPushButton::clicked, this, [this, version] {
            perform(UiAction::AcceptEula, {{"checked", true}, {"eulaVersion", version}});
        });
        connect(reject, &QPushButton::clicked, this, [this] {
            rejectionClock_.start(); rejectionTimer_.start(); perform(UiAction::RejectEula, {});
        });
    } else if (changes) {
        auto* contents = new QWidget; auto* column = pageColumn(contents, 12);
        const auto changelog = status_.value("changelog").toObject();
        if (changelog.value("sections").isArray()) {
            for (const auto& value : changelog.value("sections").toArray()) {
                const auto section = value.toObject(); column->addWidget(textLabel(theme_, section.value("title").toString(), 15));
                for (const auto& item : section.value("items").toArray()) column->addWidget(textLabel(theme_, "• " + item.toString()));
            }
        } else column->addWidget(textLabel(theme_, status_.value("changelogText").toString()));
        auto* scroll = new ScrollArea(theme_, body_); scroll->setWidget(contents); scroll->setMinimumHeight(240); body->addWidget(scroll);
        auto* acknowledge = actionButton(theme_, "我已知晓", {}, ButtonKind::Primary, footer_);
        acknowledge->setObjectName("acknowledgeChangelog"); acknowledge->setEnabled(!busy_);
        const QString version = status_.value("currentVersion").toString();
        actions->addWidget(acknowledge);
        connect(acknowledge, &QPushButton::clicked, this, [this, version] { perform(UiAction::AcknowledgeChangelog, {{"version", version}}); });
    } else {
        body->addWidget(textLabel(theme_, failure_.isError() ? "读取或确认未完成，底层页面仍保持锁定。" : "正在准备…", 13, true));
        if (failure_.isError()) { auto* retry = actionButton(theme_, "重试", "refresh", ButtonKind::Secondary, footer_); retry->setEnabled(!busy_); actions->addWidget(retry); connect(retry, &QPushButton::clicked, this, [this] { status_ = {}; finishing_ = false; start(); }); }
    }
    error_->setText(failure_.message + (failure_.detail.isEmpty() ? QString{} : "\n" + failure_.detail)); error_->setVisible(failure_.isError());
    panel_->setFixedWidth(changes && !exiting ? 620 : 540);
    panel_->resize(panel_->width(), std::clamp(panel_->sizeHint().height(), 220, std::max(220, height() - 64)));
    panel_->move((width() - panel_->width()) / 2, (height() - panel_->height()) / 2); update();
}
void StartupGate::paintEvent(QPaintEvent*) { QPainter painter(this); painter.fillRect(rect(), theme_.tokens().overlay); }
void StartupGate::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    panel_->resize(panel_->width(), std::clamp(panel_->sizeHint().height(), 220, std::max(220, height() - 64)));
    panel_->move((width() - panel_->width()) / 2, (height() - panel_->height()) / 2);
}
void StartupGate::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { event->accept(); return; }
    QWidget::keyPressEvent(event);
}
} // namespace nickel::ui
