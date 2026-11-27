#pragma once

#include "community.hpp"

#include <span>

namespace nickel::tasks { class Task; }


namespace nickel::resources {

enum class ModState { Enabled, Disabled, Old };
enum class ModAction { Enable, Disable, Delete, Copy, InstallLocal, Update };

struct ModEntry {
    std::string fileName;
    Strings physicalFiles;
    std::string name;
    std::string description;
    std::string version;
    Strings modIds;
    Strings tags;
    std::string iconEntry;
    std::string duplicateGroup;
    bool enabled = true;
    bool metadataLoaded = false;
    bool updateAvailable = false;
    std::string revision;
};

struct ModMetadata {
    std::string name;
    std::string description;
    std::string version;
    Strings ids;
    Strings tags;
    std::string iconEntry;
    std::string identityKey;
};

struct ModUpdate {
    std::string fileName;
    std::optional<DownloadPlan> available;
    bool identified = false;
    std::string originalSha256;
    InstanceIdentity instance;
    std::string instanceRevision;
    std::map<Source, Error> providerErrors;
};

// Actual published prefix; pending durability is explicit, never a retry hint.
struct ModUpdateReceipt {
    std::string taskId;
    std::string fileName;
    Path destination;
    std::string stepId;
    std::string sha256;
    bool final = false;
    bool durabilityPending = false;
};

struct ModUpdateResult {
    std::vector<ModUpdateReceipt> receipts;
    std::optional<Error> error;
    bool cancelled = false;
    bool finalCommitted = false;
    bool durabilityPending = false;
};


struct ResourceFileHashes {
    std::uint64_t sizeBytes = 0;
    std::string sha1;
    std::string sha512;
    std::string sha256;
    std::uint32_t curseForgeFingerprint = 0;
};

struct ArchiveEntry {
    std::string name;
    std::uint64_t expandedBytes = 0;
    bool regular = true;
    bool directory = false;
};

// These callbacks adapt the shared ArchiveService and task-owned replacement.
struct ModServices {
    ResourceServices resources;
    std::function<std::vector<ArchiveEntry>(const Path&, const Cancellation&)> archiveEntries;
    std::function<Bytes(const Path&, const std::string&, std::size_t, const Cancellation&)> readArchiveEntry;
    std::function<ResourceFileHashes(const Path&, const Cancellation&, std::uint64_t)> hashes;
    std::function<InstallReceipt(const InstanceContext&, const std::string&, const std::string&,
                                 const DownloadPlan&, std::shared_ptr<void>, const Cancellation&,
                                 const ProgressCallback&)> replace;
    std::function<DownloadPlan(const DownloadPlan&, const ProviderSettings&, tasks::Task&)> prepareInTask;
    // Only canonical Task publication creates receipts, including pending outcomes.
    std::function<ModUpdateReceipt(const InstanceContext&, const std::string&, const std::string&,
                                   const DownloadPlan&, std::shared_ptr<void>, const std::string&,
                                   tasks::Task&, bool, const ProgressCallback&)> replaceInTask;
};

struct ModLimits {
    std::size_t maximumFiles = 10000;
    std::size_t maximumSelection = 1024;
    std::size_t maximumManifestBytes = 256 * 1024;
    std::size_t maximumIconBytes = 512 * 1024;
    std::uint64_t maximumArchiveBytes = 512ULL * 1024 * 1024;
    unsigned maximumConcurrent = 4;
};

class ModsService final {
public:
    explicit ModsService(ModServices services, CommunityService* community = nullptr, ModLimits limits = {});
    std::vector<ModEntry> list(const InstanceIdentity& instance, const Cancellation& cancel = {});
    std::vector<ModEntry> scan(const InstanceIdentity& instance, const Cancellation& cancel = {});
    std::optional<ModMetadata> metadata(const InstanceIdentity& instance, const std::string& fileName, const Cancellation& cancel = {});
    Bytes icon(const InstanceIdentity& instance, const std::string& fileName, const std::string& iconEntry, const Cancellation& cancel = {});
    Strings setEnabled(const InstanceIdentity& instance, const Strings& fileNames, bool enabled, const Cancellation& cancel = {});
    std::uint64_t remove(const InstanceIdentity& instance, const Strings& fileNames, const Cancellation& cancel = {});
    Strings installLocal(const InstanceIdentity& instance, const std::vector<Path>& nativeDialogPaths, const Cancellation& cancel = {});
    Strings copyTo(const InstanceIdentity& instance, const Strings& fileNames, const Path& nativeDialogDirectory, const Cancellation& cancel = {});
    std::vector<ModUpdate> checkUpdates(const InstanceIdentity& instance, const Strings& fileNames, const Cancellation& cancel = {});
    std::vector<InstallReceipt> update(const InstanceIdentity& instance, const Strings& fileNames,
                                      const Cancellation& cancel = {}, const ProgressCallback& progress = {});
    std::vector<InstallReceipt> updateConfirmed(const InstanceIdentity& instance, const std::vector<ModUpdate>& updates,
                                               const Cancellation& cancel = {}, const ProgressCallback& progress = {});
    // The caller reserves resourceRoot/modsRoot and supplies the one registered Task.
    // This path owns terminal handling; do not follow it with complete_work().
    ModUpdateResult updateConfirmed(const InstanceIdentity& instance, const std::vector<ModUpdate>& updates,
                                    tasks::Task& operation, const ProgressCallback& progress = {});
private:
    ModServices services_;
    CommunityService* community_;
    ModLimits limits_;
    InstanceContext context(const InstanceIdentity& instance) const;
    std::shared_ptr<void> acquireConfirmedMutation(const InstanceContext& selected) const;
    void validateConfirmedUpdates(const InstanceContext& selected, const std::vector<ModUpdate>& updates) const;
    DownloadPlan prepareConfirmedUpdate(const InstanceContext& selected, const ModUpdate& update,
                                        const Cancellation& cancel,
                                        const std::function<DownloadPlan(const DownloadPlan&)>& prepare = {}) const;
};

bool validResourceFileName(std::string_view name);
std::optional<std::pair<std::string, ModState>> splitModFileName(std::string_view name);
bool validArchiveRelativePath(std::string_view name);
ModMetadata readModMetadata(const std::map<std::string, Bytes>& manifests, const std::string& fileName, const std::string& loader);
std::uint32_t curseForgeFingerprint(std::span<const std::uint8_t> bytes);
ResourceFileHashes hashResourceFile(const Path& path, const Cancellation& cancel = {}, std::uint64_t maximumBytes = 512ULL * 1024 * 1024);
std::uint32_t curseForgeFingerprintFile(const Path& path, const Cancellation& cancel = {}, std::uint64_t maximumBytes = 512ULL * 1024 * 1024);

} // namespace nickel::resources
