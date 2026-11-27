// Portable-fallback stop-token family test. Compiled with
// NICKEL_FORCE_PORTABLE_CANCELLATION, so every NICKEL_STOP_* route and every
// nickel::stop_compat primitive below is the in-project implementation even on
// toolchains (MSVC) whose standard library would otherwise provide the family.
// This binary must not link nickel_foundation: the library objects are built
// on the standard path and mixing the two implementations of the same inline
// header classes in one binary would be an ODR violation.
#include "core/common/stop_compat.hpp"
#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
namespace compat = nickel::stop_compat;
using compat::stop_source;
using compat::stop_token;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void routing_selects_the_portable_implementation() {
    static_assert(NICKEL_STOP_COMPAT_PORTABLE, "the forced-fallback build must activate the portable layer");
    static_assert(std::is_same_v<NICKEL_STOP_SOURCE, ::nickel::stop_compat::stop_source>,
                  "the forced-fallback build must route cancellation.hpp to the portable stop_source");
    static_assert(std::is_same_v<NICKEL_STOP_TOKEN, ::nickel::stop_compat::stop_token>,
                  "the forced-fallback build must route cancellation.hpp to the portable stop_token");
}

void request_stop_is_idempotent() {
    stop_source source;
    require(source.stop_possible() && !source.stop_requested(), "a fresh source can request a stop");
    require(source.request_stop(), "the first request_stop() wins");
    require(!source.request_stop(), "the second request_stop() is a no-op");
    require(!source.request_stop(), "further request_stop() calls keep reporting false");
    require(source.stop_requested(), "stop_requested() stays true");
}

void stateless_sources_and_tokens_mimic_nostopstate() {
    stop_source stateless{compat::nostopstate};
    require(!stateless.stop_possible() && !stateless.stop_requested(), "nostopstate source cannot stop");
    require(!stateless.request_stop(), "request_stop() on a stateless source reports false");
    auto token = stateless.get_token();
    require(!token.stop_possible() && !token.stop_requested(), "token of a stateless source cannot stop");
    stop_token empty;
    require(empty == token, "stateless tokens compare equal");
    require(!empty.stop_possible() && !empty.stop_requested(), "default token is inert");
}

void token_outlives_source() {
    stop_token token;
    {
        stop_source source;
        token = source.get_token();
        require(token.stop_possible(), "token with a live source can stop");
        require(source.request_stop(), "stop requested before the source dies");
    }
    require(token.stop_requested(), "a requested stop survives source destruction");
    require(token.stop_possible(),
            "standard semantics: stop_possible() stays true once a stop was requested");
    {
        stop_token without_request;
        {
            stop_source quiet;
            without_request = quiet.get_token();
        }
        require(!without_request.stop_requested() && !without_request.stop_possible(),
                "without a request and without any source the token cannot stop");
    }
    stop_token other;
    require(other != token, "fresh token differs from a stateful token");
}

void token_swap_and_equality() {
    stop_source first;
    stop_source second;
    auto left = first.get_token();
    auto right = second.get_token();
    require(left != right, "tokens of distinct sources differ");
    left.swap(right);
    require(left == second.get_token() && right == first.get_token(), "swap exchanges token states");
    swap(left, right);  // ADL: hidden-friend swap, same call shape the app code uses
    require(left == first.get_token() && right == second.get_token(), "free swap exchanges back");
    first.request_stop();
    require(left.stop_requested() && !right.stop_requested(), "stop state follows the token");
}

void cancellation_facade_on_portable_fallback() {
    nickel::Cancellation cancellation;
    require(!cancellation.cancelled(), "portable cancellation starts clean");
    cancellation.check();
    auto token = cancellation.stop_token();
    cancellation.cancel();
    cancellation.cancel();
    require(cancellation.cancelled(), "cancel() marks the cancellation");
    require(token.stop_requested(), "token taken before cancel() observes the stop");
    bool threw = false;
    try {
        cancellation.check();
    } catch (const nickel::ErrorException& error) {
        threw = error.error().code == "cancelled";
    }
    require(threw, "check() throws the cancelled domain error");
}

void concurrent_request_stop_exactly_one_winner() {
    stop_source source;
    std::atomic<int> winners{0};
    std::atomic<int> callbacks{0};
    compat::stop_callback count{source.get_token(), [&callbacks] { ++callbacks; }};
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) {
        threads.emplace_back([&source, &winners] {
            if (source.request_stop()) ++winners;
        });
    }
    for (auto& thread : threads) thread.join();
    require(winners.load() == 1, "exactly one concurrent request_stop() wins");
    require(callbacks.load() == 1, "callback registered before the race runs exactly once");
}

void callbacks_fire_once_and_respect_lifetime() {
    stop_source source;
    std::atomic<int> fires{0};
    {
        compat::stop_callback early{source.get_token(), [&fires] { ++fires; }};
        require(fires.load() == 0, "callback does not run at registration");
    }
    source.request_stop();
    require(fires.load() == 0, "callback destroyed before the stop never runs");
    {
        compat::stop_callback late{source.get_token(), [&fires] { ++fires; }};
        require(fires.load() == 1, "registration after the stop fires immediately");
    }
    require(fires.load() == 1, "already-fired callback does not run again on destruction");
}

