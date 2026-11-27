#pragma once

#include "core/common/cancellation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace nickel::app {

enum class RequestKind { Query, Command, DescriptionImage };

struct RequestTicket {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    RequestKind kind = RequestKind::Query;
    Cancellation cancellation;
};

// Every UI subscription owns its stop source, including subscriptions to cached work.
class RequestRegistry final {
public:
    explicit RequestRegistry(std::size_t capacity = 64);
    ~RequestRegistry();
    RequestRegistry(const RequestRegistry&) = delete;
    RequestRegistry& operator=(const RequestRegistry&) = delete;

    [[nodiscard]] std::shared_ptr<RequestTicket> begin(std::uint64_t id, RequestKind kind);
    [[nodiscard]] bool accepts(const std::shared_ptr<RequestTicket>& ticket) const;
    [[nodiscard]] bool finish(const std::shared_ptr<RequestTicket>& ticket);
    void cancel(std::uint64_t id);
    void shutdown();
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_;
    const std::size_t capacity_;
    std::uint64_t generation_ = 0;
    bool closed_ = false;
    std::unordered_map<std::uint64_t, std::shared_ptr<RequestTicket>> requests_;
};

} // namespace nickel::app
