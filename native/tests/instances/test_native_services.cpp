#include "core/packs/native_services.hpp"
#include "core/packs/native_service_detail.hpp"

#include <algorithm>
#include <iostream>
#include <latch>
#include <thread>

using namespace nickel::packs;
namespace io = nickel::instances::detail;
namespace instances = nickel::instances;
namespace mc = nickel::minecraft;
namespace resources = nickel::resources;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Fn> void rejected(Fn&& operation, std::string_view code = {}) {
    try { operation(); }
    catch (const nickel::ErrorException& error) {
        if (!code.empty()) require(error.error().code == code, "Unexpected safe rejection category");
        return;
    }
    throw std::runtime_error("Expected native contract rejection");
}
struct Fixture {
    fs::path directory = fs::current_path() / ("native-" + io::random_id());
    Fixture() { require(fs::create_directory(directory), "Synthetic fixture identity conflict"); }
    ~Fixture() { std::error_code ignored; fs::remove_all(directory, ignored); }
};
void write(const fs::path& file, std::string_view bytes) { io::atomic_write(file, bytes); }

void download_contract() {
    Fixture fixture;
    const auto bytes = fixture.directory / "synthetic.jar";
    write(bytes, "synthetic resource bytes");
    PackFile file{"mods/synthetic.jar", {"https://example.invalid/synthetic.jar"}, fs::file_size(bytes),
        hash_file(bytes, "sha1"), hash_file(bytes, "sha512")};
    DownloadRequest input{file, fixture.directory / "mods/synthetic.jar"};
    const auto request = detail::native_download_request(input);
    require(request.destination == input.target && request.expected_size == file.size && request.max_bytes == file.size,
            "Native download lost the target or exact bounded size");
    require(request.trust == nickel::transport::TrustPolicy::PublicHttps && request.require_digest
            && !request.reuse_verified_existing && !request.resume, "Native download weakened fresh owned-target policy");
    require(request.digests.size() == 2 && request.candidates.size() == 1 && request.candidates.front().headers.empty(),
            "Native download lost hash verification or added credentials");
    auto invalid = input;
    invalid.file.sha1.reset(); invalid.file.sha512.reset();
    rejected([&] { (void)detail::native_download_request(invalid); }, "integrity");
    invalid = input; invalid.file.downloads = {"http://example.invalid/file"};
    rejected([&] { (void)detail::native_download_request(invalid); });
    invalid = input; invalid.file.downloads = {"https://user:synthetic@example.invalid/file"};
    rejected([&] { (void)detail::native_download_request(invalid); });
    invalid = input; invalid.file.path = "../escape.jar";
    rejected([&] { (void)detail::native_download_request(invalid); }, "unsafe_path");
    invalid = input; invalid.file.sha1 = "not-a-digest";
    rejected([&] { (void)detail::native_download_request(invalid); });
    invalid = input; invalid.file.downloads.resize(17, "https://example.invalid/file");
    rejected([&] { (void)detail::native_download_request(invalid); }, "limit");
    invalid = input; invalid.file.size = max_pack_file_bytes + 1;
    rejected([&] { (void)detail::native_download_request(invalid); }, "limit");
    write(input.target, "synthetic external collision");
    rejected([&] { (void)detail::native_download_request(input); }, "conflict");
    require(io::read_bounded(input.target, 128) == "synthetic external collision", "Native download changed an existing file");
}

