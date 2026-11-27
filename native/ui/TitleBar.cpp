#include "TitleBar.hpp"
#include "BackgroundView.hpp"
#include "PageShell.hpp"
#include "ThemeArtwork.hpp"
#include "ProductIdentity.hpp"
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QWindow>
#include <algorithm>

namespace nickel::ui {
TitleBar::TitleBar(Theme& theme, Navigation& navigation, QWidget* parent)
    : QWidget(parent), theme_(theme), navigation_(navigation),
      brand_(textLabel(theme, productDisplayName(), 19, false, this)),
      tabs_(new TabStrip(theme, this)),
      minimize_(actionButton(theme, {}, "minus", ButtonKind::Ghost, this)),
      close_(actionButton(theme, {}, "close", ButtonKind::Ghost, this)) {
    setObjectName("titleBar"); setFixedHeight(52); setAutoFillBackground(false);
    QFont font = displayFont(19, QFont::Bold); font.setLetterSpacing(QFont::AbsoluteSpacing, .855);
    brand_->setObjectName("productBrand"); brand_->setFont(font); brand_->setTextInteractionFlags(Qt::NoTextInteraction);
    brand_->setAttribute(Qt::WA_TransparentForMouseEvents);
    tabs_->setObjectName("mainNavigation"); tabs_->setAccessibleName("主导航");
    tabs_->addItem("首页", "home"); tabs_->addItem("资源", "resources");
    tabs_->addItem("设置", "settings"); tabs_->addItem("更多", "versions");
    minimize_->setObjectName("minimizeWindow"); minimize_->setRadius(8);
    close_->setObjectName("closeWindow"); close_->setRadius(8);
    minimize_->setAccessibleName("最小化"); minimize_->setToolTip("最小化");
    close_->setAccessibleName("关闭"); close_->setToolTip("关闭");
    minimize_->setMinimumHeight(32); close_->setMinimumHeight(32);
    connect(minimize_, &QPushButton::clicked, this, [this] { window()->showMinimized(); });
    connect(close_, &QPushButton::clicked, this, [this] { window()->close(); });
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        static constexpr Route routes[] = {Route::Home, Route::Resources, Route::Settings, Route::More};
        if (index >= 0 && index < 4) navigation_.navigate(routes[index]);
    });
    connect(&navigation_, &Navigation::routeChanged, this, [this](Route) {
        const int index = navigation_.tabIndex(); QSignalBlocker block(tabs_);
        if (index >= 0) tabs_->setCurrentIndex(index);
        tabs_->setHighlightVisible(index >= 0);
    });
    connect(&theme_, &Theme::changed, this, qOverload<>(&QWidget::update));
}
void TitleBar::paintEvent(QPaintEvent*) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    paintWindowBackdrop(painter, *this);
    painter.fillRect(rect(), theme_.tokens().chrome);
    const qreal artworkWidth = std::min(168., width() / 2. - 355.);
    if (width() > 820 && artworkWidth > 0)
        paintThemeArtwork(painter, QRectF(width() / 2. - 208. - artworkWidth, 0, artworkWidth, height()),
                          theme_.appearance(), theme_.tokens(), ArtworkPlacement::Titlebar);
}
void TitleBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    brand_->setGeometry(20, 0, 132, 52);
    const QSize size = tabs_->sizeHint(); tabs_->setGeometry((width() - size.width()) / 2, 7, size.width(), 38);
    minimize_->setGeometry(width() - 92, 10, 40, 32); close_->setGeometry(width() - 48, 10, 40, 32);
}
void TitleBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && window()->windowHandle()) {
        if (window()->windowHandle()->startSystemMove()) { event->accept(); return; }
    }
    QWidget::mousePressEvent(event);
}
void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized(); event->accept(); return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
} // namespace nickel::ui
