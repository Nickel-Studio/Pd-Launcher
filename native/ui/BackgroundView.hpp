#pragma once

#include "Theme.hpp"
#include <QImage>
#include <QPainter>
#include <QWidget>

namespace nickel::ui {
// GUI-thread only; Qt blurs a bounded image plane, never a foreground widget tree.
QImage blurRasterImage(const QImage& image, qreal radius);
class BackgroundView final : public QWidget {
    Q_OBJECT
public:
    explicit BackgroundView(Theme& theme, QWidget* parent = nullptr);
    bool setImage(const QImage& approvedImage);
    void setUserBackgroundSelected(bool selected);
    void refresh();
    const QImage& frame() const { return frame_; }
    const QImage& imageLayer() const { return imageLayer_; }
    void paintBackdrop(QPainter& painter, const QWidget& target);
signals:
    void frameChanged();
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool event(QEvent*) override;
private:
    void rebuildImageLayer(const QSize& pixels);
    Theme& theme_;
    QImage approvedImage_, imageLayer_, frame_, backdrop_;
    qint64 layerImageKey_ = 0;
    int layerBlur_ = -1;
    bool userBackgroundSelected_ = false;
};
bool paintWindowBackdrop(QPainter& painter, const QWidget& target);
} // namespace nickel::ui
