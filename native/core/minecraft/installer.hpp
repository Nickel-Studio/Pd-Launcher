#pragma once

#include "services.hpp"

namespace nickel::minecraft {

struct LoaderArtifact {
    std::string url;
    std::string filename;
    FileHashes hashes;
    std::optional<std::uint64_t> size;
    Json profile;
    Json rawMetadata;
    std::optional<std::string> forgeRequirement;
};

struct LoaderRequest {
    LoaderKind kind = LoaderKind::Vanilla;
    std::string version = "latest";
    std::optional<LoaderArtifact> artifact;
};

struct DependencyMod {
    std::string name;
    DownloadFile file;
    std::vector<std::string> minecraftVersions;
    std::vector<LoaderKind> loaders;
};

struct InstallRequest {
    std::filesystem::path root;
    std::filesystem::path gameDirectory;
    std::string versionId;
    std::string instanceName;
    std::string reservationId;
    std::vector<LoaderRequest> loaders;
    std::vector<DependencyMod> dependencyMods;
    std::vector<JavaCandidate> javaCandidates;
    std::optional<std::filesystem::path> manualJava;
    Json profile;
    Json assetIndex;
    bool installJava = true;
    bool repair = false;
    bool offline = false;
    std::string manifestUrl = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
    std::string javaManifestUrl = "https://launchermeta.mojang.com/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json";
    std::string resourcesBase = "https://resources.download.minecraft.net";
    Platform platform = Platform::current();
};

struct ProcessorOutput {
    std::filesystem::path destination;
    FileHashes hashes;
    std::uint64_t maximumBytes = 512ULL * 1024 * 1024;
};

struct ArchiveFile {
    std::filesystem::path archive;
    std::string entry;
    std::filesystem::path destination;
    FileHashes hashes;
    std::uint64_t maximumBytes = 256ULL * 1024 * 1024;
};

struct ProcessorPlan {
    std::string label;
    ProcessSpec process;
    std::vector<ProcessorOutput> outputs;
    std::optional<DownloadFile> managedDownload;
    std::filesystem::path mainClassJar;
    std::vector<std::filesystem::path> classpath;
    std::vector<CopyFile> copiesBefore;
    std::vector<WriteFile> writesBefore;
    std::filesystem::path generatedMinecraft;
    std::chrono::milliseconds timeout = std::chrono::minutes(5);
    std::uint64_t maximumOutputBytes = 1024ULL * 1024 * 1024;
    std::uint64_t maximumOutputFiles = 4096;
};

struct InstallPlan {
    std::filesystem::path root;
    std::filesystem::path gameDirectory;
    ResolvedVersion version;
    JavaRequirement javaRequirement;
    JavaSelection java;
    std::vector<LoaderSelection> loaders;
    std::vector<DownloadFile> downloads;
    std::vector<WriteFile> writes;
    std::vector<CopyFile> copies;
    std::vector<NativeArchive> natives;
    std::vector<ProcessorPlan> processors;
    std::vector<std::filesystem::path> classpath;
    std::optional<RuntimePlan> runtime;
    std::vector<ArchiveFile> archiveFiles;
    std::vector<std::filesystem::path> directories;
    std::filesystem::path profilePath;
    Platform platform = Platform::current();
    bool offline = false;
};

struct InstalledVersion {
    std::string versionId;
    std::string minecraftVersion;
    std::filesystem::path root;
    std::filesystem::path gameDirectory;
    std::filesystem::path profilePath;
    Json profile;
    JavaSelection java;
    std::vector<LoaderSelection> loaders;
    std::vector<std::filesystem::path> classpath;
    std::vector<DownloadFile> files;
};

class Installer {
public:
    explicit Installer(Services services = native_services());
    InstallPlan plan(const InstallRequest& request, const Cancellation& cancellation,
                     const ProgressCallback& progress = {});
    InstalledVersion install(const InstallRequest& request, const Cancellation& cancellation,
                              const ProgressCallback& progress = {});
    InstalledVersion execute(InstallPlan plan, const Cancellation& cancellation,
                              const ProgressCallback& progress = {});
    InstallPlan plan(const InstallRequest& request, std::stop_token cancellation,
                     const ProgressCallback& progress = {});
    InstalledVersion install(const InstallRequest& request, std::stop_token cancellation,
                              const ProgressCallback& progress = {});
    InstalledVersion execute(InstallPlan plan, std::stop_token cancellation,
                              const ProgressCallback& progress = {});
    bool drain_owned_installer(std::chrono::milliseconds timeout);
    std::vector<std::uint64_t> owned_installer_pids() const;
    const Services& services() const noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
    Services services_;
    InstallPlan plan_checked(const InstallRequest&, const Services&, const CancelCheck&, const ProgressCallback&);
    InstalledVersion execute_checked(InstallPlan, const Services&, const CancelCheck&, const ProgressCallback&);
};

Json installed_version_json(const InstalledVersion& version);

}
