#include "AcceptancePaths.hpp"
#include "AppServices.hpp"
#include "BuildInfo.hpp"
#include "PerformanceApplication.hpp"
#include "PerformanceProbe.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/paths.hpp"
#include "platform/process.hpp"
#include "ui/MainWindow.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QMessageBox>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

int requested_dpi(const nickel::app::AcceptancePaths& paths) {
    const auto state = paths.request().find("expectedState");
    if (state == paths.request().end() || !state->is_object() || !state->contains("dpiPercent")) return 0;
    const auto& dpi = state->at("dpiPercent");
    if (!dpi.is_number_integer()) throw std::runtime_error("Invalid acceptance DPI request");
    const auto result = dpi.get<int>();
    if (result < 50 || result > 800) throw std::runtime_error("Invalid acceptance DPI request");
    return result;
}
} // namespace

int main(int argc, char* argv[]) {
    std::optional<qint64> entry_ns;
    try { entry_ns = nickel::app::PerformanceApplication::monotonic_nanoseconds(); }
    catch (...) { /* Ordinary startup does not depend on the performance clock. */ }
    // Keep original argc/argv: Qt reconstructs the Windows command line as Unicode.
    nickel::app::PerformanceApplication application(argc, argv);
    const auto display_name = QString::fromUtf8(nickel::app::build::display_name.data());
    QCoreApplication::setApplicationName(QString::fromUtf8(nickel::app::build::executable_name.data()));
    QCoreApplication::setApplicationVersion(QString::fromUtf8(nickel::app::build::version_label.data()));
    QCoreApplication::setOrganizationDomain(QString::fromUtf8(nickel::app::build::application_id.data()));
    QApplication::setApplicationDisplayName(display_name);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/nickel/brand/icon.png")));

    const auto qt_arguments = QCoreApplication::arguments();
    const bool acceptance_requested = nickel::app::has_acceptance_arguments(qt_arguments);
    std::optional<nickel::app::AcceptancePaths> accepted;
    std::unique_ptr<nickel::app::PerformanceRecord> record;
    try {
        auto arguments = nickel::app::parse_production_arguments(qt_arguments);
        nickel::PathContext paths;
        if (arguments.acceptance_context) {
            accepted.emplace(nickel::app::AcceptancePaths::from_context(*arguments.acceptance_context, nickel::current_executable()));
            accepted->configure_process_environment();
            paths = accepted->context();
        } else paths = nickel::PathContext::system();
        const auto executable = paths.executable;
        if (arguments.performance) {
            if (!entry_ns || !accepted->request().contains("launchContext"))
                throw std::runtime_error("The performance clock or guarded phase channel is unavailable");
            record = std::make_unique<nickel::app::PerformanceRecord>(*accepted, *entry_ns);
        }
        const auto dpi = accepted ? requested_dpi(*accepted) : 0;
        int code = EXIT_FAILURE;
        {
            if (accepted) accepted->verify();
            nickel::app::AppServices services({std::move(paths), executable, std::move(arguments.business_arguments)});
            if (record) record->marker("servicesInitialized");
            nickel::ui::MainWindow window(services);
            std::unique_ptr<nickel::app::PerformanceProbe> probe;
            std::unique_ptr<QTimer> phase_timer;
            bool phase_in_flight = false;
            if (record) {
                probe = std::make_unique<nickel::app::PerformanceProbe>(application, services);
                const auto capture = [&] {
                    try {
                        const auto begin = nickel::app::PerformanceApplication::monotonic_nanoseconds();
                        const auto image = window.captureRenderedFrame(64 * 1024 * 1024);
                        record->rendered_frame(image, &window, begin);
                        phase_in_flight = false;
                    } catch (...) { QCoreApplication::exit(EXIT_FAILURE); }
                };
                application.observe_first_frame(window, [&](const nickel::app::FrameMeasurement& measurement) {
                    record->frame(measurement);
                });
                QObject::connect(&window, &nickel::ui::MainWindow::startupReady, &window, [&] {
                    try {
                        record->marker("startupReady", {{"basis", "normal-onboarding-CLI-portable-chain"}, {"presentationObserved", false}});
                    } catch (...) { QCoreApplication::exit(EXIT_FAILURE); }
                });
                QObject::connect(&application, &QCoreApplication::aboutToQuit, &window, [&] {
                    try { record->marker("aboutToQuit", {{"normalQtShutdownRequested", true}}); }
                    catch (...) { QCoreApplication::exit(EXIT_FAILURE); }
                });
                phase_timer = std::make_unique<QTimer>();
                phase_timer->setInterval(50);
                QObject::connect(phase_timer.get(), &QTimer::timeout, probe.get(), [&, capture] {
                    if (phase_in_flight) return;
                    try {
                        const auto phase = record->poll_phase_request();
                        if (!phase) return;
                        phase_in_flight = true;
                        if (phase->request.at("phase") == "shutdown") {
                            record->marker("normalShutdownRequested", {{"method", "MainWindow::close"}, {"presented", false}});
                            phase_timer->stop();
                            window.close();
                            return;
                        }
                        probe->request_phase_round_trip(*phase, window, [&, capture](const nickel::app::InputMeasurement& measurement) {
                            record->input(measurement);
                            if (!measurement.successful) { QCoreApplication::exit(EXIT_FAILURE); return; }
                            // The fresh matching runtime reply observes actual route/theme before this capture.
                            QTimer::singleShot(0, &window, capture);
                        });
                    } catch (...) { QCoreApplication::exit(EXIT_FAILURE); }
                });
            }
            window.show();
            if (dpi) application.verify_acceptance_window_dpi(window, dpi);
            if (record) {
                record->marker("windowShown", {{"showRequested", true}, {"presentationObserved", false}});
                phase_timer->start();
            }
            code = application.exec();
            if (record) record->marker("eventLoopReturned", {{"exitCode", code}});
        }
        if (accepted) accepted->verify();
        if (record) record->finish(code);
        return code;
    } catch (...) {
        if (record) {
            try { record->marker("startupOrRuntimeFailure", {{"errorCode", "native-initialization-failed"}, {"success", false}}); }
            catch (...) { /* Failure must not become a successful evidence receipt. */ }
        }
        // Acceptance failures are non-interactive and precede any system-root fallback.
        if (acceptance_requested) {
            std::fputs("Palladium: explicit acceptance initialization failed; no fallback selected.\n", stderr);
        } else {
            QMessageBox::critical(nullptr, QObject::tr("%1 could not start").arg(display_name),
                QObject::tr("%1 could not initialize its native services. "
                            "No fallback configuration directory was selected.").arg(display_name));
        }
        return EXIT_FAILURE;
    }
}
