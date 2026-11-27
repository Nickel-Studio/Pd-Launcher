#pragma once

#include "Theme.hpp"
#include <QByteArray>
#include <QPainter>
#include <QRectF>

namespace nickel::ui {
// Generated SVG for the window background/titlebar. Motifs are migrated legacy path data:
// tune opacity/tokens here instead of hand-editing the SVG geometry.
enum class ArtworkPlacement { Background, Titlebar };
QByteArray themeArtworkSvg(const Appearance& appearance, const ThemeTokens& tokens, ArtworkPlacement placement);
void paintThemeArtwork(QPainter& painter, const QRectF& target, const Appearance& appearance,
                       const ThemeTokens& tokens, ArtworkPlacement placement);
} // namespace nickel::ui
