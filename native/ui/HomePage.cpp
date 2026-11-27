#include "Pages.hpp"
#include "Dialogs.hpp"
#include "LaunchError.hpp"
#include "ListModel.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFocusEvent>
#include <QGraphicsOpacityEffect>
#include <QJsonArray>
#include <QPainter>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace nickel::ui {
namespace {
class CharacterActionEffect final : public QGraphicsOpacityEffect {
public:
    explicit CharacterActionEffect(QObject* parent) : QGraphicsOpacityEffect(parent) { setOpacity(0); }
protected:
    void draw(QPainter* painter) override {
        if (qFuzzyIsNull(opacity())) return;
        QPoint offset;
        const QPixmap pixmap = sourcePixmap(Qt::LogicalCoordinates, &offset, QGraphicsEffect::NoPad);
        if (pixmap.isNull()) return;
        const QPointF center = QRectF(QPointF{}, pixmap.deviceIndependentSize()).center();
        const qreal scale = .86 + .14 * opacity();
        painter->save(); painter->setOpacity(painter->opacity() * opacity());
        painter->translate(QPointF(offset) + center); painter->scale(scale, scale);
        painter->drawPixmap(-center, pixmap); painter->restore();
    }
};
class CharacterPresentation final : public QWidget {
public:
    CharacterPresentation(Theme& theme, QWidget* parent) : QWidget(parent), theme_(theme), animation_(this) {
        setObjectName("homeCharacter");
        QEasingCurve easing(QEasingCurve::BezierSpline);
        easing.addCubicBezierSegment(QPointF(.16, 1), QPointF(.3, 1), QPointF(1, 1));
        animation_.setEasingCurve(easing);
        connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            for (auto* effect : effects_) if (effect) effect->setOpacity(value.toReal());
        });
        connect(&theme_, &Theme::motionChanged, this, [this](bool) { reveal(); });
    }
    void setActions(Button* edit, Button* switchAccount) {
        actions_ = {edit, switchAccount};
        for (std::size_t i = 0; i < actions_.size(); ++i) {
            effects_[i] = new CharacterActionEffect(actions_[i]);
            actions_[i]->setGraphicsEffect(effects_[i]); actions_[i]->installEventFilter(this);
        }
    }
protected:
    bool event(QEvent* event) override {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::Enter) { hovered_ = true; reveal(); }
        else if (event->type() == QEvent::Leave) { hovered_ = false; reveal(); }
        else if (event->type() == QEvent::Hide) { hovered_ = false; keyboardFocus_.fill(false); reveal(); }
        return handled;
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        for (std::size_t i = 0; i < actions_.size(); ++i) {
            if (watched != actions_[i]) continue;
            if (event->type() == QEvent::FocusIn) {
                const auto reason = static_cast<QFocusEvent*>(event)->reason();
                keyboardFocus_[i] = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason;
                reveal();
            } else if (event->type() == QEvent::FocusOut) { keyboardFocus_[i] = false; reveal(); }
        }
        return QWidget::eventFilter(watched, event);
    }
