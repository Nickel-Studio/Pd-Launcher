#pragma once

#include "Controls.hpp"
#include "Navigation.hpp"
#include <QWidget>

namespace nickel::ui {
class TitleBar final : public QWidget {
    Q_OBJECT
public:
    TitleBar(Theme& theme, Navigation& navigation, QWidget* parent = nullptr);
    TabStrip* tabs() const { return tabs_; }
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    Theme& theme_;
    Navigation& navigation_;
    QLabel* brand_;
    TabStrip* tabs_;
    Button* minimize_;
    Button* close_;
};
} // namespace nickel::ui
