#include "app/AppServices.hpp"

#include "app/WorkerQueue.hpp"
#include "app/application_state.hpp"
#include "app/service/public_dto.hpp"
#include "app/service/request_registry.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <mutex>
#include <stop_token>
#include <utility>

namespace nickel::app {

struct ServiceDelivery {
    std::mutex mutex;
    QPointer<AppServices> target;
    std::uint64_t last_launch_ticket = 0;

    template<class Function>
    void post(Function function) {
        std::lock_guard lock(mutex);
        if (!target) return;
        const auto guard = target;
        QMetaObject::invokeMethod(target.data(), [guard, function = std::move(function)]() mutable {
            if (guard) function(guard);
        }, Qt::QueuedConnection);
    }

    bool accept_launch(std::uint64_t ticket) {
        std::lock_guard lock(mutex);
        if (!target || ticket == 0 || ticket <= last_launch_ticket) return false;
        last_launch_ticket = ticket;
        return true;
    }

    void close() {
        std::lock_guard lock(mutex);
        target.clear();
    }
};

struct ScheduledMonitor {
    ApplicationState::BackgroundWork work;
    std::shared_ptr<std::atomic<bool>> executing;
};

namespace {

constexpr std::size_t worker_capacity = 64;
constexpr std::size_t monitor_capacity = 16;
constexpr std::size_t ordinary_capacity = worker_capacity - monitor_capacity;

struct WorkPermit {
    explicit WorkPermit(std::shared_ptr<std::atomic<std::size_t>> value) : count(std::move(value)) {}
    ~WorkPermit() { release(); }
    void release() noexcept {
        if (!released.exchange(true)) count->fetch_sub(1);
    }
    std::shared_ptr<std::atomic<std::size_t>> count;
    std::atomic<bool> released{false};
};

struct MonitorInvocation;
thread_local MonitorInvocation* current_monitor = nullptr;

struct MonitorInvocation {
    MonitorInvocation(std::shared_ptr<WorkPermit> slot, std::shared_ptr<std::atomic<bool>> gate)
        : permit(std::move(slot)), executing(std::move(gate)), previous(current_monitor) {
        current_monitor = this;
    }
    ~MonitorInvocation() {
        current_monitor = previous;
        executing->store(false);
    }
    std::shared_ptr<WorkPermit> permit;
    std::shared_ptr<std::atomic<bool>> executing;
    MonitorInvocation* previous;
    bool successor_accepted = false;
};

std::shared_ptr<WorkPermit> reserve_work(const std::shared_ptr<std::atomic<std::size_t>>& count,
                                       std::size_t capacity) {
    auto current = count->load();
    while (current < capacity) {
        if (!count->compare_exchange_weak(current, current + 1)) continue;
        try { return std::make_shared<WorkPermit>(count); }
        catch (...) { count->fetch_sub(1); throw; }
    }
    return {};
}

AppServicesOptions system_options() {
    AppServicesOptions result;
    result.paths = PathContext::system();
    result.executable = result.paths.executable;
    if (QCoreApplication::instance()) {
        const auto arguments = QCoreApplication::arguments();
        for (qsizetype index = 1; index < arguments.size(); ++index)
            result.startup_arguments.push_back(arguments.at(index).toUtf8().toStdString());
    }
    return result;
}

void send_reply(const QPointer<AppServices>& target, RequestKind kind, const ui::UiReply& reply) {
    if (kind == RequestKind::Query) emit target->queryReady(reply);
    else emit target->commandFinished(reply);
}

void enqueue_operation(const std::function<bool(WorkerQueue::Work)>& submit,
                       const std::shared_ptr<RequestRegistry>& registry,
                       const std::shared_ptr<ServiceDelivery>& delivery,
                       ui::RequestId id, RequestKind kind,
                       std::function<OperationResult(const Cancellation&)> operation,
                       const std::shared_ptr<ApplicationState>& long_operation = {},
                       const std::shared_ptr<ApplicationState>& owner = {},
                       const std::shared_ptr<BackgroundOperation>& accepted = {}) {
    std::shared_ptr<RequestTicket> ticket;
    ui::UiError admission_error = public_error("busy");
    try { ticket = registry->begin(id, kind); }
    catch (const std::exception& error) { admission_error = public_error(error); }
    catch (...) { admission_error = public_error("internal"); }
    if (!ticket) {
        if (accepted) {
            owner->finish_operation(accepted, Json::object(), admission_error.code.toStdString());
            owner->publish(ui::UiTopic::Downloads, owner->downloads_snapshot());
        }
        delivery->post([id, kind, admission_error](const QPointer<AppServices>& target) {
            send_reply(target, kind, ui::UiReply{id, {}, admission_error});
        });
        return;
    }
    auto work_cancel = accepted ? accepted->cancellation : ticket->cancellation;
    WorkerQueue::Work work = [registry, delivery, ticket, owner, accepted, work_cancel, operation = std::move(operation)]
                              (const Cancellation& shutdown) mutable {
        std::stop_callback stop(shutdown.stop_token(), [owner, accepted, work_cancel]() mutable {
            if (accepted) (void)owner->cancel_operation(accepted->operation_id);
            else work_cancel.cancel();
        });
        ui::UiReply reply{ticket->id, {}, {}};
        OperationResult result;
        try {
            if (accepted) owner->start_operation(accepted);
            work_cancel.check();
            result = operation(work_cancel);
            if (!accepted) work_cancel.check();
            if (accepted) {
                owner->finish_operation(accepted, result.data, result.error_code);
                if (result.data.is_object()) result.data["operationId"] = accepted->operation_id;
                for (const auto& [topic, snapshot] : result.changes) owner->publish(topic, snapshot);
            }
            reply.data = to_public_json(result.data);
            if (!result.error_code.empty()) reply.error = public_error(result.error_code);
        } catch (const std::exception& error) {
            reply.error = public_error(error);
            if (accepted) owner->finish_operation(accepted, Json::object(), reply.error.code.toStdString());
        } catch (...) {
            reply.error = public_error("internal");
            if (accepted) owner->finish_operation(accepted, Json::object(), "internal");
        }
        if (accepted) owner->publish(ui::UiTopic::Downloads, owner->downloads_snapshot());
        if (!registry->accepts(ticket)) return;
        std::vector<std::pair<ui::UiTopic, QJsonObject>> changes;
        if (!reply.error.isError()) {
            try {
                if (!accepted) {
                    for (const auto& [topic, snapshot] : result.changes) {
                        if (owner && topic == ui::UiTopic::Launch) owner->publish(topic, snapshot);
                        else changes.emplace_back(topic, to_public_json(snapshot).toObject());
                    }
                }
            } catch (...) {
                reply.error = public_error("internal");
                changes.clear();
                result.media.clear();
            }
        } else result.media.clear();
        delivery->post([registry, ticket, reply = std::move(reply), changes = std::move(changes),
                        media = std::move(result.media)](const QPointer<AppServices>& target) {
            if (!registry->finish(ticket)) return;
            for (const auto& [topic, snapshot] : changes) {
                emit target->stateChanged(topic, snapshot);
                if (!target) return;
            }
            // Consumers approve opaque media keys from the reply before accepting pixels.
            send_reply(target, ticket->kind, reply);
            if (!target) return;
            for (const auto& [key, image] : media) {
                emit target->mediaReady(QString::fromUtf8(key), image);
                if (!target) return;
            }
        });
    };
    bool queued = false;
    ui::UiError rejected = public_error("busy");
    try {
        if (long_operation) {
            long_operation->schedule_operation(std::move(work), work_cancel);
            queued = true;
        } else queued = submit(std::move(work));
    } catch (const std::exception& error) {
        rejected = public_error(error);
    } catch (...) {
        rejected = public_error("internal");
    }
    if (!queued) {
        if (accepted) {
            owner->finish_operation(accepted, Json::object(), rejected.code.toStdString());
            owner->publish(ui::UiTopic::Downloads, owner->downloads_snapshot());
        }
        delivery->post([registry, ticket, rejected](const QPointer<AppServices>& target) {
            if (registry->finish(ticket))
                send_reply(target, ticket->kind, ui::UiReply{ticket->id, {}, rejected});
        });
    }
}

bool needs_long_admission(ui::UiQuery query) {
    switch (query) {
    case ui::UiQuery::Accounts:
    case ui::UiQuery::InstanceRoots:
    case ui::UiQuery::Settings:
    case ui::UiQuery::Downloads:
    case ui::UiQuery::RuntimeInformation:
    case ui::UiQuery::Onboarding:
    case ui::UiQuery::Mods:
    case ui::UiQuery::InstalledResources:
        return false;
    default:
        return true;
    }
}

bool owns_background_operation(ui::UiAction action) {
    switch (action) {
    case ui::UiAction::LaunchGame:
    case ui::UiAction::InstallVersion:
    case ui::UiAction::ImportPack:
    case ui::UiAction::InstallProject:
    case ui::UiAction::DownloadProjectFile:
    case ui::UiAction::ImportResources:
    case ui::UiAction::DeleteResource:
    case ui::UiAction::SetModEnabled:
    case ui::UiAction::RenameMod:
    case ui::UiAction::DeleteMods:
    case ui::UiAction::CheckModUpdates:
    case ui::UiAction::UpdateMods:
    case ui::UiAction::ExportMods:
    case ui::UiAction::ExportPack:
    case ui::UiAction::CleanJunk:
        return true;
    default:
        return false;
    }
}

bool needs_long_admission(ui::UiAction action) {
    // Authorization returns promptly; the single canonical task owns long admission.
    switch (action) {
    case ui::UiAction::LaunchGame:
    case ui::UiAction::CheckModUpdates:
    case ui::UiAction::InstallProject:
    case ui::UiAction::DownloadProjectFile:
        return false;
    default:
        break;
    }
    if (owns_background_operation(action)) return true;
    switch (action) {
    case ui::UiAction::BeginMicrosoftLogin:
    case ui::UiAction::LoginSkinServer:
    case ui::UiAction::SelectCharacter:
    case ui::UiAction::RefreshCharacters:
    case ui::UiAction::UpdateOfflineSkin:
    case ui::UiAction::ResetOfflineSkin:
    case ui::UiAction::ScanJunk:
    case ui::UiAction::CleanJunk:
    case ui::UiAction::ExportLogs:
    case ui::UiAction::ExportErrorReport:
    case ui::UiAction::FinishOnboarding:
        return true;
    default:
        return false;
    }
}

} // namespace

AppServices::AppServices(QObject* parent) : AppServices(system_options(), parent) {}

AppServices::AppServices(AppServicesOptions options, QObject* parent)
    : UiServices(parent), requests_(std::make_shared<RequestRegistry>(64)),
      state_(std::make_shared<ApplicationState>(std::move(options))),
      workers_(std::make_unique<WorkerQueue>(2, worker_capacity)), delivery_(std::make_shared<ServiceDelivery>()) {
    delivery_->target = this;
    qRegisterMetaType<ui::UiReply>();
    qRegisterMetaType<ui::UiTopic>();
    qRegisterMetaType<ui::DescriptionImageReply>();
    const auto delivery = delivery_;
    const std::weak_ptr<ApplicationState> state = state_;
    state_->set_sink([delivery, state](ui::UiTopic topic, Json value) {
        const auto owner = state.lock();
        if (!owner || owner->stopping.load()) return;
        const auto ticket = topic == ui::UiTopic::Launch
            ? value.value(launch_delivery_ticket_key, std::uint64_t{}) : 0;
        if (topic == ui::UiTopic::Launch) value.erase(launch_delivery_ticket_key);
        const auto snapshot = to_public_json(value).toObject();
        delivery->post([delivery, state, topic, snapshot, ticket, canonical = std::move(value)]
                       (const QPointer<AppServices>& target) {
            const auto current = state.lock();
            if (!current || current->stopping.load()) return;
            if (topic == ui::UiTopic::Launch) {
                {
                    std::lock_guard lock(current->state_mutex);
                    const auto id = canonical.value("launchId", std::string{});
                    const auto found = current->launches.find(id.empty() ? current->last_launch_id : id);
                    const auto latest = found == current->launches.end() ? Json{{"state", "idle"}} : found->second->public_state;
                    if (canonical != latest) return;
                }
                if (!delivery->accept_launch(ticket)) return;
            }
            emit target->stateChanged(topic, snapshot);
        });
    });
    state_->set_scheduler([delivery](std::chrono::milliseconds delay, ApplicationState::BackgroundWork work) {
        std::lock_guard lock(delivery->mutex);
        return delivery->target && delivery->target->scheduleWork(delay, std::move(work));
    });
    state_->set_monitor_scheduler([delivery](std::chrono::milliseconds delay, ApplicationState::BackgroundWork work) {
        std::lock_guard lock(delivery->mutex);
        return delivery->target && delivery->target->scheduleMonitorWork(delay, std::move(work));
    });
}

AppServices::~AppServices() {
    // Close delivery first so no queued reply can observe a half-torn-down state; clear the sinks
    // before state shutdown so cancelled work cannot publish into this dead target.
    delivery_->close();
    requests_->shutdown();
    state_->set_sink({});
    state_->set_scheduler({});
    state_->set_monitor_scheduler({});
    state_->shutdown();
    workers_->stop();
    workers_.reset();
    state_.reset();
}

bool AppServices::submitWork(Work work) {
    if (!work || state_->stopping.load()) return false;
    const auto permit = reserve_work(ordinary_pending_, ordinary_capacity);
    if (!permit) return false;
    return workers_->submit([permit, work = std::move(work)](const Cancellation& shutdown) {
        permit->release();
        work(shutdown);
    });
}

bool AppServices::scheduleWork(std::chrono::milliseconds delay, Work work) {
    if (!work || state_->stopping.load() || delay.count() < 0 || delay > std::chrono::hours(1)) return false;
    if (scheduled_.fetch_add(1) >= 64) {
        scheduled_.fetch_sub(1);
        return false;
    }
    std::shared_ptr<Work> pending;
    try { pending = std::make_shared<Work>(std::move(work)); }
    catch (...) { scheduled_.fetch_sub(1); throw; }
    const bool posted = QMetaObject::invokeMethod(this, [this, delay, pending] {
        QTimer::singleShot(delay, this, [this, pending] { runScheduled(pending); });
    }, Qt::QueuedConnection);
    if (!posted) scheduled_.fetch_sub(1);
    return posted;
}

bool AppServices::scheduleMonitorWork(std::chrono::milliseconds delay, Work work) {
    if (!work || state_->stopping.load() || delay.count() < 0 || delay > std::chrono::hours(1)) return false;
    auto* continuation = current_monitor && current_monitor->permit->count == monitor_pending_
        ? current_monitor : nullptr;
    if (continuation && continuation->successor_accepted) return false;
    const auto permit = continuation ? continuation->permit : reserve_work(monitor_pending_, monitor_capacity);
    if (!permit) return false;
    const auto executing = continuation ? continuation->executing : std::make_shared<std::atomic<bool>>(false);
    auto pending = std::make_shared<ScheduledMonitor>();
    pending->executing = executing;
    pending->work = [permit, executing, work = std::move(work)](const Cancellation& shutdown) {
        MonitorInvocation invocation(permit, executing);
        work(shutdown);
    };
    const bool posted = QMetaObject::invokeMethod(this, [this, delay, pending] {
        QTimer::singleShot(delay, this, [this, pending] { runMonitorScheduled(pending); });
    }, Qt::QueuedConnection);
    if (posted && continuation) continuation->successor_accepted = true;
    return posted;
}

void AppServices::runScheduled(const std::shared_ptr<Work>& work) {
    if (state_->stopping.load()) { scheduled_.fetch_sub(1); return; }
    if (submitWork(*work)) scheduled_.fetch_sub(1);
    else QTimer::singleShot(std::chrono::milliseconds(50), this, [this, work] { runScheduled(work); });
}

void AppServices::runMonitorScheduled(const std::shared_ptr<ScheduledMonitor>& work) {
    if (state_->stopping.load()) return;
    // A chain keeps its slot, and even a zero-delay successor cannot overlap its current callback.
    if (!work->executing->exchange(true)) {
        bool queued = false;
        try { queued = workers_->submit(work->work); }
        catch (...) { work->executing->store(false); }
        if (queued) return;
        work->executing->store(false);
    }
    QTimer::singleShot(std::chrono::milliseconds(50), this, [this, work] { runMonitorScheduled(work); });
}

void AppServices::request(ui::RequestId id, ui::UiQuery query, const QJsonObject& scope) {
    const auto state = state_;
    enqueue_operation([this](Work work) { return submitWork(std::move(work)); }, requests_, delivery_, id, RequestKind::Query,
        [state, query, scope](const Cancellation& cancellation) {
            return state->query(query, from_public_json(scope), cancellation);
        }, needs_long_admission(query) ? state : nullptr);
}

void AppServices::submit(ui::RequestId id, ui::UiAction action, const QJsonObject& arguments) {
    const auto state = state_;
    const auto submit = [this](Work work) { return submitWork(std::move(work)); };
    std::shared_ptr<BackgroundOperation> accepted;
    try {
        if (owns_background_operation(action)) accepted = state->begin_operation(from_public_json(arguments));
        if (action == ui::UiAction::CancelLaunch || action == ui::UiAction::CancelMicrosoftLogin
            || action == ui::UiAction::CancelCharacterSelection || action == ui::UiAction::CancelDownload)
            state->preflight_cancel(action, from_public_json(arguments));
    } catch (const std::exception& error) {
        const auto failure = public_error(error);
        enqueue_operation(submit, requests_, delivery_, id, RequestKind::Command,
            [failure](const Cancellation&) -> OperationResult {
                fail_static(failure.code.toUtf8().toStdString());
            });
        return;
    }
    enqueue_operation(submit, requests_, delivery_, id, RequestKind::Command,
        [state, action, arguments](const Cancellation& cancellation) {
            return state->action(action, from_public_json(arguments), cancellation);
        }, needs_long_admission(action) ? state : nullptr, state, accepted);
}

void AppServices::resolveDescriptionImage(ui::RequestId id, const QString& documentKey,
                                          const QString& opaqueKey) {
    const auto registry = requests_;
    const auto delivery = delivery_;
    const auto state = state_;
    auto ticket = registry->begin(id, RequestKind::DescriptionImage);
    const auto rejected = [delivery, id, documentKey, opaqueKey] {
        delivery->post([id, documentKey, opaqueKey](const QPointer<AppServices>& target) {
            emit target->descriptionImageReady({id, documentKey, opaqueKey, {}, public_error("busy")});
        });
    };
    if (!ticket) { rejected(); return; }
    const auto accepted = submitWork([registry, delivery, state, ticket, documentKey, opaqueKey]
                                          (const Cancellation& shutdown) {
        Cancellation cancellation = ticket->cancellation;
        std::stop_callback stop(shutdown.stop_token(), [cancellation]() mutable { cancellation.cancel(); });
        ui::DescriptionImageReply reply{ticket->id, documentKey, opaqueKey, {}, {}};
        std::shared_ptr<DescriptionLease> document;
        try {
            cancellation.check();
            if (documentKey.size() > 128 || opaqueKey.size() > 256) fail_static("invalid_argument");
            document = state->description(documentKey.toUtf8().toStdString());
            const auto key = opaqueKey.toUtf8().toStdString();
            if (!document->resources->approved(key)) fail_static("unauthorized");
            std::stop_callback release(document->cancellation.stop_token(),
                [cancellation]() mutable { cancellation.cancel(); });
            const auto resource = document->resources->requestVisible(key, cancellation);
            if (resource.state == documents::ResourceState::Denied) fail_static("unauthorized");
            if (resource.state != documents::ResourceState::Ready) fail_static("network");
            reply.image = ui::imageFromRaster(*resource.image);
            cancellation.check();
        } catch (const std::exception& error) {
            reply.error = public_error(error);
        } catch (...) {
            reply.error = public_error("internal");
        }
        if (!registry->accepts(ticket)) return;
        delivery->post([registry, ticket, document, reply = std::move(reply)]
                       (const QPointer<AppServices>& target) {
            if (document && document->released.load()) { registry->cancel(ticket->id); return; }
            if (registry->finish(ticket)) emit target->descriptionImageReady(reply);
        });
    });
    if (!accepted) {
        delivery->post([registry, ticket, documentKey, opaqueKey](const QPointer<AppServices>& target) {
            if (registry->finish(ticket))
                emit target->descriptionImageReady({ticket->id, documentKey, opaqueKey, {}, public_error("busy")});
        });
    }
}

void AppServices::releaseDescription(const QString& documentKey) {
    if (documentKey.size() <= 128) state_->release_description(documentKey.toUtf8().toStdString());
}

void AppServices::cancelRequest(ui::RequestId id) { requests_->cancel(id); }

} // namespace nickel::app
