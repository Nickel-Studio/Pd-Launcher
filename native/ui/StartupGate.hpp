#pragma once

#include "PageShell.hpp"
#include <QElapsedTimer>
#include <QTimer>

namespace nickel::ui {
class StartupGate final : public QWidget {
    Q_OBJECT
public:
    StartupGate(Theme& theme, Bindings& bindings, QWidget* parent = nullptr);
    void start();
    void apply(const QJsonObject& status);
signals:
    void ready();
    void rejectionFinished();
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
private:
    void render();
    void perform(UiAction action, const QJsonObject& arguments);
    Theme& theme_;
    Bindings& bindings_;
    Panel* panel_;
    QVBoxLayout* layout_;
    QLabel* title_;
    QWidget* body_;
    QLabel* error_;
    QWidget* footer_;
    QJsonObject status_;
    UiError failure_;
    QTimer rejectionTimer_;
    QElapsedTimer rejectionClock_;
    quint64 generation_ = 0;
    bool busy_ = false;
    bool finishing_ = false;
    bool finished_ = false;
};
} // namespace nickel::ui
