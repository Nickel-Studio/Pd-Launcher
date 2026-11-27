#include "Icons.hpp"

#include <QCache>
#include <QHash>
#include <QSet>
#include <QSvgRenderer>

namespace nickel::ui {
namespace {
struct IconEntry { const char* key; const char* body; double stroke = 1.75; };
// SVG bodies migrated from the removed legacy React icon set; no path approximation,
// font glyph replacement, or hand-redrawn game artwork.
const IconEntry entries[] = {
    {"home", R"(<path d="M3 10.4 12 3l9 7.4"/><path d="M5.4 9.2V20.6h13.2V9.2"/><path d="M9.6 20.6v-6h4.8v6"/>)"},
    {"versions", R"(<path d="M12 3 3 7.5l9 4.5 9-4.5L12 3Z"/><path d="M3 12.4 12 16.9l9-4.5"/><path d="M3 16.9 12 21.4l9-4.5"/>)"},
    {"resources", R"(<path d="M21 8 12 3 3 8v8l9 5 9-5V8Z"/><path d="M3 8l9 5 9-5"/><path d="M12 13v8"/>)"},
    {"settings", R"(<path d="M4 6.5h6"/><path d="M14 6.5h6"/><circle cx="12" cy="6.5" r="2.1"/><path d="M4 12h10"/><path d="M18 12h2"/><circle cx="16" cy="12" r="2.1"/><path d="M4 17.5h6"/><path d="M14 17.5h6"/><circle cx="12" cy="17.5" r="2.1"/>)"},
    {"microsoft", R"(<g stroke="none" fill="currentColor"><rect x="3.2" y="3.2" width="7.6" height="7.6" rx="1.2"/><rect x="13.2" y="3.2" width="7.6" height="7.6" rx="1.2"/><rect x="3.2" y="13.2" width="7.6" height="7.6" rx="1.2"/><rect x="13.2" y="13.2" width="7.6" height="7.6" rx="1.2"/></g>)"},
    {"account/shield", R"(<path d="M12 3 19 6v6c0 4.4-3 7.7-7 9-4-1.3-7-4.6-7-9V6l7-3Z"/><path d="M9.2 12.1 11.1 14 14.9 10.1"/>)", 1.7},
    {"account/image", R"(<rect x="3.6" y="4.8" width="16.8" height="14.4" rx="2.6"/><circle cx="8.9" cy="9.7" r="1.5"/><path d="M4.6 17.4 9.7 12.3l4.1 4.1 2.3-2.3 3.3 3.3"/>)", 1.7},
    {"account/user", R"(<circle cx="12" cy="8" r="3.6"/><path d="M4.8 20.2a7.2 7.2 0 0 1 14.4 0"/>)", 1.7},
    {"account/layers", R"(<path d="M12 3.2 3.4 7.6l8.6 4.4 8.6-4.4L12 3.2Z"/><path d="M3.4 12.3 12 16.7l8.6-4.4"/><path d="M3.4 16.7 12 21.1l8.6-4.4"/>)", 1.7},
    {"account/sliders", R"(<path d="M5 6.5h5"/><path d="M14.5 6.5H19"/><circle cx="12.2" cy="6.5" r="2.1"/><path d="M5 17.5h4.5"/><path d="M14 17.5h5"/><circle cx="11.7" cy="17.5" r="2.1"/>)", 1.7},
    {"account/eye", R"(<path d="M2.8 12s3.4-5.2 9.2-5.2S21.2 12 21.2 12s-3.4 5.2-9.2 5.2S2.8 12 2.8 12Z"/><circle cx="12" cy="12" r="2.6"/>)", 1.7},
    {"account/eyeOff", R"(<path d="M4.2 5.4 19.8 18.6"/><path d="M9.6 7.1A9.6 9.6 0 0 1 12 6.8c5.8 0 9.2 5.2 9.2 5.2a15 15 0 0 1-3 3.5"/><path d="M6.2 9A15.6 15.6 0 0 0 2.8 12s3.4 5.2 9.2 5.2c1.1 0 2.1-.2 3-.5"/><path d="M10.4 10.4a2.3 2.3 0 0 0 3.2 3.2"/>)", 1.7},
    {"account/lock", R"(<rect x="5" y="10.4" width="14" height="9.6" rx="2.4"/><path d="M8.4 10.4V7.8a3.6 3.6 0 0 1 7.2 0v2.6"/><path d="M12 14.2v2.2"/>)", 1.7},
    {"user", R"(<circle cx="12" cy="8" r="3.7"/><path d="M4.6 20.2a7.4 7.4 0 0 1 14.8 0"/>)"},
    {"minus", R"(<path d="M5 12h14"/>)"},
    {"close", R"(<path d="M6 6l12 12"/><path d="M18 6 6 18"/>)"},
    {"check", R"(<path d="M4.8 12.6 9.4 17.2 19.2 6.8"/>)"},
    {"chevronDown", R"(<path d="M6 9.2 12 15.2 18 9.2"/>)"},
    {"spark", R"(<path d="M12 3.2 13.7 8l4.8 1.7L13.7 11.4 12 16.2 10.3 11.4 5.5 9.7 10.3 8 12 3.2Z"/><path d="M18.4 14.6l.8 2.2 2.2.8-2.2.8-.8 2.2-.8-2.2-2.2-.8 2.2-.8.8-2.2Z"/>)"},
    {"wrench", R"(<path d="M15.4 3.4a4.6 4.6 0 0 0-6 6L3.2 15.6l2.3 2.3 6.2-6.2a4.6 4.6 0 0 0 6-6l-2.7 2.6-1.9-1.9 2.6-2.7Z"/><path d="M6.4 17.6l.9.9"/>)"},
    {"refresh", R"(<path d="M20.4 12a8.4 8.4 0 1 1-2.5-6"/><path d="M20.4 4.2v5.2h-5.2"/>)"},
    {"plus", R"(<path d="M12 5v14"/><path d="M5 12h14"/>)"},
    {"trash", R"(<path d="M4 6.6h16"/><path d="M9.2 6.6V4.4h5.6v2.2"/><path d="M6.4 6.6 7.3 20h9.4l.9-13.4"/><path d="M10.4 10v6"/><path d="M13.6 10v6"/>)"},
    {"shield", R"(<path d="M12 3 19 6v6c0 4.4-3 7.7-7 9-4-1.3-7-4.6-7-9V6l7-3Z"/><path d="M9.2 12.2 11.2 14.2 15 10.2"/>)"},
    {"download", R"(<path d="M12 3.2v11.4"/><path d="M7.6 10.4 12 14.8l4.4-4.4"/><path d="M4.6 19.6h14.8"/>)"},
    {"globe", R"(<circle cx="12" cy="12" r="8.4"/><path d="M3.6 12h16.8"/><path d="M12 3.6c2.6 2.7 2.6 14.1 0 16.8"/><path d="M12 3.6c-2.6 2.7-2.6 14.1 0 16.8"/>)"},
    {"image", R"(<rect x="3.4" y="4.6" width="17.2" height="14.8" rx="2.6"/><circle cx="8.8" cy="9.6" r="1.6"/><path d="M4.4 17.4 9.6 12.2l4.2 4.2 2.4-2.4 3.4 3.4"/>)"},
    {"palette", R"(<path d="M12 3.2a8.8 8.8 0 0 0 0 17.6c1.2 0 2-.8 2-1.9 0-.5-.2-.9-.5-1.2-.3-.3-.5-.6-.5-1.1 0-1 .8-1.8 1.8-1.8h1.5a4.5 4.5 0 0 0 4.5-4.5c0-3.9-3.9-7.1-8.8-7.1Z"/><circle cx="7.4" cy="11.2" r="1.15" fill="currentColor" stroke="none"/><circle cx="10.2" cy="7.4" r="1.15" fill="currentColor" stroke="none"/><circle cx="15" cy="8.4" r="1.15" fill="currentColor" stroke="none"/>)"},
    {"play", R"(<path d="M7.6 5.2 18.4 12 7.6 18.8V5.2Z" fill="currentColor" stroke="none"/>)"},
    {"pencil", R"(<path d="M4.6 19.4h3l9.6-9.6-3-3-9.6 9.6v3Z"/><path d="M14.2 6.8l3 3"/>)"},
    {"swap", R"(<path d="M4 9h13.2l-3.2-3.2"/><path d="M20 15H6.8l3.2 3.2"/>)"},
    {"folder", R"(<path d="M3.2 7.6a2 2 0 0 1 2-2h3.2l1.9 2.1h8.5a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2H5.2a2 2 0 0 1-2-2V7.6Z"/><path d="M3.2 9.8h17.6"/>)"},
    {"back", R"(<path d="M10.4 5.6 4 12l6.4 6.4"/><path d="M4 12h16"/>)"},
    {"detail/back", R"(<path d="M14.5 5.5 8 12l6.5 6.5"/>)"},
    {"mods", R"(<path d="M7.6 9.6V7.1a1.6 1.6 0 0 1 3.2 0v2.5"/><path d="M13.2 9.6V7.1a1.6 1.6 0 0 1 3.2 0v2.5"/><rect x="3.6" y="9.6" width="16.8" height="10.2" rx="2.2"/>)"},
    {"pack", R"(<path d="M20.4 8.4v9.4a2.3 2.3 0 0 1-2.3 2.3H8.4"/><rect x="3.4" y="3.4" width="13.3" height="13.3" rx="2.8"/><circle cx="7.7" cy="7.8" r="1.25" fill="currentColor" stroke="none"/><path d="M3.8 14.4 8.1 10.5l4.5 3.9"/>)"},
    {"shader", R"(<circle cx="12" cy="12" r="3.9"/><path d="M12 2.9v2.5"/><path d="M12 18.6v2.5"/><path d="M2.9 12h2.5"/><path d="M18.6 12h2.5"/><path d="m5.7 5.7 1.8 1.8"/><path d="m16.5 16.5 1.8 1.8"/><path d="m18.3 5.7-1.8 1.8"/><path d="m7.5 16.5-1.8 1.8"/>)"},
    {"world", R"(<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12h17"/><path d="M12 3.5c2.6 2.8 2.6 14.2 0 17"/><path d="M12 3.5c-2.6 2.8-2.6 14.2 0 17"/>)"},
    {"bundle", R"(<path d="M12 3.2 3.4 7.4 12 11.6l8.6-4.2L12 3.2Z"/><path d="m3.4 12.2 8.6 4.2 8.6-4.2"/><path d="m3.4 16.8 8.6 4.2 8.6-4.2"/>)"},
    {"folderOpen", R"(<path d="M3.4 7.4a2 2 0 0 1 2-2h3.4l1.7 2.2h6.1a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2H5.4a2 2 0 0 1-2-2Z"/><path d="M11.6 14.6h4.8"/><path d="m14.6 12.8 1.8 1.8-1.8 1.8"/>)"},
    {"alert", R"(<path d="M12 3.8 21 19.6H3Z"/><path d="M12 9.8v4.2"/><circle cx="12" cy="16.9" r="1" fill="currentColor" stroke="none"/>)"},
    {"arrowUp", R"(<path d="M12 19.4V5.6"/><path d="M6.4 11.2 12 5.6l5.6 5.6"/>)"},
    {"downloadTray", R"(<path d="M12 3.6v10.2"/><path d="M7.8 9.6 12 13.8l4.2-4.2"/><path d="M4.8 16v2.6a1.8 1.8 0 0 0 1.8 1.8h10.8a1.8 1.8 0 0 0 1.8-1.8V16"/>)"},
    {"stop", R"(<circle cx="12" cy="12" r="8.4"/><path d="M8.6 12h6.8"/>)"}
};
}
QByteArray iconSvg(const QString& name, const QColor& color) {
    QString key = name == "layers" ? QStringLiteral("versions") : name;
    for (const auto& entry : entries) {
        if (key != QLatin1String(entry.key)) continue;
        QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"";
        svg += QByteArray::number(entry.stroke); svg += "\">"; svg += entry.body; svg += "</svg>";
        svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1()); return svg;
    }
    return {};
}
void paintIcon(QPainter& painter, const QRectF& target, const QString& name, const QColor& color) {
    if (name.isEmpty() || target.isEmpty()) return;
    static QCache<QString, QSvgRenderer> renderers(128);
    const QString key = name + '/' + color.name(QColor::HexRgb);
    auto* renderer = renderers.object(key);
    if (!renderer) {
        const QByteArray svg = iconSvg(name, color); if (svg.isEmpty()) return;
        auto* created = new QSvgRenderer(svg);
        if (!created->isValid()) { delete created; return; }
        renderers.insert(key, created); renderer = created;
    }
    painter.save(); painter.setOpacity(painter.opacity() * color.alphaF());
    renderer->render(&painter, target); painter.restore();
}
QImage functionalImage(const QString& key) {
    static const QHash<QString, QString> icons{
        {"grass", "icons/grass.png"}, {"command_block", "icons/command_block.png"},
        {"cobblestone", "icons/cobblestone.png"}, {"anvil", "icons/anvil.png"}, {"neoforge", "icons/neoforge.png"},
        {"fabric", "icons/fabric.png"}, {"grass_path", "icons/grass_path.png"}, {"egg", "icons/egg.png"},
        {"gold_block", "icons/gold_block.png"}, {"redstone_block", "icons/redstone_block.png"},
        {"redstone_lamp_on", "icons/redstone_lamp_on.png"}, {"redstone_lamp_off", "icons/redstone_lamp_off.png"},
        {"optifabric", "icons/optifabric.png"}, {"defaultredstone", "icons/redstone_block.png"},
        {"defaultmod", "mod-placeholder.png"}
    };
    if (key.isEmpty()) return {};
    const QString alias = icons.value(key, key);
    if (alias.contains("..") || alias.contains('\\') || alias.contains('?') || alias.contains('#')) return {};
    QString resource;
    if (alias.startsWith(":/nickel/functional/") || alias.startsWith(":/nickel/brand/")) resource = alias;
    else {
        if (alias.startsWith('/') || alias.contains(':')) return {};
        resource = ":/nickel/functional/" + alias;
    }
    static QCache<QString, QImage> images(8 * 1024 * 1024);
    if (const auto* cached = images.object(resource)) return *cached;
    QImage image(resource);
    if (image.isNull() || image.sizeInBytes() > 4 * 1024 * 1024) return {};
    images.insert(resource, new QImage(image), int(image.sizeInBytes()));
    return image;
}
bool isPixelArtResource(const QString& key) {
    const QString alias = key.startsWith(":/nickel/functional/") ? key.mid(QStringLiteral(":/nickel/functional/").size()) : key;
    static const QSet<QString> blocks{"grass", "command_block", "cobblestone", "anvil", "neoforge", "fabric", "grass_path", "egg",
        "gold_block", "redstone_block", "redstone_lamp_on", "redstone_lamp_off", "optifabric", "defaultredstone"};
    return blocks.contains(alias) || alias.startsWith("blocks/") || alias.startsWith("icons/") || alias.startsWith("skins/");
}
void paintContainedImage(QPainter& painter, const QRectF& target, const QImage& image, bool pixelated) {
    if (image.isNull() || target.isEmpty()) return;
    QSizeF size = image.size(); size.scale(target.size(), Qt::KeepAspectRatio);
    const QRectF contained(target.center() - QPointF(size.width()/2., size.height()/2.), size);
    painter.save(); painter.setRenderHint(QPainter::SmoothPixmapTransform, !pixelated);
    painter.drawImage(contained, image); painter.restore();
}
} // namespace nickel::ui
