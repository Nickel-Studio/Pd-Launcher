#include "core/network/control.hpp"
#include "core/common/cancellation.hpp"

#include <algorithm>
#include <thread>
#include <vector>

namespace nickel::transport::detail {
CancelCheck cancellation_check(const Cancellation& cancel) {
    return [token = cancel.stop_token()] { return token.stop_requested(); };
}
std::shared_ptr<Cancellation> make_cancellation() { return std::make_shared<Cancellation>(); }
void request_cancel(const Cancellation& cancel) {
    auto shared = cancel;
    shared.cancel();
}
void check_cancel(const CancelCheck& cancelled) {
    if (cancelled && cancelled())
        throw ErrorException(Error{"cancelled", "Operation cancelled"});
}
void interruptible_wait(std::chrono::milliseconds delay, const CancelCheck& cancelled) {
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (std::chrono::steady_clock::now() < deadline) {
        check_cancel(cancelled);
        std::this_thread::sleep_for(std::min(std::chrono::milliseconds(20),
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())));
    }
    check_cancel(cancelled);
}
std::shared_ptr<std::mutex> cancellation_gate(const Cancellation& cancel) {
    struct Entry { std::stop_token token; std::weak_ptr<std::mutex> gate; };
    static std::mutex mutex;
    static std::vector<Entry> entries;
    std::lock_guard lock(mutex);
    std::erase_if(entries, [](const Entry& entry) { return entry.gate.expired(); });
    const auto token = cancel.stop_token();
    for (const auto& entry : entries) {
        if (entry.token == token) {
            if (auto gate = entry.gate.lock()) return gate;
        }
    }
    auto gate = std::make_shared<std::mutex>();
    entries.push_back({token, gate});
    return gate;
}
} // namespace nickel::transport::detail
