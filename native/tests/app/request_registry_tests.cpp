#include "app/service/request_registry.hpp"

#include "core/common/error.hpp"

#include <atomic>
#include <cassert>
#include <iostream>
#include <stop_token>
#include <thread>
#include <vector>

using nickel::ErrorException;
using nickel::app::RequestKind;
using nickel::app::RequestRegistry;

namespace {

template<class Work>
void expect_code(const char* code, Work&& work) {
    bool caught = false;
    try {
        work();
    } catch (const ErrorException& error) {
        caught = error.error().code == code;
    }
    assert(caught);
}

void identity_and_generation() {
    RequestRegistry requests(2);
    auto first = requests.begin(1, RequestKind::Query);
    auto other = requests.begin(2, RequestKind::Query);
    assert(requests.accepts(first));
    expect_code("busy", [&] { requests.begin(3, RequestKind::Command); });
    auto replacement = requests.begin(1, RequestKind::Command);
    assert(first->cancellation.cancelled());
    assert(!requests.accepts(first));
    assert(!requests.finish(first));
    assert(replacement->generation > first->generation);
    assert(requests.accepts(replacement));
    assert(requests.accepts(other));
    assert(requests.finish(replacement));
    assert(!requests.finish(replacement));
    assert(!requests.accepts(replacement));
    assert(requests.finish(other));
    assert(requests.size() == 0);
}

void subscription_cancellation() {
    RequestRegistry requests;
    auto first = requests.begin(10, RequestKind::DescriptionImage);
    auto second = requests.begin(11, RequestKind::DescriptionImage);
    auto stop_copy = first->cancellation;
    requests.cancel(10);
    assert(stop_copy.cancelled());
    assert(first->cancellation.cancelled());
    assert(!second->cancellation.cancelled());
    assert(!requests.accepts(first));
    assert(requests.accepts(second));
    assert(!requests.finish(first));
    assert(requests.finish(second));
    requests.cancel(999);
    expect_code("invalid_argument", [&] { requests.begin(0, RequestKind::Query); });
}

void stop_callbacks_are_not_locked() {
    RequestRegistry requests;
    auto first = requests.begin(20, RequestKind::Query);
    std::atomic<int> calls{0};
    std::stop_callback cancelled(first->cancellation.stop_token(), [&] {
        auto nested = requests.begin(21, RequestKind::Query);
        assert(requests.finish(nested));
        ++calls;
    });
    requests.cancel(20);
    assert(calls == 1);
    auto before_replacement = requests.begin(22, RequestKind::Query);
    std::stop_callback replaced(before_replacement->cancellation.stop_token(), [&] {
        assert(requests.size() == 1);
        ++calls;
    });
    auto replacement = requests.begin(22, RequestKind::Query);
    assert(calls == 2);
    requests.cancel(22);
    assert(replacement->cancellation.cancelled());
}

void bounded_parallel_lifetime() {
    RequestRegistry requests(64);
    std::atomic<int> completed{0};
    std::vector<std::thread> workers;
    for (unsigned i = 1; i <= 32; ++i) {
        workers.emplace_back([&, i] {
            auto ticket = requests.begin(i, RequestKind::Query);
            assert(requests.accepts(ticket));
            if (i % 2 == 0) {
                requests.cancel(i);
                assert(ticket->cancellation.cancelled());
                assert(!requests.finish(ticket));
            } else {
                assert(requests.finish(ticket));
            }
            ++completed;
        });
    }
    for (auto& worker : workers) worker.join();
    assert(completed == 32);
    assert(requests.size() == 0);
    auto active = requests.begin(100, RequestKind::Command);
    std::atomic<bool> callback{false};
    std::stop_callback on_shutdown(active->cancellation.stop_token(), [&] {
        assert(requests.size() == 0);
        callback = true;
    });
    requests.shutdown();
    assert(callback);
    assert(active->cancellation.cancelled());
    assert(!requests.accepts(active));
    expect_code("cancelled", [&] { requests.begin(101, RequestKind::Query); });
    requests.shutdown();
}

} // namespace

int main() {
    identity_and_generation();
    subscription_cancellation();
    stop_callbacks_are_not_locked();
    bounded_parallel_lifetime();
    std::cout << "request registry: 4 control groups passed\n";
}
