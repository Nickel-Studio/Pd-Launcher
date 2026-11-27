#include "ThemeArtwork.hpp"
#include <QCache>
#include <QImage>
#include <QLinearGradient>
#include <QSvgRenderer>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace nickel::ui {
namespace {
enum class Motif { None, Lattice, Ribbons, Abyss, Folds, Orbits };
Motif motifFor(const Appearance& appearance) {
    switch (appearance.accent) {
    case Accent::Nickel: return Motif::Lattice;
    case Accent::Copper: return Motif::Ribbons;
    case Accent::Abyss: return Motif::Abyss;
    case Accent::Paper: return Motif::Folds;
    case Accent::FortyTwo: return Motif::Orbits;
    case Accent::Custom:
        switch (appearance.custom.decoration) {
        case Decoration::Lattice: return Motif::Lattice;
        case Decoration::Ribbons: return Motif::Ribbons;
        case Decoration::Orbits: return Motif::Orbits;
        case Decoration::Folds: return Motif::Folds;
        case Decoration::None: return Motif::None;
        }
        return Motif::None;
    default: return Motif::None;
    }
}
// Both titlebar and background motif path data are migrated verbatim from the removed legacy
// ThemeArtwork.tsx reference; do not hand-redraw or approximate these paths.
QByteArray titlebarMotif(Motif motif) {
    switch (motif) {
    case Motif::Lattice: return R"(<g fill="none" stroke="currentColor" stroke-width="1"><path d="M24 44 52 14 86 24 112 5 143 29 179 12 219 34M24 44 86 24 143 29 219 34M52 14 65 51 112 5 143 29 159 55 219 34"/><path d="m179 12 8 31-28 12"/><circle cx="86" cy="24" r="3" fill="currentColor"/><circle cx="143" cy="29" r="2" fill="currentColor"/></g>)";
    case Motif::Ribbons: return R"(<g fill="none" stroke="currentColor"><path d="M-8 51C55 61 52 4 117 14S174 63 246 20" stroke-width="8" opacity="0.2"/><path d="M-8 45C55 55 52-2 117 8S174 57 246 14M-8 60C55 70 52 13 117 23S174 72 246 29" stroke-width="1.2"/><path d="M178 5v9m-4-5h8M56 47v8m-4-4h8" stroke-width="1"/></g>)";
    case Motif::Abyss: return R"(<g fill="none" stroke="currentColor" stroke-width="1.2"><path d="M-10 35Q35 8 78 35T166 35 254 35M-10 43Q35 16 78 43T166 43 254 43M-10 51Q35 24 78 51T166 51 254 51"/><circle cx="167" cy="11" r="2" fill="currentColor"/><path d="M182 14h14m-7-7v14" opacity="0.5"/></g>)";
    case Motif::Folds: return R"(<g stroke="currentColor" stroke-width="1"><path d="M55 0v16m77-16v12m59-12v23" fill="none"/><path d="m39 24 16-8 20 9-6 24-24-2Zm78-7 15-5 15 7-5 25-22-5Zm58 14 16-8 17 9-6 22-22-2Z" fill="currentColor" fill-opacity="0.1"/><path d="m55 16-10 31m10-31 14 33m63-37-12 27m12-27 10 32m49-21-11 29" fill="none"/></g>)";
    case Motif::Orbits: return R"svg(<g fill="none" stroke="currentColor" stroke-width="1"><ellipse cx="112" cy="32" rx="77" ry="20" transform="rotate(-9 112 32)"/><ellipse cx="112" cy="32" rx="47" ry="29" transform="rotate(23 112 32)"/><path d="m103 21-11 16h15m-2-18v26m10-20q10-12 14-2t-13 17h16" stroke-width="1.8"/><circle cx="186" cy="22" r="3" fill="currentColor"/><circle cx="50" cy="47" r="2" fill="currentColor"/></g>)svg";
    default: return {};
    }
}
QByteArray backgroundMotif(Motif motif) {
    switch (motif) {
    case Motif::Lattice: return R"svg(<ellipse cx="1060" cy="110" rx="360" ry="290" fill="url(#glow)"/><g fill="currentColor" fill-opacity="0.06" stroke="currentColor" stroke-width="1.1"><path d="m818 50 94-81 124 72-36 146-128-24Zm94-81 88 218m-182-137 218-9m-164 122 40-194"/><path d="m1000 187 126-39 132 104-58 139-156-32Zm126-39 74 243m-200-204 200 204m-156-32 214-107"/><path d="m64 667 102-78 84 96-40 120-160-38Zm102-78 44 216m-146-138 186 18m-200 82 116-178"/></g><g fill="currentColor"><circle cx="1000" cy="187" r="4"/><circle cx="872" cy="163" r="2"/><circle cx="210" cy="805" r="4"/></g>)svg";
    case Motif::Ribbons: return R"svg(<ellipse cx="1140" cy="10" rx="450" ry="420" fill="url(#glow)"/><g fill="none" stroke="currentColor"><path d="M812-20C744 180 1171 32 1116 230S1140 493 1240 414" stroke-width="78" opacity="0.08"/><path d="M788-20C720 180 1147 32 1092 230S1116 493 1216 414M834-20C766 180 1193 32 1138 230S1162 493 1262 414" stroke-width="1.4"/><path d="M-42 604C85 590 61 749 227 776S350 838 380 840" stroke-width="48" opacity="0.06"/><path d="M-42 585C85 571 61 730 227 757S350 819 380 821M-42 622C85 608 61 767 227 794S350 856 380 858" stroke-width="1.2"/></g><path d="m1020 58 5 17 17 5-17 5-5 17-5-17-17-5 17-5Zm153 261 3 10 10 3-10 3-3 10-3-10-10-3 10-3Z" fill="currentColor" opacity="0.6"/>)svg";
    case Motif::Abyss: return R"svg(<ellipse cx="1150" cy="280" rx="340" ry="360" fill="url(#glow)"/><g fill="none" stroke="currentColor" stroke-width="1.1"><path d="M807-40C717 109 970 112 1014 215S936 388 1277 462M836-40C746 109 999 112 1043 215S965 388 1306 462M865-40C775 109 1028 112 1072 215S994 388 1335 462M894-40C804 109 1057 112 1101 215S1023 388 1364 462"/><path d="M-85 555Q75 641 23 734T262 854M-65 533Q95 619 43 712T282 832M-45 511Q115 597 63 690T302 810" opacity="0.65"/><path d="m1140 92 16-9 16 9-16 9Z"/><circle cx="1156" cy="92" r="26" opacity="0.45"/></g><g fill="currentColor"><circle cx="945" cy="65" r="2"/><circle cx="1146" cy="223" r="3"/><circle cx="87" cy="644" r="2"/></g>)svg";
    case Motif::Folds: return R"svg(<ellipse cx="1080" cy="20" rx="350" ry="370" fill="url(#glow)"/><g stroke="currentColor" stroke-width="1" fill="none"><path d="M932-15v99m151-99v49m89-49v176" opacity="0.7"/><path d="m881 114 51-30 69 32-18 91-76-9Zm162-57 40-23 47 25-11 63-61-3Zm86 130 43-26 54 28-15 80-66-10Z" fill="currentColor" fill-opacity="0.08"/><path d="m932 84-25 114m25-114 51 123m-102-93 120 2m82-82-25 85m25-85 36 88m-76-65 87 2m42 102-27 98m27-98 39 108"/><path d="m-40 710 171-96 105 106-81 140Zm0 0 195 150m-24-246 24 246m-195-150 276 10" fill="currentColor" fill-opacity="0.06"/></g>)svg";
    case Motif::Orbits: return R"svg(<ellipse cx="1054" cy="180" rx="350" ry="320" fill="url(#glow)"/><g fill="none" stroke="currentColor" stroke-width="1.1"><ellipse cx="1080" cy="108" rx="202" ry="124" transform="rotate(-24 1080 108)"/><ellipse cx="1080" cy="108" rx="132" ry="195" transform="rotate(32 1080 108)" opacity="0.7"/><ellipse cx="1080" cy="108" rx="232" ry="146" transform="rotate(-24 1080 108)" opacity="0.3"/><path d="M966 226q-28 55 32 72 30-58-32-72Zm0 0 32 72M94 630q-72 21-41 89 70-24 41-89Zm0 0-41 89" fill="currentColor" fill-opacity="0.06"/><ellipse cx="69" cy="747" rx="153" ry="57" transform="rotate(20 69 747)" opacity="0.6"/><path d="m1063 69-42 58h56m-7-66v94m27-71q34-36 48-7t-46 65h52" stroke-width="2" opacity="0.4"/></g><g fill="currentColor"><circle cx="886" cy="177" r="5"/><circle cx="1169" cy="-16" r="3"/><circle cx="198" cy="797" r="4"/></g>)svg";
    default: return {};
    }
}
}
QByteArray themeArtworkSvg(const Appearance& appearance, const ThemeTokens& tokens, ArtworkPlacement placement) {
    const Motif motif = motifFor(appearance); if (motif == Motif::None) return {};
    QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"";
    svg += placement == ArtworkPlacement::Background ? "0 0 1200 800\">" : "0 0 240 64\">";
    if (placement == ArtworkPlacement::Background) {
        svg += R"(<defs><radialGradient id="glow"><stop offset="0" stop-color="currentColor" stop-opacity="0.4"/><stop offset="0.55" stop-color="currentColor" stop-opacity="0.12"/><stop offset="1" stop-color="currentColor" stop-opacity="0"/></radialGradient></defs>)";
        svg += backgroundMotif(motif);
    } else svg += titlebarMotif(motif);
    svg += "</svg>";
    svg.replace("currentColor", (tokens.dark ? tokens.accent : tokens.accentText).name(QColor::HexRgb).toLatin1());
    return svg;
}
void paintThemeArtwork(QPainter& painter, const QRectF& target, const Appearance& appearance,
                       const ThemeTokens& tokens, ArtworkPlacement placement) {
    if (target.isEmpty()) return;
    const QByteArray svg = themeArtworkSvg(appearance, tokens, placement); if (svg.isEmpty()) return;
    static QCache<QByteArray, QSvgRenderer> renderers(24);
    auto* renderer = renderers.object(svg);
    if (!renderer) {
        auto* created = new QSvgRenderer(svg);
        if (!created->isValid()) { delete created; return; }
        renderers.insert(svg, created); renderer = created;
    }
    const qreal dpr = painter.device()->devicePixelRatioF();
    const QSize pixels(std::max(1, int(std::ceil(target.width() * dpr))), std::max(1, int(std::ceil(target.height() * dpr))));
    if (qint64(pixels.width()) * pixels.height() > 4 * 1024 * 1024) return;
    QImage layer(pixels, QImage::Format_ARGB32_Premultiplied); if (layer.isNull()) return;
    layer.setDevicePixelRatio(dpr); layer.fill(Qt::transparent);
    const QSizeF viewbox = placement == ArtworkPlacement::Background ? QSizeF(1200, 800) : QSizeF(240, 64);
    const qreal scale = placement == ArtworkPlacement::Background
        ? std::max(target.width() / viewbox.width(), target.height() / viewbox.height())
        : std::min(target.width() / viewbox.width(), target.height() / viewbox.height());
    {
        QPainter art(&layer); art.setRenderHint(QPainter::Antialiasing);
        const QSizeF rendered = viewbox * scale;
        renderer->render(&art, QRectF(QPointF((target.width() - rendered.width()) / 2, (target.height() - rendered.height()) / 2), rendered));
        QLinearGradient mask;
        if (placement == ArtworkPlacement::Background) {
            const qreal angle = 115. * std::numbers::pi / 180.;
            const QPointF direction(std::sin(angle), -std::cos(angle));
            const qreal extent = (target.width() * std::abs(direction.x()) + target.height() * std::abs(direction.y())) / 2;
            const QPointF center(target.width() / 2, target.height() / 2);
            mask = QLinearGradient(center - direction * extent, center + direction * extent);
            mask.setColorAt(0, Qt::black); mask.setColorAt(.31, Qt::transparent);
            mask.setColorAt(.57, Qt::transparent); mask.setColorAt(.84, Qt::black); mask.setColorAt(1, Qt::black);
        } else {
            mask = QLinearGradient(0, 0, target.width(), 0);
            mask.setColorAt(0, Qt::transparent); mask.setColorAt(.15, Qt::black);
            mask.setColorAt(.85, Qt::black); mask.setColorAt(1, Qt::transparent);
        }
        art.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        art.fillRect(QRectF(QPointF(0, 0), target.size()), mask);
    }
    qreal opacity = appearance.accent == Accent::Custom ? std::clamp(appearance.custom.decorationOpacity, 0., 1.) : .35;
    if (!tokens.dark && placement == ArtworkPlacement::Background) opacity *= .8;
    painter.save(); painter.setOpacity(painter.opacity() * opacity); painter.drawImage(target.topLeft(), layer); painter.restore();
}
} // namespace nickel::ui
