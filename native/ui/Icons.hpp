#pragma once

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QString>

namespace nickel::ui {
// Geometry matches the removed legacy React icon set; keys are stable. Unknown names return
// an empty SVG or null image, so callers must draw their own fallback instead of a glyph.
QByteArray iconSvg(const QString& name, const QColor& color);
void paintIcon(QPainter& painter, const QRectF& target, const QString& name, const QColor& color);
QImage functionalImage(const QString& key);
bool isPixelArtResource(const QString& key);
void paintContainedImage(QPainter& painter, const QRectF& target, const QImage& image, bool pixelated = false);
} // namespace nickel::ui
