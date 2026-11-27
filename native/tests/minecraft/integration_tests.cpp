#include "test_support.hpp"
#include "services_native.hpp"
#include "core/archive/archive.hpp"
#include "core/network/hash.hpp"
#include "core/tasks/tasks.hpp"
#include "core/tasks/secure_fs.hpp"
#include "platform/process.hpp"
#include "platform/services.hpp"
#include <cstdlib>
#include <array>
#include <span>

using namespace test;
namespace net = nickel::transport;
namespace {
std::string digest(std::string_view bytes, net::HashAlgorithm algorithm) {
    net::detail::IncrementalHash hash(algorithm);
    hash.update({reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
    return hash.finish();
}
FileHashes hashes(std::string_view bytes) { return {digest(bytes, net::HashAlgorithm::Sha1), digest(bytes, net::HashAlgorithm::Sha256), digest(bytes, net::HashAlgorithm::Md5)}; }
Services make(const std::string& base, const Cancellation& cancel = {}) {
    (void)base;
    NativeServiceOptions options; options.trust = net::TrustPolicy::ControlledLoopback; options.useMirrors = false;
    net::NetworkSnapshot snapshot; snapshot.version_list_source = net::SourcePolicy::OfficialFirst; snapshot.file_download_source = net::SourcePolicy::OfficialFirst;
    snapshot.total_timeout = std::chrono::seconds(10);
    auto factory = make_native_services(std::make_shared<net::HttpClient>(snapshot), options);
    auto services = factory.forOperation(cancel);
    const auto fetch = services.fetch;
    services.fetch = [fetch](std::string_view url, std::uint64_t maximum, const CancelCheck& check) {
        try { return fetch(url, maximum, check); }
        catch (...) { std::cerr << "Controlled fetch failed: limit=" << maximum << '\n'; throw; }
    };
    const auto download = services.download;
    services.download = [download](const DownloadFile& file, const CancelCheck& check, const ProgressCallback& progress) {
        try { download(file, check, progress); }
        catch (...) {
            std::cerr << "Controlled download failed: " << path_utf8(file.destination.filename())
                      << " size=" << file.size.value_or(0) << " limit=" << file.maximumBytes << '\n';
            throw;
        }
    };
    return services;
}
void transport_and_archives(const std::string& base) {
    Tree tree("native-network"); Cancellation cancel; auto services = make(base, cancel); auto check = cancellation_check(services, cancel);
    require(services.fetch(base + "/hello.txt", 128, check) == "controlled-body", "Real HttpClient metadata fetch");
    rejects([&] { services.fetch(base + "/hello.txt", 2, check); });
    auto file = artifact_download(Json{{"url", base + "/hello.txt"}}, tree.root / "file.txt", FileKind::Library);
    file.hashes = hashes("controlled-body"); file.size = 15;
    ensure_file(services, file, check);
    require(services.verify(file.destination, file.hashes, file.size, check), "Real SHA1/SHA256/MD5 verification");
    auto wrong = file; wrong.destination = tree.root / "wrong.txt"; wrong.hashes.sha256 = std::string(64, '0');
    put(wrong.destination, "preserve-existing");
    rejects([&] { ensure_file(services, wrong, check); });
    require(get(wrong.destination) == "preserve-existing", "Hash failure cannot replace destination");
    for (const auto name : {"invalid.jar", "traversal.zip", "duplicate.zip"}) {
        auto bad = artifact_download(Json{{"url", base + '/' + name}}, tree.root / name, FileKind::Library);
        rejects([&] { ensure_file(services, bad, check); });
        require(!fs::exists(bad.destination), "Invalid archive cannot commit");
    }
    const auto inventory = parse_metadata(services.fetch(base + "/inventory.json", 1024 * 1024, check));
    auto native = artifact_download(inventory.at("/native.jar"), tree.root / "native.jar", FileKind::Native);
    ensure_file(services, native, check);
    NativeArchive extraction{native.destination, tree.root / "natives"};
    services.extractNatives(extraction, check); services.extractNatives(extraction, check);
    require(get(extraction.destination / "lwjgl.dll") == "controlled-native" && !fs::exists(extraction.destination / "META-INF"), "Actual native extraction, excludes and repeat verification");
    put(extraction.destination / "lwjgl.dll", "foreign-edit");
    rejects([&] { services.extractNatives(extraction, check); }, "minecraft.native.conflict");
    auto runtime = artifact_download(inventory.at("/runtime.zip"), tree.root / "runtime.zip", FileKind::Java);
    ensure_file(services, runtime, check);
    const auto destination = tree.root / "java", manifestPath = tree.root / "java.nickel-runtime-files.json";
    services.extractRuntime(runtime.destination, destination, 1, check);
    require(fs::is_regular_file(destination / "bin/java.exe"), "Actual runtime archive root stripping");
    auto manifest = parse_metadata(get(manifestPath));
    require(manifest.contains("bin/java.exe") && !manifest.contains("bin\\java.exe"), "Runtime ownership manifests use portable relative keys");
    services.extractRuntime(runtime.destination, destination, 1, check);
#ifdef _WIN32
    auto legacy = manifest; legacy["bin\\java.exe"] = legacy.at("bin/java.exe"); legacy.erase("bin/java.exe");
    services.atomicWrite(manifestPath, legacy.dump(), check);
    services.extractRuntime(runtime.destination, destination, 1, check);
    require(parse_metadata(get(manifestPath)) == manifest, "Hash-verified legacy native-separator ownership migrates to portable keys");
    auto duplicate = manifest; duplicate["bin\\java.exe"] = duplicate.at("bin/java.exe");
    services.atomicWrite(manifestPath, duplicate.dump(), check);
    rejects([&] { services.extractRuntime(runtime.destination, destination, 1, check); }, "minecraft.runtime.path");
    require(parse_metadata(get(manifestPath)) == duplicate, "Conflicting ownership cannot mutate the runtime manifest");
    services.atomicWrite(manifestPath, manifest.dump(), check);
#endif
    auto escaped = manifest; escaped["../unowned.txt"] = hashes("user-data").sha256.value();
    services.atomicWrite(manifestPath, escaped.dump(), check);
    rejects([&] { services.extractRuntime(runtime.destination, destination, 1, check); }, "minecraft.runtime.path");
    services.atomicWrite(manifestPath, manifest.dump(), check);
    put(destination / "unowned.txt", "user-data");
    services.extractRuntime(runtime.destination, destination, 1, check);
    require(get(destination / "unowned.txt") == "user-data", "Runtime overlay cannot overwrite unrelated files");
    put(destination / "bin/java.exe", "foreign-java-edit");
    rejects([&] { services.extractRuntime(runtime.destination, destination, 1, check); }, "minecraft.runtime.conflict");
    require(get(destination / "bin/java.exe") == "foreign-java-edit", "Owned runtime externally modified file is preserved");
}
void owned_processes(const std::string& base, const fs::path& executable) {
    auto services = make(base);
    ProcessSpec spec; spec.executable = executable; spec.workingDirectory = executable.parent_path(); spec.arguments = {"--flood", "yes", "--sleep", "20"};
    std::mutex mutex; std::array<std::string, 2> outputs;
    auto child = services.spawn(spec, [&](bool error, std::string_view bytes) { std::lock_guard lock(mutex); outputs[error ? 1 : 0].append(bytes); });
    const auto preparationOwner = child->native_process();
    require(preparationOwner && preparationOwner == child->native_process() && preparationOwner->pid() == child->pid() &&
            preparationOwner->purpose() == nickel::ProcessPurpose::Preparation,
            "Native adapter borrows the exact original owned preparation process, not a PID proxy");
    const auto code = child->wait({}, false, std::chrono::seconds(10));
    require(code == 0 && child->drain_owned(std::chrono::milliseconds(0)) && outputs[0].ends_with("stdout-tail") && outputs[1].ends_with("stderr-tail"), "Owned process wait includes complete dual EOF/drain");
    require(child->native_process() == preparationOwner, "Borrowed native owner remains the same object through root exit and dual EOF");
    require(outputs[0].size() > 512 * 1024 && outputs[1].size() > 512 * 1024, "Both pipes drain beyond capture limit without deadlock");
    Cancellation cancel; spec.arguments = {"--sleep", "10000"};
    child = services.spawn(spec, {});
    auto cancelled = std::async(std::launch::async, [&] { rejects([&] { child->wait(cancellation_check(services, cancel), true, std::chrono::seconds(15)); }, "cancelled"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(80)); cancel.cancel();
    require(cancelled.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "Owned preparation cancel returns promptly"); cancelled.get();
    require(!child->running() && child->drain_owned(std::chrono::seconds(2)), "Preparation cancellation really terminates and drains the exact child");
    spec.purpose = ProcessKind::Game; spec.arguments = {"--sleep", "150"}; child = services.spawn(spec, {});
    const auto gameOwner = child->native_process();
    require(gameOwner && gameOwner == child->native_process() && gameOwner->pid() == child->pid() &&
            gameOwner->purpose() == nickel::ProcessPurpose::Game,
            "Native Game adapter exposes the original foundation owner without creating another process");
    // A live owned Game with no created window must answer None without borrowing
    // another process; the ownership check is only bypassed for a genuinely live root.
    require(child->running() && !nickel::find_game_window(*gameOwner),
            "A live owned Game without a window searched another process or fabricated a window");
    require(child->wait({}, false, std::chrono::seconds(3)) == 0, "Game process can complete naturally");
    require(child->native_process() == gameOwner, "Borrowed native owner changed across root exit and drain");
    rejects([&] { (void)nickel::find_game_window(*gameOwner); }, "PermissionDenied");
    spec.arguments = {"--sleep", "10000"}; child = services.spawn(spec, {});
    rejects([&] { child->wait(cancellation_check(services, cancel), true, std::chrono::seconds(1)); }, "cancelled");
    require(child->running(), "Preparation cancellation cannot terminate a game-purpose child");
    child->terminate_owned();
    (void)child->wait({}, false, std::chrono::seconds(3));
    require(!child->running() && child->drain_owned(std::chrono::seconds(2)), "Explicit game stop uses the exact owned-game API and drains both streams");
#ifdef _WIN32
    Tree tree("raw-shell");
    const auto systemRoot = std::getenv("SystemRoot"); require(systemRoot != nullptr, "Windows system shell is available");
    spec = {}; spec.executable = fs::u8path(systemRoot) / "System32/cmd.exe"; spec.workingDirectory = tree.root;
    const auto output = tree.root / "quoted path & symbol.txt";
    spec.windowsShellCommand = "echo controlled-shell>\"" + path_utf8(output) + "\"";
    child = services.spawn(spec, {});
    require(child->wait({}, true, std::chrono::seconds(3)) == 0 && get(output).find("controlled-shell") != std::string::npos,
            "Raw Windows shell syntax preserves space/ampersand output path quoting");
    spec.windowsShellCommand = "\"" + path_utf8(executable) + "\" --sleep 10000";
    child = services.spawn(spec, {});
    rejects([&] { child->wait({}, true, std::chrono::milliseconds(80)); }, "minecraft.process.timeout");
    require(!child->running() && child->drain_owned(std::chrono::seconds(2)), "Shell timeout terminates only the owned preparation tree and drains");
#endif
}
void network_cancellation(const std::string& base) {
    Tree tree("native-cancel"); Cancellation cancel; auto services = make(base, cancel);
    auto file = artifact_download(Json{{"url", base + "/slow.bin"}, {"size", 4 * 1024 * 1024}}, tree.root / "slow.bin", FileKind::Library);
    auto download = std::async(std::launch::async, [&] { rejects([&] { ensure_file(services, file, cancellation_check(services, cancel)); }, "cancelled"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(120)); cancel.cancel();
    require(download.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "Real transport cancels mid-download promptly"); download.get();
    require(!fs::exists(file.destination), "Cancelled network transfer cannot publish destination");
    auto registry = std::make_shared<nickel::tasks::Registry>();
    nickel::tasks::TaskSpec spec; spec.target = tree.root / "task-target";
    auto task = registry->begin(spec);
    NativeServiceOptions options; options.task = task; options.trust = net::TrustPolicy::ControlledLoopback; options.useMirrors = false;
    auto factory = make_native_services(std::make_shared<net::HttpClient>(), options);
    Cancellation request; auto taskServices = factory.forOperation(request);
    task->cancel();
    rejects([&] { cancellation_check(taskServices, request)(); }, "cancelled");
}
void installation_and_launch(const std::string& base, const fs::path& executable) {
    Tree tree("native-install"); auto services = make(base); Cancellation cancel; auto check = cancellation_check(services, cancel);
    InstallRequest request; request.root = tree.root / "managed"; request.gameDirectory = tree.root / "game"; request.versionId = "1.20.1"; request.instanceName = "controlled-pack";
    request.profile = parse_metadata(services.fetch(base + "/profile.json", 1024 * 1024, check));
    request.manualJava = executable; request.installJava = false; request.resourcesBase = base + "/assets"; request.platform = {"windows", "amd64", "10.0.19045", {}};
    put(request.gameDirectory / ".nickel-pack.pending", "owned by instances");
    const auto installed = Installer(services).install(request, cancel);
    require(installed.versionId == "controlled-pack" && fs::is_regular_file(installed.profilePath), "Actual HttpClient/hash/archive/process install completes");
    require(get(request.gameDirectory / "resources/sounds/test.ogg") == "controlled-asset" && fs::is_regular_file(request.root / "versions/controlled-pack/natives/lwjgl.dll"), "Actual assets/library/natives materialization");
    require(fs::exists(request.gameDirectory / ".nickel-pack.pending"), "Minecraft does not clear instances pending state");
    // Only a controlled fixture owner publishes this test instance.
    fs::remove(request.gameDirectory / ".nickel-pack.pending");
    LaunchRequest launch; launch.launchId = "controlled-owned-launch"; launch.version = installed;
    launch.java = java(executable); launch.identity = {"offline", "Controlled Player", "01234567890123456789012345678901", "CONTROLLED_SECRET"};
    launch.assetIndex = parse_metadata(services.fetch(base + "/index.json", 1024 * 1024, check));
    launch.platform = request.platform; launch.settings.gameArguments = {"--sleep", "250"};
    Launcher launcher(services); std::mutex mutex; std::vector<LaunchEvent> events;
    Cancellation preparation;
    auto session = launcher.launch(launch, preparation, [&](const LaunchEvent& event) { std::lock_guard lock(mutex); events.push_back(event); });
    const auto nativeOwner = session->native_process();
    require(nativeOwner && nativeOwner == session->native_process() && nativeOwner->pid() == session->pid() &&
            nativeOwner->purpose() == nickel::ProcessPurpose::Game && launcher.session(launch.launchId) == session,
            "GameSession forwards one stable native owner while retaining the original shared process lifetime");
    preparation.cancel(); require(!launcher.cancel_prepare(launch.launchId) && session->running(), "Actual owned game survives stale preparation cancellation");
    require(session->running() && !nickel::find_game_window(*nativeOwner),
            "A live GameSession without a window searched another process or fabricated a window");
    require(session->wait() == 0, "Actual mock Java launch completes with full argv");
    require(session->native_process() == nativeOwner, "Borrowed GameSession owner changed across root exit and drain");
    rejects([&] { (void)nickel::find_game_window(*nativeOwner); }, "PermissionDenied");
    std::lock_guard lock(mutex); std::string publicOutput;
    for (const auto& event : events) publicOutput += event.message;
    require(events.back().kind == "exited" && publicOutput.find("CONTROLLED_SECRET") == std::string::npos, "Dual EOF exit ordering and split-secret redaction on real pipes");
}
void runtime_install(const std::string& base) {
    Tree tree("native-runtime"); auto services = make(base); Cancellation cancel; auto check = cancellation_check(services, cancel);
    auto request = install_request(tree);
    request.profile = parse_metadata(services.fetch(base + "/profile.json", 1024 * 1024, check));
    request.javaCandidates.clear(); request.javaManifestUrl = base + "/all.json"; request.resourcesBase = base + "/assets"; request.installJava = true;
    services.discoverJava = [](const CancelCheck& check) { check(); return std::vector<JavaCandidate>{}; };
    // Discovery alone is controlled to force download; every I/O and probe still uses the real adapters.
    auto installed = Installer(services).install(request, cancel);
    require(installed.java.source == "downloaded" && installed.java.major == 17 && fs::is_regular_file(*installed.java.path), "Mojang-shaped runtime download and actual owned executable probe");
}
struct MockMajor {
    std::optional<std::string> previous;
    explicit MockMajor(unsigned major) {
        if (const auto value = std::getenv("NICKEL_TEST_JAVA_MAJOR")) previous = value;
        set(std::to_string(major));
    }
    ~MockMajor() { set(previous.value_or("")); }
    static void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s("NICKEL_TEST_JAVA_MAJOR", value.c_str());
#else
        if (value.empty()) unsetenv("NICKEL_TEST_JAVA_MAJOR");
        else setenv("NICKEL_TEST_JAVA_MAJOR", value.c_str(), 1);
#endif
    }
};
LoaderRequest pinned(const Services& services, const Json& inventory, const std::string& base,
                     LoaderKind kind, std::string version, std::string name, const CancelCheck& check) {
    LoaderArtifact artifact;
    if (name.ends_with(".json")) artifact.profile = parse_metadata(services.fetch(base + '/' + name, 1024 * 1024, check));
    else {
        const auto file = artifact_download(inventory.at('/' + name), fs::u8path(name), FileKind::Loader);
        artifact.url = file.url; artifact.filename = name; artifact.hashes = file.hashes; artifact.size = file.size;
    }
    return {kind, std::move(version), std::move(artifact)};
}
void loader_matrix(const std::string& base, const fs::path& executable) {
    auto services = make(base); Cancellation cancel; auto check = cancellation_check(services, cancel);
    const auto inventory = parse_metadata(services.fetch(base + "/inventory.json", 1024 * 1024, check));
    for (const auto scenario : {"fabric", "quilt", "forge", "neoforge", "optifine", "liteloader", "forge-optifine", "fabric-optifine", "forge-lite-optifine"}) {
        Tree tree(scenario);
        const std::string selected(scenario);
        const bool old = selected == "liteloader" || selected == "forge-lite-optifine";
        MockMajor major(old ? 8 : 17);
        auto request = install_request(tree); request.versionId = old ? "1.12.2" : "1.20.1";
        request.instanceName = "controlled-" + selected; request.manualJava = executable;
        request.profile = parse_metadata(services.fetch(base + (old ? "/old-profile.json" : "/profile.json"), 1024 * 1024, check));
        request.resourcesBase = base + "/assets";
        const auto add = [&](LoaderKind kind, std::string version, std::string name) {
            request.loaders.push_back(pinned(services, inventory, base, kind, std::move(version), std::move(name), check));
        };
        if (selected.starts_with("fabric")) add(LoaderKind::Fabric, "0.15.1", "fabric-profile.json");
        if (selected == "quilt") add(LoaderKind::Quilt, "0.24.0", "quilt-profile.json");
        if (selected.starts_with("forge")) add(LoaderKind::Forge, old ? "14.23.5.2860" : "47.1.0", request.versionId + "-forge-installer.jar");
        if (selected == "neoforge") add(LoaderKind::NeoForge, "1.20.1-47.1.0", "1.20.1-neoforge-installer.jar");
        if (selected == "liteloader" || selected == "forge-lite-optifine") add(LoaderKind::LiteLoader, "1.12.2-SNAPSHOT-r0", "liteloader-profile.json");
        if (selected.find("optifine") != std::string::npos) {
            add(LoaderKind::OptiFine, old ? "1.12.2_HD_U_G5" : "1.20.1_HD_U_I6", "OptiFine_" + request.versionId + "_HD_U_" + (old ? "G5" : "I6") + ".jar");
            if (selected.starts_with("forge")) request.loaders.back().artifact->forgeRequirement = old ? "Forge 14.23.5.2860" : "Forge 47.1.0";
        }
        if (selected == "fabric-optifine") {
            const std::array<std::pair<std::string, std::string>, 2> mods{{{"Fabric API", "fabric-api.jar"}, {"OptiFabric", "optifabric.jar"}}};
            for (const auto& [name, artifact] : mods) {
                auto file = artifact_download(inventory.at('/' + artifact), request.gameDirectory / "mods" / artifact, FileKind::Loader);
                request.dependencyMods.push_back({name, file, {request.versionId}, {LoaderKind::Fabric}});
            }
        }
        put(request.gameDirectory / ".nickel-pack.pending", "fixture owner");
        Installer installer(services);
        const auto installed = installer.install(request, cancel);
        require(installed.versionId == request.instanceName && installed.loaders.size() == request.loaders.size(), "All requested loader identities survive native installation");
        require(get(request.gameDirectory / ".nickel-pack.pending") == "fixture owner", "Loader installation preserves reservation ownership");
        require(installer.owned_installer_pids().empty() && installer.drain_owned_installer(std::chrono::seconds(2)), "Every native loader processor is joined and drained");
        fs::remove(request.gameDirectory / ".nickel-pack.pending");
        LaunchRequest launch; launch.launchId = request.instanceName; launch.version = installed; launch.platform = request.platform;
        launch.identity = {"offline", "Controlled Player", "01234567890123456789012345678901", "CONTROLLED_SECRET"};
        launch.settings.disableWrapper = true; launch.settings.disableLwjglAgent = true;
        Launcher launcher(services);
        const auto plan = launcher.prepare(launch, cancel);
        require(!plan.mainClass.empty() && std::find(plan.process.arguments.begin(), plan.process.arguments.end(), "-jar") == plan.process.arguments.end(), "Each loader uses its metadata main class and complete classpath");
        auto session = launcher.launch(launch, cancel);
        require(session->wait() == 0, "Actual owned mock child executes each loader launch plan");
        std::cout << "PASS controlled loader " << selected << " (not a genuine loader/game run)\n";
    }
}
void processor_matrix(const std::string& base, const fs::path& executable) {
    auto services = make(base); Cancellation cancel; auto check = cancellation_check(services, cancel);
    const auto inventory = parse_metadata(services.fetch(base + "/inventory.json", 1024 * 1024, check));
    for (const auto scenario : {"bad-hash", "nonzero", "slow", "mapping"}) {
        Tree tree(scenario); auto request = install_request(tree); request.manualJava = executable; request.resourcesBase = base + "/assets";
        request.profile = parse_metadata(services.fetch(base + "/profile.json", 1024 * 1024, check));
        request.loaders = {pinned(services, inventory, base, LoaderKind::Forge, "47.1.0", "1.20.1-forge-" + std::string(scenario) + ".jar", check)};
        put(request.gameDirectory / ".nickel-pack.pending", "fixture owner");
        Installer installer(services);
        auto plan = installer.plan(request, cancel);
        if (std::string_view(scenario) == "slow") {
            require(plan.processors.size() == 1, "Only the client processor is scheduled");
            plan.processors.front().timeout = std::chrono::milliseconds(80);
            rejects([&] { installer.execute(plan, cancel); }, "minecraft.process.timeout");
        } else if (std::string_view(scenario) == "bad-hash") rejects([&] { installer.execute(plan, cancel); }, "minecraft.processor.integrity");
        else if (std::string_view(scenario) == "nonzero") rejects([&] { installer.execute(plan, cancel); }, "minecraft.processor.exit");
        else require(installer.execute(plan, cancel).versionId == request.instanceName, "Managed Mojang mapping replacement uses native HTTP/hash rather than processor networking");
        require(installer.owned_installer_pids().empty() && installer.drain_owned_installer(std::chrono::seconds(2)), "Failed or successful processor leaves no undrained owned child");
        require(fs::exists(request.gameDirectory / ".nickel-pack.pending"), "Processor outcomes do not publish the reserved instance");
        if (std::string_view(scenario) != "mapping") require(!fs::exists(plan.profilePath), "Rejected processor cannot publish the final profile");
    }
}
void secure_paths(const std::string& base) {
    namespace sd = nickel::tasks::detail;
    Tree tree("secure-deep");
    Cancellation cancel; auto services = make(base); auto check = cancellation_check(services, cancel);
    const auto nested = tree.root / std::string(80, 'a') / std::string(80, 'b') / std::string(80, 'c');
    unsigned failures = 0;
    const auto attempt = [&](std::string_view phase, const auto& action) {
        try { action(); std::cout << "PASS deep path " << phase << '\n' << std::flush; }
        catch (const ErrorException& error) {
            ++failures; std::cerr << "FAIL deep path " << phase << ": " << error.error().code << ": " << error.what() << '\n';
        }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL deep path " << phase << ": " << error.what() << '\n'; }
    };
    const auto verify_content = [&](const fs::path& path, std::string_view expected) {
        const auto native = filesystem_path(path);
        const auto bytes = get(path);
        const auto pinned = sd::read_bounded(path, 32);
        std::cout << "deep-read logical=" << path.native().size() << " native=" << native.native().size()
                  << " file=" << fs::file_size(native) << " stream=" << bytes.size() << " pinned=" << pinned.size()
                  << " streamSha256=" << digest(bytes, net::HashAlgorithm::Sha256)
                  << " expectedSha256=" << digest(expected, net::HashAlgorithm::Sha256) << '\n' << std::flush;
        require(bytes == expected && pinned.size() == expected.size(), "Production deep-path published content differs from expected bytes");
    };
    attempt("secure-fs-create-atomic-rename-read", [&] {
        sd::ParentGuard parents(nested, true);
        const auto part = nested / "artifact.part", destination = nested / "artifact.jar";
        auto file = sd::SecureFile::create_new(part);
        file.write(std::as_bytes(std::span("controlled", 10))); file.flush(); const auto identity = file.identity(); file.close();
        sd::write_atomic(nested / "journal.json", "{}");
        sd::rename_owned_no_replace(part, destination, identity);
        require(sd::read_bounded(destination, 32).size() == 10, "Secure filesystem publishes and reads deep paths");
    });
    attempt("secure-fs-mixed-separator", [&] {
        const auto directory = nested / fs::path("mixed/relative");
        sd::ParentGuard parents(directory, true);
        auto file = sd::SecureFile::create_new(directory / "mixed.txt");
        file.write(std::as_bytes(std::span("mixed", 5))); file.flush(); file.close();
        require(sd::read_bounded(directory / "mixed.txt", 32).size() == 5, "Secure filesystem normalizes mixed separators before native calls");
    });
    attempt("foundation-atomicWrite", [&] {
        services.atomicWrite(nested / "profile.json", "{}", check);
        verify_content(nested / "profile.json", "{}");
    });
    attempt("transport-staging-download", [&] {
        auto file = artifact_download(Json{{"url", base + "/hello.txt"}}, nested / "artifact-http.txt", FileKind::Metadata);
        file.hashes = hashes("controlled-body"); file.size = 15;
        ensure_file(services, file, check);
        verify_content(file.destination, "controlled-body");
    });
    require(failures == 0, "One or more production deep-path APIs failed");
}
void inspect_cached_jar(const fs::path& source, std::string_view expectedSha1) {
    Cancellation cancel;
    NativeServiceOptions options; options.useMirrors = false;
    auto factory = make_native_services(std::make_shared<net::HttpClient>(), options);
    auto services = factory.forOperation(cancel); auto check = cancellation_check(services, cancel);
    const auto descriptor = artifact_download(Json{{"url", ""}, {"sha1", expectedSha1}}, source, FileKind::Library);
    require(services.verify(source, descriptor.hashes, {}, check), "Cached real JAR matches the supplied official SHA1");
    const auto entries = services.archiveEntries(source, check);
    const auto manifest = services.archiveRead(source, "META-INF/MANIFEST.MF", 64 * 1024, check);
    require(!entries.empty() && !manifest.empty(), "Real JAR passes native whole-archive CRC and manifest validation");
    std::cout << "PASS cached real JAR: bytes=" << fs::file_size(source) << " entries=" << entries.size()
              << " sha1=" << expectedSha1 << " (offline inspect only, not installation/launch)\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "--inspect-jar") {
            require(argc == 4, "Expected --inspect-jar, exact cached JAR path and official SHA1");
            inspect_cached_jar(fs::absolute(fs::u8path(argv[2])), argv[3]);
            return 0;
        }
        require(argc == 3 || argc == 4, "Expected controlled server URL, mock executable and optional case");
        const std::string base(argv[1]); const auto executable = fs::absolute(fs::u8path(argv[2]));
        const std::string selected = argc == 4 ? argv[3] : "all";
        if (selected == "all" || selected == "processes") { owned_processes(base, executable); std::cout << "PASS actual owned process pipes/cancellation/explicit-game-stop/raw-shell\n"; }
        if (selected == "all" || selected == "network") { network_cancellation(base); std::cout << "PASS actual HttpClient request/task cancellation\n"; }
        if (selected == "all" || selected == "archives") { transport_and_archives(base); std::cout << "PASS actual HTTP/digests/archive/runtime ownership\n"; }
        if (selected == "all" || selected == "installation") { installation_and_launch(base, executable); std::cout << "PASS actual install/launch adapters with mock Java\n"; }
        if (selected == "all" || selected == "runtime") { runtime_install(base); std::cout << "PASS controlled runtime download with actual executable probe\n"; }
        if (selected == "all" || selected == "loaders") loader_matrix(base, executable);
        if (selected == "all" || selected == "processors") processor_matrix(base, executable);
        if (selected == "all" || selected == "paths") secure_paths(base);
        require(selected == "all" || selected == "processes" || selected == "network" || selected == "archives" || selected == "installation" || selected == "runtime" || selected == "loaders" || selected == "processors" || selected == "paths", "Unknown controlled test case");
        std::cout << "Minecraft native adapter / controlled HTTP / mock Java case passed (not real Minecraft acceptance)\n";
        return 0;
    } catch (const ErrorException& error) { std::cerr << error.error().code << ": " << error.what() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
