#pragma once

#include "BackgroundView.hpp"
#include "Bindings.hpp"
#include "Navigation.hpp"
#include "PageShell.hpp"
#include "Theme.hpp"
#include <QImage>
#include <QPointer>
#include <QStackedWidget>
#include <QWidget>
#include <array>

namespace nickel::ui {

class TitleBar;
class StartupGate;
class DownloadBubble;

class MainWindow final : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(UiServices& services, QWidget* parent = nullptr);
    ~MainWindow() override;
    Navigation& navigation() { return navigation_; }
    Theme& theme() { return theme_; }
    RoutePage* routePage(Route route) const;
    void setBackgroundImage(const QImage& approvedImage);
    void applySettings(const QJsonObject& authoritativeSettings);
    // GUI-thread rendering only; bounds capture planes, not process peak or presentation.
    [[nodiscard]] QImage captureRenderedFrame(qsizetype maximumWorkingBytes);
    // Measured GUI-thread state for the existing production observation protocol; no I/O or expected-state echo.
    [[nodiscard]] QJsonObject observedVisualState() const;

signals:
    void startupReady();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void changeEvent(QEvent*) override;

private:
    void switchRoute(Route route);
    void receiveSnapshot(UiTopic topic, const QJsonObject& snapshot);
    void displayError(const UiError& error);
    void bootstrap();
    Qt::Edges resizeEdges(const QPoint& position) const;
    UiServices& services_;
    Theme theme_;
    Bindings bindings_;
    Navigation navigation_;
    BackgroundView* background_ = nullptr;
    TitleBar* titlebar_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    StartupGate* startup_ = nullptr;
    DownloadBubble* downloads_ = nullptr;
    QPointer<QWidget> toast_;
    std::array<RoutePage*, 7> routes_{};
    Route activeRoute_ = Route::Home;
    QJsonObject settings_;
    QString backgroundKey_;
    Qt::Edges resizing_;
    QPoint resizeStart_;
    QRect resizeGeometry_;
    bool bootstrapped_ = false;
    bool ready_ = false;
    bool capturingFrame_ = false;
    bool paintingFrame_ = false;
};

} // namespace nickel::ui