#pragma once

#include "community.hpp"
#include "core/packs/packs.hpp"

namespace nickel::resources {

std::optional<packs::PackFile> resolveLocalResourceBlocking(
    CommunityService& community, const Path& file, std::string_view relative,
    bool modrinthOnly, const Cancellation& cancel = {},
    std::uint64_t maximumBytes = 512ULL * 1024 * 1024);

using ExportResolver = std::function<std::optional<packs::PackFile>(
    const Path&, std::string_view, std::stop_token)>;
ExportResolver makeExportResolver(std::shared_ptr<CommunityService> community,
                                  bool modrinthOnly,
                                  std::uint64_t maximumBytes = 512ULL * 1024 * 1024);
// Bind both exact descriptors and the provider URL approval required by packs.
packs::ExportServices bindExportResolver(packs::ExportServices services,
                                        std::shared_ptr<CommunityService> community,
                                        bool modrinthOnly,
                                        std::uint64_t maximumBytes = 512ULL * 1024 * 1024);

} // namespace nickel::resources
