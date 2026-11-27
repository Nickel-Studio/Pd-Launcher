#include "app/WorkerQueue.hpp"
#include "app/service/operation_queue.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace {
using nickel::Cancellation;
using nickel::Json;
using nickel::app::OperationQueue;
using nickel::app::WorkerQueue;
using namespace std::chrono_literals;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void real_admission_and_handlers() {
    struct Progress {
        std::mutex mutex;
        std::condition_variable changed;
        bool release = false;
        unsigned entered = 0;
        unsigned finished = 0;
        unsigned active = 0;
        unsigned maximum = 0;
        unsigned cancelled_handlers = 0;
    };
    const auto progress = std::make_shared<Progress>();
    const auto operations = std::make_shared<OperationQueue>();
    WorkerQueue workers(2, 8);
    const auto dispatch = [&workers](OperationQueue::Work work) { return workers.submit(std::move(work)); };
    const auto job = [progress](const Cancellation& cancellation) {
        std::stop_callback wake(cancellation.stop_token(), [progress] { progress->changed.notify_all(); });
        std::unique_lock lock(progress->mutex);
        const auto index = ++progress->entered;
        ++progress->active;
        progress->maximum = std::max(progress->maximum, progress->active);
        progress->changed.notify_all();
        progress->changed.wait_for(lock, 5s, [&] { return progress->release || cancellation.cancelled(); });
        if (cancellation.cancelled()) ++progress->cancelled_handlers;
        --progress->active;
        ++progress->finished;
        progress->changed.notify_all();
        lock.unlock();
        if (index == 1) throw std::runtime_error("Expected first-operation failure");
    };
    Cancellation first;
    Cancellation second;
    operations->enqueue(job, first, dispatch);
    bool started;
    {
        std::unique_lock lock(progress->mutex);
        started = progress->changed.wait_for(lock, 2s, [&] { return progress->entered == 1; });
    }
    operations->enqueue(job, second, dispatch);
    second.cancel();
    const auto response = std::make_shared<std::promise<Json>>();
    auto query = response->get_future();
    const bool submitted = workers.submit([response](const Cancellation& cancellation) {
        try {
            cancellation.check();
            response->set_value(Json::parse("{\"counter\":18446744073709551615}"));
        } catch (...) { response->set_exception(std::current_exception()); }
    });
    const bool query_ready = query.wait_for(2s) == std::future_status::ready;
    unsigned entered_before_release;
    {
        std::lock_guard lock(progress->mutex);
        entered_before_release = progress->entered;
        progress->release = true;
    }
    progress->changed.notify_all();
    bool finished;
    unsigned maximum;
    unsigned cancelled_handlers;
    {
        std::unique_lock lock(progress->mutex);
        finished = progress->changed.wait_for(lock, 3s, [&] { return progress->finished == 2; });
        maximum = progress->maximum;
        cancelled_handlers = progress->cancelled_handlers;
    }
    operations->close();
    workers.stop();
    require(started && submitted && query_ready, "Serial long work starves the real query worker");
    require(entered_before_release == 1 && maximum == 1 && finished, "Long work bypasses serial admission");
    require(cancelled_handlers == 1, "Cancelled pending operation skips its owner handler");
    require(query.get()["counter"] == std::numeric_limits<std::uint64_t>::max(), "Query loses UINT64 data");
}

struct Captured {
    Captured(std::weak_ptr<OperationQueue> owner, std::shared_ptr<std::promise<void>> released)
        : owner(std::move(owner)), released(std::move(released)) {}
    ~Captured() {
        // Capture destruction must permit re-entry after the admission mutex is released.
        if (const auto queue = owner.lock()) (void)queue->pending();
        released->set_value();
    }
    std::weak_ptr<OperationQueue> owner;
    std::shared_ptr<std::promise<void>> released;
};

void rejected_and_closed_captures() {
    const auto operations = std::make_shared<OperationQueue>(1);
    const auto rejected_signal = std::make_shared<std::promise<void>>();
    auto rejected_release = rejected_signal->get_future();
    auto rejected_capture = std::make_shared<Captured>(operations, rejected_signal);
    bool rejected = false;
    try {
        operations->enqueue([capture = std::move(rejected_capture)](const Cancellation&) {}, Cancellation{},
                            [](OperationQueue::Work) { return false; });
    } catch (const nickel::ErrorException& error) { rejected = error.error().code == "busy"; }
    require(rejected && rejected_release.wait_for(1s) == std::future_status::ready,
            "Rejected dispatcher leaks or destroys captures under its mutex");
    require(operations->pending() == 0, "Rejected start retains an admitted operation");

    const auto closed_signal = std::make_shared<std::promise<void>>();
    auto closed_release = closed_signal->get_future();
    auto closed_capture = std::make_shared<Captured>(operations, closed_signal);
    OperationQueue::Work admitted_runner;
    const auto hold_dispatch = [&admitted_runner](OperationQueue::Work work) {
        admitted_runner = std::move(work);
        return true;
    };
    Cancellation cancelled;
    operations->enqueue([capture = std::move(closed_capture)](const Cancellation&) {}, cancelled, hold_dispatch);
    operations->close();
    admitted_runner = {};
    require(cancelled.cancelled() && closed_release.wait_for(1s) == std::future_status::ready,
            "Shutdown retains pending captures or skips shared cancellation");
    require(operations->pending() == 0, "Closed queue retains a pending operation");
}
} // namespace

int main() {
    try {
        real_admission_and_handlers();
        rejected_and_closed_captures();
        std::cout << "PASS real OperationQueue/WorkerQueue admission, query, cancellation, exception and capture controls\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL queue controls: " << failure.what() << '\n';
        return 1;
    }
}
