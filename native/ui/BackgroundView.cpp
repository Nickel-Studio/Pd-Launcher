#include "BackgroundView.hpp"
#include "ThemeArtwork.hpp"
#include <QEvent>
#include <QGraphicsBlurEffect>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainterPath>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace nickel::ui {
namespace {
constexpr qint64 maximumRenderPixels = 4 * 1024 * 1024;
QSize renderSize(const QSize& logical, qreal dpr) {
    if (logical.isEmpty()) return {};
    if (!std::isfinite(dpr)) dpr = 1;
    // Reserve the CSS inset -8% / scale(1.05) overscan before allocating planes.
    const qreal scale = std::min({std::clamp(dpr, 1., 4.), 4096. / logical.width(), 4096. / logical.height(),
        std::sqrt(double(maximumRenderPixels) / (double(logical.width()) * logical.height() * 1.5))});
    return {std::max(1, int(std::floor(logical.width() * scale))), std::max(1, int(std::floor(logical.height() * scale)))};
}
QColor alpha(QColor color, qreal value) { color.setAlphaF(value); return color; }
void ellipseGradient(QPainter& painter, const QRectF& frame, const QPointF& origin, const QSizeF& radii,
                     const QGradientStops& stops) {
    painter.save(); painter.translate(frame.width() * origin.x(), frame.height() * origin.y());
    painter.scale(frame.width() * radii.width(), frame.height() * radii.height());
    QRadialGradient gradient(QPointF(0, 0), 1); gradient.setStops(stops);
    const QRectF bounds(-origin.x() / radii.width(), -origin.y() / radii.height(), 1. / radii.width(), 1. / radii.height());
    painter.fillRect(bounds, gradient); painter.restore();
}
}
QImage blurRasterImage(const QImage& image, qreal radius) {
    if (image.isNull() || !std::isfinite(radius) || radius < 0
        || qint64(image.width()) * image.height() > maximumRenderPixels) return {};
    if (radius == 0) return image;
    if (!qGuiApp || QThread::currentThread() != qGuiApp->thread()) return {};
    QGraphicsScene scene;
    QPixmap pixmap = QPixmap::fromImage(image); pixmap.setDevicePixelRatio(1);
    auto* item = scene.addPixmap(pixmap); item->setTransformationMode(Qt::SmoothTransformation);
    auto* effect = new QGraphicsBlurEffect; effect->setBlurHints(QGraphicsBlurEffect::QualityHint);
    effect->setBlurRadius(std::min(radius, 160.)); item->setGraphicsEffect(effect);
    const QRectF source(image.rect()); scene.setSceneRect(source);
    QImage result(image.size(), QImage::Format_ARGB32_Premultiplied); if (result.isNull()) return {};
    result.fill(Qt::transparent); QPainter painter(&result);
    scene.render(&painter, QRectF(result.rect()), source, Qt::IgnoreAspectRatio);
    return result;
}
BackgroundView::BackgroundView(Theme& theme, QWidget* parent) : QWidget(parent), theme_(theme) {
    setObjectName("windowBackground"); setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground); setAutoFillBackground(false); setFocusPolicy(Qt::NoFocus);
    connect(&theme_, &Theme::changed, this, &BackgroundView::refresh);
}
bool BackgroundView::setImage(const QImage& image) {
    if (!image.isNull() && (image.sizeInBytes() > 64 * 1024 * 1024 || image.width() > 8192 || image.height() > 8192)) return false;
    if (approvedImage_.cacheKey() == image.cacheKey()) return true;
    approvedImage_ = image; imageLayer_ = {}; layerImageKey_ = 0; refresh(); return true;
}
void BackgroundView::setUserBackgroundSelected(bool selected) {
    if (userBackgroundSelected_ == selected) return;
    userBackgroundSelected_ = selected; refresh();
}
void BackgroundView::rebuildImageLayer(const QSize& pixels) {
    const int blur = theme_.appearance().backgroundBlur;
    if (approvedImage_.isNull()) { imageLayer_ = {}; layerImageKey_ = 0; return; }
    if (imageLayer_.size() == pixels && layerImageKey_ == approvedImage_.cacheKey() && layerBlur_ == blur) return;
    const qreal scale = std::max(double(pixels.width()) / width(), double(pixels.height()) / height());
    const qreal radius = blur * scale;
    const int padX = std::max(int(std::ceil(pixels.width() * .109)), int(std::ceil(radius * 2.5 + 2)));
    const int padY = std::max(int(std::ceil(pixels.height() * .109)), int(std::ceil(radius * 2.5 + 2)));
    const QSize expanded(pixels.width() + 2 * padX, pixels.height() + 2 * padY);
    if (qint64(expanded.width()) * expanded.height() > maximumRenderPixels) { imageLayer_ = {}; return; }
    QImage covered(expanded, QImage::Format_ARGB32_Premultiplied); if (covered.isNull()) { imageLayer_ = {}; return; }
    covered.fill(Qt::transparent);
    {
        QPainter painter(&covered); painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const qreal cover = std::max(double(expanded.width()) / approvedImage_.width(), double(expanded.height()) / approvedImage_.height());
        const QSizeF size(approvedImage_.width() * cover, approvedImage_.height() * cover);
        painter.drawImage(QRectF(QPointF((expanded.width() - size.width()) / 2, (expanded.height() - size.height()) / 2), size), approvedImage_);
    }
    const QImage blurred = blurRasterImage(covered, radius);
    imageLayer_ = blurred.copy(padX, padY, pixels.width(), pixels.height());
    layerImageKey_ = approvedImage_.cacheKey(); layerBlur_ = blur;
}
void BackgroundView::refresh() {
    const QSize pixels = renderSize(size(), devicePixelRatioF()); if (pixels.isEmpty()) return;
    rebuildImageLayer(pixels); backdrop_ = {};
    frame_ = QImage(pixels, QImage::Format_ARGB32_Premultiplied); if (frame_.isNull()) return;
    const auto& tokens = theme_.tokens(); frame_.fill(tokens.background);
    const QRectF canvas(frame_.rect());
    {
        QPainter painter(&frame_); painter.setRenderHint(QPainter::Antialiasing);
        ellipseGradient(painter, canvas, {.5, -.14}, {1.30, .82}, {{0, tokens.background2}, {.7, alpha(tokens.background2, 0)}, {1, alpha(tokens.background2, 0)}});
        if (!userBackgroundSelected_ && approvedImage_.isNull())
            paintThemeArtwork(painter, canvas, theme_.appearance(), tokens, ArtworkPlacement::Background);
        if (!imageLayer_.isNull()) {
            painter.setOpacity(theme_.appearance().backgroundOpacity); painter.drawImage(0, 0, imageLayer_); painter.setOpacity(1);
            QLinearGradient scrim(0, 0, 0, canvas.height());
            scrim.setColorAt(0, alpha(tokens.background, .24)); scrim.setColorAt(.52, alpha(tokens.background, .42)); scrim.setColorAt(1, alpha(tokens.background, .66));
            painter.fillRect(canvas, scrim);
            ellipseGradient(painter, canvas, {.5, .06}, {1.20, .96}, {{0, alpha(tokens.background, 0)}, {.34, alpha(tokens.background, 0)}, {1, alpha(tokens.background, .46)}});
        }
    }
    update(); emit frameChanged();
}
void BackgroundView::paintBackdrop(QPainter& painter, const QWidget& target) {
    if (frame_.isNull() || width() <= 0 || height() <= 0 || target.window() != window()) return;
    const qreal scaleX = double(frame_.width()) / width(), scaleY = double(frame_.height()) / height();
    if (backdrop_.isNull()) backdrop_ = blurRasterImage(frame_, 22 * std::max(scaleX, scaleY));
    if (backdrop_.isNull()) return;
    // mapTo accepts ancestors, not siblings; both layers share the window origin.
    const QPoint offset = target.mapTo(window(), QPoint(0, 0)) - mapTo(window(), QPoint(0, 0));
    painter.save(); painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRectF(target.rect()), backdrop_, QRectF(offset.x() * scaleX, offset.y() * scaleY, target.width() * scaleX, target.height() * scaleY));
    painter.restore();
}
void BackgroundView::paintEvent(QPaintEvent*) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const qreal radius = window()->isMaximized() ? 0 : 14;
    QPainterPath clip; clip.addRoundedRect(QRectF(rect()), radius, radius); painter.setClipPath(clip);
    if (frame_.isNull()) painter.fillPath(clip, theme_.tokens().background);
    else painter.drawImage(QRectF(rect()), frame_);
}
void BackgroundView::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); refresh(); }
bool BackgroundView::event(QEvent* event) {
    const bool accepted = QWidget::event(event);
    if (event->type() == QEvent::DevicePixelRatioChange) refresh();
    return accepted;
}
bool paintWindowBackdrop(QPainter& painter, const QWidget& target) {
    auto* background = target.window()->findChild<BackgroundView*>("windowBackground", Qt::FindDirectChildrenOnly);
    if (!background || background->frame().isNull()) return false;
    background->paintBackdrop(painter, target); return true;
}
} // namespace nickel::ui
