#include "app/service/operation_queue.hpp"
#include "app/WorkerQueue.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

using nickel::Cancellation;
using nickel::ErrorException;
using nickel::app::OperationQueue;
using nickel::app::WorkerQueue;
using namespace std::chrono_literals;

namespace {
template<class Work>
void rejected(Work work, const char* code) {
    bool caught = false;
    try { work(); }
    catch (const ErrorException& error) { caught = error.error().code == code; }
    assert(caught);
}

void serial_and_control() {
    WorkerQueue workers(2, 64);
    auto queue = std::make_shared<OperationQueue>(4);
    const OperationQueue::Dispatcher dispatch = [&](auto work) { return workers.submit(std::move(work)); };
    std::promise<void> started, release, short_done, finished;
    auto release_future = release.get_future().share();
    std::vector<int> order;
    std::atomic<int> live{0}, maximum{0};
    queue->enqueue([&](const Cancellation&) {
        maximum.store(++live);
        order.push_back(1);
        started.set_value();
        assert(release_future.wait_for(3s) == std::future_status::ready);
        --live;
    }, {}, dispatch);
    assert(started.get_future().wait_for(3s) == std::future_status::ready);
    Cancellation cancelled;
    queue->enqueue([&](const Cancellation& token) {
        assert(token.cancelled());
        order.push_back(2);
    }, cancelled, dispatch);
    cancelled.cancel();
    queue->enqueue([&](const Cancellation&) {
        maximum.store(std::max(maximum.load(), ++live));
        order.push_back(3);
        --live;
        throw std::runtime_error("controlled operation failure");
    }, {}, dispatch);
    queue->enqueue([&](const Cancellation&) { order.push_back(4); finished.set_value(); }, {}, dispatch);
    rejected([&] { queue->enqueue([](const Cancellation&) {}, {}, dispatch); }, "busy");
    assert(workers.submit([&](const Cancellation&) { short_done.set_value(); }));
    assert(short_done.get_future().wait_for(3s) == std::future_status::ready);
    assert(queue->pending() == 4);
    release.set_value();
    assert(finished.get_future().wait_for(3s) == std::future_status::ready);
    workers.stop();
    assert(maximum.load() == 1);
    assert((order == std::vector<int>{1, 2, 3, 4}));
}

void rejection_and_reentry() {
    auto queue = std::make_shared<OperationQueue>(2);
    rejected([&] {
        queue->enqueue([](const Cancellation&) {}, {}, [&](auto) {
            rejected([&] { queue->enqueue([](const Cancellation&) {}, {}, [](auto) { return true; }); }, "busy");
            return false;
        });
    }, "busy");
    assert(queue->pending() == 0);
    OperationQueue::Work scheduled;
    const OperationQueue::Dispatcher dispatch = [&](auto work) { scheduled = std::move(work); return true; };
    int calls = 0;
    queue->enqueue([&](const Cancellation&) {
        ++calls;
        queue->enqueue([&](const Cancellation&) { ++calls; }, {}, dispatch);
    }, {}, dispatch);
    auto work = std::move(scheduled);
    work({});
    assert(calls == 2 && queue->pending() == 0);
    Cancellation cancelled;
    cancelled.cancel();
    rejected([&] { queue->enqueue([](const Cancellation&) {}, cancelled, dispatch); }, "cancelled");
}

void close_without_callback_locks() {
    auto queue = std::make_shared<OperationQueue>(2);
    OperationQueue::Work scheduled;
    const OperationQueue::Dispatcher dispatch = [&](auto work) { scheduled = std::move(work); return true; };
    Cancellation token;
    bool callback = false, ran = false;
    std::stop_callback cancelled(token.stop_token(), [&] {
        callback = true;
        assert(queue->pending() == 0);
    });
    queue->enqueue([&](const Cancellation&) { ran = true; }, token, dispatch);
    queue->close();
    assert(callback && token.cancelled());
    scheduled({});
    assert(!ran && queue->pending() == 0);
    rejected([&] { queue->enqueue([](const Cancellation&) {}, {}, dispatch); }, "cancelled");
}
} // namespace

int main() {
    serial_and_control();
    rejection_and_reentry();
    close_without_callback_locks();
    std::cout << "operation admission: serial/control/FIFO/cancel/failure/shutdown passed\n";
}
