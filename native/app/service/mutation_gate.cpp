#include "app/service/mutation_gate.hpp"

#include "core/common/error.hpp"

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace nickel::app {

struct MutationGate::State {
    struct Root {
        bool exclusive = false;
        std::unordered_set<std::string> instances;
    };
    std::mutex mutex;
    std::unordered_map<std::string, Root> roots;
};

MutationGate::MutationGate() : state_(std::make_shared<State>()) {}

MutationGate::Lease::Lease(std::shared_ptr<State> state, std::string root, std::string instance)
    : state_(std::move(state)), root_(std::move(root)), instance_(std::move(instance)) {}

MutationGate::Lease::~Lease() { release(); }

MutationGate::Lease::Lease(Lease&& other) noexcept
    : state_(std::move(other.state_)), root_(std::move(other.root_)),
      instance_(std::move(other.instance_)) {}

MutationGate::Lease& MutationGate::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        root_ = std::move(other.root_);
        instance_ = std::move(other.instance_);
    }
    return *this;
}

void MutationGate::Lease::release() noexcept {
    if (!state_) return;
    auto state = std::move(state_);
    std::lock_guard lock(state->mutex);
    const auto found = state->roots.find(root_);
    if (found == state->roots.end()) return;
    if (instance_.empty()) found->second.exclusive = false;
    else found->second.instances.erase(instance_);
    if (!found->second.exclusive && found->second.instances.empty()) state->roots.erase(found);
}

MutationGate::Lease MutationGate::instance(std::string_view root, std::string_view directory_id,
                                          const Cancellation& cancellation) {
    cancellation.check();
    if (root.empty() || directory_id.empty()) {
        throw ErrorException(Error{"invalid_argument", "Instance authorization is missing"});
    }
    std::string root_key(root);
    std::string instance_key(directory_id);
    std::lock_guard lock(state_->mutex);
    auto& scope = state_->roots[root_key];
    if (scope.exclusive || scope.instances.contains(instance_key)) {
        throw ErrorException(Error{"busy", "Another operation owns this instance"});
    }
    scope.instances.insert(instance_key);
    return Lease(state_, std::move(root_key), std::move(instance_key));
}

MutationGate::Lease MutationGate::root(std::string_view root, const Cancellation& cancellation) {
    cancellation.check();
    if (root.empty()) {
        throw ErrorException(Error{"invalid_argument", "Root authorization is missing"});
    }
    std::string root_key(root);
    std::lock_guard lock(state_->mutex);
    auto& scope = state_->roots[root_key];
    if (scope.exclusive || !scope.instances.empty()) {
        throw ErrorException(Error{"busy", "Another operation owns this root"});
    }
    scope.exclusive = true;
    return Lease(state_, std::move(root_key), {});
}

} // namespace nickel::app