void multiple_callbacks_all_fire() {
    stop_source source;
    std::atomic<int> total{0};
    compat::stop_callback one{source.get_token(), [&total] { ++total; }};
    compat::stop_callback two{source.get_token(), [&total] { ++total; }};
    compat::stop_callback three{source.get_token(), [&total] { ++total; }};
    compat::stop_callback four{source.get_token(), [&total] { ++total; }};
    source.request_stop();
    require(total.load() == 4, "every registered callback runs exactly once");
}

void callback_destruction_never_races_the_invocation() {
    // Stress the registration / request_stop / destruction triangle: the
    // destructor must either remove the callback before it runs or block until
    // the concurrent invocation finished, and the callback must never run
    // after its owner died.
    for (int iteration = 0; iteration < 300; ++iteration) {
        stop_source source;
        auto token = source.get_token();
        std::atomic<int> fires{0};
        std::atomic<bool> inside{false};
        std::unique_ptr<compat::stop_callback<std::function<void()>>> callback(
            new compat::stop_callback<std::function<void()>>(token, [&] {
                inside.store(true, std::memory_order_release);
                ++fires;
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                inside.store(false, std::memory_order_release);
            }));
        std::thread requestor([&source, iteration] {
            std::this_thread::sleep_for(std::chrono::microseconds(iteration % 97));
            source.request_stop();
        });
        std::thread destroyer([&callback, iteration] {
            std::this_thread::sleep_for(std::chrono::microseconds((iteration * 31) % 113));
            callback.reset();
        });
        requestor.join();
        destroyer.join();
        require(!inside.load(std::memory_order_acquire),
                "destruction never returns while the callback is still running");
        require(fires.load() <= 1, "the callback runs at most once");
    }
}

void callbacks_registered_during_request_stop_fire_immediately() {
    stop_source source;
    std::atomic<int> fires{0};
    compat::stop_callback first{source.get_token(), [&] {
        // Registering another callback while the stop is in flight must invoke
        // it synchronously in the registering thread, like the standard.
        compat::stop_callback nested{source.get_token(), [&fires] { ++fires; }};
    }};
    source.request_stop();
    require(fires.load() == 1, "callback registered after the stop fires immediately, even nested");
}

void callback_may_request_other_stops() {
    stop_source outer;
    stop_source inner;
    compat::stop_callback link{outer.get_token(), [&inner] { inner.request_stop(); }};
    outer.request_stop();
    require(inner.stop_requested(), "a callback may stop another source without deadlocking");
}

void jthread_stops_and_joins_on_destruction() {
    std::atomic<bool> observed_stop{false};
    {
        compat::jthread worker{[&observed_stop](stop_token token) {
            while (!token.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            observed_stop.store(true, std::memory_order_release);
        }};
        require(worker.joinable(), "worker thread is joinable");
        require(!worker.get_stop_token().stop_requested(), "worker token starts clean");
        require(worker.get_stop_source().request_stop(), "explicit request_stop() works");
        worker.join();
        require(observed_stop.load(std::memory_order_acquire), "worker observed the stop request");
    }
}

void jthread_destructor_stops_pending_worker() {
    std::atomic<bool> observed_stop{false};
    {
        compat::jthread worker{[&observed_stop](stop_token token) {
            while (!token.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            observed_stop.store(true, std::memory_order_release);
        }};
    }
    require(observed_stop.load(std::memory_order_acquire),
            "jthread destructor requests the stop and joins the worker");
}

void jthread_without_token_argument() {
    std::atomic<bool> ran{false};
    compat::jthread plain{[&ran] { ran.store(true, std::memory_order_release); }};
    plain.join();
    require(ran.load(std::memory_order_acquire), "callable without stop_token parameter is supported");
}

void jthread_move_assignment_stops_the_previous_worker() {
    std::atomic<bool> observed_stop{false};
    compat::jthread first{[&observed_stop](stop_token token) {
        while (!token.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        observed_stop.store(true, std::memory_order_release);
    }};
    compat::jthread second{[] {}};
    first = std::move(second);
    require(observed_stop.load(std::memory_order_acquire), "move assignment stops the previous worker");
    require(!first.get_stop_token().stop_requested(), "the moved-in thread owns a fresh source");
    first.join();
}

void vector_of_jthreads_stops_all() {
    std::atomic<int> stopped{0};
    std::vector<compat::jthread> workers;
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([&stopped](stop_token token) {
            while (!token.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            ++stopped;
        });
    }
    workers.clear();
    require(stopped.load() == 4, "destroying a vector of jthreads stops and joins every worker");
}
}  // namespace

int main() {
    try {
        routing_selects_the_portable_implementation();
        request_stop_is_idempotent();
        stateless_sources_and_tokens_mimic_nostopstate();
        token_outlives_source();
        token_swap_and_equality();
        cancellation_facade_on_portable_fallback();
        concurrent_request_stop_exactly_one_winner();
        callbacks_fire_once_and_respect_lifetime();
        multiple_callbacks_all_fire();
        callback_destruction_never_races_the_invocation();
        callbacks_registered_during_request_stop_fire_immediately();
        callback_may_request_other_stops();
        jthread_stops_and_joins_on_destruction();
        jthread_destructor_stops_pending_worker();
        jthread_without_token_argument();
        jthread_move_assignment_stops_the_previous_worker();
        vector_of_jthreads_stops_all();
        std::cout << "foundation.stop_compat_fallback: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "foundation.stop_compat_fallback FAILED: " << error.what() << "\n";
        return 1;
    }
}
