#pragma once

#include <QApplication>
#include <QPointer>
#include <QWidget>

#include <chrono>
#include <functional>
#include <optional>

namespace nickel::app {

// Optional fields stay empty when the platform stage was not observed. Each value is evidence
// for that stage only and never proves a later stage or production presentation.
struct FrameMeasurement {
    std::optional<qint64> paint_complete_ns;
    std::optional<qint64> backing_store_flush_complete_ns;
    std::optional<qint64> present_dwm_confirmed_ns;
    std::optional<qint64> dwm_flush_hresult;
    std::optional<quint64> native_window_id;
    std::optional<quint32> native_window_dpi;
    QString presentation_status;
    QString flush_method;
};

class PerformanceApplication final : public QApplication {
public:
    using Clock = std::chrono::steady_clock;
    using FrameSink = std::function<void(const FrameMeasurement&)>;

    PerformanceApplication(int& argc, char** argv);
    static qint64 monotonic_nanoseconds();
    static QString clock_method();
    void verify_acceptance_window_dpi(QWidget& window, int percent) const;
    void observe_first_frame(QWidget& window, FrameSink sink);
    [[nodiscard]] const FrameMeasurement& frame_measurement() const;
    bool notify(QObject* receiver, QEvent* event) override;

private:
    QPointer<QWidget> observed_;
    FrameSink frame_sink_;
    FrameMeasurement frame_;
    bool frame_reported_ = false;
};

} // namespace nickel::app