private:
    void reveal() {
        const qreal target = hovered_ || keyboardFocus_[0] || keyboardFocus_[1] ? 1 : 0;
        const qreal current = effects_[0] ? effects_[0]->opacity() : 0;
        animation_.stop();
        const int duration = isVisible() ? theme_.motionDuration(180) : 0;
        if (!duration || qFuzzyCompare(current + 1, target + 1)) {
            for (auto* effect : effects_) if (effect) effect->setOpacity(target);
        } else {
            animation_.setStartValue(current); animation_.setEndValue(target);
            animation_.setDuration(duration); animation_.start();
        }
    }
    Theme& theme_;
    QVariantAnimation animation_;
    std::array<Button*, 2> actions_{};
    std::array<CharacterActionEffect*, 2> effects_{};
    std::array<bool, 2> keyboardFocus_{};
    bool hovered_ = false;
};
// Account, character and launch entry page. It only submits account/launch commands and renders
// service snapshots; it never owns a game process or decides launch lifecycle transitions.
class HomePage final : public RoutePage {
public:
    HomePage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent)
        : RoutePage(theme, bindings, navigation, parent) {
        setObjectName("homePage"); auto* row = rowLayout(this, 24); row->setContentsMargins(8, 8, 8, 8);
        card_ = new Panel(theme, this); card_->setObjectName("homeCard"); card_->setFixedWidth(400); card_->setShadow(3);
        auto* column = card_->column(24, 20); auto* top = new QWidget(card_); auto* topRow = rowLayout(top, 8);
        tabs_ = new TabStrip(theme, top); tabs_->setObjectName("accountTypes"); tabs_->setExpanding(true);
        tabs_->addItem("正版", "account/shield"); tabs_->addItem("皮肤站", "account/image"); tabs_->addItem("离线", "account/user");
        topRow->addWidget(tabs_, 1); column->addWidget(top);
        body_ = new QStackedWidget(card_); body_->setMinimumHeight(248); column->addWidget(body_, 1);
        buildIdentity(); buildMicrosoft(); buildSkinLogin(); buildOffline();
        loading_ = new LoadingCard(theme, "正在读取账号…", body_); body_->addWidget(loading_); body_->setCurrentWidget(loading_);
        auto* progress = new QWidget(body_); auto* progressColumn = pageColumn(progress, 12);
        progressColumn->addStretch(); progressTitle_ = textLabel(theme, "正在启动游戏", 19); progressTitle_->setAlignment(Qt::AlignCenter);
        progressText_ = textLabel(theme, {}, 12, true); progressText_->setAlignment(Qt::AlignCenter);
        progressColumn->addWidget(progressTitle_); progressColumn->addWidget(progressText_);
        cancelLaunch_ = actionButton(theme, "取消启动", {}, ButtonKind::Ghost, progress); cancelLaunch_->setObjectName("cancelLaunch");
        progressColumn->addWidget(cancelLaunch_, 0, Qt::AlignHCenter); progressColumn->addStretch(); body_->addWidget(progress); progress_ = progress;
        auto* launch = new QWidget(card_); auto* launchColumn = pageColumn(launch, 12);
        launch_ = actionButton(theme, "启动游戏", "play", ButtonKind::Primary, launch); launch_->setObjectName("launchGame"); launch_->setMinimumHeight(62); launch_->setRadius(24); launch_->setSubtitle("尚未选择实例"); launch_->setEnabled(false); launchColumn->addWidget(launch_);
        auto* secondary = new QWidget(launch); auto* secondaryRow = rowLayout(secondary, 12);
        versions_ = actionButton(theme, "选择版本", "versions", ButtonKind::Outline, secondary); versions_->setObjectName("chooseVersion"); versions_->setMinimumHeight(40);
        versionSettings_ = actionButton(theme, "版本设置", "settings", ButtonKind::Outline, secondary); versionSettings_->setMinimumHeight(40); versionSettings_->setEnabled(false);
        secondaryRow->addWidget(versions_, 1); secondaryRow->addWidget(versionSettings_, 1); launchColumn->addWidget(secondary); column->addWidget(launch);
        row->addWidget(card_); row->addStretch();
        connect(versions_, &QPushButton::clicked, this, [this] { navigation_.navigate(Route::Versions); });
        connect(versionSettings_, &QPushButton::clicked, this, [this] { navigation_.openVersionSettings(selected_); });
        connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
            if (applying_ || index < 0 || starting_ || accountsSwitching_) return;
            password_->clear(); passwordStage_ = false; skinBody_->setCurrentIndex(0);
            accountMutationPending_ = true; accountsSwitching_ = true; updateLaunch();
            command(UiAction::SetAccountType, {{"type", accountTypes_.at(size_t(index))}}, [this](const UiReply& reply) {
                accountMutationPending_ = false;
                if (reply.error.isError()) { accountsSwitching_ = false; report(reply.error); applyAccounts(accounts_); }
                else reloadAccounts();
            });
        });
        connect(cancelLaunch_, &QPushButton::clicked, this, [this] {
            const QString id = launchState_.value("launchId").toString();
            if (!starting_ || cancelCommandPending_ || id.isEmpty() || launchState_.value("cancelRequested").toBool()) return;
            cancelCommandPending_ = true; updateLaunch();
            command(UiAction::CancelLaunch, {{"launchId", id}}, [this, id](const UiReply& reply) {
                cancelCommandPending_ = false;
                if (!reply.error.isError() && starting_ && launchState_.value("launchId").toString() == id) launchState_.insert("cancelRequested", true);
                report(reply.error); updateLaunch();
            });
        });
        connect(launch_, &QPushButton::clicked, this, [this] {
            if (accountsSwitching_ || starting_ || launchCommandPending_ || launchAwaitingState_
                || launchState_.value("state").toString() == "running" || !selected_.valid()) return;
            const auto revision = launchRevision_; launchCommandPending_ = true; launchAwaitingState_ = true; updateLaunch();
            command(UiAction::LaunchGame, selected_.scope(), [this, revision](const UiReply& reply) {
                launchCommandPending_ = false;
                if (reply.error.isError()) { launchAwaitingState_ = false; report(reply.error); }
                else if (launchRevision_ == revision) {
                    const auto data = reply.data.toObject(); const auto state = data.value("launch").isObject() ? data.value("launch").toObject() : data;
                    if (!state.value("state").toString().isEmpty() && !state.value("launchId").toString().isEmpty()) applyLaunch(state);
                    else reloadLaunch();
                }
                updateLaunch();
            });
        });
        connect(&bindings.services(), &UiServices::mediaReady, this, [this](const QString& key, const QImage& image) {
            if (key == active_.value("avatarKey").toString()) avatar_->setImage(image, true);
        }, Qt::QueuedConnection);
    }
    void enter() override {
        const auto revision = launchRevision_;
        reloadAccounts(); query(UiQuery::Bootstrap, {}, "bootstrap", [this, revision](const UiReply& reply) {
            if (reply.error.isError()) { report(reply.error); return; }
            const auto state = reply.data.toObject();
            if (state.value("selectedInstance").isObject()) applyInstance(state.value("selectedInstance").toObject());
            if (state.value("launch").isObject() && launchRevision_ == revision) applyLaunch(state.value("launch").toObject());
        });
        showLaunchFailure();
    }
    void leave() override {
        if (launchErrorDialog_) launchErrorDialog_->hide();
        if (accountDialog_) accountDialog_->hide();
        password_->clear(); cancelMicrosoft(); RoutePage::leave();
    }
    void snapshot(UiTopic topic, const QJsonObject& state) override {
        if (topic == UiTopic::Accounts) { invalidate("accounts"); applyAccounts(state); }
        if (topic == UiTopic::Launch) { invalidate("launch-bootstrap"); applyLaunch(state); }
        if (topic == UiTopic::Instances && state.contains("selectedInstance")) applyInstance(state.value("selectedInstance").toObject());
        if (topic == UiTopic::Settings && state.contains("offlineUsername")) { offlineName_->setText(state.value("offlineUsername").toString()); updateLaunch(); }
    }
