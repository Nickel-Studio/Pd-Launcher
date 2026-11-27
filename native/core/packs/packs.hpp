#pragma once

#include "core/instances/startup.hpp"
#include <functional>
#include <map>

namespace nickel::packs {
using Json = nickel::Json;
namespace fs = std::filesystem;
inline constexpr std::string_view mrpack_index = "modrinth.index.json";
inline constexpr std::string_view portable_manifest_file = "nickel.portable.json";
inline constexpr std::string_view portable_format = "nickel-portable";
inline constexpr std::uint64_t max_pack_file_bytes = 4ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t max_pack_bytes = 32ULL * 1024 * 1024 * 1024;
struct ArchiveLimits {
    std::size_t entries = 100000;
    std::uint64_t file_bytes = max_pack_file_bytes;
    std::uint64_t total_bytes = max_pack_bytes;
};
struct ArchiveEntry {
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t compressed_size = 0;
    bool directory = false;
    bool link = false;
};
struct ArchiveInput {
    std::string name;
    std::optional<fs::path> file;
    std::string bytes;
};
class ArchiveBackend {
public:
    virtual ~ArchiveBackend() = default;
    virtual std::vector<ArchiveEntry> inspect(const fs::path& archive, const ArchiveLimits& limits,
                                            std::stop_token cancel) = 0;
    virtual std::string read(const fs::path& archive, std::string_view name, std::uint64_t max_bytes,
                             std::stop_token cancel) = 0;
    virtual void extract_file(const fs::path& archive, std::string_view name, const fs::path& destination,
                              std::uint64_t size, std::stop_token cancel) = 0;
    virtual void write(const fs::path& target, std::span<const ArchiveInput> inputs,
                       const ArchiveLimits& limits, std::stop_token cancel) = 0;
    virtual void release(const fs::path&) noexcept {}
};
void validate_archive_entries(std::span<const ArchiveEntry> entries, const ArchiveLimits& limits);
std::string validate_archive_path(std::string_view path, bool directory = false);
void validate_pack_path(std::string_view path);
void validate_download_url(std::string_view url);
std::string hash_file(const fs::path& file, std::string_view algorithm, std::stop_token cancel = {});

struct PackFile {
    std::string path;
    std::vector<std::string> downloads;
    std::uint64_t size = 0;
    std::optional<std::string> sha1;
    std::optional<std::string> sha512;
};
struct PackLoader { std::string kind; std::string version; };
struct PackManifest {
    std::string name;
    std::string version;
    std::string minecraft_version;
    std::optional<PackLoader> loader;
    std::vector<PackFile> files;
    Json unknown = Json::object();
};
PackManifest parse_mrpack_manifest(std::string_view bytes);
Json mrpack_manifest_to_json(const PackManifest& manifest);
struct PackPlan {
    fs::path archive;
    PackManifest manifest;
    std::string instance_name;
    Json source = Json::object();
    std::optional<Json> instance_settings;
    std::vector<std::pair<std::string, ArchiveEntry>> overrides;
    std::string archive_sha512;
};
struct DownloadRequest {
    PackFile file;
    fs::path target;
};
struct InstallerRequest {
    fs::path game_root;
    fs::path instance_directory;
    std::string instance_id;
    std::string minecraft_version;
    std::optional<PackLoader> loader;
};
// Synchronous final boundary: invoke the supplied commit exactly once, without retaining it.
// Downloads, installation, owned drain and transaction completion stay outside this callback.
using FinalPublish = std::function<void(const std::function<void()>&)>;
struct InstallServices {
    std::shared_ptr<ArchiveBackend> archive;
    // Required production adapters. Neither callback may execute commands from pack contents.
    std::function<void(const DownloadRequest&, std::stop_token)> download;
    std::function<instances::InstallIdentity(const InstallerRequest&, std::stop_token)> install;
    std::function<bool(std::chrono::milliseconds)> drain_owned_installer;
    // Empty means direct domain commit; task adapters use their existing commit_external gate.
    FinalPublish final_publish;
};
PackPlan inspect_pack(const fs::path& archive, std::string_view instance_name,
                      ArchiveBackend& backend, std::stop_token cancel = {});
struct ImportResult { std::string instance_id; fs::path directory; };
ImportResult import_pack(PackPlan plan, const fs::path& game_root, const InstallServices& services,
                         const instances::OnboardingGate& gate, std::stop_token cancel = {});

struct ExportFile {
    std::string path;
    fs::path source_path;
    std::string group;
    bool selected = false;
    bool directory = false;
    std::optional<std::uint64_t> size;
    std::string display_name;
};
struct ExportPreview {
    std::string name;
    std::string minecraft_version;
    std::optional<PackLoader> loader;
    std::vector<ExportFile> files;
    instances::SettingsProjection instance_settings;
    std::optional<std::string> instance_settings_error;
};
enum class ExportFormat { mrpack, portable };
struct ExportRequest {
    fs::path game_root;
    std::string instance_id;
    std::string name;
    std::string version;
    std::vector<std::string> selections;
    bool bundle_resources = true;
    bool modrinth_only = false;
    ExportFormat format = ExportFormat::mrpack;
    bool include_launcher = false;
    bool include_personalization = false;
};
ExportRequest export_request_from_json(const Json& value);
struct ExportServices {
    std::shared_ptr<ArchiveBackend> archive;
    std::function<std::optional<PackFile>(const fs::path&, std::string_view, std::stop_token)> resolve_resource;
    std::optional<fs::path> launcher;
    std::string launcher_os;
    std::string launcher_arch;
    std::function<void(const fs::path&, std::stop_token)> validate_launcher_artifact;
    // Release owner supplies trusted embedded license/source files, never user files.
    std::vector<ArchiveInput> launcher_notices;
    Json personalization = Json::object();
    std::optional<fs::path> background;
    // Community approves provider origins; absent approval permits only the Modrinth CDN.
    std::function<void(std::string_view)> validate_resource_download_url;
};
ExportPreview preview_export(const fs::path& root, std::string_view id, const Json& global_settings,
                              std::stop_token cancel = {});
std::optional<std::pair<std::string, bool>> content_group(std::string_view path);
bool sensitive_content_path(std::string_view path);
bool safe_content_text(std::string_view text);
void export_pack(const ExportRequest& request, const fs::path& destination,
                  const Json& global_settings, const ExportServices& services,
                  const instances::OnboardingGate& gate, std::stop_token cancel = {});

// Foundation owns PortableManifest. This layer validates its canonical JSON envelope, not another DTO.
Json validate_portable_manifest(const Json& manifest);
std::optional<Json> read_portable_manifest(const fs::path& root);
Json validate_personalization(const Json& value, const Json& manifest);
struct PortableBootstrap {
    fs::path root;
    Json manifest;
    fs::path payload;
    fs::path game_root;
    fs::path data_root;
    std::optional<Json> personalization;
    std::optional<Json> instance_settings;
};
std::optional<PortableBootstrap> load_portable_bootstrap(const fs::path& executable_directory,
                                                        ArchiveBackend& backend, std::stop_token cancel = {});
struct BootstrapStatus {
    std::string package_id;
    std::string name;
    fs::path game_directory;
    std::string state;
    std::optional<std::string> instance_id;
};
std::optional<BootstrapStatus> portable_bootstrap_status(const fs::path& root);
std::optional<ImportResult> start_portable_bootstrap(const fs::path& root,
                                                    const InstallServices& services,
                                                    const instances::OnboardingGate& gate,
                                                    std::stop_token cancel = {});
void seed_personalization(const PortableBootstrap& bootstrap);
Json personalization_for_display(const PortableBootstrap& bootstrap, const Json& value);
Json personalization_for_storage(const PortableBootstrap& bootstrap, const Json& value);
} // namespace nickel::packs