void installer_contract() {
    Fixture fixture;
    const auto root = fixture.directory / "game";
    fs::create_directory(root);
    auto transaction = instances::InstanceTransaction::reserve(root, "Pack", "1.20.1", "fabric", "0.16.9", false);
    const InstallerRequest input{root, transaction.directory(), "Pack", "1.20.1", PackLoader{"fabric", "0.16.9"}};
    const mc::Platform platform{"windows", "x64", "synthetic", {}};
    const auto request = detail::native_installer_request(input, platform);
    const auto game = transaction.directory() / "instances/Pack";
    require(request.root == game && request.gameDirectory == game && request.versionId == "1.20.1"
            && request.instanceName == "1.20.1" && request.reservationId == "Pack", "Native installer changed reserved outer identity");
    require(request.loaders.size() == 1 && request.loaders.front().kind == mc::LoaderKind::Fabric
            && request.loaders.front().version == "0.16.9" && !request.loaders.front().artifact,
            "Native installer added pack-supplied executable artifacts");
    require(fs::exists(transaction.directory() / instances::pending_file) && instances::scan_instances(root).empty(),
            "Native installer request exposed a pending instance");
    rejected([&] { (void)instances::load_profile(root, "Pack"); }, "pending_instance");
    auto invalid = input; invalid.instance_directory = fixture.directory;
    rejected([&] { (void)detail::native_installer_request(invalid, platform); }, "identity_mismatch");
    invalid = input; invalid.minecraft_version = "1.21";
    rejected([&] { (void)detail::native_installer_request(invalid, platform); }, "identity_mismatch");
    invalid = input; invalid.loader = PackLoader{"fabric", "0.17.0"};
    rejected([&] { (void)detail::native_installer_request(invalid, platform); }, "identity_mismatch");

    mc::InstalledVersion installed;
    installed.root = game; installed.gameDirectory = game;
    installed.versionId = "1.20.1"; installed.minecraftVersion = "1.20.1";
    installed.profilePath = game / "versions/1.20.1/1.20.1.json";
    installed.profile = Json{{"id", "1.20.1"}, {"type", "release"}, {"mainClass", "synthetic.Main"}};
    installed.loaders = {{mc::LoaderKind::Fabric, "0.16.9"}};
    write(installed.profilePath, installed.profile.dump());
    const auto identity = detail::native_install_identity(input, installed);
    require(identity.minecraft_version == "1.20.1" && identity.loader == "fabric" && identity.loader_version == "0.16.9"
            && identity.descriptor == installed.profile, "Native installer lost the verified persisted identity");
    auto wrong = installed; wrong.root = root;
    rejected([&] { (void)detail::native_install_identity(input, wrong); }, "identity_mismatch");
    wrong = installed; wrong.profilePath = transaction.directory() / "Pack.json";
    rejected([&] { (void)detail::native_install_identity(input, wrong); }, "identity_mismatch");
    wrong = installed; wrong.profile["id"] = "other";
    rejected([&] { (void)detail::native_install_identity(input, wrong); }, "identity_mismatch");
    wrong = installed; wrong.loaders.front().kind = mc::LoaderKind::Quilt;
    rejected([&] { (void)detail::native_install_identity(input, wrong); }, "identity_mismatch");
    wrong = installed; wrong.loaders.push_back({mc::LoaderKind::Forge, "47.0.0"});
    rejected([&] { (void)detail::native_install_identity(input, wrong); }, "identity_mismatch");
    std::stop_source stop; stop.request_stop();
    rejected([&] { (void)detail::native_install_identity(input, installed, stop.get_token()); }, "cancelled");
    transaction.check_owned();
    transaction.cleanup();
    require(!fs::exists(input.instance_directory), "Native contract test left an owned pending instance");
}

void resource_contract() {
    Fixture fixture;
    const auto snapshot = fixture.directory / "resource.jar";
    write(snapshot, "synthetic public resource");
    const auto sha1 = hash_file(snapshot, "sha1"), sha512 = hash_file(snapshot, "sha512");
    resources::ResolvedResourceFile resolved;
    resolved.source = resources::Source::Modrinth;
    resolved.projectId = "synthetic"; resolved.versionId = "synthetic-version";
    resolved.file = {"https://example.invalid/resource.jar", "resource.jar", fs::file_size(snapshot), true, sha1, sha512};
    const auto file = detail::native_resource_file(snapshot, "mods/resource.jar", sha1, sha512, resolved, true);
    require(file.path == "mods/resource.jar" && file.sha1 == sha1 && file.sha512 == sha512 && file.size == fs::file_size(snapshot),
            "Native resource mapping lost the selected target or local digests");
    auto invalid = resolved; invalid.source = resources::Source::CurseForge;
    rejected([&] { (void)detail::native_resource_file(snapshot, "mods/resource.jar", sha1, sha512, invalid, true); }, "integrity");
    invalid = resolved; invalid.file.sha1.assign(40, '0');
    rejected([&] { (void)detail::native_resource_file(snapshot, "mods/resource.jar", sha1, sha512, invalid, true); }, "integrity");
    invalid = resolved; ++invalid.file.sizeBytes;
    rejected([&] { (void)detail::native_resource_file(snapshot, "mods/resource.jar", sha1, sha512, invalid, true); }, "integrity");
    invalid = resolved; invalid.file.url = "https://example.invalid/file?apiKey=synthetic";
    rejected([&] { (void)detail::native_resource_file(snapshot, "mods/resource.jar", sha1, sha512, invalid, true); });
    rejected([&] { (void)detail::native_resource_file(snapshot, "versions/escape.jar", sha1, sha512, resolved, true); }, "protected_path");
    std::stop_source stop; stop.request_stop();
    rejected([&] { (void)detail::native_resource_file(snapshot, "mods/resource.jar", sha1, sha512, resolved, true, stop.get_token()); }, "cancelled");
    write(snapshot, "different public resource");
    require(fs::file_size(snapshot) == resolved.file.sizeBytes, "Synthetic source-change fixture must preserve size");
    rejected([&] { (void)detail::native_resource_file(snapshot, "mods/resource.jar", sha1, sha512, resolved, true); }, "integrity");
}

