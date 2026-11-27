#pragma once

#include "core/config/paths.hpp"
#include "ui/UiServices.hpp"

#include "core/common/cancellation.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nickel::app {

struct AppServicesOptions {
    PathContext paths;
    std::filesystem::path executable;
    std::vector<std::string> startup_arguments;
};

class ApplicationState;
class RequestRegistry;
class WorkerQueue;
struct ServiceDelivery;
struct ScheduledMonitor;
struct AppServicesTestProbe;

// Owns the worker pool, request registry and ApplicationState. Inputs arrive on the GUI thread;
// results are re-posted through a queued delivery, so no worker touches widgets directly.
class AppServices final : public ui::UiServices {
    Q_OBJECT
public:
    explicit AppServices(QObject* parent = nullptr);
    explicit AppServices(AppServicesOptions options, QObject* parent = nullptr);
    ~AppServices() override;

    void request(ui::RequestId id, ui::UiQuery query, const QJsonObject& scope) override;
    void submit(ui::RequestId id, ui::UiAction action, const QJsonObject& arguments) override;
    void resolveDescriptionImage(ui::RequestId id, const QString& documentKey,
                                  const QString& opaqueKey) override;
    void releaseDescription(const QString& documentKey) override;
    void cancelRequest(ui::RequestId id) override;

private:
    friend struct AppServicesTestProbe;
    using Work = std::function<void(const Cancellation&)>;
    bool submitWork(Work work);
    bool scheduleWork(std::chrono::milliseconds delay, Work work);
    bool scheduleMonitorWork(std::chrono::milliseconds delay, Work work);
    void runScheduled(const std::shared_ptr<Work>& work);
    void runMonitorScheduled(const std::shared_ptr<ScheduledMonitor>& work);
    std::atomic<std::size_t> scheduled_{0};
    std::shared_ptr<std::atomic<std::size_t>> ordinary_pending_ = std::make_shared<std::atomic<std::size_t>>(0);
    std::shared_ptr<std::atomic<std::size_t>> monitor_pending_ = std::make_shared<std::atomic<std::size_t>>(0);
    std::shared_ptr<RequestRegistry> requests_;
    std::shared_ptr<ApplicationState> state_;
    std::unique_ptr<WorkerQueue> workers_;
    std::shared_ptr<ServiceDelivery> delivery_;
};

} // namespace nickel::app
