#include "export.hpp"
#include "mods.hpp"
#include "utility.hpp"

namespace nickel::resources {

std::optional<packs::PackFile> resolveLocalResourceBlocking(
    CommunityService& community, const Path& file, std::string_view relative,
    bool modrinthOnly, const Cancellation& cancel, std::uint64_t maximumBytes) {
    cancel.check();
    if (!validArchiveRelativePath(relative) || relative.ends_with('/'))
        fail("policy_denied", "Export resource path is not a regular relative file");
    const auto slash = relative.find('/');
    const auto group = relative.substr(0, slash);
    if (slash == std::string_view::npos || (group != "mods" && group != "resourcepacks" &&
        group != "shaderpacks" && group != "datapacks"))
        fail("policy_denied", "Export resource is outside approved content groups");
    detail::FileReader pin(file, maximumBytes);
    const auto hashes = hashResourceFile(file, cancel, maximumBytes);
    const auto resolved = community.resolveFileHashes(hashes.sha1, hashes.sha512,
        modrinthOnly, hashes.curseForgeFingerprint, cancel);
    pin.verifyUnchanged();
    cancel.check();
    if (!resolved) return {};
    if (resolved->file.sizeBytes != hashes.sizeBytes || resolved->file.sha1 != hashes.sha1 ||
        resolved->file.sha512 != hashes.sha512 || (modrinthOnly && resolved->source != Source::Modrinth))
        fail("integrity", "Exact export descriptor differs from the local resource");
    packs::PackFile result;
    result.path = std::string(relative);
    result.downloads = {checkedUrl(resolved->file.url, providerFileRules())};
    result.size = hashes.sizeBytes;
    result.sha1 = hashes.sha1;
    result.sha512 = hashes.sha512;
    return result;
}

ExportResolver makeExportResolver(std::shared_ptr<CommunityService> community,
                                  bool modrinthOnly, std::uint64_t maximumBytes) {
    if (!community || !maximumBytes || maximumBytes > 512ULL * 1024 * 1024)
        fail("invalid_input", "Export resolver requires a service and bounded file budget");
    return [community = std::move(community), modrinthOnly, maximumBytes](
        const Path& file, std::string_view relative, std::stop_token stop) {
        Cancellation cancel;
        std::stop_callback stopped(stop, [&] { cancel.cancel(); });
        return resolveLocalResourceBlocking(*community, file, relative, modrinthOnly, cancel, maximumBytes);
    };
}

packs::ExportServices bindExportResolver(packs::ExportServices services,
                                        std::shared_ptr<CommunityService> community,
                                        bool modrinthOnly, std::uint64_t maximumBytes) {
    services.resolve_resource = makeExportResolver(std::move(community), modrinthOnly, maximumBytes);
    services.validate_resource_download_url = [modrinthOnly](std::string_view url) {
        const auto approved = checkedUrl(url, providerFileRules());
        if (modrinthOnly && parseUrl(approved).host != "cdn.modrinth.com")
            fail("policy_denied", "Modrinth-only export cannot approve another provider");
    };
    return services;
}

} // namespace nickel::resources
