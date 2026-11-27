#pragma once

#include "network.hpp"
#include "core/documents/body.hpp"

#include <mutex>

namespace nickel::resources {

struct InstallRequest {
    InstanceIdentity instance;
    std::string instanceRevision;
    Source source = Source::Modrinth;
    ResourceKind kind = ResourceKind::Mod;
    std::string projectId;
    std::string versionId;
    std::string newInstanceName;
};

struct DownloadPlan {
    Source source = Source::Modrinth;
    ResourceKind kind = ResourceKind::Mod;
    std::string projectId;
    std::string versionId;
    VersionFile file;
    std::vector<RequestCandidate> candidates;
};

struct ResolvedResourceFile {
    Source source = Source::Modrinth;
    std::string projectId;
    std::string versionId;
    VersionFile file;
};

struct InstallReceipt {
    std::string taskId;
    std::string fileName;
    Path destination;
    bool alreadyPresent = false;
};

struct Progress {
    std::string stage;
    std::uint64_t completedBytes = 0;
    std::uint64_t totalBytes = 0;
};
using ProgressCallback = std::function<void(const Progress&)>;

// Narrow callback boundaries into the shared instance/task/transaction services.
// A mutation lease MUST survive until the final transaction commit.
struct ResourceServices {
    std::function<InstanceContext(const InstanceIdentity&)> context;
    std::function<std::shared_ptr<void>(const InstanceIdentity&, const std::string&)> acquireMutation;
    std::function<InstallReceipt(const InstanceContext&, const DownloadPlan&, std::shared_ptr<void>,
                                 const Cancellation&, const ProgressCallback&)> install;
    std::function<InstallReceipt(const InstallRequest&, const DownloadPlan&, const Cancellation&,
                                 const ProgressCallback&)> installPack;
};

// All calls run on a caller-owned worker, never the GUI thread.
class CommunityService final {
public:
    explicit CommunityService(CachedHttp& http, ProviderSettings settings = {}, ResourceServices services = {});
    void setSettings(ProviderSettings settings);
    ProviderSettings settings() const;
    InstanceContext instanceContext(const InstanceIdentity& instance) const;
    SearchPage search(const SearchQuery& query, const Cancellation& cancel = {});
    Project project(Source source, const std::string& idOrSlug, const Cancellation& cancel = {});
    VersionPage versions(Source source, ResourceKind kind, const std::string& idOrSlug, Filter filter,
                         std::uint32_t offset, std::uint32_t limit, const Cancellation& cancel = {}, bool forceRefresh = false);
    std::vector<ProjectVersion> allVersions(Source source, ResourceKind kind, const std::string& idOrSlug,
                                           const Cancellation& cancel = {}, bool forceRefresh = false);
    ProjectVersion exactVersion(Source source, const std::string& projectId, const std::string& versionId,
                                const Cancellation& cancel = {});
    ResolvedDependency resolveDependency(Source source, const Dependency& dependency, const Cancellation& cancel = {});
    documents::FullDescription fullDescription(Source source, const std::string& projectId,
                                              const Cancellation& cancel = {}, bool forceRefresh = false);
    DownloadPlan prepareDownload(Source source, ResourceKind kind, const std::string& projectId,
                                 const std::string& versionId, const Cancellation& cancel = {});
    InstallReceipt install(const InstallRequest& request, const Cancellation& cancel = {}, const ProgressCallback& progress = {});
    ProjectVersion lookupModrinthHash(const std::string& sha1, const Cancellation& cancel = {});
    std::optional<ProjectVersion> lookupCurseForgeFingerprint(std::uint32_t fingerprint, const Cancellation& cancel = {});
    std::optional<ResolvedResourceFile> resolveFileHashes(const std::string& sha1, const std::string& sha512,
                                                        bool modrinthOnly, std::optional<std::uint32_t> fingerprint = {},
                                                        const Cancellation& cancel = {});
private:
    CachedHttp& http_;
    mutable std::mutex settingsMutex_;
    ProviderSettings settings_;
    ResourceServices services_;
    CachedResponse json(Source source, const std::string& suffix, const std::string& scope,
                        ResponseValidator validator, const Cancellation& cancel, bool refresh = false,
                        const Bytes& body = {});
};

SourceSearchPage readSearchResponse(Source source, ResourceKind kind, const Json& document,
                                   std::uint32_t offset, std::uint32_t limit);
std::vector<ProjectVersion> readVersionsResponse(Source source, const Json& document, const std::string& expectedProject = {});
Project readProjectResponse(Source source, const Json& document, const std::string& expectedIdentity = {});
ProjectVersion readVersionResponse(Source source, const Json& object, const std::string& expectedProject = {});

} // namespace nickel::resources
