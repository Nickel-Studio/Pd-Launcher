#include "internal.hpp"
#include <algorithm>
#include <exception>
#include <set>
#include <type_traits>
#include <unordered_map>

namespace nickel::packs {
namespace {
using namespace detail;
std::string exact_version(const Json& value, std::string_view key) {
    const auto text = required_text(value, key, 128);
    if (text == "." || text == ".." || std::any_of(text.begin(), text.end(), [](unsigned char c) {
            return std::isspace(c) || std::string_view("/\\:*?<>|\"").find(static_cast<char>(c)) != std::string_view::npos;
        })) throw Error("invalid_schema", "Pack dependency requires an exact version");
    return text;
}
struct ReleaseArchive {
    ArchiveBackend& backend;
    fs::path path;
    ~ReleaseArchive() { backend.release(path); }
};
std::string target_key(std::string_view value) {
    auto key = io::ascii_lower(value);
#ifdef _WIN32
    key = instances::root_identity(key);
#endif
    return key;
}
void verify_target_paths(const PackPlan& plan) {
    std::vector<ArchiveEntry> entries;
    for (const auto& file : plan.manifest.files) entries.push_back({file.path, file.size, file.size, false, false});
    std::set<std::string> paths;
    for (const auto& file : plan.manifest.files) paths.insert(target_key(file.path));
    for (const auto& [path, entry] : plan.overrides) {
        if (!paths.insert(target_key(path)).second) throw Error("conflict", "Pack download and override collide");
        entries.push_back({path, entry.size, entry.compressed_size, entry.directory, entry.link});
    }
    validate_archive_entries(entries, {});
}
void verify_source_metadata(const Json& source) {
    constexpr std::string_view allowed[]{"platform", "origin", "name", "version", "portablePackageId", "projectId", "fileId"};
    io::strict_keys(source, allowed);
    for (const auto& item : source.items()) {
        if (!item.value().is_string()) throw Error("invalid_schema", "Invalid pack source metadata");
        const auto text = required_text(source, item.key());
        if (embedded_credentials(text) || instances::contains_host_path(text)) throw Error("unsafe_content", "Private pack source metadata");
    }
    if (source.contains("portablePackageId")) {
        const auto id = source["portablePackageId"].get<std::string>();
        if (id.size() != 32 || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c); })) throw Error("invalid_schema", "Invalid portable source identity");
    }
}
}
PackManifest parse_mrpack_manifest(std::string_view text) {
    const auto value = io::parse_strict_json(text, instances::descriptor_max_bytes);
    constexpr std::string_view root_keys[]{"formatVersion", "game", "versionId", "name", "summary", "files", "dependencies"};
    constexpr std::string_view required[]{"formatVersion", "game", "versionId", "name", "files", "dependencies"};
    io::strict_keys(value, root_keys, required);
    if (required_unsigned(value, "formatVersion", 1) != 1 || required_text(value, "game") != "minecraft") throw Error("invalid_schema", "Unsupported mrpack format");
    PackManifest result;
    result.name = required_text(value, "name");
    result.version = required_text(value, "versionId");
    if (value.contains("summary")) result.unknown["summary"] = required_text(value, "summary", 4096);
    const auto& dependencies = value["dependencies"];
    constexpr std::string_view dependency_keys[]{"minecraft", "forge", "neoforge", "fabric-loader", "quilt-loader"};
    constexpr std::string_view minecraft_key[]{"minecraft"};
    io::strict_keys(dependencies, dependency_keys, minecraft_key);
    result.minecraft_version = exact_version(dependencies, "minecraft");
    for (const auto& item : dependencies.items()) {
        if (item.key() == "minecraft") continue;
        if (result.loader) throw Error("invalid_schema", "Pack requests multiple loaders");
        const auto kind = item.key() == "fabric-loader" ? "fabric" : item.key() == "quilt-loader" ? "quilt" : item.key();
        result.loader = PackLoader{kind, exact_version(dependencies, item.key())};
    }
    const auto& files = value["files"];
    if (!files.is_array() || files.size() > ArchiveLimits{}.entries) throw Error("limit", "Invalid mrpack file count");
    std::set<std::string> paths;
    std::uint64_t total = 0;
    for (const auto& entry : files) {
        constexpr std::string_view file_keys[]{"path", "hashes", "env", "downloads", "fileSize"};
        constexpr std::string_view file_required[]{"path", "hashes", "downloads", "fileSize"};
        io::strict_keys(entry, file_keys, file_required);
        PackFile file;
        file.path = required_text(entry, "path", 1024);
        validate_pack_path(file.path);
        if (!paths.insert(target_key(file.path)).second) throw Error("conflict", "Duplicate mrpack target");
        bool client = true;
        if (entry.contains("env")) {
            constexpr std::string_view env_keys[]{"client", "server"};
            io::strict_keys(entry["env"], env_keys);
            for (const auto& item : entry["env"].items()) {
                if (!item.value().is_string()) throw Error("invalid_schema", "Invalid pack environment");
                const auto side = item.value().get<std::string>();
                if (side != "required" && side != "optional" && side != "unsupported") throw Error("invalid_schema", "Invalid pack environment");
                if (item.key() == "client" && side == "unsupported") client = false;
            }
        }
        file.size = required_unsigned(entry, "fileSize", max_pack_file_bytes);
        if (file.size > max_pack_bytes - total) throw Error("limit", "mrpack total byte quota exceeded");
        total += file.size;
        constexpr std::string_view hash_keys[]{"sha1", "sha512"};
        io::strict_keys(entry["hashes"], hash_keys);
        file.sha1 = optional_hash(entry["hashes"], "sha1", 40);
        file.sha512 = optional_hash(entry["hashes"], "sha512", 128);
        if (!file.sha1 && !file.sha512) throw Error("invalid_schema", "Missing mrpack digest");
        if (!entry["downloads"].is_array() || entry["downloads"].empty() || entry["downloads"].size() > 32) throw Error("invalid_schema", "Missing or excessive pack URLs");
        for (const auto& url : entry["downloads"]) {
            if (!url.is_string()) throw Error("invalid_schema", "Pack download URL is not text");
            auto text_url = url.get<std::string>();
            validate_download_url(text_url);
            file.downloads.push_back(std::move(text_url));
        }
        if (client) result.files.push_back(std::move(file));
    }
    std::vector<ArchiveEntry> target_entries;
    for (const auto& file : result.files) target_entries.push_back({file.path, file.size, file.size, false, false});
    validate_archive_entries(target_entries, {});
    return result;
}
Json mrpack_manifest_to_json(const PackManifest& manifest) {
    Json value{{"formatVersion", 1}, {"game", "minecraft"}, {"versionId", manifest.version}, {"name", manifest.name},
               {"files", Json::array()}, {"dependencies", {{"minecraft", manifest.minecraft_version}}}};
    if (manifest.loader) {
        const auto& kind = manifest.loader->kind;
        const auto key = kind == "fabric" ? "fabric-loader" : kind == "quilt" ? "quilt-loader" : kind;
        value["dependencies"][key] = manifest.loader->version;
    }
    if (manifest.unknown.contains("summary")) value["summary"] = manifest.unknown["summary"];
    for (const auto& file : manifest.files) value["files"].push_back(file_to_json(file));
    (void)parse_mrpack_manifest(value.dump());
    return value;
}
PackPlan inspect_pack(const fs::path& file, std::string_view name, ArchiveBackend& backend, std::stop_token cancel) {
    io::check_cancelled(cancel);
    instances::validate_name(name);
    NativeFile input(file, NativeFile::Mode::read);
    if (input.size() > max_pack_file_bytes) throw Error("limit", "Local pack archive exceeds 4 GiB");
    ReleaseArchive release{backend, file};
    const auto entries = backend.inspect(file, {}, cancel);
    validate_archive_entries(entries, {});
    PackPlan plan;
    plan.archive = file;
    plan.instance_name = name;
    plan.manifest = parse_mrpack_manifest(backend.read(file, mrpack_index, instances::descriptor_max_bytes, cancel));
    plan.source = {{"platform", "modrinth"}, {"origin", "local"}, {"name", plan.manifest.name}, {"version", plan.manifest.version}};
    std::map<std::string, std::pair<std::string, ArchiveEntry>> selected;
    for (const auto* prefix : {"overrides/", "client-overrides/"}) {
        const std::string_view root(prefix);
        for (const auto& entry : entries) {
            io::check_cancelled(cancel);
            if (entry.name == root.substr(0, root.size() - 1) && entry.directory) continue;
            if (!entry.name.starts_with(root)) continue;
            auto path = validate_archive_path(std::string_view(entry.name).substr(root.size()), entry.directory);
            validate_pack_path(path);
            if (entry.directory) continue;
            const auto key = target_key(path);
            const auto existing = selected.find(key);
            if (existing != selected.end() && existing->second.first != path) throw Error("conflict", "Pack override target case collision");
            selected[key] = {path, entry};
        }
    }
    for (auto& [key, value] : selected) { (void)key; plan.overrides.push_back(std::move(value)); }
    verify_target_paths(plan);
    plan.archive_sha512 = hash_file(file, "sha512", cancel);
    plan.instance_settings = settings_for_payload(file);
    input.unchanged();
    return plan;
}
ImportResult import_pack(PackPlan plan, const fs::path& root, const InstallServices& services,
                         const instances::OnboardingGate& gate, std::stop_token cancel) {
    static_assert(std::is_nothrow_move_constructible_v<ImportResult>);
    gate.require_finished();
    io::check_cancelled(cancel);
    if (!services.archive || !services.install || !services.drain_owned_installer) throw Error("missing_service", "Pack import requires archive, installer and owned drain services");
    instances::validate_name(plan.instance_name);
    verify_source_metadata(plan.source);
    if (plan.instance_settings) plan.instance_settings = instances::validate_portable_settings(*plan.instance_settings);
    if (!root.is_absolute()) throw Error("unsafe_path", "Pack game root must be absolute");
    services.archive->release(plan.archive);
    Workspace workspace(root);
    const auto staged = workspace.directory() / "input.mrpack";
    copy_file(plan.archive, staged, max_pack_file_bytes, cancel);
    if (hash_file(staged, "sha512", cancel) != plan.archive_sha512) throw Error("source_changed", "Pack changed after preview");
    auto checked = inspect_pack(staged, plan.instance_name, *services.archive, cancel);
    checked.source = plan.source;
    checked.instance_settings = plan.instance_settings;
    if (!checked.manifest.files.empty() && !services.download) throw Error("missing_service", "Pack requires a bounded download service");
    const auto loader = checked.manifest.loader ? std::optional<std::string>(checked.manifest.loader->kind) : std::nullopt;
    const auto build = checked.manifest.loader ? checked.manifest.loader->version : std::string{};
    auto transaction = instances::InstanceTransaction::reserve(root, checked.instance_name, checked.manifest.minecraft_version, loader, build, false);
    bool installer_started = false;
    bool installer_drained = false;
    try {
        transaction.check_owned();
        InstallerRequest request{transaction.root(), transaction.directory(), transaction.profile().name,
                                 checked.manifest.minecraft_version, checked.manifest.loader};
        std::optional<instances::InstallIdentity> installed;
        std::exception_ptr install_failure;
        installer_started = true;
        try { installed = services.install(request, cancel); }
        catch (...) { install_failure = std::current_exception(); }
        try { installer_drained = services.drain_owned_installer(std::chrono::seconds(5)); }
        catch (...) { installer_drained = false; }
        if (!installer_drained) {
            transaction.leave_for_recovery(false);
            throw Error("installer_drain", "Owned installer did not report complete ordered EOF; instance remains pending");
        }
        if (install_failure) std::rethrow_exception(install_failure);
        io::check_cancelled(cancel);
        transaction.check_owned();
        for (const auto& file : checked.manifest.files) {
            io::check_cancelled(cancel);
            transaction.check_owned();
            const auto target = transaction.directory() / path_from_utf8(file.path);
            reject_links(target);
            if (io::path_exists(target)) throw Error("conflict", "Installer file collides with pack download");
            io::create_directories(target.parent_path());
            services.download({file, target}, cancel);
            io::check_cancelled(cancel);
            transaction.check_owned();
            verify_pack_file(file, target, cancel);
            validate_content_file(target, file.path);
        }
        ReleaseArchive release{*services.archive, staged};
        for (const auto& [path, entry] : checked.overrides) {
            io::check_cancelled(cancel);
            transaction.check_owned();
            const auto target = transaction.directory() / path_from_utf8(path);
            reject_links(target);
            if (io::path_exists(target)) throw Error("conflict", "Installer file collides with pack override");
            services.archive->extract_file(staged, entry.name, target, entry.size, cancel);
            validate_content_file(target, path);
        }
        if (checked.instance_settings) instances::apply_portable_settings(transaction.directory(), *checked.instance_settings);
        transaction.check_owned();
        io::check_cancelled(cancel);
        gate.require_finished();
        transaction.complete(*installed);
        ImportResult result{transaction.profile().name, transaction.directory()};
        bool invoked = false;
        bool published = false;
        std::exception_ptr publication_failure;
        const std::function<void()> commit = [&] {
            if (invoked) throw Error("publication", "Final pack commit may only be invoked once");
            invoked = true;
            try {
                transaction.commit(checked.manifest.name, checked.source, cancel);
                published = true;
            } catch (...) {
                publication_failure = std::current_exception();
                throw;
            }
        };
        try {
            if (services.final_publish) services.final_publish(commit);
            else commit();
        } catch (...) {
            // The marker is already retired: a late wrapper cancellation cannot undo publication.
            if (!published) throw;
        }
        if (publication_failure) std::rethrow_exception(publication_failure);
        if (!published) {
            io::check_cancelled(cancel);
            throw Error("publication", "Final publication callback did not commit the prepared instance");
        }
        return result;
    } catch (...) {
        services.archive->release(staged);
        if (installer_started && !installer_drained) { try { transaction.leave_for_recovery(false); } catch (...) {} }
        else { try { transaction.cleanup(); } catch (...) {} }
        throw;
    }
}
} // namespace nickel::packs
