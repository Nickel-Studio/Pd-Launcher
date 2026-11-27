#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QObject>
#include <QPalette>
#include <QTimer>
#include <QVariantAnimation>
#include <array>

namespace nickel::ui {

enum class ThemeMode { System, Light, Dark };
enum class Accent { Mono, Emerald, Blue, Violet, Amber, Rose, Nickel, Copper, Abyss, Paper, FortyTwo, Custom };
enum class Decoration { None, Lattice, Ribbons, Orbits, Folds };

struct CustomTheme {
    double hue = 174;
    double saturation = 39;
    double lightness = 44;
    double titlebarHueShift = 0;
    Decoration decoration = Decoration::Lattice;
    double decorationOpacity = 0.35;
};

// Persisted user choice; ThemeTokens is the derived palette actually used for painting.
struct Appearance {
    ThemeMode mode = ThemeMode::System;
    Accent accent = Accent::Mono;
    CustomTheme custom;
    bool titlebarTint = false;
    bool reducedMotion = false;
    bool shadows = true;
    int backgroundBlur = 0;
    double backgroundOpacity = 0.45;
    double windowOpacity = 1;
};

struct ThemeTokens {
    QColor background, background2, surface, surface2, surface3, surface4;
    QColor text, muted, faint, danger, success, warning, overlay;
    QColor accent, accentStrong, accentForeground, accentSoft, accentRing, accentText, chrome;
    QColor fill1, fill2, fill3, fill4, hairline;
    bool dark = true;
};

struct AccentInfo { Accent id; const char* key; const char* label; const char* color; };
const std::array<AccentInfo, 12>& accentOptions();
Accent accentFromKey(const QString& key);
QString accentKey(Accent accent);
QColor mix(const QColor& a, const QColor& b, double amount);
double contrastRatio(const QColor& a, const QColor& b);
ThemeTokens makeThemeTokens(const Appearance& appearance, bool systemDark, qint64 elapsedMs = 0);
QFont bodyFont(int pixelSize = 13, QFont::Weight weight = QFont::Normal);
QFont displayFont(int pixelSize = 19, QFont::Weight weight = QFont::DemiBold);
QFont monoFont(int pixelSize = 12);

class Theme final : public QObject {
    Q_OBJECT
public:
    explicit Theme(QObject* parent = nullptr);
    const ThemeTokens& tokens() const { return tokens_; }
    const Appearance& appearance() const { return appearance_; }
    QPalette palette() const;
    void setAppearance(const Appearance& appearance);
    void setWindowVisible(bool visible);
    void setSystemPreferences(bool dark, bool reducedMotion);
    // Freezes animation time for visual-baseline tests; no wall clock or timer drives tokens then.
    void setDeterministicTime(qint64 elapsedMs);
    bool reducedMotion() const { return appearance_.reducedMotion || systemReducedMotion_; }
    int motionDuration(int milliseconds) const { return reducedMotion() ? 0 : milliseconds; }

signals:
    void changed();
    void motionChanged(bool reduced);

private:
    void refresh(bool animate);
    void synchronizeClock();
    Appearance appearance_;
    ThemeTokens tokens_;
    bool systemDark_ = true;
    bool systemReducedMotion_ = false;
    bool windowVisible_ = true;
    bool deterministic_ = false;
    qint64 deterministicMs_ = 0;
    qint64 elapsedMs_ = 0;
    QElapsedTimer clock_;
    QTimer timer_;
    QVariantAnimation transition_;
};

} // namespace nickel::ui
