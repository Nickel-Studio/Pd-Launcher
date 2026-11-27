#pragma once

#include <QString>
#include <QWidget>

namespace nickel::ui {

// Internal UI projection of the single service-owned launch lifecycle.
enum class LaunchWindowPhase { Preparing, Running, Draining, Finished };
struct LaunchWindowState {
    quint64 generation = 0;
    QString launchId;
    QString launcherVisibility;
    LaunchWindowPhase phase = LaunchWindowPhase::Preparing;
    bool gameWindowSeen = false;
    bool rootExited = false;
};

enum class LauncherWindowAction { None, Hide, Minimize, Close, Restore };

class LauncherWindowLifecycle final {
public:
    [[nodiscard]] LauncherWindowAction observe(const LaunchWindowState& state) {
        if (!state.generation || state.launchId.isEmpty() || !validVisibility(state.launcherVisibility)
            || state.generation < generation_) return LauncherWindowAction::None;
        if (state.generation > generation_) {
            generation_ = state.generation;
            launchId_ = state.launchId;
            visibility_ = state.launcherVisibility;
            applied_ = false;
            finished_ = false;
        } else if (state.launchId != launchId_ || state.launcherVisibility != visibility_ || finished_) {
            return LauncherWindowAction::None;
        }
        if (state.phase == LaunchWindowPhase::Finished) {
            finished_ = true;
            if (!applied_) return LauncherWindowAction::None;
            if (visibility_ == QStringLiteral("hide")) return LauncherWindowAction::Restore;
            if (visibility_ == QStringLiteral("hide-close")) return LauncherWindowAction::Close;
            return LauncherWindowAction::None;
        }
        // Spawn, splash, preparation, root exit and output-drain timeout are not window readiness.
        if (applied_ || state.phase != LaunchWindowPhase::Running || !state.gameWindowSeen || state.rootExited)
            return LauncherWindowAction::None;
        applied_ = true;
        if (visibility_ == QStringLiteral("hide") || visibility_ == QStringLiteral("hide-close"))
            return LauncherWindowAction::Hide;
        if (visibility_ == QStringLiteral("minimize")) return LauncherWindowAction::Minimize;
        if (visibility_ == QStringLiteral("close")) return LauncherWindowAction::Close;
        return LauncherWindowAction::None;
    }

private:
    static bool validVisibility(const QString& value) {
        return value == QStringLiteral("keep") || value == QStringLiteral("hide")
            || value == QStringLiteral("hide-close") || value == QStringLiteral("close")
            || value == QStringLiteral("minimize");
    }
    quint64 generation_ = 0;
    QString launchId_, visibility_;
    bool applied_ = false;
    bool finished_ = false;
};

// No game HWND, process ownership, QApplication quit or service cancellation is performed here.
inline bool applyLauncherWindowAction(QWidget& launcher, LauncherWindowAction action) {
    switch (action) {
    case LauncherWindowAction::Hide: launcher.hide(); break;
    case LauncherWindowAction::Minimize: launcher.showMinimized(); break;
    case LauncherWindowAction::Close: return launcher.close();
    case LauncherWindowAction::Restore:
        launcher.setWindowState(launcher.windowState() & ~Qt::WindowMinimized);
        launcher.show();
        break;
    case LauncherWindowAction::None: break;
    }
    return false;
}

} // namespace nickel::ui
