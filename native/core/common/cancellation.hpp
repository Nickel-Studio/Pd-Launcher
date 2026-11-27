#pragma once

#include "core/common/stop_compat.hpp"
#include "core/common/error.hpp"

namespace nickel {
class Cancellation {
public:
    Cancellation() = default;
    void cancel() noexcept { source_.request_stop(); }
    [[nodiscard]] bool cancelled() const noexcept { return source_.stop_requested(); }
    [[nodiscard]] NICKEL_STOP_TOKEN stop_token() const noexcept { return source_.get_token(); }
    void check() const {
        if (cancelled()) throw ErrorException(Error{"cancelled", "Operation cancelled"});
    }
private:
    NICKEL_STOP_SOURCE source_;
};
}
