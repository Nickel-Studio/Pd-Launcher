#include "settings.hpp"
#include "core_adapters.hpp"
#include "core/common/error.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace nickel::app {
namespace {

void validate_fields(const Json& fields) {
    if (!fields.is_object())
        throw ErrorException({"invalid_settings", "A settings patch must be an object", Json::object()});
    std::vector<std::pair<const Json*, unsigned>> pending{{&fields, 0}};
    std::size_t nodes = 0;
    while (!pending.empty()) {
        const auto [value, depth] = pending.back();
        pending.pop_back();
        if (depth > 32 || ++nodes > 8192)
            throw ErrorException({"invalid_settings", "The settings patch exceeds its structural budget", Json::object()});
        if (value->is_object() || value->is_array())
            for (const auto& child : *value) pending.emplace_back(&child, depth + 1);
        if (value->is_number_float() && !std::isfinite(value->get<double>()))
            throw ErrorException({"invalid_settings", "The settings patch contains an invalid number", Json::object()});
    }
    if (fields.dump().size() > settings_max_bytes)
        throw ErrorException({"invalid_settings", "The settings patch is too large", Json::object()});
}

} // namespace

void merge_settings_fields(Json& current, const Json& changed) {
    validate_fields(changed);
    if (!current.is_object()) current = Json::object();
    // Unlike RFC 7396, null is an explicit value (for local setting inheritance).
    current.update(changed, true);
}

Json apply_settings_patch(JsonRepository& repository, std::string_view document,
                          const Json& changed, const std::shared_ptr<transport::SharedBudget>& budget,
                          const Cancellation& cancellation) {
    if (document != "settings.json" && document != "game.json")
        throw ErrorException({"invalid_settings", "The settings document is not writable through this port", Json::object()});
    if (!budget) throw std::invalid_argument("The shared network budget is required");
    transport::BudgetLimits committed_limits;
    auto updated = repository.update(document, Json::object(), [&](Json& current) {
        merge_settings_fields(current, changed);
        if (document == "settings.json") committed_limits = network_configuration(current).limits;
    }, settings_max_bytes, &cancellation);
    if (document == "settings.json") budget->configure(committed_limits);
    return updated;
}

} // namespace nickel::app
