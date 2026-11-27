#pragma once

#include "core/common/stop_compat.hpp"

namespace nickel {
class Cancellation {
public:
    Cancellation() = default;
    void cancel() noexcept { source_.request_stop(); }
    bool cancelled() const noexcept { return source_.stop_requested(); }
    NICKEL_STOP_TOKEN stop_token() const noexcept { return source_.get_token(); }
private:
    NICKEL_STOP_SOURCE source_;
};
} // namespace nickel
