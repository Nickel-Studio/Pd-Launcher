#pragma once

#include "internal.hpp"
#include "native_services.hpp"
#include "core/common/cancellation.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <utility>

namespace nickel::packs::detail {
class NativeCancellation {
public:
    explicit NativeCancellation(std::stop_token stop)
        : callback_(stop, [this] { cancellation_.cancel(); }) {}
    const Cancellation& get() const noexcept { return cancellation_; }
private:
    Cancellation cancellation_;
    std::stop_callback<std::function<void()>> callback_;
};

inline void transfer_final_publish(InstallServices& destination, NativeInstallOptions& options) noexcept {
    destination.final_publish = std::exchange(options.final_publish, {});
}

template<class Fn>
bool native_owned_drain(std::timed_mutex& mutex, std::chrono::milliseconds timeout, Fn&& drain) {
    if (timeout <= std::chrono::milliseconds::zero()) return false;
    const auto started = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex, std::defer_lock);
    if (!lock.try_lock_for(timeout)) return false;
    const auto elapsed = std::chrono::ceil<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (elapsed >= timeout) return false;
    // Waiting for an active installation consumes the same budget as joining its owned children.
    return std::forward<Fn>(drain)(timeout - elapsed);
}

template<class Fn>
decltype(auto) native_service_call(Fn&& operation, std::string_view category, std::string_view message) {
    try { return std::forward<Fn>(operation)(); }
    catch (const instances::Error&) { throw; }
    catch (const nickel::ErrorException& error) {
        const auto& code = error.error().code;
        const auto safe_code = code == "Cancelled" || code == "cancelled" ? "cancelled"
            : code == "UnsafePath" || code == "unsafe_path" ? "unsafe_path"
            : code == "LimitExceeded" || code == "limit" ? "limit"
            : code == "AlreadyExists" || code == "Conflict" || code == "conflict" ? "conflict" : category;
        throw Error(std::string(safe_code), std::string(message));
    }
    catch (const std::exception&) { throw Error(std::string(category), std::string(message)); }
}
} // namespace nickel::packs::detail
