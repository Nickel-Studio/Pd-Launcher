#include "app/service/request_registry.hpp"

#include "core/common/error.hpp"

#include <limits>
#include <utility>
#include <vector>

namespace nickel::app {

RequestRegistry::RequestRegistry(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
        throw ErrorException(Error{"invalid_argument", "Request capacity must be positive"});
    }
}

RequestRegistry::~RequestRegistry() { shutdown(); }

std::shared_ptr<RequestTicket> RequestRegistry::begin(std::uint64_t id, RequestKind kind) {
    if (id == 0) {
        throw ErrorException(Error{"invalid_argument", "Request identifier must be nonzero"});
    }
    std::shared_ptr<RequestTicket> previous;
    std::shared_ptr<RequestTicket> ticket;
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            throw ErrorException(Error{"cancelled", "Application is shutting down"});
        }
        const auto found = requests_.find(id);
        if (found == requests_.end() && requests_.size() >= capacity_) {
            throw ErrorException(Error{"busy", "Too many pending requests"});
        }
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
            throw ErrorException(Error{"busy", "Request generation exhausted"});
        }
        ticket = std::make_shared<RequestTicket>();
        ticket->id = id;
        ticket->generation = ++generation_;
        ticket->kind = kind;
        if (found != requests_.end()) {
            previous = std::move(found->second);
            found->second = ticket;
        } else {
            requests_.emplace(id, ticket);
        }
    }
    // Stop callbacks may call into domains; never invoke them under the registry lock.
    if (previous) previous->cancellation.cancel();
    return ticket;
}

bool RequestRegistry::accepts(const std::shared_ptr<RequestTicket>& ticket) const {
    if (!ticket || ticket->cancellation.cancelled()) return false;
    std::lock_guard lock(mutex_);
    const auto found = requests_.find(ticket->id);
    return !closed_ && found != requests_.end() && found->second == ticket &&
           found->second->generation == ticket->generation;
}

bool RequestRegistry::finish(const std::shared_ptr<RequestTicket>& ticket) {
    if (!ticket) return false;
    std::lock_guard lock(mutex_);
    const auto found = requests_.find(ticket->id);
    if (closed_ || found == requests_.end() || found->second != ticket ||
        found->second->generation != ticket->generation || ticket->cancellation.cancelled()) {
        return false;
    }
    requests_.erase(found);
    return true;
}

void RequestRegistry::cancel(std::uint64_t id) {
    std::shared_ptr<RequestTicket> ticket;
    {
        std::lock_guard lock(mutex_);
        const auto found = requests_.find(id);
        if (found == requests_.end()) return;
        ticket = std::move(found->second);
        requests_.erase(found);
    }
    ticket->cancellation.cancel();
}

void RequestRegistry::shutdown() {
    std::vector<std::shared_ptr<RequestTicket>> tickets;
    {
        std::lock_guard lock(mutex_);
        if (closed_) return;
        closed_ = true;
        tickets.reserve(requests_.size());
        for (auto& [id, ticket] : requests_) {
            (void)id;
            tickets.push_back(std::move(ticket));
        }
        requests_.clear();
    }
    for (const auto& ticket : tickets) ticket->cancellation.cancel();
}

std::size_t RequestRegistry::size() const {
    std::lock_guard lock(mutex_);
    return requests_.size();
}

} // namespace nickel::app
