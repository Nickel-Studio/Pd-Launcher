#include "PerformanceApplication.hpp"

#include <QEvent>
#include <QThread>
#include <QWindow>

#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nickel::app {

PerformanceApplication::PerformanceApplication(int& argc, char** argv) : QApplication(argc, argv) {}


qint64 PerformanceApplication::monotonic_nanoseconds() {
#ifdef Q_OS_WIN
    LARGE_INTEGER counter{}, frequency{};
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency)
        || counter.QuadPart < 0 || frequency.QuadPart <= 0 || frequency.QuadPart > 1000000000LL)
        throw std::runtime_error("The performance clock is unavailable");
    const auto seconds = counter.QuadPart / frequency.QuadPart;
    if (seconds > std::numeric_limits<qint64>::max() / 1000000000LL)
        throw std::runtime_error("The performance clock is out of range");
    return seconds * 1000000000LL + (counter.QuadPart % frequency.QuadPart) * 1000000000LL / frequency.QuadPart;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
#endif
}

QString PerformanceApplication::clock_method() {
#ifdef Q_OS_WIN
    return QStringLiteral("QueryPerformanceCounter");
#else
    return QStringLiteral("std::chrono::steady_clock");
#endif
}


void PerformanceApplication::verify_acceptance_window_dpi(QWidget& window, int percent) const {
#ifdef Q_OS_WIN
    if (QApplication::platformName() != QStringLiteral("windows")
        || !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        throw std::runtime_error("Actual per-monitor-v2 Windows DPI is required");
    const auto handle = reinterpret_cast<HWND>(window.winId());
    const auto dpi = GetDpiForWindow(handle);
    if (!dpi || static_cast<quint64>(dpi) * 100 != static_cast<quint64>(percent) * 96)
        throw std::runtime_error("The actual window DPI differs from the acceptance request");
#else
    (void)window;
    (void)percent;
    throw std::runtime_error("Actual Windows window DPI is unavailable on this platform");
#endif
}

void PerformanceApplication::observe_first_frame(QWidget& window, FrameSink sink) {
    if (QThread::currentThread() != thread() || !sink || observed_)
        throw std::runtime_error("Invalid first-frame observation lifecycle");
    observed_ = &window;
    frame_sink_ = std::move(sink);
    frame_ = {};
    frame_.presentation_status = QStringLiteral("missing-actual-presenter-receipt");
    frame_reported_ = false;
}

const FrameMeasurement& PerformanceApplication::frame_measurement() const { return frame_; }

bool PerformanceApplication::notify(QObject* receiver, QEvent* event) {
    if (!observed_ || !frame_sink_ || frame_reported_) return QApplication::notify(receiver, event);
    const auto window = observed_;
    const bool paint = receiver == window.data() && event->type() == QEvent::Paint && event->spontaneous();
    const bool delivered = QApplication::notify(receiver, event);
    if (!paint || !window || observed_ != window || frame_reported_) return delivered;
    try {
        frame_.paint_complete_ns = monotonic_nanoseconds();
        // Paint delivery is observable; dispatch return does not prove a backing-store flush or pixels.
        frame_.flush_method = QStringLiteral("unobserved-backing-store-flush");
#ifdef Q_OS_WIN
        if (QApplication::platformName() == QStringLiteral("windows") && window->windowHandle()) {
            const auto handle = reinterpret_cast<HWND>(window->winId());
            DWORD owner = 0;
            if (!GetWindowThreadProcessId(handle, &owner) || owner != GetCurrentProcessId())
                throw std::runtime_error("The observed window is not owned by this process");
            frame_.native_window_id = static_cast<quint64>(window->winId());
            const auto dpi = GetDpiForWindow(handle);
            if (dpi) frame_.native_window_dpi = static_cast<quint32>(dpi);
        }
#endif
        frame_reported_ = true;
        frame_sink_(frame_);
    } catch (...) {
        QCoreApplication::exit(EXIT_FAILURE);
    }
    return delivered;
}

} // namespace nickel::app
