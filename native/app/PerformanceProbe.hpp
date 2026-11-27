#pragma once

#include "AcceptancePaths.hpp"
#include "PerformanceApplication.hpp"
#include "ui/Bindings.hpp"

#include <QObject>

#include <functional>
#include <memory>
#include <optional>

namespace nickel::ui { class MainWindow; }

namespace nickel::app {

struct InputMeasurement {
    std::optional<qint64> posted_ns;
    std::optional<qint64> dispatched_ns;
    std::optional<qint64> reply_ns;
    QString request_nonce;
    ui::RequestId reply_id = 0;
    QString error_code;
    Json current_state = Json::object();
    std::string current_state_sha256;
    bool successful = false;
};

class PerformanceRecord final {
public:
    PerformanceRecord(const AcceptancePaths& paths, qint64 entry_ns);
    ~PerformanceRecord();
    PerformanceRecord(const PerformanceRecord&) = delete;
    PerformanceRecord& operator=(const PerformanceRecord&) = delete;
    [[nodiscard]] std::optional<AcceptancePhaseRequest> poll_phase_request();
    void marker(const char* name, const Json& fields = Json::object());
    void frame(const FrameMeasurement& measurement);
    void rendered_frame(const QImage& image, const QWidget* window = nullptr,
                        std::optional<qint64> capture_begin_ns = std::nullopt);
    void input(const InputMeasurement& measurement);
    void finish(int exit_code);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class PerformanceProbe final : public QObject {
public:
    using InputSink = std::function<void(const InputMeasurement&)>;

    PerformanceProbe(PerformanceApplication& application, ui::UiServices& services, QObject* parent = nullptr);
    void request_input_round_trip(const QString& request_nonce, InputSink sink);
    void request_phase_round_trip(const AcceptancePhaseRequest& phase, ui::MainWindow& window, InputSink sink);
    [[nodiscard]] const InputMeasurement& input_measurement() const;

protected:
    bool event(QEvent* event) override;

private:
    PerformanceApplication& application_;
    ui::Bindings bindings_;
    InputSink input_sink_;
    InputMeasurement input_;
    QPointer<ui::MainWindow> phase_window_;
    Json phase_state_ = Json::object();
    bool in_flight_ = false;
};

} // namespace nickel::app