void cancellation_contract() {
    std::stop_source stop;
    detail::NativeCancellation bridge(stop.get_token());
    require(!bridge.get().cancelled(), "Native cancellation began cancelled");
    stop.request_stop();
    require(bridge.get().cancelled(), "Native stop token did not cancel the canonical source");
    detail::NativeCancellation already(stop.get_token());
    require(already.get().cancelled(), "Already requested native cancellation was lost");
    rejected([&] { bridge.get().check(); }, "cancelled");
    try {
        detail::native_service_call([] { throw nickel::ErrorException(nickel::Error{"network", "synthetic private detail", Json{{"path", "synthetic private path"}}}); },
                                    "download", "Safe native download failure");
    } catch (const nickel::ErrorException& error) {
        require(error.error().code == "download" && error.error().message == "Safe native download failure"
                && error.error().details.empty(), "Native service failure leaked provider details");
        return;
    }
    throw std::runtime_error("Expected redacted service failure");
}

void final_publish_transfer_contract() {
    InstallServices service;
    int gate_calls = 0, commit_calls = 0;
    const std::function<void()> commit = [&] { ++commit_calls; };
    std::weak_ptr<int> lifetime;
    {
        NativeInstallOptions options{.platform = mc::Platform{"windows", "x64", "synthetic", {}}};
        auto owned = std::make_shared<int>(42);
        lifetime = owned;
        options.final_publish = [&, owned](const std::function<void()>& publication) {
            require(*owned == 42 && &publication == &commit, "Native final publication changed its closure or ownership");
            ++gate_calls;
            publication();
        };
        detail::transfer_final_publish(service, options);
        require(service.final_publish && !options.final_publish && gate_calls == 0 && commit_calls == 0,
                "Native options transfer invoked or retained the final publication callback");
    }
    require(!lifetime.expired(), "Native service lost its final publication callback after options destruction");
    service.final_publish(commit);
    require(gate_calls == 1 && commit_calls == 1, "Native service did not forward the same commit exactly once");
    NativeInstallOptions direct{.platform = mc::Platform{"windows", "x64", "synthetic", {}}};
    detail::transfer_final_publish(service, direct);
    require(!service.final_publish && lifetime.expired(), "Empty native options retained a stale task publication gate");
}

void drain_contract() {
    std::timed_mutex mutex;
    std::latch ready(1), release(1);
    std::jthread active([&] {
        std::lock_guard lock(mutex);
        ready.count_down();
        release.wait();
    });
    ready.wait();
    bool invoked = false;
    const auto started = std::chrono::steady_clock::now();
    const auto drained = detail::native_owned_drain(mutex, std::chrono::milliseconds(20), [&](auto) {
        invoked = true;
        return true;
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    release.count_down();
    active.join();
    require(!drained && !invoked, "Timed-out installation lock invoked the owned-child drain");
    require(elapsed < std::chrono::seconds(5), "Owned drain ignored its lock-wait timeout");
    require(detail::native_owned_drain(mutex, std::chrono::seconds(1), [&](std::chrono::milliseconds remaining) {
        require(remaining > std::chrono::milliseconds::zero() && remaining <= std::chrono::seconds(1),
                "Owned-child drain received an invalid remaining budget");
        return true;
    }), "Available owned-child drain did not succeed");
    invoked = false;
    require(!detail::native_owned_drain(mutex, std::chrono::milliseconds::zero(), [&](auto) {
        invoked = true;
        return true;
    }) && !invoked, "Zero-budget drain executed child work");
    require(!detail::native_owned_drain(mutex, std::chrono::seconds(1), [](auto) { return false; }),
            "Incomplete child drain was reported as successful");
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view group = argc > 1 ? argv[1] : "all";
        if (group == "download" || group == "all") download_contract();
        if (group == "installer" || group == "all") installer_contract();
        if (group == "resource" || group == "all") resource_contract();
        if (group == "cancellation" || group == "all") {
            cancellation_contract();
            final_publish_transfer_contract();
        }
        if (group == "drain" || group == "all") drain_contract();
        std::cout << "Native service contracts passed: " << group << '\n';
        return 0;
    } catch (const nickel::ErrorException& error) {
        std::cerr << error.error().code << ": " << error.error().message << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
