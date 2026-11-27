#include "native_services.hpp"
#include "internal.hpp"

#include <algorithm>

namespace nickel::packs::detail {
namespace {
minecraft::LoaderKind loader_kind(std::string_view kind) {
    if (kind == "fabric") return minecraft::LoaderKind::Fabric;
    if (kind == "quilt") return minecraft::LoaderKind::Quilt;
    if (kind == "forge") return minecraft::LoaderKind::Forge;
    if (kind == "neoforge") return minecraft::LoaderKind::NeoForge;
    throw Error("invalid_loader", "Pack loader is not supported by the native installer bridge");
}
fs::path reserved_game_directory(const InstallerRequest& request) {
    instances::validate_name(request.instance_id);
    if (!instances::valid_minecraft_version(request.minecraft_version))
        throw Error("invalid_version", "Pack Minecraft version is invalid");
    if (!request.game_root.is_absolute() || !request.instance_directory.is_absolute())
        throw Error("unsafe_path", "Pack installer paths must be absolute");
    const auto expected = request.game_root / "versions" / path_from_utf8(request.instance_id);
    if (request.instance_directory.lexically_normal() != expected.lexically_normal())
        throw Error("identity_mismatch", "Installer target differs from the final reserved instance");
    reject_links(request.instance_directory);
    if (!fs::is_directory(io::native_path(request.instance_directory)) || !instances::is_pending(request.instance_directory))
        throw Error("pending_instance", "Native pack installation requires a pending reservation");
    // Public load_profile intentionally rejects pending instances; this private bridge owns the reservation.
    const auto document = io::read_optional_json(request.instance_directory / instances::profile_file, 64 * 1024);
    if (!document) throw Error("identity_mismatch", "Reserved instance profile is missing");
    const auto profile = instances::profile_from_json(*document, request.instance_id);
    if (profile.ready || profile.minecraft_version != request.minecraft_version
        || profile.loader != (request.loader ? std::optional<std::string>(request.loader->kind) : std::nullopt)
        || profile.loader_version != (request.loader ? request.loader->version : std::string{}))
        throw Error("identity_mismatch", "Pack install request differs from the reserved profile");
    return request.instance_directory / "instances" / path_from_utf8(request.instance_id);
}
std::string normalized_hash(std::string_view value, std::size_t length) {
    const auto result = optional_hash(Json{{"digest", value}}, "digest", length);
    if (!result) throw Error("integrity", "Native pack service requires a verified digest");
    return *result;
}
} // namespace

transport::DownloadRequest native_download_request(const DownloadRequest& request) {
    validate_pack_path(request.file.path);
    if (!request.target.is_absolute()) throw Error("unsafe_path", "Pack download target must be absolute");
    reject_links(request.target);
    if (io::path_exists(request.target)) throw Error("conflict", "Pack download refuses an existing file");
    if (request.file.size > max_pack_file_bytes || request.file.downloads.empty() || request.file.downloads.size() > 16)
        throw Error("limit", "Pack download metadata exceeds the bounded native request");
    transport::DownloadRequest result;
    result.destination = request.target;
    result.expected_size = request.file.size;
    result.max_bytes = std::max<std::uint64_t>(1, request.file.size);
    result.service = "nickel.pack";
    result.trust = transport::TrustPolicy::PublicHttps;
    result.require_digest = true;
    result.reuse_verified_existing = false;
    result.resume = false;
    for (const auto& url : request.file.downloads) {
        validate_download_url(url);
        result.candidates.push_back({url, false, {}});
    }
    if (request.file.sha1) result.digests.push_back({transport::HashAlgorithm::Sha1, normalized_hash(*request.file.sha1, 40)});
    if (request.file.sha512) result.digests.push_back({transport::HashAlgorithm::Sha512, normalized_hash(*request.file.sha512, 128)});
    if (result.digests.empty()) throw Error("integrity", "Pack download requires SHA1 or SHA512");
    return result;
}

minecraft::InstallRequest native_installer_request(const InstallerRequest& request,
                                                   const minecraft::Platform& platform) {
    const auto game = reserved_game_directory(request);
    minecraft::InstallRequest result{.root = game, .gameDirectory = game,
        .versionId = request.minecraft_version, .instanceName = request.minecraft_version,
        .reservationId = request.instance_id, .loaders = {}, .dependencyMods = {},
        .javaCandidates = {}, .manualJava = std::nullopt, .profile = nullptr,
        .assetIndex = nullptr, .platform = platform};
    if (request.loader) {
        if (request.loader->version.empty() || request.loader->version.size() > 128
            || instances::contains_host_path(request.loader->version) || instances::contains_credentials(request.loader->version))
            throw Error("invalid_loader", "Pack loader version is invalid");
        result.loaders.push_back({loader_kind(request.loader->kind), request.loader->version, std::nullopt});
    }
    return result;
}

instances::InstallIdentity native_install_identity(const InstallerRequest& request,
                                                   const minecraft::InstalledVersion& installed,
                                                   std::stop_token cancel) {
    io::check_cancelled(cancel);
    const auto game = reserved_game_directory(request);
    const auto descriptor_path = game / "versions" / path_from_utf8(request.minecraft_version)
        / path_from_utf8(request.minecraft_version + ".json");
    if (installed.root.lexically_normal() != game.lexically_normal()
        || installed.gameDirectory.lexically_normal() != game.lexically_normal()
        || installed.profilePath.lexically_normal() != descriptor_path.lexically_normal()
        || installed.versionId != request.minecraft_version || installed.minecraftVersion != request.minecraft_version)
        throw Error("identity_mismatch", "Native installer returned a different game identity or path");
    reject_links(descriptor_path);
    const auto descriptor = io::parse_strict_json(io::read_bounded(descriptor_path, instances::descriptor_max_bytes), instances::descriptor_max_bytes);
    if (!descriptor.is_object() || io::string_at(descriptor, "id") != request.minecraft_version
        || !installed.profile.is_object() || installed.profile != descriptor)
        throw Error("identity_mismatch", "Native installed descriptor differs from the persisted game profile");
    std::vector<minecraft::LoaderSelection> actual;
    for (const auto& loader : installed.loaders) if (loader.kind != minecraft::LoaderKind::Vanilla) actual.push_back(loader);
    if (request.loader) {
        if (actual.size() != 1 || actual.front().kind != loader_kind(request.loader->kind) || actual.front().version != request.loader->version)
            throw Error("identity_mismatch", "Native installed loader differs from the reserved pack loader");
    } else if (!actual.empty()) throw Error("identity_mismatch", "Native vanilla pack unexpectedly includes a loader");
    io::check_cancelled(cancel);
    return {request.minecraft_version, request.loader ? std::optional<std::string>(request.loader->kind) : std::nullopt,
        request.loader ? request.loader->version : std::string{}, descriptor, nullptr};
}

PackFile native_resource_file(const fs::path& snapshot, std::string_view relative,
                              std::string_view sha1, std::string_view sha512,
                              const resources::ResolvedResourceFile& resolved,
                              bool modrinth_only, std::stop_token cancel) {
    io::check_cancelled(cancel);
    validate_pack_path(relative);
    if (modrinth_only && resolved.source != resources::Source::Modrinth)
        throw Error("integrity", "Modrinth-only export resolved a different provider");
    const auto first = normalized_hash(sha1, 40), second = normalized_hash(sha512, 128);
    if (normalized_hash(resolved.file.sha1, 40) != first || normalized_hash(resolved.file.sha512, 128) != second)
        throw Error("integrity", "Resolved resource digest differs from the selected snapshot");
    validate_download_url(resolved.file.url);
    NativeFile pin(snapshot, NativeFile::Mode::read);
    if (pin.size() > max_pack_file_bytes || pin.size() != resolved.file.sizeBytes)
        throw Error("integrity", "Resolved resource size differs from the selected snapshot");
    PackFile result{std::string(relative), {resolved.file.url}, pin.size(), first, second};
    verify_pack_file(result, snapshot, cancel);
    pin.unchanged();
    io::check_cancelled(cancel);
    return result;
}
} // namespace nickel::packs::detail
