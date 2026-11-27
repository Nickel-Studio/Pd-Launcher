#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace nickel::transport {

struct BudgetLimits {
    std::uint32_t connections{10};
    std::uint64_t bytes_per_second{};
    std::uint64_t buffer_bytes{64ULL * 1024 * 1024};
};
struct BudgetStats {
    BudgetLimits limits;
    std::uint32_t active_connections{};
    std::uint32_t queued_connections{};
    std::uint64_t reserved_buffer_bytes{};
    std::uint64_t transferred_bytes{};
};

class SharedBudget {
public:
    using CancelCheck = std::function<bool()>;
    class Reservation {
    public:
        Reservation() noexcept;
        ~Reservation();
        Reservation(Reservation&&) noexcept;
        Reservation& operator=(Reservation&&) noexcept;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        explicit operator bool() const noexcept;
    private:
        friend class SharedBudget;
        struct Lease;
        explicit Reservation(std::unique_ptr<Lease> lease);
        std::unique_ptr<Lease> lease_;
    };

    explicit SharedBudget(BudgetLimits limits = {});
    ~SharedBudget();
    SharedBudget(const SharedBudget&) = delete;
    SharedBudget& operator=(const SharedBudget&) = delete;
    void configure(BudgetLimits limits);
    BudgetStats stats() const;
    Reservation acquire_connection(const CancelCheck& cancelled);
    Reservation reserve_buffer(std::uint64_t bytes, const CancelCheck& cancelled);
    void throttle(std::size_t bytes, const CancelCheck& cancelled);
    void wake() noexcept;
    static std::shared_ptr<SharedBudget> process();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace nickel::transport
