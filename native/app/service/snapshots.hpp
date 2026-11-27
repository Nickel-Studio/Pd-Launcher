#pragma once

#include "core/accounts/accounts.hpp"
#include "core/common/json.hpp"
#include "core/instances/settings.hpp"
#include "core/resources/types.hpp"
#include "core/tasks/tasks.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace nickel::app {

// Canonical one-way projections for the UI: no account tokens, OS handles or internal paths.
std::string build_version();
Json build_changelog();
Json default_app_settings();
Json effective_settings_json(const instances::EffectiveSettings& settings);
Json instance_scope(const resources::InstanceIdentity& identity, const std::filesystem::path& root);
Json account_dto(const accounts::PublicAccount& account, bool privacy);
Json project_dto(const resources::Project& project);
Json version_dto(const resources::ProjectVersion& version);
Json task_dto(const tasks::Snapshot& snapshot);
std::string safe_public_message(std::string_view code);
std::uint64_t unix_milliseconds();
std::uint32_t bounded_unsigned(const Json& value, std::string_view field, std::uint32_t fallback,
                               std::uint32_t maximum);
resources::Strings selected_ids(const Json& value, std::string_view field = "ids");
resources::Source selected_source(const Json& value);
resources::ResourceKind selected_kind(const Json& value);

} // namespace nickel::app
