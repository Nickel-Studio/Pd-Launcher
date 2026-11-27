#pragma once

#include "PageShell.hpp"

namespace nickel::ui {
RoutePage* createHomePage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent = nullptr);
RoutePage* createVersionsPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent = nullptr);
RoutePage* createVersionSettingsPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent = nullptr);
RoutePage* createResourcesPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent = nullptr);
RoutePage* createSettingsPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent = nullptr);
RoutePage* createMorePage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent = nullptr);
RoutePage* createDownloadDetailPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent = nullptr);
} // namespace nickel::ui
