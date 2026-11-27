#include "WorkerQueue.hpp"

#include <stdexcept>
#include <utility>

namespace nickel::app {

WorkerQueue::WorkerQueue(std::size_t workers, std::size_t capacity) : capacity_(capacity) {
    if (!workers || workers > 4 || !capacity || capacity > 1024)
        throw std::invalid_argument("Application worker queue bounds are invalid");
    workers_.reserve(workers);
    try {
        for (std::size_t index = 0; index < workers; ++index)
            workers_.emplace_back([this] { run(); });
    } catch (...) {
        stop();
        workers_.clear();
        throw;
    }
}

WorkerQueue::~WorkerQueue() {
    stop();
    workers_.clear();
}

bool WorkerQueue::submit(Work work) {
    if (!work) return false;
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || pending_.size() >= capacity_) return false;
        pending_.push_back(std::move(work));
    }
    changed_.notify_one();
    return true;
}

void WorkerQueue::stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        pending_.clear();
    }
    cancellation_.cancel();
    changed_.notify_all();
}

std::size_t WorkerQueue::pending() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}

void WorkerQueue::run() {
    for (;;) {
        Work work;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_) return;
            work = std::move(pending_.front());
            pending_.pop_front();
        }
        // Request wrappers translate errors to public replies; one failed request must not kill a worker.
        try { work(cancellation_); } catch (...) {}
    }
}

} // namespace nickel::app
