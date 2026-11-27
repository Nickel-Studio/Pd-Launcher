#include "Theme.hpp"

#include <QGuiApplication>
#include <QFontDatabase>
#include <QStyleHints>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace nickel::ui {
namespace {
const std::array<AccentInfo, 12> accents{{
    {Accent::Mono, "mono", "黑与白", "#f2f4f5"},
    {Accent::Emerald, "emerald", "翡翠绿", "#3ecf8e"},
    {Accent::Blue, "blue", "海蓝色", "#5a97e8"},
    {Accent::Violet, "violet", "魅紫色", "#9b8cf0"},
    {Accent::Amber, "amber", "琥珀色", "#e0aa4c"},
    {Accent::Rose, "rose", "玫瑰红", "#ea6f8b"},
    {Accent::Nickel, "nickel", "钯光", "#8dbcae"},
    {Accent::Copper, "copper", "铜霞", "#e4a27c"},
    {Accent::Abyss, "abyss", "深海", "#75acd1"},
    {Accent::Paper, "paper", "纸灯", "#e3c48d"},
    {Accent::FortyTwo, "fortyTwo", "四十二", "#459c93"},
    {Accent::Custom, "custom", "自定义", "#449c93"}
}};
QColor alpha(QColor color, double opacity) { color.setAlphaF(opacity); return color; }
double luminance(const QColor& color) {
    auto linear = [](double c) { return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4); };
    return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
}
QColor hsl(double hue, double saturation, double lightness) {
    hue = std::fmod(std::fmod(hue, 360.) + 360., 360.);
    return QColor::fromHslF(hue / 360., std::clamp(saturation / 100., 0., 1.),
                           std::clamp(lightness / 100., 0., 1.));
}
void ensureBundledTextFont() {
    if (!qGuiApp) return;
    static const int fontId = QFontDatabase::addApplicationFont(QStringLiteral(":/nickel/brand/fonts/nickel-text-subset.ttf"));
    Q_UNUSED(fontId);
}
}

const std::array<AccentInfo, 12>& accentOptions() { return accents; }
Accent accentFromKey(const QString& key) {
    for (const auto& entry : accents) if (key == QLatin1String(entry.key)) return entry.id;
    return Accent::Mono;
}
QString accentKey(Accent accent) { return QString::fromLatin1(accents.at(static_cast<size_t>(accent)).key); }
QColor mix(const QColor& a, const QColor& b, double amount) {
    amount = std::clamp(amount, 0., 1.);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount,
                            a.greenF() + (b.greenF() - a.greenF()) * amount,
                            a.blueF() + (b.blueF() - a.blueF()) * amount,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * amount);
}
double contrastRatio(const QColor& a, const QColor& b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + .05) / (std::min(la, lb) + .05);
}

