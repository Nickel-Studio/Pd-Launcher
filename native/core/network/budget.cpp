#include "core/network/budget.hpp"
#include "core/network/control.hpp"
#include "core/network/errors.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace nickel::transport {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto cancel_slice = std::chrono::milliseconds(20);
BudgetLimits checked_limits(BudgetLimits limits) {
    if (!limits.connections || limits.connections > 64 ||
        limits.bytes_per_second > 128ULL * 1024 * 1024 ||
        limits.buffer_bytes < 64 * 1024 || limits.buffer_bytes > 1024ULL * 1024 * 1024)
        detail::fail("budget_limits", "Invalid shared transfer budget");
    return limits;
}
void check_cancel(const SharedBudget::CancelCheck& cancelled) {
    detail::check_cancel(cancelled);
}
} // namespace

struct SharedBudget::Impl {
    mutable std::mutex mutex;
    std::condition_variable changed;
    BudgetLimits limits;
    std::uint32_t connections{};
    std::uint64_t buffers{};
    std::uint64_t transferred{};
    std::uint64_t sequence{};
    std::deque<std::uint64_t> connection_queue, buffer_queue, rate_queue;
    Clock::time_point rate_at{Clock::now()};
    long double tokens{};

    void credit() {
        const auto now = Clock::now();
        const auto elapsed = std::chrono::duration<long double>(now - rate_at).count();
        const auto cap = std::max<long double>(16 * 1024,
                                              static_cast<long double>(limits.bytes_per_second) / 10);
        tokens = std::min(cap, tokens + elapsed * limits.bytes_per_second);
        rate_at = now;
    }
};
struct SharedBudget::Reservation::Lease {
    std::shared_ptr<Impl> state;
    bool connection{};
    std::uint64_t bytes{};
    ~Lease() {
        if (!state) return;
        std::lock_guard lock(state->mutex);
        if (connection) --state->connections;
        else state->buffers -= bytes;
        state->changed.notify_all();
    }
};
SharedBudget::Reservation::Reservation() noexcept = default;
SharedBudget::Reservation::Reservation(std::unique_ptr<Lease> lease) : lease_(std::move(lease)) {}
SharedBudget::Reservation::~Reservation() = default;
SharedBudget::Reservation::Reservation(Reservation&&) noexcept = default;
SharedBudget::Reservation& SharedBudget::Reservation::operator=(Reservation&&) noexcept = default;
SharedBudget::Reservation::operator bool() const noexcept { return static_cast<bool>(lease_); }

SharedBudget::SharedBudget(BudgetLimits limits) : impl_(std::make_shared<Impl>()) {
    impl_->limits = checked_limits(limits);
}
SharedBudget::~SharedBudget() = default;
void SharedBudget::configure(BudgetLimits limits) {
    limits = checked_limits(limits);
    std::lock_guard lock(impl_->mutex);
    impl_->credit();
    impl_->limits = limits;
    impl_->tokens = 0;
    impl_->rate_at = Clock::now();
    impl_->changed.notify_all();
}
BudgetStats SharedBudget::stats() const {
    std::lock_guard lock(impl_->mutex);
    return {impl_->limits, impl_->connections,
            static_cast<std::uint32_t>(impl_->connection_queue.size()),
            impl_->buffers, impl_->transferred};
}
SharedBudget::Reservation SharedBudget::acquire_connection(const CancelCheck& cancelled) {
    auto lease = std::make_unique<Reservation::Lease>();
    lease->state = impl_;
    std::unique_lock lock(impl_->mutex);
    const auto ticket = ++impl_->sequence;
    impl_->connection_queue.push_back(ticket);
    try {
        for (;;) {
            check_cancel(cancelled);
            if (impl_->connection_queue.front() == ticket &&
                impl_->connections < impl_->limits.connections) {
                impl_->connection_queue.pop_front();
                ++impl_->connections;
                lease->connection = true;
                impl_->changed.notify_all();
                return Reservation(std::move(lease));
            }
            impl_->changed.wait_for(lock, cancel_slice);
        }
    } catch (...) {
        std::erase(impl_->connection_queue, ticket);
        impl_->changed.notify_all();
        lease->state.reset();
        throw;
    }
}
SharedBudget::Reservation SharedBudget::reserve_buffer(std::uint64_t bytes,
                                                       const CancelCheck& cancelled) {
    auto lease = std::make_unique<Reservation::Lease>();
    lease->state = impl_;
    std::unique_lock lock(impl_->mutex);
    const auto ticket = ++impl_->sequence;
    impl_->buffer_queue.push_back(ticket);
    try {
        for (;;) {
            check_cancel(cancelled);
            if (bytes > impl_->limits.buffer_bytes)
                detail::fail("response_limit", "Response exceeds shared buffer budget");
            if (impl_->buffer_queue.front() == ticket && impl_->buffers <= impl_->limits.buffer_bytes &&
                bytes <= impl_->limits.buffer_bytes - impl_->buffers) {
                impl_->buffer_queue.pop_front();
                impl_->buffers += bytes;
                lease->bytes = bytes;
                impl_->changed.notify_all();
                return Reservation(std::move(lease));
            }
            impl_->changed.wait_for(lock, cancel_slice);
        }
    } catch (...) {
        std::erase(impl_->buffer_queue, ticket);
        impl_->changed.notify_all();
        lease->state.reset();
        throw;
    }
}
void SharedBudget::throttle(std::size_t bytes, const CancelCheck& cancelled) {
    std::unique_lock lock(impl_->mutex);
    impl_->transferred += bytes;
    const auto ticket = ++impl_->sequence;
    impl_->rate_queue.push_back(ticket);
    try {
        while (bytes) {
            check_cancel(cancelled);
            impl_->credit();
            if (impl_->rate_queue.front() == ticket) {
                if (!impl_->limits.bytes_per_second) break;
                const auto amount = std::min<std::size_t>(bytes, 16 * 1024);
                if (impl_->tokens >= amount) {
                    impl_->tokens -= amount;
                    bytes -= amount;
                    if (bytes) {
                        impl_->rate_queue.pop_front();
                        impl_->rate_queue.push_back(ticket);
                        impl_->changed.notify_all();
                    }
                    continue;
                }
            }
            impl_->changed.wait_for(lock, cancel_slice);
        }
        std::erase(impl_->rate_queue, ticket);
        impl_->changed.notify_all();
    } catch (...) {
        std::erase(impl_->rate_queue, ticket);
        impl_->changed.notify_all();
        throw;
    }
}
void SharedBudget::wake() noexcept { impl_->changed.notify_all(); }
std::shared_ptr<SharedBudget> SharedBudget::process() {
    static auto budget = std::make_shared<SharedBudget>();
    return budget;
}
} // namespace nickel::transport
