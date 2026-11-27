#include "app/application_state.hpp"
#include "app/service/public_dto.hpp"
#include "app/service/snapshots.hpp"
#include "core/network/control.hpp"

#include <algorithm>
#include <utility>

namespace nickel::app {

void ApplicationState::set_scheduler(ScheduleSink scheduler) {
    std::lock_guard lock(sink_mutex_);
    scheduler_ = std::move(scheduler);
}

void ApplicationState::set_monitor_scheduler(ScheduleSink scheduler) {
    std::lock_guard lock(sink_mutex_);
    monitor_scheduler_ = std::move(scheduler);
}

bool ApplicationState::schedule(std::chrono::milliseconds delay, BackgroundWork work) const {
    if (stopping.load() || !work || delay.count() < 0 || delay > std::chrono::hours(1)) return false;
    ScheduleSink scheduler;
    {
        std::lock_guard lock(sink_mutex_);
        scheduler = scheduler_;
    }
    return scheduler && !stopping.load() && scheduler(delay, std::move(work));
}

bool ApplicationState::schedule_monitor(std::chrono::milliseconds delay, BackgroundWork work) const {
    if (stopping.load() || !work || delay.count() < 0 || delay > std::chrono::hours(1)) return false;
    ScheduleSink scheduler;
    {
        std::lock_guard lock(sink_mutex_);
        scheduler = monitor_scheduler_;
    }
    return scheduler && !stopping.load() && scheduler(delay, std::move(work));
}

std::shared_ptr<BackgroundOperation> ApplicationState::begin_operation(const Json& args) {
    auto operation = std::make_shared<BackgroundOperation>();
    operation->operation_id = args.contains("operationId") ? operation_id(args, "operationId")
        : "operation-" + tasks::detail::unique_id();
    const auto snapshots = tasks->snapshots();
    std::shared_ptr<BackgroundOperation> retired;
    std::lock_guard lock(state_mutex);
    if (stopping.load()) fail_static("cancelled");
    if (background_operations_.contains(operation->operation_id)
        || std::any_of(snapshots.begin(), snapshots.end(), [&](const auto& task) {
            return task.id == operation->operation_id;
        })) fail_static("conflict");
    if (background_operations_.size() >= 64) {
        auto oldest = background_operations_.end();
        for (auto item = background_operations_.begin(); item != background_operations_.end(); ++item) {
            const auto& prior = item->second;
            if (!prior->handler_complete) continue;
            const auto task = std::find_if(snapshots.begin(), snapshots.end(), [&](const auto& snapshot) {
                return snapshot.id == prior->task_id;
            });
            if (task != snapshots.end() && (task->status == tasks::TaskStatus::Running
                || task->status == tasks::TaskStatus::Cancelling || task->status == tasks::TaskStatus::Committing)) continue;
            if (oldest == background_operations_.end() || prior->generation < oldest->second->generation) oldest = item;
        }
        if (oldest == background_operations_.end()) fail_static("busy");
        retired = std::move(oldest->second);
        background_operations_.erase(oldest);
    }
    operation->generation = ++operation_generation_;
    background_operations_.emplace(operation->operation_id, operation);
    return operation;
}

void ApplicationState::start_operation(const std::shared_ptr<BackgroundOperation>& operation) {
    if (!operation) return;
    std::lock_guard lock(state_mutex);
    const auto found = background_operations_.find(operation->operation_id);
    if (stopping.load() || found == background_operations_.end() || found->second != operation
        || operation->handler_complete) fail_static("cancelled");
    operation->handler_started = true;
}

void ApplicationState::register_operation_task(const Cancellation& cancellation, const std::shared_ptr<tasks::Task>& task) {
    if (!task || task->cancellation().stop_token() != cancellation.stop_token()) fail_static("integrity");
    bool closed;
    {
        std::lock_guard lock(state_mutex);
        std::erase_if(operation_tasks_, [](const auto& item) { return item.second.expired(); });
        if (!operation_tasks_.contains(task->id()) && operation_tasks_.size() >= 64) fail_static("busy");
        for (const auto& [id, prior] : operation_tasks_) {
            const auto live = prior.lock();
            if (live && live != task && live->cancellation().stop_token() == cancellation.stop_token()) fail_static("conflict");
        }
        for (const auto& [id, operation] : background_operations_) {
            if (operation->cancellation.stop_token() != cancellation.stop_token()) continue;
            if (!operation->task_id.empty() && operation->task_id != task->id()) fail_static("conflict");
            operation->task_id = task->id();
            break;
        }
        operation_tasks_[task->id()] = task;
        closed = stopping.load();
    }
    if (closed) {
        (void)task->cancel();
        fail_static("cancelled");
    }
}

std::shared_ptr<tasks::Task> ApplicationState::operation_task(const Cancellation& cancellation) const {
    std::shared_ptr<tasks::Task> task;
    {
        std::lock_guard lock(state_mutex);
        for (const auto& [id, candidate] : operation_tasks_) {
            const auto live = candidate.lock();
            if (live && live->cancellation().stop_token() == cancellation.stop_token()) {
                task = live;
                break;
            }
        }
    }
    return task;
}

void ApplicationState::finish_operation(const std::shared_ptr<BackgroundOperation>& operation, const Json& result,
                                         std::string error_code) {
    if (!operation) return;
    std::string task_id;
    if (result.is_object() && result.contains("taskId")) task_id = operation_id(result, "taskId");
    std::lock_guard lock(state_mutex);
    const auto found = background_operations_.find(operation->operation_id);
    if (found == background_operations_.end() || found->second != operation || operation->handler_complete) return;
    if (!task_id.empty()) {
        if (!operation->task_id.empty() && operation->task_id != task_id) fail_static("integrity");
        operation->task_id = std::move(task_id);
    }
    operation->error_code = std::move(error_code);
    operation->handler_complete = true;
}

bool ApplicationState::cancel_operation(std::string_view id) {
    std::shared_ptr<BackgroundOperation> operation;
    {
        std::lock_guard lock(state_mutex);
        const auto found = background_operations_.find(std::string(id));
        if (found != background_operations_.end()) operation = found->second;
    }
    return operation ? cancel_background(operation->cancellation) : tasks->cancel(id);
}

Json ApplicationState::pending_operation_snapshot(const std::set<std::string>& known_tasks) const {
    Json result = Json::array();
    std::lock_guard lock(state_mutex);
    for (const auto& [id, operation] : background_operations_) {
        if (!operation->task_id.empty()) {
            if (known_tasks.contains(operation->task_id)) continue;
            const auto task = operation_tasks_.find(operation->task_id);
            if (operation->handler_complete && (task == operation_tasks_.end() || task->second.expired())) continue;
        }
        const bool terminal = operation->handler_complete && operation->task_id.empty();
        const auto status = !operation->task_id.empty() ? "queued" : terminal
            ? operation->error_code.empty() ? "completed" : operation->error_code == "cancelled" ? "cancelled" : "error"
            : operation->cancellation.cancelled() ? "cancelling" : operation->handler_started ? "running" : "queued";
        const auto message = operation->error_code.empty() ? std::string{} : safe_public_message(operation->error_code);
        Json row{{"id", id}, {"operationId", id}, {"status", status}, {"title", "Prepare operation"},
            {"label", "Prepare operation"}, {"terminal", terminal},
            {"canCancel", !terminal && !operation->cancellation.cancelled()}, {"message", message},
            {"error", operation->error_code.empty() ? Json(nullptr) : Json{{"code", public_error(operation->error_code).code.toStdString()},
                {"message", message}}}};
        if (!operation->task_id.empty()) row["taskId"] = operation->task_id;
        if (!terminal) row["stages"] = Json::array({Json{{"name", operation->handler_started ? "prepare" : "queued"},
            {"completed", false}}});
        else row["stages"] = Json::array();
        result.push_back(std::move(row));
    }
    return result;
}

bool ApplicationState::pending_operations(const Cancellation* excluding) const {
    std::lock_guard lock(state_mutex);
    return std::any_of(background_operations_.begin(), background_operations_.end(), [&](const auto& item) {
        return !item.second->handler_complete && item.second->task_id.empty()
            && (!excluding || item.second->cancellation.stop_token() != excluding->stop_token());
    });
}

bool ApplicationState::cancel_background(const Cancellation& cancellation, std::shared_ptr<tasks::Task> task) const {
    if (task) return task->cancel();
    const auto gate = transport::detail::cancellation_gate(cancellation);
    std::unique_lock ordering(*gate, std::try_to_lock);
    if (!ordering.owns_lock()) return false;
    std::string task_id;
    {
        std::lock_guard lock(state_mutex);
        for (const auto& [id, operation] : background_operations_) {
            if (operation->cancellation.stop_token() != cancellation.stop_token()) continue;
            task_id = operation->task_id;
            if (operation->handler_complete && task_id.empty()) return false;
            break;
        }
        for (const auto& [id, launch] : launches) {
            if (launch->cancellation.stop_token() != cancellation.stop_token()) continue;
            if (!launch->preparing.load() || launch->public_state.value("state", std::string{}) == "running") return false;
            task = launch->preparation_task;
            break;
        }
        if (!task) for (const auto& [id, registered] : operation_tasks_) {
            const auto live = registered.lock();
            if (live && live->cancellation().stop_token() == cancellation.stop_token()) { task = live; break; }
        }
    }
    if (task || !task_id.empty()) {
        ordering.unlock();
        return task ? task->cancel() : tasks->cancel(task_id);
    }
    auto token = cancellation;
    token.cancel();
    return true;
}

void ApplicationState::schedule_operation(BackgroundWork work, const Cancellation& cancellation,
                                          std::shared_ptr<tasks::Task> task) {
    if (stopping.load()) fail_static("cancelled");
    if (task && task->cancellation().stop_token() != cancellation.stop_token()) fail_static("integrity");
    const auto weak = weak_from_this();
    operations_->enqueue(std::move(work), cancellation, [weak](OperationQueue::Work operation) {
        const auto state = weak.lock();
        return state && state->schedule(std::chrono::milliseconds(0), std::move(operation));
    }, [weak, cancellation, task = std::move(task)] {
        if (const auto state = weak.lock()) (void)state->cancel_background(cancellation, task);
        else if (task) (void)task->cancel();
        else {
            const auto gate = transport::detail::cancellation_gate(cancellation);
            std::unique_lock ordering(*gate, std::try_to_lock);
            if (ordering.owns_lock()) { auto token = cancellation; token.cancel(); }
        }
    });
}

void ApplicationState::preflight_cancel(ui::UiAction action, const Json& args) {
    if (stopping.load()) fail_static("cancelled");
    std::optional<Cancellation> token;
    if (action == ui::UiAction::CancelDownload) {
        (void)action_downloads(action, args);
        return;
    } else if (action == ui::UiAction::CancelLaunch) {
        const auto id = operation_id(args, "launchId");
        {
            std::lock_guard lock(state_mutex);
            const auto found = launches.find(id);
            if (found == launches.end()) fail_static("not_found");
            token = found->second->cancellation;
        }
        (void)cancel_background(*token);
        return;
    } else if (action == ui::UiAction::CancelMicrosoftLogin) {
        const auto id = operation_id(args, "operationId");
        const auto expected = bounded_text(args, "sessionId", 128, false);
        std::lock_guard lock(state_mutex);
        const auto found = auth_sessions.find(id);
        if (found == auth_sessions.end()) fail_static("not_found");
        if (!expected.empty() && expected != found->second->session_id) fail_static("unauthorized");
        token = found->second->cancellation;
    } else if (action == ui::UiAction::CancelCharacterSelection) {
        const auto id = bounded_text(args, "sessionId", 128);
        std::lock_guard lock(state_mutex);
        const auto found = auth_sessions.find(id);
        if (found == auth_sessions.end() || !found->second->operation_id.empty()) fail_static("not_found");
        token = found->second->cancellation;
    } else fail_static("invalid_argument");
    if (token) token->cancel();
}

OperationResult ApplicationState::action_downloads(ui::UiAction action, const Json& args) {
    if (action == ui::UiAction::DismissDownloadBubble) {
        std::lock_guard lock(state_mutex);
        download_bubble_dismissed = true;
    } else if (action != ui::UiAction::RefreshDownloads) {
        const auto id = operation_id(args, "id");
        std::shared_ptr<BackgroundOperation> operation;
        std::string task_id = id;
        bool handler_complete = false;
        {
            std::lock_guard lock(state_mutex);
            const auto found = background_operations_.find(id);
            if (found != background_operations_.end()) {
                operation = found->second;
                task_id = operation->task_id;
                handler_complete = operation->handler_complete;
            }
        }
        const auto snapshots = tasks->snapshots();
        const auto task = std::find_if(snapshots.begin(), snapshots.end(), [&](const auto& item) { return item.id == task_id; });
        if (!operation && task == snapshots.end()) fail_static("not_found");
        const bool terminal = task != snapshots.end()
            ? task->status == tasks::TaskStatus::Succeeded || task->status == tasks::TaskStatus::Failed
                || task->status == tasks::TaskStatus::Cancelled
            : handler_complete && task_id.empty();
        if (action == ui::UiAction::CancelDownload) {
            if (!terminal && !cancel_operation(id)
                && !(operation && operation->cancellation.cancelled())) fail_static("busy");
        } else if (action == ui::UiAction::DismissDownload || action == ui::UiAction::ClearDownloadError) {
            if (!terminal) fail_static("busy");
            std::lock_guard lock(state_mutex);
            auto& ids = action == ui::UiAction::DismissDownload ? dismissed_downloads : cleared_download_errors;
            if (ids.size() >= 64 && !ids.contains(id)) ids.erase(ids.begin());
            ids.insert(id);
        } else fail_static("invalid_argument");
    }
    OperationResult result;
    result.data = downloads_snapshot();
    result.changes.emplace_back(ui::UiTopic::Downloads, result.data);
    return result;
}

} // namespace nickel::app
