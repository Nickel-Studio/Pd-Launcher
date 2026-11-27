#pragma once

#include "core/accounts/foundation_adapter.hpp"
#include "core/config/repository.hpp"
#include "core/config/vault.hpp"
#include "core/network/http.hpp"
#include "core/resources/network.hpp"

#include <memory>

namespace nickel::app {

using RepositoryAccountStorage = accounts::RepositoryAccountStorage;
using VaultAdapter = accounts::FoundationSecretVault;

struct NetworkConfiguration {
    transport::NetworkSnapshot snapshot;
    transport::BudgetLimits limits;
    resources::ProviderSettings community;
};

NetworkConfiguration network_configuration(const Json& settings);
resources::HttpFetch community_http(std::shared_ptr<transport::HttpClient> client);

} // namespace nickel::app
