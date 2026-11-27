// Production-path stop-token family test: this translation unit must always
// resolve to the standard family (NICKEL_STOP_COMPAT_PORTABLE == 0). It pins
// the routing contract and exercises the Cancellation facade plus the std
// primitives that the application layer depends on.
#include "core/common/stop_compat.hpp"
#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void production_routing_uses_standard_family() {
    static_assert(!NICKEL_STOP_COMPAT_PORTABLE, "production stop-compat test must run on the standard path");
    static_assert(std::is_same_v<NICKEL_STOP_SOURCE, ::std::stop_source>,
                  "production cancellation must route to std::stop_source");
    static_assert(std::is_same_v<NICKEL_STOP_TOKEN, ::std::stop_token>,
                  "production cancellation must route to std::stop_token");
}

void cancellation_facade_semantics() {
    nickel::Cancellation cancellation;
    require(!cancellation.cancelled(), "a fresh cancellation is not cancelled");
    cancellation.check();
    cancellation.cancel();
    cancellation.cancel();
    require(cancellation.cancelled(), "cancel marks the cancellation");
    bool threw = false;
    try {
        cancellation.check();
    } catch (const nickel::ErrorException& error) {
        threw = error.error().code == "cancelled";
    }
    require(threw, "check() throws the cancelled domain error after cancel()");
}

void token_equality_and_observation() {
    nickel::Cancellation first;
    nickel::Cancellation second;
    auto left = first.stop_token();
    auto right = first.stop_token();
    require(left == right, "tokens from one cancellation share state");
    require(left != second.stop_token(), "independent cancellations have distinct tokens");
    require(left.stop_possible() && !left.stop_requested(), "token observes a pending stop request");
    first.cancel();
    require(left.stop_requested(), "token observes the stop after cancel()");
    require(second.stop_token() != left, "canceling one cancellation leaves the other untouched");
}

void concurrent_request_stop_is_single_winner() {
    std::stop_source source;
    std::atomic<int> winners{0};
    std::atomic<int> callbacks{0};
    std::stop_callback count{source.get_token(), [&callbacks] { ++callbacks; }};
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) {
        threads.emplace_back([&source, &winners] {
            if (source.request_stop()) ++winners;
        });
    }
    for (auto& thread : threads) thread.join();
    require(winners.load() == 1, "exactly one concurrent request_stop() wins");
    require(callbacks.load() == 1, "registered callback runs exactly once");
    require(source.stop_requested(), "stop_requested() is true after the race");
}

void stop_aware_condition_variable_wakes_on_stop() {
    std::condition_variable_any variable;
    std::mutex mutex;
    std::stop_source source;
    bool flagged = false;
    std::thread waiter([&] {
        std::unique_lock<std::mutex> lock(mutex);
        variable.wait(lock, source.get_token(), [&] { return flagged; });
        require(source.stop_requested(), "stop-aware wait returned because of the stop request");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    source.request_stop();
    waiter.join();
}
}  // namespace

int main() {
    try {
        production_routing_uses_standard_family();
        cancellation_facade_semantics();
        token_equality_and_observation();
        concurrent_request_stop_is_single_winner();
        stop_aware_condition_variable_wakes_on_stop();
        std::cout << "foundation.stop_compat: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "foundation.stop_compat FAILED: " << error.what() << "\n";
        return 1;
    }
}
