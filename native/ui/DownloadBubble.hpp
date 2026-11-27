#pragma once

#include "PageShell.hpp"

namespace nickel::ui {
class DownloadBubble final : public Panel {
    Q_OBJECT
public:
    DownloadBubble(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent = nullptr);
    void apply(const QJsonObject& snapshot);
private:
    QLabel* label_;
    Button* open_;
    Button* dismiss_;
};
} // namespace nickel::ui
