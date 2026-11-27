#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>

namespace nickel { class Cancellation; }
namespace nickel::transport::detail {
using CancelCheck = std::function<bool()>;
CancelCheck cancellation_check(const Cancellation& cancel);
std::shared_ptr<Cancellation> make_cancellation();
void request_cancel(const Cancellation& cancel);
void check_cancel(const CancelCheck& cancelled);
void interruptible_wait(std::chrono::milliseconds delay, const CancelCheck& cancelled);

// One ordering gate per shared cancellation state: cancellation precedes or follows publication.
std::shared_ptr<std::mutex> cancellation_gate(const Cancellation& cancel);
} // namespace nickel::transport::detail
