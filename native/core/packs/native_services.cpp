#include "native_services.hpp"
#include "native_service_detail.hpp"
#include "core/resources/export.hpp"

#include <mutex>

namespace nickel::packs {
namespace {
struct InstallState {
    explicit InstallState(NativeInstallOptions input)
        : options(std::move(input)), installer(options.installer_services
              ? *options.installer_services : minecraft::native_services()) {}
    NativeInstallOptions options;
    minecraft::Installer installer;
    std::timed_mutex mutex;
    bool started = false;
};
} // namespace

InstallServices native_install_services(std::shared_ptr<transport::HttpClient> http, NativeInstallOptions options) {
    if (!http) throw instances::Error("missing_service", "Native pack installation requires the shared HTTP client");
    auto state = detail::native_service_call([&] { return std::make_shared<InstallState>(std::move(options)); },
                                            "missing_service", "Native installer services are unavailable");
    InstallServices result;
    result.archive = native_archive_backend();
    detail::transfer_final_publish(result, state->options);
    result.download = [http, state](const DownloadRequest& input, std::stop_token stop) {
        detail::io::check_cancelled(stop);
        auto request = detail::native_download_request(input);
        detail::NativeCancellation cancellation(stop);
        const auto receipt = detail::native_service_call([&] {
            return http->download(request, cancellation.get(), state->options.download_progress);
        }, "download", "Native pack download failed");
        if (receipt.destination.lexically_normal() != input.target.lexically_normal() || receipt.size != input.file.size)
            throw instances::Error("integrity", "Native download receipt differs from the reserved pack file");
        detail::io::check_cancelled(stop);
        detail::verify_pack_file(input.file, input.target, stop);
    };
    result.install = [state](const InstallerRequest& input, std::stop_token stop) {
        std::lock_guard lock(state->mutex);
        if (state->started) throw instances::Error("conflict", "Native install service ownership is single-use");
        state->started = true;
        detail::io::check_cancelled(stop);
        auto request = detail::native_installer_request(input, state->options.platform);
        request.javaCandidates = state->options.java_candidates;
        request.manualJava = state->options.manual_java;
        request.installJava = state->options.install_java;
        detail::NativeCancellation cancellation(stop);
        const auto installed = detail::native_service_call([&] {
            return state->installer.install(request, cancellation.get(), state->options.installer_progress);
        }, "installer", "Native pack installation failed");
        return detail::native_install_identity(input, installed, stop);
    };
    result.drain_owned_installer = [state](std::chrono::milliseconds timeout) {
        return detail::native_owned_drain(state->mutex, timeout, [&](std::chrono::milliseconds remaining) {
            return detail::native_service_call([&] { return state->installer.drain_owned_installer(remaining); },
                                              "installer_drain", "Native owned installer drain failed");
        });
    };
    return result;
}

ExportServices native_export_services(std::shared_ptr<resources::CommunityService> community,
                                     bool modrinth_only, ExportServices trusted_artifacts) {
    if (!community) throw instances::Error("missing_service", "Native pack export requires the shared community service");
    trusted_artifacts.archive = native_archive_backend();
    auto resolver = detail::native_service_call([&] { return resources::makeExportResolver(community, modrinth_only); },
                                               "resource", "Native resource export policy is unavailable");
    trusted_artifacts.resolve_resource = [resolver = std::move(resolver)](const fs::path& snapshot,
                                                                        std::string_view relative, std::stop_token stop) -> std::optional<PackFile> {
        detail::io::check_cancelled(stop);
        detail::NativeFile pin(snapshot, detail::NativeFile::Mode::read);
        auto result = detail::native_service_call([&] { return resolver(snapshot, relative, stop); },
                                                 "resource", "Native resource resolution failed");
        pin.unchanged();
        detail::io::check_cancelled(stop);
        if (result) detail::verify_pack_file(*result, snapshot, stop);
        pin.unchanged();
        return result;
    };
    trusted_artifacts.validate_resource_download_url = [](std::string_view url) {
        detail::native_service_call([&] { (void)resources::checkedUrl(url, resources::providerFileRules()); },
                                    "unsafe_url", "Resource origin is not approved by its provider");
    };
    return trusted_artifacts;
}
} // namespace nickel::packs
