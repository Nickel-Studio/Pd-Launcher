#include "core/packs/packs.hpp"
#include "core/packs/archive_bridge.hpp"
#include "core/packs/internal.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <thread>
#include <miniz.h>

using namespace nickel::packs;
namespace io = nickel::instances::detail;
namespace instances = nickel::instances;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Fn> void rejected(Fn&& fn, std::string_view code = {}) {
    try { fn(); }
    catch (const nickel::ErrorException& error) {
        if (!code.empty() && error.error().code != code) throw std::runtime_error("Expected " + std::string(code) + ", got " + error.error().code);
        return;
    }
    throw std::runtime_error("Expected safe rejection");
}
struct Fixture {
    fs::path root = fs::current_path() / ("pack-fixture-" + io::random_id());
    Fixture() { require(io::create_directory_no_replace(root), "Fixture identity conflict"); }
    ~Fixture() { try { io::remove_owned_tree(root); } catch (...) {} }
    fs::path game(std::string_view label = "game") {
        const auto result = root / instances::path_from_utf8(label);
        io::create_directories(result);
        return result;
    }
};
void write(const fs::path& path, std::string_view text) { io::atomic_write(path, text); }
Json index(std::string_view name = "Synthetic Pack") {
    return {{"formatVersion", 1}, {"game", "minecraft"}, {"versionId", "1.0"}, {"name", name},
            {"dependencies", {{"minecraft", "1.20.1"}, {"fabric-loader", "0.16.9"}}}, {"files", Json::array()}};
}
std::unique_ptr<instances::OnboardingGate> accepted(const fs::path& root) {
    auto gate = std::make_unique<instances::OnboardingGate>(root / "Nickel", "0.1.0");
    gate->accept_eula(true, instances::eula_version, 1700000000);
    gate->acknowledge_changelog("0.1.0");
    require(gate->finish([] {}), "Onboarding did not finish");
    return gate;
}
instances::InstallIdentity install_identity(const InstallerRequest& request) {
    require(instances::is_pending(request.instance_directory), "Installer did not use reserved final directory");
    const auto version = request.instance_directory / "versions" / request.minecraft_version;
    write(version / (request.minecraft_version + ".json"), Json{{"id", request.minecraft_version}, {"type", "release"}, {"mainClass", "net.minecraft.client.main.Main"}}.dump());
    write(version / (request.minecraft_version + ".jar"), "SYNTHETIC_GAME_JAR");
    return {request.minecraft_version, request.loader ? std::optional<std::string>(request.loader->kind) : std::nullopt,
            request.loader ? request.loader->version : "", Json{{"id", request.minecraft_version}, {"type", "release"},
            {"mainClass", "net.minecraft.client.main.Main"}, {"releaseTime", "2025-01-01T00:00:00Z"}, {"syntheticUnknown", 42}}, nullptr};
}
InstallServices services(const std::shared_ptr<ArchiveBackend>& backend, int& installs) {
    InstallServices result;
    result.archive = backend;
    result.install = [&](const InstallerRequest& request, std::stop_token cancel) { io::check_cancelled(cancel); ++installs; return install_identity(request); };
    result.drain_owned_installer = [](auto) { return true; };
    return result;
}
std::string ready_instance(const fs::path& game, std::string_view name = "Example") {
    auto transaction = instances::InstanceTransaction::reserve(game, name, "1.20.1", "fabric", "0.16.9");
    const InstallerRequest request{game, transaction.directory(), std::string(name), "1.20.1", PackLoader{"fabric", "0.16.9"}};
    transaction.complete(install_identity(request));
    transaction.commit("Display differs from identity", {{"origin", "synthetic"}});
    return std::string(name);
}
void raw_zip(const fs::path& file, const std::vector<std::pair<std::string, std::string>>& entries, mz_uint compression = MZ_BEST_SPEED) {
    mz_zip_archive zip{};
    require(mz_zip_writer_init_heap_v2(&zip, 0, 0, MZ_ZIP_FLAG_WRITE_ZIP64) != 0, "Raw ZIP fixture init failed");
    try {
        for (const auto& [name, text] : entries) require(mz_zip_writer_add_mem(&zip, name.c_str(), text.data(), text.size(), compression) != 0, "Raw ZIP fixture add failed");
        void* bytes = nullptr;
        std::size_t size = 0;
        require(mz_zip_writer_finalize_heap_archive(&zip, &bytes, &size) != 0, "Raw ZIP fixture finalization failed");
        mz_zip_writer_end(&zip);
        try { io::write_exclusive(file, std::string_view(static_cast<const char*>(bytes), size)); }
        catch (...) { mz_free(bytes); throw; }
        mz_free(bytes);
    } catch (...) { if (zip.m_pState) mz_zip_writer_end(&zip); throw; }
}
void make_pack(const fs::path& target, ArchiveBackend& backend, Json manifest = index(),
               std::vector<ArchiveInput> extra = {}) {
    extra.push_back({std::string(mrpack_index), {}, manifest.dump()});
    backend.write(target, extra, {}, {});
}
void manifest_security() {
    auto parsed = parse_mrpack_manifest(index().dump());
    require(parsed.minecraft_version == "1.20.1" && parsed.loader && parsed.loader->kind == "fabric" && parsed.loader->version == "0.16.9", "Exact mrpack identity lost");
    require(parse_mrpack_manifest(mrpack_manifest_to_json(parsed).dump()).version == "1.0", "mrpack version round-trip failed");
    for (const auto* bad : {"../escape", "a\\b", "/escape", "a//b", "a/../b", "a/./b", "NUL.txt", "COM1/x", "a:stream", "a./b", "a/b ", "a/\"b"}) rejected([&] { validate_archive_path(bad); }, "unsafe_path");
    for (const auto* bad : {"nickel.profile.json", ".nickel-pack.pending", "versions/id/id.json", "libraries/file.jar", "accounts.json", "config/client_secret.json", "config/private.key", "logs/latest.log"}) rejected([&] { validate_pack_path(bad); }, "protected_path");
    validate_pack_path("config/client.json");
    validate_download_url("https://cdn.modrinth.com/data/example/file.jar?version=1");
    for (const auto* bad : {"http://example.test/a", "https://user:synthetic@example.test/a", "https://example.test/a#fragment", "https:///a", "https://example.test:99999/a", "https://example.test/a?api%4bey=SYNTHETIC", "https://example.test/a\\b"}) rejected([&] { validate_download_url(bad); }, "unsafe_url");
    auto wrong = index(); wrong["future"] = true; rejected([&] { parse_mrpack_manifest(wrong.dump()); }, "invalid_schema");
    wrong = index(); wrong["dependencies"]["forge"] = "47.3.0"; rejected([&] { parse_mrpack_manifest(wrong.dump()); }, "invalid_schema");
    wrong = index(); wrong["dependencies"]["future-loader"] = "1"; rejected([&] { parse_mrpack_manifest(wrong.dump()); }, "invalid_schema");
    wrong = index(); wrong["dependencies"]["minecraft"] = "1.20.1/escape"; rejected([&] { parse_mrpack_manifest(wrong.dump()); }, "invalid_schema");
    const auto duplicate = std::string("{\"formatVersion\":1,\"formatVersion\":1}"); rejected([&] { parse_mrpack_manifest(duplicate); }, "invalid_json");
    std::cout << "mrpack exact identities/unknown schema/URLs/device/traversal/protected content: PASS\n";
}
void zip_security() {
    Fixture f;
    auto backend = make_zip_backend();
    const auto archive = f.root / instances::path_from_utf8("zip-\xe4\xb8\xad\xe6\x96\x87.zip");
    backend->write(archive, std::vector<ArchiveInput>{{"config/example.txt", {}, std::string(200000, 'A')}, {"empty", {}, {}}}, {}, {});
    const auto entries = backend->inspect(archive, {}, {});
    require(entries.size() == 2 && entries[0].size == 200000, "ZIP stream metadata incorrect");
    require(backend->read(archive, "config/example.txt", 200000, {}).size() == 200000, "ZIP bounded read failed");
    rejected([&] { backend->read(archive, "config/example.txt", 10, {}); }, "limit");
    const auto target = f.root / "out.txt";
    backend->extract_file(archive, "config/example.txt", target, 200000, {});
    require(fs::file_size(target) == 200000, "Streaming extraction failed");
    rejected([&] { backend->extract_file(archive, "config/example.txt", target, 200000, {}); }, "conflict");
    require(io::read_bounded(target, 200000) == std::string(200000, 'A'), "Existing target overwritten");
    rejected([&] { backend->inspect(archive, {1, max_pack_file_bytes, max_pack_bytes}, {}); }, "limit");
    rejected([&] { backend->inspect(archive, {100, 199999, max_pack_bytes}, {}); }, "limit");
    std::stop_source stop; stop.request_stop();
    rejected([&] { backend->extract_file(archive, "config/example.txt", f.root / "cancel.txt", 200000, stop.get_token()); }, "cancelled");
    require(!fs::exists(f.root / "cancel.txt"), "Cancelled ZIP extraction published");
    backend->release(archive);
    int count = 0;
    for (const auto& names : std::vector<std::vector<std::string>>{{"../escape"}, {"C:/escape"}, {"config/A", "config/a"}, {"file", "file/child"}, {"NUL.txt"}}) {
        const auto hostile = f.root / ("hostile-" + std::to_string(count++) + ".zip");
        std::vector<std::pair<std::string, std::string>> data;
        for (const auto& name : names) data.emplace_back(name, "SYNTHETIC");
        raw_zip(hostile, data);
        rejected([&] { backend->inspect(hostile, {}, {}); });
        backend->release(hostile);
    }
    const auto link = f.root / "link.zip";
    raw_zip(link, {{"config/link", "outside"}});
    auto bytes = io::read_bounded(link, 4096);
    const auto central = bytes.find(std::string("PK\x01\x02", 4));
    require(central != std::string::npos, "Raw ZIP central directory missing");
    bytes[central + 5] = 3;
    const auto attrs = static_cast<std::uint32_t>(0120777U << 16);
    for (int i = 0; i < 4; ++i) bytes[central + 38 + i] = static_cast<char>((attrs >> (8 * i)) & 255);
    write(link, bytes);
    rejected([&] { backend->inspect(link, {}, {}); }, "unsafe_path");
    backend->release(link);
    const auto corrupt = f.root / "corrupt.zip";
    raw_zip(corrupt, {{"file", "SYNTHETIC_CRC_BYTES"}}, 0);
    bytes = io::read_bounded(corrupt, 4096);
    const auto payload = bytes.find("SYNTHETIC_CRC_BYTES");
    require(payload != std::string::npos, "Stored CRC fixture missing");
    bytes[payload] = 'X'; write(corrupt, bytes);
    backend->inspect(corrupt, {}, {});
    rejected([&] { backend->extract_file(corrupt, "file", f.root / "corrupt.txt", 19, {}); });
    require(!fs::exists(f.root / "corrupt.txt"), "Bad CRC file published");
    backend->release(corrupt);
    const auto source = f.root / "source.txt"; write(source, "SOURCE");
    std::error_code ec; fs::create_hard_link(source, f.root / "hard.txt", ec);
    if (!ec) rejected([&] { detail::copy_file(f.root / "hard.txt", f.root / "copied.txt", 100, {}); }, "unsafe_path");
    std::cout << "real ZIP64 stream/read/no-overwrite/cancel/case/links/CRC/quotas/Unicode: PASS\n";
}
void import_transaction() {
    Fixture f;
    auto backend = make_zip_backend();
    auto gate = accepted(f.root);
    const auto archive = f.root / "input.mrpack";
    make_pack(archive, *backend, index(), {{"overrides/config/client.json", {}, "{\"enabled\":true}"}, {"overrides/options.txt", {}, "fullscreen:false\n"}});
    int installs = 0;
    auto service = services(backend, installs);
    const auto game = f.game();
    auto plan = inspect_pack(archive, "Imported", *backend);
    const auto result = import_pack(plan, game, service, *gate);
    require(installs == 1 && result.instance_id == "Imported" && !instances::is_pending(result.directory), "Pack transaction failed");
    require(instances::scan_instances(game).size() == 1 && instances::load_profile(game, "Imported")->loader_version == "0.16.9", "Imported identity not published exactly");
    require(io::read_bounded(result.directory / "config/client.json", 1024) == "{\"enabled\":true}", "Overrides did not extract");
    const auto descriptor = io::parse_strict_json(io::read_bounded(result.directory / "Imported.json", instances::descriptor_max_bytes), instances::descriptor_max_bytes);
    require(descriptor["id"] == "Imported" && descriptor["nickelCustom"]["name"] == "Synthetic Pack" && descriptor["syntheticUnknown"] == 42, "Descriptor or display identity lost");
    rejected([&] { import_pack(plan, game, service, *gate); }, "conflict");
    require(installs == 1, "Conflicting pack called installer");
    std::stop_source cancel;
    auto cancelled_service = service;
    cancelled_service.install = [&](const InstallerRequest& request, std::stop_token) { auto result = install_identity(request); cancel.request_stop(); return result; };
    rejected([&] { import_pack(inspect_pack(archive, "Cancelled", *backend), game, cancelled_service, *gate, cancel.get_token()); }, "cancelled");
    require(!fs::exists(game / "versions/Cancelled"), "Cancelled owned target was not removed");
    auto failed = service;
    failed.install = [](const InstallerRequest&, std::stop_token) -> instances::InstallIdentity { throw instances::Error("synthetic_install", "Synthetic installer failure"); };
    rejected([&] { import_pack(inspect_pack(archive, "Failed", *backend), game, failed, *gate); }, "synthetic_install");
    require(!fs::exists(game / "versions/Failed"), "Failed target leaked");
    auto timeout = service;
    timeout.drain_owned_installer = [](auto) { return false; };
    rejected([&] { import_pack(inspect_pack(archive, "NoEOF", *backend), game, timeout, *gate); }, "installer_drain");
    require(instances::is_pending(game / "versions/NoEOF"), "Undrained installer target removed");
    require(!instances::recover_interrupted_instances(game).skipped.empty(), "Undrained active process not protected from recovery");
    auto lost = service;
    lost.install = [](const InstallerRequest& request, std::stop_token) {
        auto result = install_identity(request); write(request.instance_directory / ".nickel-pack.pending", std::string(32, 'a')); return result;
    };
    rejected([&] { import_pack(inspect_pack(archive, "LostOwner", *backend), game, lost, *gate); }, "ownership_changed");
    require(fs::is_directory(game / "versions/LostOwner"), "Foreign target deleted after ownership change");
    const auto altered = f.root / "altered.mrpack"; make_pack(altered, *backend);
    const auto altered_plan = inspect_pack(altered, "Altered", *backend);
    write(altered, "CHANGED_SYNTHETIC_ARCHIVE");
    rejected([&] { import_pack(altered_plan, game, service, *gate); }, "source_changed");
    instances::OnboardingGate unfinished(f.root / "other-config", "0.1.0");
    rejected([&] { import_pack(plan, f.game("other"), service, unfinished); });
    const auto with_download = f.root / "download.mrpack";
    const auto remote_source = f.root / "remote.jar"; write(remote_source, "SYNTHETIC_REMOTE_JAR");
    const PackFile remote{"mods/remote.jar", {"https://cdn.modrinth.com/data/example/remote.jar"}, fs::file_size(remote_source), hash_file(remote_source, "sha1"), hash_file(remote_source, "sha512")};
    auto manifest = index(); manifest["files"].push_back(detail::file_to_json(remote)); make_pack(with_download, *backend, manifest);
    auto download = service;
    download.download = [&](const DownloadRequest& request, std::stop_token stop) { detail::copy_file(remote_source, request.target, request.file.size, stop); };
    auto installed = import_pack(inspect_pack(with_download, "Downloaded", *backend), game, download, *gate);
    require(hash_file(installed.directory / "mods/remote.jar", "sha512") == *remote.sha512, "Real download hash was not checked");
    download.download = [](const DownloadRequest& request, std::stop_token) { write(request.target, "BAD_SYNTHETIC_BYTES"); };
    rejected([&] { import_pack(inspect_pack(with_download, "BadHash", *backend), game, download, *gate); }, "integrity");
    require(!fs::exists(game / "versions/BadHash"), "Unverified download published");
    std::cout << "install callback/final-root identity/download hashes/cancel/no-overwrite/EOF/ownership/onboarding/source change: PASS\n";
}
void final_publish_contract() {
    Fixture fixture;
    auto backend = make_zip_backend();
    auto gate = accepted(fixture.root);
    const auto game = fixture.game();
    const auto remote_source = fixture.root / "remote.jar";
    write(remote_source, "SYNTHETIC_PUBLISH_RESOURCE");
    const PackFile remote{"mods/remote.jar", {"https://cdn.modrinth.com/data/example/remote.jar"},
        26, hash_file(remote_source, "sha1"), hash_file(remote_source, "sha512")};
    auto manifest = index();
    manifest["files"].push_back(detail::file_to_json(remote));
    const auto archive = fixture.root / "publish.mrpack";
    make_pack(archive, *backend, manifest, {{"overrides/config/client.json", {}, "{\"enabled\":true}"}});
    std::vector<std::string> stages;
    bool inside_publication = false;
    const auto fresh_services = [&] {
        InstallServices result;
        result.archive = backend;
        result.install = [&](const InstallerRequest& request, std::stop_token stop) {
            require(!inside_publication, "Installer ran inside the final task publication gate");
            io::check_cancelled(stop);
            stages.push_back("install");
            return install_identity(request);
        };
        result.drain_owned_installer = [&](auto) {
            require(!inside_publication, "Owned child drain ran inside the final task publication gate");
            stages.push_back("drain");
            return true;
        };
        result.download = [&](const DownloadRequest& request, std::stop_token stop) {
            require(!inside_publication, "Download ran inside the final task publication gate");
            stages.push_back("download");
            detail::copy_file(remote_source, request.target, request.file.size, stop);
        };
        return result;
    };
    const auto prepared = [&](std::string_view id) {
        require(stages == std::vector<std::string>{"install", "drain", "download"},
            "Final publication boundary did not follow the original installer/drain/download graph");
        const auto directory = game / "versions" / instances::path_from_utf8(id);
        require(instances::is_pending(directory), "Instance became public before final publication");
        const auto profile = io::read_optional_json(directory / instances::profile_file, 65536);
        require(profile && (*profile)["ready"] == true, "Transaction completion ran inside final publication");
        require(io::read_bounded(directory / "config/client.json", 1024) == "{\"enabled\":true}",
            "Override preparation did not finish before final publication");
    };
    int final_calls = 0;
    auto service = fresh_services();
    std::stop_source late;
    service.final_publish = [&](const std::function<void()>& commit) {
        ++final_calls;
        prepared("LateCancel");
        inside_publication = true;
        commit();
        inside_publication = false;
        require(!instances::is_pending(game / "versions/LateCancel"), "Final callback did not publish exactly once");
        late.request_stop();
    };
    const auto result = import_pack(inspect_pack(archive, "LateCancel", *backend), game, service, *gate, late.get_token());
    require(final_calls == 1 && result.instance_id == "LateCancel" && !instances::is_pending(result.directory),
        "Late cancellation changed a successful final publication");

    stages.clear();
    service = fresh_services();
    std::stop_source thrown_late;
    service.final_publish = [&](const std::function<void()>& commit) {
        prepared("LateWrapper");
        commit();
        thrown_late.request_stop();
        throw instances::Error("cancelled", "Synthetic cancellation after an irreversible commit");
    };
    const auto wrapper_result = import_pack(inspect_pack(archive, "LateWrapper", *backend), game, service, *gate, thrown_late.get_token());
    require(wrapper_result.instance_id == "LateWrapper" && !instances::is_pending(wrapper_result.directory),
        "Late wrapper cancellation reported a published instance as uncommitted");

    stages.clear();
    service = fresh_services();
    service.final_publish = [&](const std::function<void()>&) { prepared("Skipped"); };
    rejected([&] { import_pack(inspect_pack(archive, "Skipped", *backend), game, service, *gate); }, "publication");
    require(!io::path_exists(game / "versions/Skipped"), "Skipped publication leaked a prepared reservation");

    stages.clear();
    service = fresh_services();
    service.final_publish = [&](const std::function<void()>&) {
        prepared("Rejected");
        throw instances::Error("synthetic_publish", "Synthetic final task gate rejection");
    };
    rejected([&] { import_pack(inspect_pack(archive, "Rejected", *backend), game, service, *gate); }, "synthetic_publish");
    require(!io::path_exists(game / "versions/Rejected"), "Rejected final gate leaked a prepared reservation");

    stages.clear();
    service = fresh_services();
    std::stop_source before;
    service.final_publish = [&](const std::function<void()>& commit) {
        prepared("BeforeCommit");
        before.request_stop();
        commit();
    };
    rejected([&] { import_pack(inspect_pack(archive, "BeforeCommit", *backend), game, service, *gate, before.get_token()); }, "cancelled");
    require(!io::path_exists(game / "versions/BeforeCommit"), "Pre-commit cancellation published a prepared reservation");

    stages.clear();
    service = fresh_services();
    std::stop_source swallowed;
    service.final_publish = [&](const std::function<void()>& commit) {
        prepared("Swallowed");
        swallowed.request_stop();
        try { commit(); } catch (const nickel::ErrorException&) {}
    };
    rejected([&] { import_pack(inspect_pack(archive, "Swallowed", *backend), game, service, *gate, swallowed.get_token()); }, "cancelled");
    require(!io::path_exists(game / "versions/Swallowed"), "Callback swallowed a failed domain commit");

    stages.clear();
    service = fresh_services();
    int repeated_rejections = 0;
    service.final_publish = [&](const std::function<void()>& commit) {
        prepared("OnceOnly");
        commit();
        try { commit(); }
        catch (const nickel::ErrorException& error) {
            require(error.error().code == "publication", "Second final commit failed in a different boundary");
            ++repeated_rejections;
        }
    };
    const auto once = import_pack(inspect_pack(archive, "OnceOnly", *backend), game, service, *gate);
    require(repeated_rejections == 1 && !instances::is_pending(once.directory), "Final commit was not single-use");
    require(instances::scan_instances(game).size() == 3, "Rejected final gates exposed extra public instances");
    std::cout << "final publish/only commit in gate/prepared drain/single-use/skipped and failed commit/late cancellation: PASS\n";
}
void long_paths() {
    Fixture fixture;
    auto base = fixture.root;
    for (unsigned index = 0; base.native().size() < 360; ++index)
        base /= "pack-deep-" + std::to_string(index) + '-' + std::string(48, 'x');
    require(!instances::path_to_utf8(base).starts_with("\\\\?\\"), "Pack deep fixture replaced the original path with a native prefix");
    const auto workspace_parent = base / "new-original-parent";
    require(!io::path_exists(workspace_parent), "Deep private workspace parent already exists");
    const auto source = fixture.root / "source.bin";
    const std::string bytes(150000, 'S');
    write(source, bytes);
    fs::path owned;
    {
        detail::Workspace workspace(workspace_parent);
        owned = workspace.directory();
        require(owned.parent_path() == workspace_parent && owned.native().size() > 400,
            "Private workspace changed or shortened the original deep parent");
        require(io::read_bounded(owned / ".nickel-export.owner", 64).size() == 32, "Deep workspace ownership marker is missing");
        require(!io::create_directory_no_replace(owned), "Deep directory reservation reused an existing entry");
        const auto target = owned / "nested/original/stream.bin";
        require(detail::copy_file(source, target, bytes.size(), {}) == bytes.size()
            && io::read_bounded(target, bytes.size()) == bytes, "Deep pinned streaming copy failed");
        rejected([&] { detail::copy_file(source, target, bytes.size(), {}); }, "conflict");
        std::stop_source stop; stop.request_stop();
        rejected([&] { detail::copy_file(source, owned / "cancelled.bin", bytes.size(), stop.get_token()); }, "cancelled");
        require(!io::path_exists(owned / "cancelled.bin"), "Deep pre-cancelled stream created output");
        workspace.cleanup();
        require(!io::path_exists(owned), "Deep owned private workspace was not removed");
    }
    auto backend = make_zip_backend();
    auto gate = accepted(fixture.root);
    instances::InstanceRegistry registry(fixture.root / "Nickel", fixture.root / "default-game");
    require(registry.ensure_default() == fixture.root / "default-game", "Deep fixture did not register its default root");
    const auto original_parent = base / "create-root-original";
    const auto game = registry.create_root(original_parent);
    require(game == original_parent / ".minecraft" && game.native().size() > 360,
        "Deep create_root changed the original public path");
    require(registry.create_root(original_parent) == game && registry.get().roots.size() == 2,
        "Repeated deep create_root registered duplicate roots");
    const auto remote_source = base / "inputs/remote.jar";
    constexpr std::string_view remote_bytes = "SYNTHETIC_DEEP_RESOURCE";
    write(remote_source, remote_bytes);
    const PackFile remote{"mods/remote.jar", {"https://cdn.modrinth.com/data/example/remote.jar"},
        remote_bytes.size(), hash_file(remote_source, "sha1"), hash_file(remote_source, "sha512")};
    auto manifest = index(); manifest["files"].push_back(detail::file_to_json(remote));
    const auto archive = base / "inputs/original.mrpack";
    make_pack(archive, *backend, manifest, {{"overrides/config/client.json", {}, "{\"enabled\":true}"}});
    int installs = 0, final_calls = 0;
    std::string publishing_id = "DeepPack";
    auto service = services(backend, installs);
    service.download = [&](const DownloadRequest& request, std::stop_token stop) {
        detail::copy_file(remote_source, request.target, request.file.size, stop);
    };
    service.final_publish = [&](const std::function<void()>& commit) {
        ++final_calls;
        const auto directory = game / "versions" / instances::path_from_utf8(publishing_id);
        const auto owner = io::read_bounded(directory / instances::pending_file, 64);
        const auto journal = game / ".nickel-pack-transactions" / (owner + ".json");
        require(journal.native().size() > 400 && io::read_optional_json(journal, 65536),
            "Deep pack final publication did not retain the original journal path");
        commit();
    };
    const auto result = import_pack(inspect_pack(archive, "DeepPack", *backend), game, service, *gate);
    require(installs == 1 && final_calls == 1 && result.directory == game / "versions/DeepPack"
        && !instances::is_pending(result.directory), "Deep whole pack import failed to publish once");
    require(hash_file(result.directory / "mods/remote.jar", "sha512") == *remote.sha512
        && io::read_bounded(result.directory / "config/client.json", 1024) == "{\"enabled\":true}",
        "Deep ZIP override or pinned download did not publish verified bytes");
    const auto target = instances::resolve_launch_target(game, "DeepPack", Json::object());
    require(target.install_root == result.directory && target.descriptor_path == result.directory / "versions/1.20.1/1.20.1.json",
        "Deep imported legacy target did not use the canonical domain resolver");
    const auto preview = preview_export(game, "DeepPack", Json::object());
    require(std::any_of(preview.files.begin(), preview.files.end(), [](const auto& file) { return file.path == "mods/remote.jar"; }),
        "Deep export preview enumerated a different directory");
    ExportRequest request;
    request.game_root = game;
    request.instance_id = "DeepPack";
    request.name = "Deep synthetic portable pack";
    request.version = "1.0";
    request.selections = {"@dir/config/config", "mods/remote.jar"};
    ExportServices exporter;
    exporter.archive = backend;
    const auto exported = base / "outputs/deep.mrpack";
    export_pack(request, exported, Json::object(), exporter, *gate);
    const auto exported_plan = inspect_pack(exported, "DeepRoundtrip", *backend);
    publishing_id = "DeepRoundtrip";
    const auto roundtrip = import_pack(exported_plan, game, service, *gate);
    require(final_calls == 2 && io::read_bounded(roundtrip.directory / "mods/remote.jar", 1024) == remote_bytes,
        "Deep mrpack export/publication/import roundtrip lost verified content or skipped its final gate");
    request.format = ExportFormat::portable;
    const auto portable = base / "outputs/deep-portable.zip";
    export_pack(request, portable, Json::object(), exporter, *gate);
    const auto extracted = base / "portable-original-root";
    io::create_directories(extracted);
    for (const auto& entry : backend->inspect(portable, {}, {}))
        if (!entry.directory) backend->extract_file(portable, entry.name, extracted / instances::path_from_utf8(entry.name), entry.size, {});
    backend->release(portable);
    service.final_publish = {};
    const auto first = start_portable_bootstrap(extracted, service, *gate);
    const auto second = start_portable_bootstrap(extracted, service, *gate);
    require(first && second && first->instance_id == "DeepPack" && second->directory == first->directory && installs == 3,
        "Original deep portable bootstrap imported more than once or changed identity");
    const auto status = portable_bootstrap_status(extracted);
    require(status && status->state == "installed" && status->game_directory == io::canonical_path(extracted) / ".minecraft"
        && !instances::path_to_utf8(status->game_directory).starts_with("\\\\?\\"), "Deep bootstrap DTO exposed a native prefix");
    instances::delete_instance(game, "DeepPack");
    instances::delete_instance(game, "DeepRoundtrip");
    registry.remove_root(game, true, {});
    require(!io::path_exists(game), "Deep published pack root could not be safely retired");
    std::cout << "raw deep create_root/private workspace/pinned streaming/ZIP import and export/portable once-only/final journal/canonical resolver/owned cleanup: PASS\n";
}
ExportRequest request_for(const fs::path& game, const std::string& id) {
    ExportRequest result;
    result.game_root = game; result.instance_id = id; result.name = "Synthetic Pack"; result.version = "1.0";
    result.selections = {"@dir/config/config", "mods/example.jar", "options.txt"};
    return result;
}
void export_roundtrip() {
    Fixture f;
    auto backend = make_zip_backend();
    auto gate = accepted(f.root);
    const auto game = f.game(); const auto id = ready_instance(game); const auto directory = game / "versions" / id;
    write(directory / "config/client.json", "{\"enabled\":true,\"apiKey\":\"${SYNTHETIC_KEY}\"}");
    write(directory / "config/jei/world/personal.json", "{\"personal\":true}");
    write(directory / "config/accounts.json", "SYNTHETIC_EXCLUDED_ACCOUNT");
    write(directory / "mods/example.jar", "SYNTHETIC_MOD_JAR"); write(directory / "options.txt", "fullscreen:false\n");
    write(directory / "saves/World/level.dat", "SYNTHETIC_WORLD");
    write(directory / "nickel.settings.json", "{\"memoryMode\":\"custom\",\"memoryMb\":6144,\"javaPath\":\"C:/SYNTHETIC/jdk\",\"preLaunchCommand\":\"echo SYNTHETIC\"}");
    const auto preview = preview_export(game, id, Json::object());
    require(preview.name == "Display differs from identity" && preview.loader && preview.loader->version == "0.16.9", "Export preview identity incorrect");
    require(std::any_of(preview.files.begin(), preview.files.end(), [](const auto& entry) { return entry.path == "@dir/saves/saves/World" && !entry.selected; }), "Personal world defaults changed");
    require(std::any_of(preview.files.begin(), preview.files.end(), [](const auto& entry) { return entry.path == "@dir/jeiPersonal/config/jei/world" && !entry.selected; }), "JEI private selection default changed");
    auto request = request_for(game, id);
    ExportServices exporter; exporter.archive = backend;
    const auto output = f.root / "export.mrpack";
    export_pack(request, output, Json::object(), exporter, *gate);
    auto entries = backend->inspect(output, {}, {});
    std::set<std::string> names; for (const auto& entry : entries) names.insert(entry.name);
    require(names.contains("modrinth.index.json") && names.contains("overrides/config/client.json") && names.contains("overrides/mods/example.jar"), "Exported archive incomplete");
    for (const auto& name : names) require(name.find("accounts") == std::string::npos && name.find("jei/world") == std::string::npos && name.find("saves/") == std::string::npos && name.find("nickel.settings") == std::string::npos, "Default export leaked excluded or personal content");
    backend->release(output);
    int installs = 0; auto installer = services(backend, installs);
    auto result = import_pack(inspect_pack(output, "Roundtrip", *backend), f.game("target"), installer, *gate);
    require(io::read_bounded(result.directory / "config/client.json", 1024).find("enabled") != std::string::npos, "Actual mrpack round-trip lost content");
    rejected([&] { export_pack(request, output, Json::object(), exporter, *gate); }, "conflict");
    const auto malicious = f.root / "unsafe.mrpack";
    write(directory / "config/client.json", "{\"apiKey\":\"SYNTHETIC_NOT_A_REAL_KEY\"}");
    rejected([&] { export_pack(request, malicious, Json::object(), exporter, *gate); }, "unsafe_content");
    require(!fs::exists(malicious), "Unsafe configuration exported");
    write(directory / "config/client.json", "{\"enabled\":true}");
    request.bundle_resources = false; request.modrinth_only = true;
    exporter.resolve_resource = [&](const fs::path& source, std::string_view path, std::stop_token cancel) -> std::optional<PackFile> {
        return PackFile{std::string(path), {"https://cdn.modrinth.com/data/example/file.jar"}, fs::file_size(source), hash_file(source, "sha1", cancel), hash_file(source, "sha512", cancel)};
    };
    const auto external = f.root / "external.mrpack"; export_pack(request, external, Json::object(), exporter, *gate);
    const auto external_plan = inspect_pack(external, "External", *backend);
    require(external_plan.manifest.files.size() == 1 && std::none_of(external_plan.overrides.begin(), external_plan.overrides.end(), [](const auto& entry) { return entry.first == "mods/example.jar"; }), "Resource resolver was not used");
    exporter.resolve_resource = [](const fs::path&, std::string_view, std::stop_token) -> std::optional<PackFile> { return {}; };
    rejected([&] { export_pack(request, f.root / "absent.mrpack", Json::object(), exporter, *gate); }, "resource_not_found");
    Json old_request{{"dir", instances::path_to_utf8(game)}, {"versionId", id}, {"name", "Pack"}, {"version", "1"}, {"files", request.selections}, {"bundleResources", true}, {"modrinthOnly", true}, {"format", "portable"}, {"includeLauncher", false}, {"includePersonalization", true}};
    const auto decoded = export_request_from_json(old_request);
    require(decoded.bundle_resources && decoded.modrinth_only && decoded.include_personalization && decoded.format == ExportFormat::portable, "Legacy export flags lost");
    old_request["accessToken"] = "SYNTHETIC"; rejected([&] { export_request_from_json(old_request); }, "invalid_schema");
    std::cout << "actual mrpack export/import/archive whitelist/personal opt-in/compact selections/credential rejection/community hashes/flags: PASS\n";
}
void portable_roundtrip() {
    Fixture f;
    auto backend = make_zip_backend();
    auto gate = accepted(f.root);
    const auto game = f.game(); const auto id = ready_instance(game); const auto directory = game / "versions" / id;
    write(directory / "config/client.json", "{\"enabled\":true}"); write(directory / "mods/example.jar", "SYNTHETIC_MOD_JAR"); write(directory / "options.txt", "fullscreen:false\n");
    write(directory / "nickel.settings.json", "{\"memoryMode\":\"custom\",\"memoryMb\":6144,\"javaPath\":\"C:/SYNTHETIC/jdk\",\"preLaunchCommand\":\"echo SYNTHETIC\",\"future\":true}");
    const auto image = f.root / "source.png"; write(image, std::string("\x89PNG\r\n\x1a\n", 8) + "SYNTHETIC_IMAGE_HEADER");
    auto request = request_for(game, id); request.format = ExportFormat::portable; request.include_personalization = true;
    ExportServices exporter; exporter.archive = backend; exporter.personalization = {{"themeMode", "dark"}, {"accent", "emerald"}, {"titlebarTint", true}, {"backgroundBlur", 16.0}, {"backgroundOpacity", 0.8}, {"accounts", Json::array()}, {"accessToken", "SYNTHETIC_EXCLUDED"}}; exporter.background = image;
    const auto output = f.root / "portable.zip"; export_pack(request, output, Json::object(), exporter, *gate);
    const auto entries = backend->inspect(output, {}, {});
    const auto manifest = validate_portable_manifest(io::parse_strict_json(backend->read(output, portable_manifest_file, 65536, {}), 65536));
    require(manifest["schemaVersion"] == 1 && manifest["payload"] == "Nickel/pack.mrpack"
        && manifest["dataDirectory"] == "Nickel" && manifest["gameDirectory"] == ".minecraft", "Portable schema 1 changed");
    const auto readme = backend->read(output, "README-portable.txt", 4096, {});
    require(readme.starts_with("Palladium portable pack (schema 1)\n")
        && readme.find("Keep Nickel and .minecraft beside the executable") != std::string::npos
        && readme.find("legacy directory names are required for schema 1 compatibility") != std::string::npos,
        "Portable display branding changed its legacy schema 1 directory guidance");
    require(manifest["packageId"].get<std::string>().size() == 32 && !manifest.contains("launcher"), "Portable identity or no-launcher flag incorrect");
    auto invalid = manifest; invalid["future"] = true; rejected([&] { validate_portable_manifest(invalid); }, "invalid_schema");
    invalid = manifest; invalid["payload"] = "../pack.mrpack"; rejected([&] { validate_portable_manifest(invalid); }, "invalid_schema");
    invalid = manifest; invalid["launcher"] = {{"path", "Nickel.exe"}, {"os", "windows"}, {"arch", "x64"}, {"unknown", true}}; rejected([&] { validate_portable_manifest(invalid); }, "invalid_schema");
    const auto root = f.root / "extracted"; fs::create_directory(root);
    for (const auto& entry : entries) if (!entry.directory) backend->extract_file(output, entry.name, root / instances::path_from_utf8(entry.name), entry.size, {});
    backend->release(output);
    auto bootstrap = load_portable_bootstrap(root, *backend);
    require(bootstrap && bootstrap->personalization && !bootstrap->personalization->contains("accounts") && !bootstrap->personalization->contains("accessToken"), "Portable appearance included unlisted fields");
    require(bootstrap->instance_settings && (*bootstrap->instance_settings)["memoryMb"] == 6144 && !bootstrap->instance_settings->contains("javaPath") && !bootstrap->instance_settings->contains("preLaunchCommand"), "Portable instance whitelist failed");
    const auto displayed = personalization_for_display(*bootstrap, *bootstrap->personalization);
    require(instances::path_from_utf8(displayed["backgroundImage"].get<std::string>()).is_absolute(), "Display background unresolved");
    require(personalization_for_storage(*bootstrap, displayed) == *bootstrap->personalization, "Portable background persisted a host path");
    auto unsafe = *bootstrap->personalization; unsafe["backgroundImage"] = "C:/SYNTHETIC/path.png"; rejected([&] { validate_personalization(unsafe, manifest); }, "unsafe_path");
    unsafe = *bootstrap->personalization; unsafe["customTheme"] = {{"hue", 42}, {"saturation", 40}, {"lightness", 40}, {"titlebarHueShift", 0}, {"decoration", "lattice"}, {"decorationOpacity", 0.35}, {"apiKey", "SYNTHETIC"}}; rejected([&] { validate_personalization(unsafe, manifest); }, "invalid_schema");
    instances::OnboardingGate unfinished(root / "other-config", "0.1.0"); int installs = 0; auto installer = services(backend, installs);
    rejected([&] { start_portable_bootstrap(root, installer, unfinished); }); require(installs == 0, "Portable bypassed onboarding");
    const auto first = start_portable_bootstrap(root, installer, *gate);
    const auto second = start_portable_bootstrap(root, installer, *gate);
    require(first && second && first->instance_id == id && second->instance_id == id && installs == 1, "Portable bootstrap imported more than once");
    require(portable_bootstrap_status(root)->state == "installed", "Portable installed state missing");
    require(instances::get_version_settings(root / ".minecraft", id)["memoryMb"] == 6144, "Portable settings not applied");
    const auto seeded = io::parse_strict_json(io::read_bounded(root / "Nickel/settings.json", 65536), 65536);
    require(seeded["backgroundImage"] == "Nickel/background.png", "Seed wrote absolute background path");
    write(root / "Nickel/settings.json", "{\"userKept\":true}"); seed_personalization(*bootstrap);
    require(io::read_bounded(root / "Nickel/settings.json", 1024) == "{\"userKept\":true}", "Portable appearance overwrote user settings");
    fs::remove(root / "Nickel/portable-import.json");
    const auto third = start_portable_bootstrap(root, installer, *gate); require(third && installs == 1 && fs::exists(root / "Nickel/portable-import.json"), "Committed identity recovery duplicated portable import");
    write(directory / "config/client.json", "{\"folder\":\"C:/SYNTHETIC/host\"}");
    rejected([&] { export_pack(request, f.root / "host-leak.zip", Json::object(), exporter, *gate); }, "unsafe_content");
    std::cout << "actual portable ZIP/mrpack round-trip/schema1/unknown nested fields/setting projection/background portability/EULA/once-only/recovery: PASS\n";
}
void policy_security() {
    Fixture f;
    int case_number = 0;
    const auto validate = [&](std::string_view extension, std::string_view text, bool expected) {
        ++case_number;
        const auto file = f.root / ("client" + std::string(extension)); write(file, text);
        try {
            if (expected) detail::validate_content_file(file, "config/client" + std::string(extension));
            else rejected([&] { detail::validate_content_file(file, "config/client" + std::string(extension)); }, "unsafe_content");
        } catch (...) { std::cerr << "Policy fixture " << case_number << " " << extension << " expected " << expected << '\n'; throw; }
    };
    validate(".json", "{\"apiKey\":\"${SYNTHETIC_KEY}\",\"password\":null,\"enabled\":true}", true);
    validate(".json", "{\"apiKey\":\"SYNTHETIC_LITERAL\"}", false);
    validate(".json", "{\"api\\u004bey\":{\"nested\":[\"SYNTHETIC_LITERAL\"]}}", false);
    validate(".json", "{\"apiKey\":1,}", false);
    validate(".yaml", "enabled: true\napiKey: ${SYNTHETIC_KEY}\n", true);
    validate(".yaml", "apiKey:\n  nested: SYNTHETIC_LITERAL\n", false);
    validate(".yaml", "apiKey: |\n  SYNTHETIC_LITERAL\n", false);
    validate(".yaml", "key: &anchor text\ncopy: *anchor\n", false);
    validate(".yaml", "key: !include private.yaml\n", false);
    validate(".yaml", "enabled: true\nenabled: false\n", false);
    validate(".toml", "enabled = true\napiKey = '${SYNTHETIC_KEY}'\n", true);
    validate(".toml", "[service.apiKey]\nnested = ['SYNTHETIC_LITERAL']\n", false);
    validate(".toml", "apiKey = [\n", false);
    validate(".properties", "enabled=true\napiKey=${SYNTHETIC_KEY}\npassword=changeme\n", true);
    validate(".properties", "apiKey=SYNTHETIC_LITERAL\n", false);
    validate(".js", "const apiKey = process.env.SYNTHETIC_KEY;\n", true);
    validate(".js", "const config = {apiKey: System.getenv('SYNTHETIC_KEY')};\n", true);
    validate(".js", "const config = {apiKey: 'SYNTHETIC_LITERAL'};\n", false);
    validate(".js", "config['api\\x4bey'] = 'SYNTHETIC_LITERAL';\n", false);
    validate(".js", "const config = {apiKey: {nested: ['SYNTHETIC_LITERAL']}};\n", false);
    validate(".js", "const config = {apiKey: `SYNTHETIC_LITERAL`};\n", false);
    validate(".txt", "url=https://example.test/a?apiKey=SYNTHETIC_LITERAL\n", false);
    validate(".txt", "-----BEGIN PRIVATE KEY-----\nSYNTHETIC_NOT_A_REAL_KEY\n-----END PRIVATE KEY-----\n", false);
    validate(".js", "config.apiKey = (System.getenv('SYNTHETIC_KEY'));", true);
    validate(".js", "const config = {apiKeys: [System.getenv('SYNTHETIC_KEY'), {primary: process.env['SYNTHETIC_OTHER']}]};", true);
    validate(".js", "config.apiKey = System.getenv('SYNTHETIC-INVALID');", false);
    validate(".js", "config.apiKey = process.env['SYNTHETIC-INVALID'];", false);
    validate(".js", "config.apiKey = [null, {}, '', '%SYNTHETIC_KEY%', {primary: '${SYNTHETIC_KEY}'}];", true);
    validate(".js", "const documentation = 'apiKey = not executed'; config['apiKey'] === 'example';", true);
    validate(".js", "config.apiKey = process.env.SYNTHETIC_KEY || 'SYNTHETIC_LITERAL';", false);
    validate(".js", "config.apiKey = process.env.SYNTHETIC_KEY / 'SYNTHETIC_LITERAL';", false);
    validate(".js", "config.apiKey = 'placeholder' + 'SYNTHETIC_LITERAL';", false);
    validate(".properties", "apiKey=${SYNTHETIC_KEY}SYNTHETIC_SUFFIX\n", false);
    validate(".properties", "apiKey=placeholder SYNTHETIC_SUFFIX\n", false);
    validate(".cfg", "apiKeys = [{nested: ['SYNTHETIC_LITERAL']}]\n", false);
    validate(".txt", "url=https://example.test/a?%61piKey=SYNTHETIC_LITERAL\n", false);
    validate(".json", "{\"url\":\"https://example.test/a\\u003fapi\\u004bey=SYNTHETIC_LITERAL\"}", false);
    validate(".yaml", "note: |\n  -----BEGIN PRIVATE KEY-----\n  SYNTHETIC_NOT_A_REAL_KEY\n", false);
    for (const auto op : {"=", "?" "?=", "||=", "&&=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>="}) {
        validate(".js", std::string("config['apiKey'] ") + op + " process.env.SYNTHETIC_KEY || 'SYNTHETIC_LITERAL';", false);
        validate(".js", std::string("config['api\\x4bey'] ") + op + " (('SYNTHETIC_LITERAL'));", false);
    }
    for (const auto& [value, allowed] : std::initializer_list<std::pair<std::string_view, bool>>{
        {"${SYNTHETIC_KEY}", true}, {"%SYNTHETIC_KEY%", true}, {"${_SYNTHETIC2}", true},
        {"${SYNTHETIC_KEY:-SYNTHETIC_LITERAL}", false}, {"${}", false}, {"${1SYNTHETIC}", false},
        {"SYNTHETIC_PREFIX${SYNTHETIC_KEY}", false}, {"%SYNTHETIC_KEY%SYNTHETIC_SUFFIX", false},
        {"%SYNTHETIC-INVALID%", false}, {"process.env.SYNTHETIC_KEY + SYNTHETIC_LITERAL", false}}) {
        const auto literal = Json(value).dump();
        validate(".json", "{\"apiKey\":" + literal + "}", allowed);
        validate(".toml", "apiKey = " + literal, allowed);
        validate(".js", "config.apiKey = (" + literal + ");", allowed);
    }
    require(safe_content_text("enabled=true\n"), "Safe configuration rejected");
    require(!sensitive_content_path("saves/Secret World/level.dat") && sensitive_content_path("saves/World/private.key"), "World names or credential descendants changed");
    require(content_group("shaderpacks/Forest.json")->first == "shaderOptions" && !content_group("config/cache/file.json"), "Content groups incorrect");
    std::cout << "synthetic JSON/YAML/TOML/properties/script placeholders/literals/encoded/nested/block/alias/tag/malformed policies: PASS\n";
}
}
int main(int argc, char** argv) {
    try {
        const std::string group = argc > 1 ? argv[1] : "all";
        if (group == "manifest" || group == "all") manifest_security();
        if (group == "zip" || group == "all") zip_security();
        if (group == "import" || group == "all") import_transaction();
        if (group == "publish" || group == "all") final_publish_contract();
        if (group == "longpath" || group == "all") long_paths();
        if (group == "export" || group == "all") export_roundtrip();
        if (group == "portable" || group == "all") portable_roundtrip();
        if (group == "policy" || group == "all") policy_security();
        return 0;
    } catch (const nickel::ErrorException& error) { std::cerr << error.error().code << ": " << error.what() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
