#pragma once

#include <QCoreApplication>
#include <QGuiApplication>
#include <QString>

namespace nickel::ui {
inline QString productDisplayName() {
    const QString name = QGuiApplication::applicationDisplayName().trimmed();
    // Qt otherwise exposes the internal application/executable name as the display name.
    if (name.isEmpty() || name == QCoreApplication::applicationName()) return QStringLiteral("Palladium");
    return name;
}

inline QString errorReportFileName() {
    return QStringLiteral("Pd-error-report.zip");
}
} // namespace nickel::ui