ThemeTokens makeThemeTokens(const Appearance& a, bool systemDark, qint64 elapsedMs) {
    ThemeTokens t;
    t.dark = a.mode == ThemeMode::Dark || (a.mode == ThemeMode::System && systemDark);
    if (t.dark) {
        t.background = QColor("#0e1011"); t.background2 = QColor("#141617");
        t.surface = QColor("#1a1d1e"); t.surface2 = QColor("#222627");
        t.surface3 = QColor("#2b2f31"); t.surface4 = QColor("#353a3c");
        t.text = QColor("#f2f4f5"); t.muted = QColor("#a8b0b3"); t.faint = QColor("#7d868a");
        t.danger = QColor("#ff6b73"); t.success = QColor("#3ecf8e"); t.warning = QColor("#e8b04a");
        t.overlay = QColor(6, 8, 9, 168);
        t.fill1 = QColor(255,255,255,10); t.fill2 = QColor(255,255,255,18);
        t.fill3 = QColor(255,255,255,26); t.fill4 = QColor(255,255,255,36);
        t.hairline = QColor(255,255,255,15);
    } else {
        t.background = QColor("#eceef0"); t.background2 = QColor("#f5f6f7");
        t.surface = QColor("#ffffff"); t.surface2 = QColor("#f4f5f7");
        t.surface3 = QColor("#eaebee"); t.surface4 = QColor("#dfe1e5");
        t.text = QColor("#14171a"); t.muted = QColor("#4d555c"); t.faint = QColor("#6f7780");
        t.danger = QColor("#d8343f"); t.success = QColor("#12a06b"); t.warning = QColor("#b87b16");
        t.overlay = QColor(24,28,32,107);
        t.fill1 = QColor(16,20,24,9); t.fill2 = QColor(16,20,24,15);
        t.fill3 = QColor(16,20,24,23); t.fill4 = QColor(16,20,24,33);
        t.hairline = QColor(16,20,24,15);
    }
    t.accent = QColor(accents.at(static_cast<size_t>(a.accent)).color);
    if (a.accent == Accent::Mono && !t.dark) t.accent = t.text;
    if (a.accent == Accent::Custom) t.accent = hsl(a.custom.hue, a.custom.saturation, a.custom.lightness);
    if (a.accent == Accent::FortyTwo) {
        const qint64 phase = ((elapsedMs % 180000) + 180000) % 180000;
        const double amount = (1. - std::cos(double(phase) / 180000. * std::numbers::pi * 2.)) / 2.;
        t.accent = mix(QColor("#459c93"), QColor("#bcdec4"), amount);
    }
    const bool rich = static_cast<int>(a.accent) >= static_cast<int>(Accent::Nickel);
    t.accentForeground = contrastRatio(t.accent, Qt::black) >= contrastRatio(t.accent, Qt::white)
        ? QColor(Qt::black) : QColor(Qt::white);
    t.accentStrong = mix(t.accent, t.accentForeground == QColor(Qt::black) ? QColor(Qt::white) : QColor(Qt::black), .1);
    if (!rich) {
        static const char* foreground[] = {"#0e1011","#05170f","#04101f","#120a2a","#1d1403","#2a0712"};
        static const char* strong[] = {"#d8dcdd","#2bb87b","#3f7fd0","#7c68e0","#c08f2f","#d35070"};
        const auto index = static_cast<size_t>(a.accent);
        t.accentForeground = QColor(foreground[index]); t.accentStrong = QColor(strong[index]);
        if (a.accent == Accent::Mono && !t.dark) {
            t.accentForeground = QColor("#f5f6f7"); t.accentStrong = QColor("#2b2f31");
        }
    }
    t.accentText = t.accent;
    for (int step = 1; contrastRatio(t.accentText, t.surface4) < 4.8 && step <= 20; ++step)
        t.accentText = mix(t.accent, t.text, double(step) / 20.);
    t.accentSoft = alpha(t.accent, a.accent == Accent::Mono ? .10 : .14);
    t.accentRing = alpha(rich ? t.accentText : t.accent, a.accent == Accent::Mono ? .32 : .4);
    QColor tint = t.accent;
    if (a.accent == Accent::Custom && a.titlebarTint) {
        tint = hsl(a.custom.hue + a.custom.titlebarHueShift, a.custom.saturation, a.custom.lightness);
    }
    const QColor chromeBase(t.dark ? "#0e1011" : "#fcfdfe");
    t.chrome = alpha(a.titlebarTint ? mix(chromeBase, tint, t.dark ? .14 : .12) : chromeBase,
                     a.titlebarTint ? (t.dark ? .88 : .90) : (t.dark ? .78 : .82));
    return t;
}

QFont bodyFont(int size, QFont::Weight weight) {
    ensureBundledTextFont();
    QFont font; font.setFamilies({"Segoe UI Variable Text", "Segoe UI", "PingFang SC", "Microsoft YaHei UI", "Microsoft YaHei", "Noto Sans SC"});
    font.setPixelSize(size); font.setWeight(weight); font.setStyleStrategy(QFont::PreferAntialias); return font;
}
QFont displayFont(int size, QFont::Weight weight) {
    QFont font = bodyFont(size, weight);
    font.setFamilies({"Segoe UI Variable Display", "Segoe UI Semibold", "PingFang SC", "Microsoft YaHei UI", "Microsoft YaHei", "Noto Sans SC"}); return font;
}
QFont monoFont(int size) {
    QFont font = bodyFont(size);
    font.setFamilies({"Cascadia Mono", "JetBrains Mono", "Consolas", "SFMono-Regular", "Noto Sans Mono", "Microsoft YaHei UI", "Microsoft YaHei", "PingFang SC", "Noto Sans SC"}); return font;
}

