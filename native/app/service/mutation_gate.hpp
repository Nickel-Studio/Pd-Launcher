#pragma once

#include "core/common/cancellation.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace nickel::app {

// Keys are supplied only after canonical registered-root/directory authorization.
class MutationGate final {
    struct State;
public:
    class Lease final {
    public:
        Lease() = default;
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    private:
        friend class MutationGate;
        Lease(std::shared_ptr<State> state, std::string root, std::string instance);
        void release() noexcept;
        std::shared_ptr<State> state_;
        std::string root_;
        std::string instance_;
    };

    MutationGate();
    [[nodiscard]] Lease instance(std::string_view authorized_root, std::string_view directory_id,
                                  const Cancellation& cancellation);
    [[nodiscard]] Lease root(std::string_view authorized_root, const Cancellation& cancellation);
private:
    std::shared_ptr<State> state_;
};

} // namespace nickel::app
