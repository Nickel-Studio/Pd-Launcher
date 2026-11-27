#include "app/service/mutation_gate.hpp"
#include "core/common/error.hpp"

#include <cassert>
#include <iostream>
#include <utility>

using nickel::Cancellation;
using nickel::ErrorException;
using nickel::app::MutationGate;

namespace {
template<class Work>
void busy(Work&& work) {
    bool caught = false;
    try { work(); } catch (const ErrorException& error) { caught = error.error().code == "busy"; }
    assert(caught);
}
}

int main() {
    MutationGate gate;
    Cancellation token;
    {
        auto first = gate.instance("registered-root", "directory-id", token);
        busy([&] { gate.instance("registered-root", "directory-id", token); });
        busy([&] { gate.root("registered-root", token); });
        auto other = gate.instance("registered-root", "other-id", token);
        auto other_root = gate.root("other-registered-root", token);
        assert(first && other && other_root);
        auto moved = std::move(first);
        assert(moved && !first);
    }
    {
        auto exclusive = gate.root("registered-root", token);
        busy([&] { gate.root("registered-root", token); });
        busy([&] { gate.instance("registered-root", "other-id", token); });
        auto elsewhere = gate.instance("other-root", "directory-id", token);
        assert(exclusive && elsewhere);
    }
    auto released = gate.instance("registered-root", "directory-id", token);
    token.cancel();
    bool cancelled = false;
    try { gate.root("other-root", token); }
    catch (const ErrorException& error) { cancelled = error.error().code == "cancelled"; }
    assert(cancelled);
    std::cout << "mutation gate: instance/root/move/cancel controls passed\n";
}
