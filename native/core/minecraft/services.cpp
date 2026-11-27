#include "services.hpp"

#include <mutex>
#include <optional>
#include <stdexcept>

namespace nickel::minecraft {
namespace {
std::mutex servicesMutex;
std::optional<Services> configuredServices;
}

void configure_native_services(Services services) {
    if (!services.cancellationCheck || !services.verify || !services.atomicWrite || !services.spawn) {
        throw std::invalid_argument("Minecraft foundation adapters are incomplete");
    }
    std::lock_guard lock(servicesMutex);
    configuredServices = std::move(services);
}

Services native_services() {
    std::lock_guard lock(servicesMutex);
    if (!configuredServices) throw std::logic_error("Minecraft native services have not been configured by AppServices");
    return *configuredServices;
}

CancelCheck cancellation_check(const Services& services, const Cancellation& cancellation,
                                std::stop_token extra) {
    return [callback = services.cancellationCheck, cancellation, extra] {
        if (extra.stop_requested()) throw ErrorException(Error{"cancelled", "Minecraft preparation cancelled"});
        cancellation.check();
        if (callback) callback(cancellation);
        if (extra.stop_requested()) throw ErrorException(Error{"cancelled", "Minecraft preparation cancelled"});
    };
}

}
