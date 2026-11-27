#pragma once

#include "core/common/cancellation.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace nickel::app {

// Serial admission on the existing worker queue; this class owns no threads.
class OperationQueue final : public std::enable_shared_from_this<OperationQueue> {
public:
    using Work = std::function<void(const Cancellation&)>;
    using Dispatcher = std::function<bool(Work)>;

    explicit OperationQueue(std::size_t capacity = 64) : capacity_(capacity) {
        if (!capacity || capacity > 64) throw ErrorException({"invalid_argument", "Invalid operation capacity"});
    }

    void enqueue(Work work, const Cancellation& cancellation, const Dispatcher& dispatch,
                 std::function<void()> cancel = {}) {
        cancellation.check();
        if (!work || !dispatch) throw ErrorException({"invalid_argument", "Missing operation dispatcher"});
        auto entry = std::make_shared<Entry>(Entry{std::move(work), cancellation, std::move(cancel)});
        bool start = false;
        {
            std::lock_guard lock(mutex_);
            if (phase_ == Phase::Closed) throw ErrorException({"cancelled", "Application is stopping"});
            if (phase_ == Phase::Starting || pending_.size() + (active_ ? 1 : 0) >= capacity_)
                throw ErrorException({"busy", "Operation queue is full"});
            pending_.push_back(entry);
            if (phase_ == Phase::Idle) {
                phase_ = Phase::Starting;
                start = true;
            }
        }
        if (!start) return;
        const auto owner = shared_from_this();
        bool accepted = false;
        try {
            accepted = dispatch([owner](const Cancellation& shutdown) { owner->run(shutdown); });
        } catch (...) {
            reject_start(entry);
            throw;
        }
        if (!accepted) {
            reject_start(entry);
            throw ErrorException({"busy", "Operation dispatcher rejected work"});
        }
        std::lock_guard lock(mutex_);
        if (phase_ == Phase::Starting) phase_ = Phase::Running;
    }

    void close() noexcept {
        std::deque<std::shared_ptr<Entry>> pending;
        std::shared_ptr<Entry> active;
        {
            std::lock_guard lock(mutex_);
            if (phase_ == Phase::Closed) return;
            phase_ = Phase::Closed;
            pending.swap(pending_);
            active = active_;
        }
        if (active) active->request_cancel();
        for (auto& entry : pending) entry->request_cancel();
        // Task cancellation honors its external commit gate; captures die outside this mutex.
    }

    std::size_t pending() const {
        std::lock_guard lock(mutex_);
        return pending_.size() + (active_ ? 1 : 0);
    }

private:
    struct Entry {
        Work work;
        Cancellation cancellation;
        std::function<void()> cancel;

        void request_cancel() noexcept {
            if (!cancel) { cancellation.cancel(); return; }
            try { cancel(); } catch (...) {}
        }
    };
    enum class Phase { Idle, Starting, Running, Closed };

    void reject_start(const std::shared_ptr<Entry>& entry) {
        std::shared_ptr<Entry> rejected;
        {
            std::lock_guard lock(mutex_);
            if (phase_ == Phase::Starting && !pending_.empty() && pending_.front() == entry) {
                rejected = std::move(pending_.front());
                pending_.pop_front();
                phase_ = Phase::Idle;
            }
        }
    }

    void run(const Cancellation& shutdown) {
        std::stop_callback stop(shutdown.stop_token(), [this] { close(); });
        for (;;) {
            std::shared_ptr<Entry> entry;
            {
                std::lock_guard lock(mutex_);
                if (phase_ == Phase::Closed || pending_.empty()) {
                    if (phase_ != Phase::Closed) phase_ = Phase::Idle;
                    active_.reset();
                    return;
                }
                phase_ = Phase::Running;
                entry = std::move(pending_.front());
                pending_.pop_front();
                active_ = entry;
            }
            // Cancelled jobs still enter their owner's terminal/rollback handler.
            try { entry->work(entry->cancellation); } catch (...) {}
            {
                std::lock_guard lock(mutex_);
                active_.reset();
            }
            entry.reset();
        }
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    Phase phase_ = Phase::Idle;
    std::deque<std::shared_ptr<Entry>> pending_;
    std::shared_ptr<Entry> active_;
};

} // namespace nickel::app
