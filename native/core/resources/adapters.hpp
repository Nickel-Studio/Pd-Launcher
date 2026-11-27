#pragma once

#include "mods.hpp"
#include "core/instances/instances.hpp"
#include "core/network/http.hpp"
#include "core/tasks/tasks.hpp"

namespace nickel::resources {

HttpFetch makeHttpFetch(std::shared_ptr<transport::HttpClient> http);
// Bind related provider/file/body requests to one caller-owned operation snapshot.
HttpFetch makeHttpFetch(std::shared_ptr<transport::HttpClient> http, const transport::NetworkSnapshot& snapshot);

// Caller supplies a provider-approved plan and an unused leaf in its owned Task
// staging directory. This only downloads/verifies; it never creates a Task,
// acquires mutation authority or publishes. Keep that authority through commit.
void downloadResourceStaged(transport::HttpClient& http, const transport::NetworkSnapshot& snapshot,
                            const DownloadPlan& plan, const Path& staged, const ModLimits& limits = {},
                            const Cancellation& cancel = {}, const transport::Progress& progress = {});

// Only registered roots are resolved. AppServices supplies the running snapshot and
// a mutation lease shared with launch, settings changes and instance deletion.
InstanceContext resolveInstanceContext(const instances::InstanceRegistry& registry,
                                       const InstanceIdentity& identity,
                                       const Json& globalSettings, bool running);

ResourceServices bindResourceInstaller(std::shared_ptr<transport::HttpClient> http,
                                       std::shared_ptr<tasks::Registry> registry,
                                       ResourceServices authority, ModLimits limits = {});
ModServices bindModServices(ModServices services, ModLimits limits = {});
// Updates preserve the installed name/state and bind caller-owned Task steps.
// The Task-aware Mods path records pending/final facts; never follow it with complete_work.
ModServices bindModUpdater(std::shared_ptr<transport::HttpClient> http,
                           std::shared_ptr<tasks::Registry> registry,
                           ModServices services, ModLimits limits = {});

} // namespace nickel::resources
