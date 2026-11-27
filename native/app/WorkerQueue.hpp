#pragma once

#include "core/common/cancellation.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nickel::app {

// Fixed worker pool with a bounded queue. submit() fails instead of blocking when the queue is
// full; stop() drops pending work, cancels the shared token and waits for workers to exit.
class WorkerQueue final {
public:
    using Work = std::function<void(const Cancellation&)>;
    explicit WorkerQueue(std::size_t workers = 2, std::size_t capacity = 64);
    ~WorkerQueue();
    WorkerQueue(const WorkerQueue&) = delete;
    WorkerQueue& operator=(const WorkerQueue&) = delete;
    bool submit(Work work);
    void stop() noexcept;
    std::size_t pending() const;

private:
    void run();
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Work> pending_;
    Cancellation cancellation_;
    bool stopping_ = false;
    std::vector<std::jthread> workers_;
};

} // namespace nickel::app
