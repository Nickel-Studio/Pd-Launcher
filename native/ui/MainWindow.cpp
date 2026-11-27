#include "MainWindow.hpp"
#include "Dialogs.hpp"
#include "DownloadBubble.hpp"
#include "Pages.hpp"
#include "StartupGate.hpp"
#include "ProductIdentity.hpp"
#include "TitleBar.hpp"
#include <QApplication>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QShowEvent>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <limits>

namespace nickel::ui {
MainWindow::MainWindow(UiServices& services, QWidget* parent)
    : QWidget(parent), services_(services), theme_(this), bindings_(services, this), navigation_(this) {
    setObjectName("nickelMainWindow"); setWindowTitle(productDisplayName());
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint);
    setAttribute(Qt::WA_TranslucentBackground); setAutoFillBackground(false); setMouseTracking(true);
    setMinimumSize(900, 600); resize(1100, 720); setFont(bodyFont()); setPalette(theme_.palette());
    background_ = new BackgroundView(theme_, this); background_->setGeometry(rect()); background_->lower();
    titlebar_ = new TitleBar(theme_, navigation_, this);
    pages_ = new QStackedWidget(this); pages_->setObjectName("routeHost"); pages_->setAutoFillBackground(false);
    downloads_ = new DownloadBubble(theme_, bindings_, navigation_, this);
    startup_ = new StartupGate(theme_, bindings_, this);
    pages_->setEnabled(false); titlebar_->setEnabled(false);
    connect(startup_, &StartupGate::ready, this, [this] {
        if (ready_) return; ready_ = true; titlebar_->setEnabled(true); pages_->setEnabled(true);
        if (auto* page = routePage(activeRoute_)) page->enter(); emit startupReady();
    });
    connect(startup_, &StartupGate::rejectionFinished, this, &QWidget::close);
    connect(&navigation_, &Navigation::routeChanged, this, &MainWindow::switchRoute);
    connect(&services_, &UiServices::stateChanged, this, &MainWindow::receiveSnapshot, Qt::QueuedConnection);
    connect(&services_, &UiServices::mediaReady, this, [this](const QString& key, const QImage& image) {
        if (!backgroundKey_.isEmpty() && key == backgroundKey_) setBackgroundImage(image);
    }, Qt::QueuedConnection);
    connect(&bindings_, &Bindings::failure, this, &MainWindow::displayError);
    connect(&theme_, &Theme::changed, this, [this] {
        setPalette(theme_.palette()); setWindowOpacity(theme_.appearance().windowOpacity);
        update(); titlebar_->update(); pages_->update();
    });
    connect(background_, &BackgroundView::frameChanged, this, [this] {
        titlebar_->update(); for (auto* panel : findChildren<Panel*>()) panel->update();
    });
    auto* back = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
    connect(back, &QShortcut::activated, this, [this] {
        if (!ready_) return;
        if (activeRoute_ == Route::DownloadDetail) navigation_.returnFromDownloads();
        else if (activeRoute_ == Route::VersionSettings) navigation_.backToVersions();
        else if (activeRoute_ == Route::Resources && navigation_.currentDetail()) navigation_.popDetail();
        else if (activeRoute_ == Route::Versions) navigation_.navigate(Route::Home);
    });
    switchRoute(Route::Home);
    QTimer::singleShot(0, this, [this] { bootstrap(); startup_->start(); });
}
MainWindow::~MainWindow() {
    bindings_.forget(this);
    delete startup_; delete downloads_; delete toast_.data();
    // Child RoutePage destructors still need Bindings; destroy them before members.
    delete pages_; routes_.fill(nullptr); delete titlebar_; delete background_;
}
RoutePage* MainWindow::routePage(Route route) const { return routes_.at(static_cast<std::size_t>(route)); }
QImage MainWindow::captureRenderedFrame(qsizetype maximumWorkingBytes) {
    if (!qApp || QThread::currentThread() != qApp->thread()) return {};
    constexpr qsizetype maximumCaptureBytes = 64 * 1024 * 1024;
    constexpr qsizetype workingBytesPerPixel = 8;
    if (maximumWorkingBytes <= 0 || maximumWorkingBytes > maximumCaptureBytes
        || capturingFrame_ || paintingFrame_) return {};
    const auto platform = QGuiApplication::platformName();
    if (platform == QStringLiteral("offscreen") || platform.startsWith(QStringLiteral("minimal"))) return {};
    if (!isVisible() || isMinimized() || !windowHandle() || !windowHandle()->isExposed()) return {};
    const qreal ratio = devicePixelRatioF();
    if (!std::isfinite(ratio) || ratio <= 0) return {};
    const qreal physicalWidth = std::ceil(qreal(width()) * ratio);
    const qreal physicalHeight = std::ceil(qreal(height()) * ratio);
    if (physicalWidth <= 0 || physicalHeight <= 0
        || physicalWidth > std::numeric_limits<int>::max() || physicalHeight > std::numeric_limits<int>::max()) return {};
    const auto widthPixels = static_cast<qsizetype>(physicalWidth);
    const auto heightPixels = static_cast<qsizetype>(physicalHeight);
    const auto pixelBudget = maximumWorkingBytes / workingBytesPerPixel;
    if (widthPixels > pixelBudget || heightPixels > pixelBudget / widthPixels) return {};
    QScopedValueRollback<bool> capturing(capturingFrame_, true);
    const auto pixmap = grab(rect());
    if (pixmap.isNull() || pixmap.width() > widthPixels || pixmap.height() > heightPixels || pixmap.depth() > 32) return {};
    auto image = pixmap.toImage();
    if (image.isNull() || image.depth() > 32 || image.sizeInBytes() > maximumWorkingBytes / 2) return {};
    return image;
}
QJsonObject MainWindow::observedVisualState() const {
    if (!qApp || QThread::currentThread() != qApp->thread()) return {};
    const auto& appearance = theme_.appearance(); const auto& tokens = theme_.tokens();
    const auto rectangle = [this](const QWidget* widget) -> QJsonObject {
        if (!widget) return {};
        const QPoint origin = widget == this ? QPoint{} : widget->mapTo(this, QPoint{});
        return {{"x", origin.x()}, {"y", origin.y()}, {"width", widget->width()}, {"height", widget->height()}};
    };
    QJsonObject result{{"route", routeKey(activeRoute_)}, {"startupReady", ready_},
        {"themeMode", appearance.mode == ThemeMode::Light ? "light" : appearance.mode == ThemeMode::Dark ? "dark" : "system"},
        {"effectiveThemeMode", tokens.dark ? "dark" : "light"}, {"accent", accentKey(appearance.accent)},
        {"reducedMotion", theme_.reducedMotion()}, {"shadows", appearance.shadows},
        {"titlebarTint", appearance.titlebarTint}, {"backgroundBlur", appearance.backgroundBlur},
        {"backgroundOpacity", appearance.backgroundOpacity}, {"windowOpacity", appearance.windowOpacity},
        {"userBackgroundSelected", !backgroundKey_.isEmpty() || !settings_.value("backgroundImage").toString().trimmed().isEmpty()},
        {"visible", isVisible()}, {"minimized", isMinimized()}, {"maximized", isMaximized()},
        {"widgetDpr", devicePixelRatioF()}, {"window", rectangle(this)},
        {"titlebar", rectangle(titlebar_)}, {"routeHost", rectangle(pages_)},
        {"colors", QJsonObject{{"background", tokens.background.name()}, {"surface", tokens.surface.name()},
                              {"text", tokens.text.name()}, {"muted", tokens.muted.name()}}}};
    if (const auto* page = routePage(activeRoute_)) {
        result.insert("pageObjectName", page->objectName());
        if (const auto* pager = page->findChild<Pager*>()) {
            result.insert("section", pager->selected());
            result.insert("pagerSidebar", rectangle(pager->findChild<QWidget*>("pagerSidebar")));
            result.insert("pagerContent", rectangle(pager->content()));
        }
        if (activeRoute_ == Route::Home) result.insert("homeCard", rectangle(page->findChild<QWidget*>("homeCard")));
        if (activeRoute_ == Route::More) {
            if (const auto* pager = page->findChild<Pager*>(); pager && pager->selected() == QStringLiteral("about")) {
                const auto* about = pager->page("about");
                result.insert("aboutIntro", rectangle(about->findChild<QWidget*>("aboutIntro")));
                result.insert("aboutIdentity", rectangle(about->findChild<QWidget*>("aboutIdentity")));
                result.insert("aboutMark", rectangle(about->findChild<QWidget*>("aboutBrandMark")));
                result.insert("aboutMarkResource", tokens.dark ? ":/nickel/brand/mark-white.png" : ":/nickel/brand/mark-black.png");
                if (const auto* name = about->findChild<QLabel*>("aboutProductName"))
                    result.insert("aboutNamePixelSize", name->font().pixelSize());
            }
        }
    }
    return result;
}
void MainWindow::switchRoute(Route route) {
    if (auto* old = routePage(activeRoute_); old && route != activeRoute_) old->leave();
    auto*& page = routes_.at(static_cast<std::size_t>(route));
    if (!page) {
        switch (route) {
        case Route::Home: page = createHomePage(theme_, bindings_, navigation_, pages_); break;
        case Route::Versions: page = createVersionsPage(theme_, bindings_, navigation_, pages_); break;
        case Route::VersionSettings: page = createVersionSettingsPage(theme_, bindings_, navigation_, pages_); break;
        case Route::Resources: page = createResourcesPage(theme_, bindings_, navigation_, pages_); break;
        case Route::Settings: page = createSettingsPage(theme_, bindings_, navigation_, pages_); break;
        case Route::More: page = createMorePage(theme_, bindings_, navigation_, pages_); break;
        case Route::DownloadDetail: page = createDownloadDetailPage(theme_, bindings_, navigation_, pages_); break;
        }
        pages_->addWidget(page); connect(page, &RoutePage::errorReported, this, &MainWindow::displayError);
    }
    activeRoute_ = route; pages_->setCurrentWidget(page); if (ready_) page->enter();
    if (downloads_) downloads_->raise(); if (startup_ && !ready_) startup_->raise();
}
void MainWindow::bootstrap() {
    if (bootstrapped_) return; bootstrapped_ = true;
    bindings_.query(UiQuery::Bootstrap, {}, this, [this](const UiReply& reply) {
        if (reply.error.isError()) { displayError(reply.error); return; }
        const auto state = reply.data.toObject();
        if (state.value("settings").isObject()) applySettings(state.value("settings").toObject());
        if (state.value("accounts").isObject()) receiveSnapshot(UiTopic::Accounts, state.value("accounts").toObject());
        if (state.value("selectedInstance").isObject()) receiveSnapshot(UiTopic::Instances, {{"selectedInstance", state.value("selectedInstance")}});
        if (state.value("downloads").isObject()) receiveSnapshot(UiTopic::Downloads, state.value("downloads").toObject());
        if (state.value("launch").isObject()) receiveSnapshot(UiTopic::Launch, state.value("launch").toObject());
    });
}
void MainWindow::receiveSnapshot(UiTopic topic, const QJsonObject& snapshot) {
    if (topic == UiTopic::Onboarding) { startup_->apply(snapshot); return; }
    if (topic == UiTopic::Settings && snapshot.value("section").toString() != "game")
        applySettings(snapshot.value("values").isObject() ? snapshot.value("values").toObject() : snapshot);
    if (topic == UiTopic::Downloads) downloads_->apply(snapshot);
    for (auto* page : routes_) if (page) page->snapshot(topic, snapshot);
}
void MainWindow::applySettings(const QJsonObject& settings) {
    settings_ = settings; Appearance appearance = theme_.appearance();
    const QString mode = settings.value("themeMode").toString();
    if (!mode.isEmpty()) appearance.mode = mode == "light" ? ThemeMode::Light : mode == "dark" ? ThemeMode::Dark : ThemeMode::System;
    if (settings.contains("accent")) appearance.accent = accentFromKey(settings.value("accent").toString());
    if (settings.contains("titlebarTint")) appearance.titlebarTint = settings.value("titlebarTint").toBool();
    if (settings.contains("reducedMotion")) appearance.reducedMotion = settings.value("reducedMotion").toBool();
    if (settings.contains("shadows")) appearance.shadows = settings.value("shadows").toBool();
    if (settings.contains("backgroundBlur")) appearance.backgroundBlur = settings.value("backgroundBlur").toInt();
    if (settings.contains("backgroundOpacity")) appearance.backgroundOpacity = settings.value("backgroundOpacity").toDouble();
    if (settings.contains("windowOpacity")) appearance.windowOpacity = settings.value("windowOpacity").toDouble();
    const auto custom = settings.value("customTheme").toObject();
    if (custom.contains("hue")) appearance.custom.hue = custom.value("hue").toDouble();
    if (custom.contains("saturation")) appearance.custom.saturation = custom.value("saturation").toDouble();
    if (custom.contains("lightness")) appearance.custom.lightness = custom.value("lightness").toDouble();
    if (custom.contains("titlebarHueShift")) appearance.custom.titlebarHueShift = custom.value("titlebarHueShift").toDouble();
    if (custom.contains("decorationOpacity")) appearance.custom.decorationOpacity = custom.value("decorationOpacity").toDouble();
    const QString decoration = custom.value("decoration").toString();
    if (!decoration.isEmpty()) appearance.custom.decoration = decoration == "none" ? Decoration::None : decoration == "ribbons" ? Decoration::Ribbons : decoration == "orbits" ? Decoration::Orbits : decoration == "folds" ? Decoration::Folds : Decoration::Lattice;
    if (settings.contains("backgroundKey")) {
        const QString key = settings.value("backgroundKey").toString();
        if (key != backgroundKey_) { backgroundKey_ = key; setBackgroundImage({}); }
    }
    if (settings.contains("backgroundImage") && settings.value("backgroundImage").isNull()) { backgroundKey_.clear(); setBackgroundImage({}); }
    background_->setUserBackgroundSelected(!backgroundKey_.isEmpty() || !settings.value("backgroundImage").toString().trimmed().isEmpty());
    theme_.setAppearance(appearance);
}
void MainWindow::setBackgroundImage(const QImage& image) {
    if (!background_->setImage(image)) displayError({"image.budget", "背景图片超出解码预算", {}, false});
}
void MainWindow::displayError(const UiError& error) {
    if (!error.isError()) return; delete toast_.data();
    auto* notice = new Panel(theme_, this, SurfaceKind::Glass); notice->setObjectName("errorToast");
    notice->setAccessibleName("操作失败"); notice->setShadow(2);
    notice->column(16, 4)->addWidget(textLabel(theme_, error.message, 12));
    if (!error.detail.isEmpty()) notice->layout()->addWidget(textLabel(theme_, error.detail, 11, true));
    notice->setFixedWidth(std::min(460, width() - 40)); notice->adjustSize(); notice->move(20, height() - notice->height() - 20);
    toast_ = notice; notice->show(); notice->raise(); if (!ready_) startup_->raise();
    QTimer::singleShot(8000, notice, &QObject::deleteLater);
}
void MainWindow::paintEvent(QPaintEvent*) {
    QScopedValueRollback<bool> painting(paintingFrame_, true);
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath shell; shell.addRoundedRect(QRectF(rect()), isMaximized() ? 0 : 14, isMaximized() ? 0 : 14);
    painter.fillPath(shell, theme_.tokens().background);
}
void MainWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event); if (!pages_) return;
    background_->setGeometry(rect()); background_->lower();
    titlebar_->setGeometry(0, 0, width(), 52); pages_->setGeometry(20, 72, width() - 40, height() - 92);
    startup_->setGeometry(rect()); downloads_->setGeometry(width() - 420, height() - 84, 400, 64);
    if (toast_) toast_->move(20, height() - toast_->height() - 20);
}
void MainWindow::showEvent(QShowEvent* event) { QWidget::showEvent(event); theme_.setWindowVisible(!isMinimized()); if (!ready_) startup_->raise(); }
void MainWindow::hideEvent(QHideEvent* event) { theme_.setWindowVisible(false); QWidget::hideEvent(event); }
Qt::Edges MainWindow::resizeEdges(const QPoint& point) const {
    if (isMaximized()) return {}; Qt::Edges edges;
    if (point.x() < 6) edges |= Qt::LeftEdge; else if (point.x() >= width() - 6) edges |= Qt::RightEdge;
    if (point.y() < 6) edges |= Qt::TopEdge; else if (point.y() >= height() - 6) edges |= Qt::BottomEdge;
    return edges;
}
void MainWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        resizing_ = resizeEdges(event->position().toPoint());
        if (resizing_) {
            if (windowHandle() && windowHandle()->startSystemResize(resizing_)) { resizing_ = {}; event->accept(); return; }
            resizeStart_ = event->globalPosition().toPoint(); resizeGeometry_ = geometry(); event->accept(); return;
        }
    }
    QWidget::mousePressEvent(event);
}
void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    if (resizing_) {
        const QPoint delta = event->globalPosition().toPoint() - resizeStart_; QRect next = resizeGeometry_;
        if (resizing_.testFlag(Qt::LeftEdge)) next.setLeft(std::min(resizeGeometry_.right() - minimumWidth() + 1, resizeGeometry_.left() + delta.x()));
        if (resizing_.testFlag(Qt::RightEdge)) next.setRight(std::max(resizeGeometry_.left() + minimumWidth() - 1, resizeGeometry_.right() + delta.x()));
        if (resizing_.testFlag(Qt::TopEdge)) next.setTop(std::min(resizeGeometry_.bottom() - minimumHeight() + 1, resizeGeometry_.top() + delta.y()));
        if (resizing_.testFlag(Qt::BottomEdge)) next.setBottom(std::max(resizeGeometry_.top() + minimumHeight() - 1, resizeGeometry_.bottom() + delta.y()));
        setGeometry(next); event->accept(); return;
    }
    const auto edges = resizeEdges(event->position().toPoint());
    const bool horizontal = edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge);
    const bool vertical = edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge);
    setCursor(horizontal && vertical ? ((edges.testFlag(Qt::LeftEdge) == edges.testFlag(Qt::TopEdge)) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor)
        : horizontal ? Qt::SizeHorCursor : vertical ? Qt::SizeVerCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(event);
}
void MainWindow::mouseReleaseEvent(QMouseEvent* event) { resizing_ = {}; QWidget::mouseReleaseEvent(event); }
void MainWindow::keyPressEvent(QKeyEvent* event) { QWidget::keyPressEvent(event); }
void MainWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event); if (event->type() == QEvent::WindowStateChange) theme_.setWindowVisible(isVisible() && !isMinimized());
}
} // namespace nickel::ui