Theme::Theme(QObject* parent) : QObject(parent) {
    if (qGuiApp) {
        systemDark_ = qGuiApp->styleHints()->colorScheme() != Qt::ColorScheme::Light;
        connect(qGuiApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme scheme) {
            systemDark_ = scheme != Qt::ColorScheme::Light; refresh(true);
        });
    }
    tokens_ = makeThemeTokens(appearance_, systemDark_);
    timer_.setInterval(500);
    connect(&timer_, &QTimer::timeout, this, [this] { refresh(false); });
    transition_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&transition_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        tokens_.accent = value.value<QColor>(); emit changed();
    });
}
QPalette Theme::palette() const {
    QPalette p;
    p.setColor(QPalette::Window, tokens_.background); p.setColor(QPalette::Base, Qt::transparent);
    p.setColor(QPalette::WindowText, tokens_.text); p.setColor(QPalette::Text, tokens_.text);
    p.setColor(QPalette::ButtonText, tokens_.text); p.setColor(QPalette::Button, tokens_.surface2);
    p.setColor(QPalette::Highlight, tokens_.accentSoft); p.setColor(QPalette::HighlightedText, tokens_.text);
    p.setColor(QPalette::PlaceholderText, tokens_.faint); p.setColor(QPalette::ToolTipBase, tokens_.surface3);
    p.setColor(QPalette::ToolTipText, tokens_.text); p.setColor(QPalette::Link, tokens_.accentText);
    p.setColor(QPalette::Disabled, QPalette::Text, tokens_.faint);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, tokens_.faint); return p;
}
void Theme::setAppearance(const Appearance& value) {
    const bool oldMotion = reducedMotion();
    appearance_ = value;
    appearance_.backgroundBlur = std::clamp(value.backgroundBlur, 0, 40);
    appearance_.backgroundOpacity = std::clamp(value.backgroundOpacity, 0., 1.);
    appearance_.windowOpacity = std::clamp(value.windowOpacity, .3, 1.);
    refresh(true); synchronizeClock();
    if (oldMotion != reducedMotion()) emit motionChanged(reducedMotion());
}
void Theme::setSystemPreferences(bool dark, bool motion) {
    const bool oldMotion = reducedMotion(); systemDark_ = dark; systemReducedMotion_ = motion;
    refresh(true); synchronizeClock();
    if (oldMotion != reducedMotion()) emit motionChanged(reducedMotion());
}
void Theme::setWindowVisible(bool visible) {
    if (windowVisible_ == visible) return;
    if (clock_.isValid()) { elapsedMs_ += clock_.elapsed(); clock_.invalidate(); }
    windowVisible_ = visible; synchronizeClock();
}
void Theme::setDeterministicTime(qint64 ms) { deterministic_ = true; deterministicMs_ = ms; timer_.stop(); refresh(false); }
void Theme::synchronizeClock() {
    const bool run = !deterministic_ && windowVisible_ && !reducedMotion() && appearance_.accent == Accent::FortyTwo;
    if (run) { if (!clock_.isValid()) clock_.start(); timer_.start(); }
    else { timer_.stop(); if (clock_.isValid()) { elapsedMs_ += clock_.elapsed(); clock_.invalidate(); } }
    refresh(false);
}
void Theme::refresh(bool animate) {
    const auto previous = tokens_.accent;
    const qint64 time = reducedMotion() ? 0 : deterministic_ ? deterministicMs_
        : elapsedMs_ + (clock_.isValid() ? clock_.elapsed() : 0);
    tokens_ = makeThemeTokens(appearance_, systemDark_, time);
    transition_.stop();
    if (animate && !reducedMotion() && appearance_.accent != Accent::Custom && appearance_.accent != Accent::FortyTwo && previous != tokens_.accent) {
        transition_.setStartValue(previous); transition_.setEndValue(tokens_.accent);
        transition_.setDuration(420); transition_.start();
    }
    emit changed();
}

} // namespace nickel::ui
