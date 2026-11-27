#pragma once

#include "core/config/repository.hpp"
#include "core/network/budget.hpp"

#include <memory>
#include <string_view>

namespace nickel::app {

// Only changed fields enter this boundary; canonical JSON never makes a GUI round trip.
// merge_settings_fields() also enforces the structural budget (depth 32, 8192 nodes, size cap).
void merge_settings_fields(Json& current, const Json& changed);
Json apply_settings_patch(JsonRepository& repository, std::string_view document,
                          const Json& changed, const std::shared_ptr<transport::SharedBudget>& budget,
                          const Cancellation& cancellation);

} // namespace nickel::app
