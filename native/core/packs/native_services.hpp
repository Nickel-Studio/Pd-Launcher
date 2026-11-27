#pragma once

#include "packs.hpp"
#include "core/archive/archive.hpp"
#include "core/network/download.hpp"
#include "core/minecraft/installer.hpp"
#include "core/resources/community.hpp"

namespace nickel::packs {

std::shared_ptr<ArchiveBackend> native_archive_backend();

struct NativeInstallOptions {
    std::optional<minecraft::Services> installer_services;
    minecraft::ProgressCallback installer_progress;
    transport::Progress download_progress;
    std::vector<minecraft::JavaCandidate> java_candidates;
    std::optional<fs::path> manual_java;
    minecraft::Platform platform = minecraft::Platform::current();
    bool install_java = true;
    FinalPublish final_publish;
};

// Create a fresh service set for each import: its child ownership must not be shared between tasks.
InstallServices native_install_services(std::shared_ptr<transport::HttpClient> http,
                                       NativeInstallOptions options = {});
ExportServices native_export_services(std::shared_ptr<resources::CommunityService> community,
                                     bool modrinth_only, ExportServices trusted_artifacts = {});

namespace detail {
transport::DownloadRequest native_download_request(const DownloadRequest& request);
minecraft::InstallRequest native_installer_request(const InstallerRequest& request,
                                                   const minecraft::Platform& platform);
instances::InstallIdentity native_install_identity(const InstallerRequest& request,
                                                   const minecraft::InstalledVersion& installed,
                                                   std::stop_token cancel = {});
PackFile native_resource_file(const fs::path& snapshot, std::string_view relative,
                              std::string_view sha1, std::string_view sha512,
                              const resources::ResolvedResourceFile& resolved,
                              bool modrinth_only, std::stop_token cancel = {});
} // namespace detail
} // namespace nickel::packs
