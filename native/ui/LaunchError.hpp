#pragma once

#include "Bindings.hpp"
#include "Dialogs.hpp"

namespace nickel::ui {
QString launchFailureKey(const QJsonObject& authoritativeLaunch);
Dialog* showLaunchErrorDialog(Theme& theme, Bindings& bindings,
                              const QJsonObject& authoritativeLaunch, QWidget* parent);
} // namespace nickel::ui