private:
    void buildIdentity() {
        identity_ = new QWidget(body_); auto* column = pageColumn(identity_, 12); column->addStretch();
        auto* character = new CharacterPresentation(theme_, identity_); auto* characterColumn = pageColumn(character, 12);
        auto* avatarRow = new QWidget(character); auto* row = rowLayout(avatarRow, 12); row->addStretch();
        auto* edit = actionButton(theme_, {}, "pencil", ButtonKind::Ghost, avatarRow); edit->setObjectName("editAccountCharacter"); edit->setFixedSize(28, 28); edit->setAccessibleName("打开官方皮肤页或切换角色"); edit->setToolTip("打开官方皮肤页或切换角色");
        avatar_ = new ImageView(theme_, QSize(72, 72), avatarRow); avatar_->setObjectName("homeAvatar"); avatar_->setFallbackIcon("user");
        auto* switchAccount = actionButton(theme_, {}, "swap", ButtonKind::Ghost, avatarRow); switchAccount->setObjectName("switchSavedAccount"); switchAccount->setFixedSize(28, 28); switchAccount->setAccessibleName("切换账号"); switchAccount->setToolTip("切换账号");
        row->addWidget(edit); row->addWidget(avatar_); row->addWidget(switchAccount); row->addStretch(); characterColumn->addWidget(avatarRow);
        name_ = textLabel(theme_, {}, 21, false, character); name_->setFont(displayFont(21, QFont::Bold)); name_->setAlignment(Qt::AlignCenter);
        email_ = textLabel(theme_, {}, 13, true, character); email_->setAlignment(Qt::AlignCenter);
        characterColumn->addWidget(name_); characterColumn->addWidget(email_); character->setActions(edit, switchAccount);
        column->addWidget(character); column->addStretch(); body_->addWidget(identity_);
        connect(edit, &QPushButton::clicked, this, [this] { profileDialog(); }); connect(switchAccount, &QPushButton::clicked, this, [this] { accountManager(); });
    }
    void buildMicrosoft() {
        microsoft_ = new QWidget(body_); auto* column = pageColumn(microsoft_, 12); column->addStretch();
        auto* login = actionButton(theme_, "使用 Microsoft 登录", "microsoft", ButtonKind::Primary, microsoft_); column->addWidget(login, 0, Qt::AlignHCenter); column->addStretch(); body_->addWidget(microsoft_);
        connect(login, &QPushButton::clicked, this, [this] { beginMicrosoft(); });
    }
    void buildSkinLogin() {
        skin_ = new QWidget(body_); auto* column = pageColumn(skin_, 12); column->addStretch(); skinBody_ = new QStackedWidget(skin_);
        auto* identify = new QWidget(skinBody_); auto* identifyRow = rowLayout(identify, 4);
        username_ = new LineEdit(theme_, identify); username_->setPlaceholderText("邮箱"); username_->setAccessibleName("皮肤站邮箱");
        auto* next = actionButton(theme_, "下一步", {}, ButtonKind::Ghost, identify); identifyRow->addWidget(username_, 1); identifyRow->addWidget(next); skinBody_->addWidget(identify);
        auto* password = new QWidget(skinBody_); auto* passwordColumn = pageColumn(password, 8); auto* who = new QWidget(password); auto* whoRow = rowLayout(who, 8);
        who_ = textLabel(theme_, {}, 12, true, who); auto* back = actionButton(theme_, "返回", {}, ButtonKind::Ghost, who); whoRow->addWidget(who_, 1); whoRow->addWidget(back); passwordColumn->addWidget(who);
        auto* passwordRowWidget = new QWidget(password); auto* passwordRow = rowLayout(passwordRowWidget, 4);
        password_ = new LineEdit(theme_, passwordRowWidget); password_->setEchoMode(QLineEdit::Password); password_->setPlaceholderText("密码"); password_->setAccessibleName("皮肤站密码");
        auto* reveal = actionButton(theme_, {}, "account/eye", ButtonKind::Ghost, passwordRowWidget); reveal->setAccessibleName("显示密码"); reveal->setFixedSize(32, 32); reveal->setCheckable(true);
        skinSubmit_ = actionButton(theme_, "登录", {}, ButtonKind::Ghost, passwordRowWidget); passwordRow->addWidget(password_, 1); passwordRow->addWidget(reveal); passwordRow->addWidget(skinSubmit_); passwordColumn->addWidget(passwordRowWidget); skinBody_->addWidget(password);
        column->addWidget(skinBody_); column->addStretch(); body_->addWidget(skin_);
        auto advance = [this] { if (!username_->text().trimmed().isEmpty()) { passwordStage_ = true; who_->setText(username_->text().trimmed()); skinBody_->setCurrentIndex(1); password_->setFocus(); } };
        connect(next, &QPushButton::clicked, this, advance); connect(username_, &QLineEdit::returnPressed, this, advance);
        connect(back, &QPushButton::clicked, this, [this] { password_->clear(); passwordStage_ = false; skinBody_->setCurrentIndex(0); username_->setFocus(); });
        connect(reveal, &QPushButton::toggled, this, [this, reveal](bool shown) { const int start = password_->selectionStart(); const int length = password_->selectedText().size(); const int cursor = password_->cursorPosition(); password_->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password); if (start >= 0) password_->setSelection(start, length); else password_->setCursorPosition(cursor); reveal->setAccessibleName(shown ? "隐藏密码" : "显示密码"); reveal->setIconName(shown ? "account/eyeOff" : "account/eye"); password_->setFocus(); });
        auto submit = [this] {
            if (skinBusy_ || password_->text().isEmpty() || username_->text().trimmed().isEmpty()) return;
            QJsonObject args{{"server", accounts_.value("server")}, {"username", username_->text().trimmed()}, {"password", password_->text()}};
            password_->clear(); skinBusy_ = true; skinSubmit_->setBusy(true);
            command(UiAction::LoginSkinServer, args, [this](const UiReply& reply) { skinBusy_ = false; skinSubmit_->setBusy(false); report(reply.error); if (!reply.error.isError()) { reloadAccounts(); if (reply.data.toObject().value("profiles").isArray()) characterDialog(reply.data.toObject()); } });
        };
        connect(skinSubmit_, &QPushButton::clicked, this, submit); connect(password_, &QLineEdit::returnPressed, this, submit);
    }
    void buildOffline() {
        offline_ = new QWidget(body_); auto* column = pageColumn(offline_, 12); column->addStretch();
        auto* avatar = new ImageView(theme_, QSize(72, 72), offline_); avatar->setResource("skins/steve-head.png", true); column->addWidget(avatar, 0, Qt::AlignHCenter);
        offlineName_ = textLabel(theme_, {}, 21); offlineName_->setObjectName("offlineUsername");
        offlineName_->setFont(displayFont(21, QFont::Bold)); offlineName_->setAlignment(Qt::AlignCenter);
        column->addWidget(offlineName_); column->addStretch(); body_->addWidget(offline_);
    }
    void reloadAccounts() {
        query(UiQuery::Accounts, {}, "accounts", [this](const UiReply& reply) {
            if (reply.error.isError()) { accountsSwitching_ = accountMutationPending_; loading_->setError(true); loading_->setLabel(reply.error.message); report(reply.error); updateLaunch(); }
            else applyAccounts(reply.data.toObject());
        });
    }
    void applyAccounts(const QJsonObject& state) {
        accounts_ = state; active_ = {}; applying_ = true; accountsSwitching_ = accountMutationPending_;
        const QString type = state.value("accountType").toString(); int index = 0;
        for (size_t i = 0; i < accountTypes_.size(); ++i) if (type == accountTypes_[i]) index = int(i);
        { QSignalBlocker block(tabs_); tabs_->setCurrentIndex(index); }
        offlineName_->setText(state.value("offlineUsername").toString());
        for (const auto& value : state.value("items").toArray()) { const auto item = value.toObject(); if (item.value("id") == state.value("activeId") && item.value("type").toString() == type) { active_ = item; break; } }
        if (!active_.isEmpty()) { name_->setText(active_.value("name").toString()); email_->setText(active_.value("emailMasked").toString()); avatar_->setResource(active_.value("avatarKey").toString(), true); }
        applying_ = false; showAccount(); updateLaunch();
        const auto login = state.value("login").toObject();
        if (deviceDialog_) updateMicrosoft(login);
        if (login.value("state").toString() == "select" && !characterDialog_) characterDialog(login);
    }
    void applyInstance(const QJsonObject& state) {
        selected_ = {state.value("root").toString(), state.value("versionId").toString()}; selectedName_ = state.value("displayName").toString();
        if (selectedName_.isEmpty()) selectedName_ = selected_.versionId; versionSettings_->setEnabled(selected_.valid()); updateLaunch();
    }
    void reloadLaunch() {
        query(UiQuery::Bootstrap, {}, "launch-bootstrap", [this](const UiReply& reply) {
            if (reply.error.isError()) { report(reply.error); updateLaunch(); return; }
            const auto state = reply.data.toObject().value("launch");
            if (state.isObject()) applyLaunch(state.toObject());
        });
    }
    void applyLaunch(const QJsonObject& state) {
        ++launchRevision_; launchState_ = state; launchAwaitingState_ = false; const QString status = state.value("state").toString();
        starting_ = status == "starting" || status == "preparing";
        if (starting_) { const QString stage = state.value("stage").toString(); progressText_->setText((stage.isEmpty() ? QStringLiteral("准备中...") : stage) + (state.value("percent").isDouble() ? " · " + QString::number(state.value("percent").toDouble(), 'f', 0) + "%" : QString{})); }
        showAccount(); updateLaunch(); showLaunchFailure();
    }
    void showLaunchFailure() {
        const QString key = launchFailureKey(launchState_);
        if (key.isEmpty()) dismissedFailureKey_.clear();
        if (launchErrorDialog_ && launchErrorDialog_->property("launchFailureKey").toString() != key) {
            if (launchErrorDialog_->isBusy()) return;
            launchErrorDialog_->setProperty("obsoleteLaunchFailure", true);
            launchErrorDialog_->reject();
        }
        if (key.isEmpty() || !isVisible() || key == dismissedFailureKey_) return;
        if (launchErrorDialog_) { launchErrorDialog_->show(); return; }
        launchErrorDialog_ = showLaunchErrorDialog(theme_, bindings_, launchState_, this);
        if (!launchErrorDialog_) return;
        const QPointer<Dialog> owner = launchErrorDialog_;
        connect(owner, &QDialog::finished, this, [this, key, owner] {
            if (launchErrorDialog_ == owner) launchErrorDialog_ = nullptr;
            if (launchFailureKey(launchState_) == key) dismissedFailureKey_ = key;
            QTimer::singleShot(0, this, [this] { showLaunchFailure(); });
        });
        connect(owner, &Dialog::busyChanged, this, [this](bool busy) {
            if (!busy) QTimer::singleShot(0, this, [this] { showLaunchFailure(); });
        });
    }
    void showAccount() { body_->setCurrentWidget(starting_ ? progress_ : tabs_->currentIndex() == 2 ? offline_ : !active_.isEmpty() ? identity_ : tabs_->currentIndex() == 1 ? skin_ : microsoft_); }
    void updateLaunch() {
        const bool running = launchState_.value("state").toString() == "running";
        const bool cancelling = launchState_.value("cancelRequested").toBool();
        launch_->setBusy(launchCommandPending_);
        launch_->setText(running ? "游戏运行中" : starting_ || launchAwaitingState_ ? cancelling ? "正在取消..." : "正在启动..." : "启动游戏");
        launch_->setIconName(running ? "refresh" : "play");
        launch_->setSubtitle(selected_.valid() ? selectedName_ : "尚未选择实例");
        const bool canLaunch = !starting_ && selected_.valid() && (tabs_->currentIndex() == 2 ? !offlineName_->text().trimmed().isEmpty() : !active_.isEmpty());
        launch_->setEnabled(!running && !accountsSwitching_ && !launchCommandPending_ && !launchAwaitingState_ && canLaunch);
        cancelLaunch_->setBusy(cancelCommandPending_);
        cancelLaunch_->setText(cancelling || cancelCommandPending_ ? "正在取消..." : "取消启动");
        cancelLaunch_->setEnabled(starting_ && !cancelling && !cancelCommandPending_ && !launchState_.value("launchId").toString().isEmpty());
        tabs_->setEnabled(!starting_ && !accountsSwitching_);
        versions_->setEnabled(!starting_); versionSettings_->setEnabled(!starting_ && selected_.valid());
    }
    void beginMicrosoft() {
        if (deviceDialog_) return; operationId_ = QUuid::createUuid().toString(QUuid::WithoutBraces); const QString operation = operationId_;
        auto* dialog = new Dialog(theme_, "登录 Minecraft", this); deviceDialog_ = dialog; dialog->resize(440, 280); dialog->confirmButton()->hide();
        deviceStatus_ = textLabel(theme_, "正在获取设备码…", 13, true, dialog); dialog->body()->addWidget(deviceStatus_);
        deviceCode_ = textLabel(theme_, {}, 31, false, dialog); deviceCode_->setAlignment(Qt::AlignCenter); deviceCode_->setFont(monoFont(31)); dialog->body()->addWidget(deviceCode_);
        auto* reopen = actionButton(theme_, "重新打开网页", "globe", ButtonKind::Ghost, dialog); auto* copy = actionButton(theme_, "复制代码", {}, ButtonKind::Ghost, dialog); dialog->actions()->insertWidget(1, reopen); dialog->actions()->insertWidget(2, copy);
        connect(copy, &QPushButton::clicked, dialog, [this] { QApplication::clipboard()->setText(deviceCode_->text()); });
        connect(reopen, &QPushButton::clicked, dialog, [this] { if (!verificationUrl_.isEmpty()) command(UiAction::OpenExternalUrl, {{"url", verificationUrl_}}); });
        connect(dialog, &QDialog::rejected, this, [this] { cancelMicrosoft(); }); dialog->open();
        command(UiAction::BeginMicrosoftLogin, {{"operationId", operation}}, [this, operation](const UiReply& reply) { if (!deviceDialog_ || operation != operationId_) return; if (reply.error.isError()) deviceDialog_->showError(reply.error); else updateMicrosoft(reply.data.toObject()); });
    }
    void updateMicrosoft(const QJsonObject& login) {
        if (!deviceDialog_ || (!login.value("operationId").toString().isEmpty() && login.value("operationId").toString() != operationId_)) return;
        sessionId_ = login.value("sessionId").toString(); verificationUrl_ = login.value("verificationUrl").toString(); deviceCode_->setText(login.value("code").toString());
        const QString status = login.value("state").toString(); deviceStatus_->setText(status == "saving" ? "正在保存账号…" : status == "waiting" ? "在浏览器中输入以下代码完成登录" : status == "error" ? login.value("message").toString() : "正在获取设备码…");
        if (status == "success") { deviceDialog_->accept(); deviceDialog_ = nullptr; operationId_.clear(); reloadAccounts(); }
    }
    void cancelMicrosoft() {
        if (operationId_.isEmpty()) return; const QJsonObject args{{"operationId", operationId_}, {"sessionId", sessionId_}}; operationId_.clear(); sessionId_.clear();
        if (deviceDialog_) { deviceDialog_->reject(); deviceDialog_ = nullptr; } command(UiAction::CancelMicrosoftLogin, args);
    }
    void accountManager() {
        if (starting_ || accountsSwitching_) return;
        if (accountDialog_) { accountDialog_->show(); return; }
        auto* dialog = new Dialog(theme_, "切换账号", this); accountDialog_ = dialog;
        dialog->setObjectName("accountSwitcherDialog"); dialog->resize(480, 440); dialog->confirmButton()->hide();
        connect(dialog, &QDialog::finished, this, [this, owner = QPointer<Dialog>(dialog)] { if (accountDialog_ == owner) accountDialog_ = nullptr; });
        auto* model = new ListModel(ListKind::Accounts, dialog); auto* list = new ListView(theme_, ListKind::Accounts, dialog);
        list->setObjectName("savedAccounts"); list->setAccessibleName("已保存的账号"); list->setSource(model); list->setSpacing(6); dialog->body()->addWidget(list);
        const auto updateRows = [model](const QJsonObject& state) {
            QVector<QJsonObject> rows;
            for (const auto& value : state.value("items").toArray()) {
                auto row = value.toObject(); row.insert("accountSwitchRow", true); row.insert("selected", row.value("id") == state.value("activeId")); rows.append(row);
            }
            std::stable_sort(rows.begin(), rows.end(), [](const QJsonObject& a, const QJsonObject& b) { return QString::localeAwareCompare(a.value("type").toString(), b.value("type").toString()) < 0; });
            QJsonArray items; for (const auto& row : rows) items.append(row); model->setItems(items);
        };
        updateRows(accounts_);
        connect(&bindings_.services(), &UiServices::mediaReady, model, &ListModel::setImage, Qt::QueuedConnection);
        connect(&bindings_.services(), &UiServices::stateChanged, dialog, [updateRows](UiTopic topic, const QJsonObject& state) { if (topic == UiTopic::Accounts) updateRows(state); }, Qt::QueuedConnection);
        const auto select = [this, dialog, list](const QJsonObject& item) {
            if (item.isEmpty() || dialog->isBusy() || accountsSwitching_) return;
            if (item.value("id") == accounts_.value("activeId")) { dialog->accept(); return; }
            accountMutationPending_ = true; accountsSwitching_ = true; updateLaunch(); dialog->setBusy(true); list->setEnabled(false);
            bindings_.command(UiAction::SetActiveAccount, {{"id", item.value("id")}, {"type", item.value("type")}}, dialog,
                [this, owner = QPointer<Dialog>(dialog), list](const UiReply& reply) {
                    accountMutationPending_ = false;
                    if (!owner) { reloadAccounts(); return; }
                    if (reply.error.isError()) { accountsSwitching_ = false; list->setEnabled(true); owner->showError(reply.error); updateLaunch(); }
                    else { active_ = {}; owner->setBusy(false); owner->accept(); reloadAccounts(); }
                });
        };
        connect(list, &QListView::clicked, dialog, [select](const QModelIndex& index) { select(index.data(ListModel::ObjectRole).toJsonObject()); });
        connect(list, &QListView::activated, dialog, [select](const QModelIndex& index) { select(index.data(ListModel::ObjectRole).toJsonObject()); });
        connect(list->nickelDelegate(), &ListDelegate::actionRequested, dialog, [this, dialog, list, updateRows](const QString& action, const QJsonObject& item) {
            if (action != "delete" || item.isEmpty() || dialog->isBusy() || accountsSwitching_) return;
            accountMutationPending_ = true; accountsSwitching_ = true; updateLaunch(); dialog->setBusy(true); list->setEnabled(false);
            bindings_.command(UiAction::RemoveAccount, {{"id", item.value("id")}}, dialog, [this, owner = QPointer<Dialog>(dialog), list, updateRows](const UiReply& reply) {
                accountMutationPending_ = false;
                if (!owner) { reloadAccounts(); return; }
                owner->setBusy(false); list->setEnabled(true);
                if (reply.error.isError()) { accountsSwitching_ = false; owner->showError(reply.error); updateLaunch(); }
                else { reloadAccounts(); bindings_.query(UiQuery::Accounts, {}, owner, [updateRows](const UiReply& result) { if (!result.error.isError()) updateRows(result.data.toObject()); }); }
            });
        });
        auto* other = actionButton(theme_, "登录其他账号", {}, ButtonKind::Primary, dialog); other->setObjectName("loginOtherAccount"); dialog->actions()->addWidget(other);
        connect(other, &QPushButton::clicked, dialog, [this, dialog, list] {
            if (dialog->isBusy() || accountsSwitching_) return;
            accountMutationPending_ = true; accountsSwitching_ = true; updateLaunch(); dialog->setBusy(true); list->setEnabled(false);
            bindings_.command(UiAction::SetActiveAccount, {{"id", ""}, {"type", accounts_.value("accountType")}}, dialog,
                [this, owner = QPointer<Dialog>(dialog), list](const UiReply& reply) {
                    accountMutationPending_ = false;
                    if (!owner) { reloadAccounts(); return; }
                    if (reply.error.isError()) { accountsSwitching_ = false; list->setEnabled(true); owner->showError(reply.error); updateLaunch(); }
                    else { active_ = {}; owner->setBusy(false); owner->accept(); reloadAccounts(); }
                });
        });
        dialog->open();
    }
    void characterDialog(const QJsonObject& outcome) {
        const QString session = outcome.value("sessionId").toString();
        if (characterDialog_ || session.isEmpty()) return;
        const bool switching = outcome.value("switchExisting").toBool();
        auto* dialog = new Dialog(theme_, switching ? "切换角色" : "选择角色", this); characterDialog_ = dialog;
        dialog->setObjectName("characterPickerDialog"); dialog->setMinimumWidth(420); dialog->resize(420, 360);
        connect(dialog, &QDialog::finished, this, [this, owner = QPointer<Dialog>(dialog)] { if (characterDialog_ == owner) characterDialog_ = nullptr; });
        auto* model = new ListModel(ListKind::Accounts, dialog); QJsonArray profiles;
        for (const auto& value : outcome.value("profiles").toArray()) {
            auto profile = value.toObject(); profile.insert("profileRow", true);
            profile.insert("selected", switching && profile.value("id").toString().remove('-').toLower() == active_.value("uuid").toString().remove('-').toLower());
            profiles.append(profile);
        }
        model->setItems(profiles); auto* list = new ListView(theme_, ListKind::Accounts, dialog);
        list->setObjectName("characterProfiles"); list->setAccessibleName("角色列表"); list->setSource(model);
        list->setSpacing(4); dialog->body()->addWidget(list);
        connect(&bindings_.services(), &UiServices::mediaReady, dialog, [model](const QString& key, const QImage& image) { model->setImage(key, image); }, Qt::QueuedConnection);
        if (model->rowCount()) list->setCurrentIndex(model->index(0));
        dialog->setConfirmText("确认"); dialog->confirmButton()->setEnabled(model->rowCount() > 0);
        const auto select = [this, dialog, list, session] {
            const QString profileId = list->currentObject().value("id").toString();
            if (profileId.isEmpty() || dialog->isBusy()) return;
            dialog->setBusy(true); list->setEnabled(false); const QPointer<Dialog> owner(dialog);
            bindings_.command(UiAction::SelectCharacter, {{"sessionId", session}, {"profileId", profileId}}, dialog, [this, owner, list](const UiReply& reply) {
                if (!owner) return;
                if (reply.error.isError()) { list->setEnabled(true); owner->showError(reply.error); }
                else { owner->setBusy(false); owner->accept(); reloadAccounts(); }
            });
        };
        connect(dialog, &Dialog::confirmed, dialog, select);
        connect(list, &QListView::activated, dialog, [select](const QModelIndex&) { select(); });
        if (switching) {
            dialog->confirmButton()->hide();
            auto* site = actionButton(theme_, "打开皮肤站角色页", "globe", ButtonKind::Primary, dialog); site->setObjectName("openSkinProfileSite");
            dialog->actions()->insertWidget(0, site);
            const QString accountId = outcome.value("accountId").toString();
            connect(site, &QPushButton::clicked, dialog, [this, dialog, accountId] {
                bindings_.command(UiAction::OpenExternalUrl, {{"accountId", accountId}, {"accountPage", "skinProfiles"}}, dialog,
                    [owner = QPointer<Dialog>(dialog)](const UiReply& reply) { if (owner && reply.error.isError()) owner->showError(reply.error); });
            });
            connect(list, &QListView::clicked, dialog, [select](const QModelIndex&) { select(); });
        }
        connect(dialog, &QDialog::rejected, this, [this, session] { command(UiAction::CancelCharacterSelection, {{"sessionId", session}}); });
        dialog->open();
    }
    void profileDialog() {
        if (active_.isEmpty()) return;
        if (tabs_->currentIndex() == 0) {
            command(UiAction::OpenExternalUrl, {{"url", "https://www.minecraft.net/zh-hans/msaprofile/mygames/editprofile"}});
        } else if (tabs_->currentIndex() == 1 && !characterDialog_) {
            const QString accountId = active_.value("id").toString();
            command(UiAction::RefreshCharacters, {{"accountId", accountId}}, [this, accountId](const UiReply& reply) {
                if (reply.error.isError()) { report(reply.error); return; }
                auto outcome = reply.data.toObject(); outcome.insert("switchExisting", true); outcome.insert("accountId", accountId);
                if (!isVisible() || active_.value("id").toString() != accountId) {
                    const QString session = outcome.value("sessionId").toString();
                    if (!session.isEmpty()) command(UiAction::CancelCharacterSelection, {{"sessionId", session}});
                    return;
                }
                characterDialog(outcome);
            });
        }
    }
    const std::array<QString, 3> accountTypes_{{"microsoft", "littleskin", "offline"}};
    Panel* card_; TabStrip* tabs_; QStackedWidget* body_; QStackedWidget* skinBody_;
    QWidget *identity_, *microsoft_, *skin_, *offline_, *progress_;
    LoadingCard* loading_; ImageView* avatar_; QLabel *name_, *email_, *who_, *progressTitle_, *progressText_, *deviceStatus_ = nullptr, *deviceCode_ = nullptr;
    LineEdit *username_, *password_; QLabel* offlineName_; Button *launch_, *cancelLaunch_, *versions_, *versionSettings_, *skinSubmit_;
    QJsonObject accounts_, active_, launchState_; InstanceIdentity selected_; QString selectedName_, operationId_, sessionId_, verificationUrl_, dismissedFailureKey_;
    QPointer<Dialog> deviceDialog_, characterDialog_, launchErrorDialog_, accountDialog_;
    quint64 launchRevision_ = 0;
    bool applying_ = false, starting_ = false, skinBusy_ = false, passwordStage_ = false, accountsSwitching_ = false, accountMutationPending_ = false;
    bool launchCommandPending_ = false, launchAwaitingState_ = false, cancelCommandPending_ = false;
};
}
RoutePage* createHomePage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent) { return new HomePage(theme, bindings, navigation, parent); }
} // namespace nickel::ui
