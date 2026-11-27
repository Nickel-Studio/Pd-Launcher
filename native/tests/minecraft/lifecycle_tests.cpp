#include "test_support.hpp"
#include "loader.hpp"
#include <algorithm>

using namespace test;
namespace {
void launch_ids() {
    validate_launch_id(std::string(128, 'x'));
    rejects([] { validate_launch_id(std::string(129, 'x')); }, "minecraft.launchId");
    for (unsigned value = 0; value < 256; ++value) {
        const auto c = static_cast<unsigned char>(value);
        const bool accepted = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        const auto id = std::string("x") + static_cast<char>(c);
        if (accepted) validate_launch_id(id); else rejects([&] { validate_launch_id(id); }, "minecraft.launchId");
    }
    auto settings = launch_settings_from_json(Json{{"preLaunchCommand", "echo test"}, {"preLaunchTimeoutMs", 4321}, {"initialMemoryMb", 1024}, {"windowsUtf8", true}, {"highPerformanceGpu", true}});
    require(settings.preLaunchCommand == "echo test" && settings.preLaunchTimeout.count() == 4321 && settings.initialMemoryMb == 1024 && settings.windowsUtf8 && settings.highPerformanceGpu, "Effective pre-launch settings are retained");
    require(!LaunchSettings{}.highPerformanceGpu && !ProcessSpec{}.highPerformanceGpu &&
            !launch_settings_from_json(Json::object()).highPerformanceGpu &&
            !launch_settings_from_json(Json{{"highPerformanceGpu", false}}).highPerformanceGpu,
            "High-performance GPU remains opt-in in both effective settings and process specifications");
    rejects([] { launch_settings_from_json(Json{{"highPerformanceGpu", "true"}}); });
}
void frozen_install() {
    Tree tree("snapshot");
    auto request = install_request(tree);
    auto services = synthetic_services();
    unsigned bound = 0, written = 0, downloaded = 0;
    const auto native = services;
    services.atomicWrite = [](auto&&...) { throw std::runtime_error("Unbound atomic write used"); };
    services.download = [](auto&&...) { throw std::runtime_error("Unbound download used"); };
    services.forOperation = [&, native](const Cancellation& cancel) {
        ++bound; auto operation = native;
        operation.atomicWrite = [&, cancel](const fs::path& path, std::string_view bytes, const CancelCheck& check) { cancel.check(); check(); ++written; put(path, bytes); };
        operation.download = [&, cancel](const DownloadFile& file, const CancelCheck& check, const ProgressCallback&) { cancel.check(); check(); ++downloaded; put(file.destination, "controlled client"); };
        return operation;
    };
    put(request.gameDirectory / ".nickel-pack.pending", "owned by instances");
    Installer installer(services);
    const auto result = installer.install(request, Cancellation{});
    require(bound == 1 && written > 0 && downloaded > 0, "One bound operation spans planning and execution");
    require(result.versionId == "test-pack" && result.minecraftVersion == "1.20.1", "Final version and base client identities retained");
    require(fs::is_regular_file(request.gameDirectory / ".nickel-pack.pending"), "Installer must not clear pending state");
    Cancellation cancelled; cancelled.cancel();
    rejects([&] { installer.install(request, cancelled); }, "cancelled");
}
InstallPlan processor_plan(const Tree& tree) {
    const auto request = install_request(tree);
    return Installer(synthetic_services()).plan(request, Cancellation{});
}
void processors() {
    for (const auto scenario : {"valid", "bad-hash", "too-large", "cancelled", "nonzero"}) {
        Tree tree(scenario); auto services = synthetic_services(); auto plan = processor_plan(tree);
        ProcessorPlan processor; processor.label = scenario; processor.process.workingDirectory = plan.root / "processor";
        processor.outputs = {{plan.root / "loader/forge/libraries/processed.jar", FileHashes{std::string(40, 'a'), {}, {}}}};
        if (std::string_view(scenario) == "too-large") processor.outputs.front().maximumBytes = 4;
        plan.processors.push_back(processor);
        Cancellation cancel; std::shared_ptr<Child> tracked;
        services.spawn = [&](const ProcessSpec& spec, const OutputCallback& output) {
            require(spec.purpose == ProcessKind::Preparation, "Processor child must have preparation purpose");
            tracked = std::make_shared<Child>(); tracked->output = output;
            tracked->code = std::string_view(scenario) == "nonzero" ? 9 : 0;
            tracked->onWait = [&](const CancelCheck& check) {
                if (std::string_view(scenario) == "cancelled") { cancel.cancel(); check(); }
                put(processor.outputs.front().destination, std::string_view(scenario) == "bad-hash" ? "wrong" : "processor-result");
            };
            return tracked;
        };
        Installer installer(services);
        if (std::string_view(scenario) == "valid") installer.execute(plan, cancel);
        else rejects([&] { installer.execute(plan, cancel); });
        require(tracked && tracked->drained && installer.owned_installer_pids().empty() && installer.drain_owned_installer(std::chrono::milliseconds(0)), "Processor must be tracked, joined and dual-output drained");
        if (std::string_view(scenario) != "valid") require(!fs::is_regular_file(plan.profilePath), "Failed processor cannot publish final profile");
    }
    Tree tree("mapping-hash"); auto services = synthetic_services(); auto plan = processor_plan(tree);
    ProcessorPlan maps; maps.outputs = {{plan.root / "loader/forge/libraries/map.txt", FileHashes{std::string(40, 'a'), {}, {}}}};
    maps.managedDownload = DownloadFile{maps.outputs.front().destination, "http://127.0.0.1/map.txt"};
    plan.processors.push_back(maps);
    rejects([&] { Installer(services).execute(plan, Cancellation{}); }, "minecraft.processor.integrity");
}
void processor_budget_preflight() {
    for (const auto scenario : {"zero-total", "zero-files", "zero-single", "file-limit", "declared-files", "zero-managed", "negative-timeout"}) {
        Tree tree(scenario); auto services = synthetic_services(); auto plan = processor_plan(tree);
        ProcessorPlan processor; processor.process.workingDirectory = plan.root / "processor";
        processor.outputs = {{plan.root / "processor/output.txt", FileHashes{std::string(40, 'a'), {}, {}}}};
        const std::string_view selected(scenario);
        if (selected == "zero-total") processor.maximumOutputBytes = 0;
        if (selected == "zero-files") processor.maximumOutputFiles = 0;
        if (selected == "zero-single") processor.outputs.front().maximumBytes = 0;
        if (selected == "file-limit") processor.maximumOutputFiles = 100001;
        if (selected == "declared-files") { processor.maximumOutputFiles = 1; processor.outputs.push_back({plan.root / "processor/second.txt", {}}); }
        if (selected == "zero-managed") { processor.managedDownload = DownloadFile{plan.root / "processor/mappings.txt", "http://127.0.0.1/mappings.txt"}; processor.managedDownload->maximumBytes = 0; }
        if (selected == "negative-timeout") processor.timeout = std::chrono::milliseconds(-1);
        plan.processors = {processor};
        unsigned writes = 0, downloads = 0, spawns = 0;
        services.atomicWrite = [&](auto&&...) { ++writes; };
        services.download = [&](auto&&...) { ++downloads; };
        services.spawn = [&](const ProcessSpec&, const OutputCallback&) { ++spawns; return std::make_shared<Child>(); };
        rejects([&] { Installer(services).execute(plan, Cancellation{}); }, selected == "negative-timeout" ? "minecraft.process.timeout" : "minecraft.processor.limit");
        require(writes == 0 && downloads == 0 && spawns == 0 && !fs::exists(plan.profilePath), "Invalid processor budgets fail before any I/O or child creation");
    }
}
void processor_aggregate_and_reuse() {
    for (const bool generated : {false, true}) {
        Tree tree(generated ? "generated-quota" : "aggregate-quota"); auto services = synthetic_services(); auto plan = processor_plan(tree);
        ProcessorPlan processor; processor.process.workingDirectory = plan.root / "processor";
        processor.maximumOutputFiles = 2;
        if (generated) processor.generatedMinecraft = plan.root / "generated";
        else {
            processor.maximumOutputBytes = 20;
            processor.outputs = {{plan.root / "processor/first.txt", FileHashes{std::string(40, 'a'), {}, {}}},
                                 {plan.root / "processor/second.txt", FileHashes{std::string(40, 'a'), {}, {}}}};
        }
        plan.processors = {processor}; std::shared_ptr<Child> child;
        services.spawn = [&](const ProcessSpec&, const OutputCallback&) {
            child = std::make_shared<Child>();
            child->onWait = [&](const CancelCheck&) {
                if (generated) for (unsigned i = 0; i < 3; ++i) put(processor.generatedMinecraft / (std::to_string(i) + ".txt"), "x");
                else for (const auto& output : processor.outputs) put(output.destination, "processor-result");
            };
            return child;
        };
        Installer installer(services);
        rejects([&] { installer.execute(plan, Cancellation{}); }, "minecraft.processor.limit");
        require(child && child->drained && installer.owned_installer_pids().empty() && !fs::exists(plan.profilePath), "Aggregate output bytes/files terminate and drain the owned child before publication");
    }
    for (const bool hashed : {false, true}) {
        Tree tree(hashed ? "verified-reuse" : "unhashed-reuse"); auto services = synthetic_services(); auto plan = processor_plan(tree);
        ProcessorPlan processor; processor.process.workingDirectory = plan.root / "processor";
        processor.outputs = {{plan.root / "processor/result.txt", hashed ? FileHashes{std::string(40, 'a'), {}, {}} : FileHashes{}}};
        put(processor.outputs.front().destination, "processor-result"); plan.processors = {processor};
        unsigned spawns = 0;
        services.spawn = [&](const ProcessSpec&, const OutputCallback&) { ++spawns; return std::make_shared<Child>(); };
        Installer installer(services);
        if (hashed) { installer.execute(plan, Cancellation{}); require(spawns == 0, "Only nonempty verified hashed processor outputs may be reused"); }
        else { rejects([&] { installer.execute(plan, Cancellation{}); }, "minecraft.processor.integrity"); require(spawns == 1 && !fs::exists(plan.profilePath), "Unhashed existing output cannot skip a processor or publish a profile"); }
        require(installer.owned_installer_pids().empty(), "Reuse decisions leave no owned processor behind");
    }
}
void cancel_and_game_handoff() {
    Tree tree("handoff"); auto request = launch_request(tree); auto services = synthetic_services();
    request.settings.highPerformanceGpu = true;
    request.version.java.path = tree.root / "unselected-java.exe";
    const auto selectedJava = request.java->path;
    std::shared_ptr<Child> game; std::vector<LaunchEvent> events; Cancellation cancel;
    std::function<void(const Error&)> gpuWarning;
    unsigned gameSpawns = 0;
    services.spawn = [&](const ProcessSpec& spec, const OutputCallback& output) {
        auto child = std::make_shared<Child>(); child->output = output;
        if (spec.purpose == ProcessKind::Game) {
            require(spec.highPerformanceGpu && spec.executable == selectedJava && spec.gpuPreferenceWarning,
                    "GPU preference follows the actual selected Game Java, not a stale version runtime");
            gpuWarning = spec.gpuPreferenceWarning;
            gpuWarning(Error{"Unsupported", "Controlled GPU acquisition failure: CONTROLLED_SECRET"});
            ++gameSpawns; game = child;
            if (output) { output(false, "CONTROLLED_"); output(false, "SECRET\n"); }
        } else {
            require(!spec.highPerformanceGpu && !spec.gpuPreferenceWarning, "Preparation and Java probes never acquire a Game GPU preference");
            if (output) output(true, "java.version = 17.0.1\nos.arch = amd64\nsun.arch.data.model = 64\n");
        }
        return child;
    };
    Launcher launcher(services);
    auto session = launcher.launch(request, cancel, [&](const LaunchEvent& event) {
        events.push_back(event);
        if (event.kind == "warning") throw std::runtime_error("Controlled warning observer failure");
    });
    require(gameSpawns == 1 && std::count_if(events.begin(), events.end(), [](const auto& event) { return event.kind == "warning"; }) == 1 &&
            std::none_of(events.begin(), events.end(), [](const auto& event) { return event.kind == "failed"; }),
            "GPU acquisition and warning observer failures do not reject, retry or stop the Game spawn");
    require(!game->native_process() && !session->native_process(), "Synthetic PIDs never become native window ownership");
    cancel.cancel();
    require(!launcher.cancel_prepare(request.launchId) && session->running() && !game->terminated, "Stale preparation cancellation cannot kill the handed-off game");
    gpuWarning(Error{"IoError", "Controlled root-exit GPU restore warning: CONTROLLED_SECRET"});
    const auto code = session->wait();
    require(code == 0 && game->drained && events.back().kind == "exited", "Game exit follows both EOF markers");
    std::string publicOutput;
    for (const auto& event : events) publicOutput += event.message;
    require(publicOutput.find("CONTROLLED_SECRET") == std::string::npos && publicOutput.find("[redacted]") != std::string::npos, "Split output chunks and GPU warnings cannot disclose the credential");
    require(std::count_if(events.begin(), events.end(), [](const auto& event) { return event.kind == "stdout-eof"; }) == 1 &&
            std::count_if(events.begin(), events.end(), [](const auto& event) { return event.kind == "stderr-eof"; }) == 1, "Both output EOF events delivered exactly once");
    session->wait();
    require(std::count_if(events.begin(), events.end(), [](const auto& event) { return event.kind == "exited"; }) == 1, "Session wait is idempotent");
    rejects([&] { launcher.launch(request, Cancellation{}); }, "minecraft.launchId");
    auto pending = request; pending.launchId = "pending"; put(request.version.gameDirectory / ".nickel-pack.pending", "pending");
    rejects([&] { launcher.launch(pending, Cancellation{}); }, "minecraft.pending");
}
void cancel_inflight() {
    Tree tree("cancel-download"); auto request = launch_request(tree); auto services = synthetic_services();
    std::promise<void> began; auto ready = began.get_future();
    services.download = [&](const DownloadFile&, const CancelCheck& check, const ProgressCallback&) {
        began.set_value();
        for (;;) { check(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    };
    Launcher launcher(services);
    auto operation = std::async(std::launch::async, [&] { rejects([&] { launcher.prepare(request, Cancellation{}); }, "cancelled"); });
    require(ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "Preparation began");
    require(launcher.cancel_prepare(request.launchId), "Named prepare cancellation accepted");
    require(operation.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "Preparation cancellation returns promptly");
    operation.get(); require(!launcher.session(request.launchId), "Cancelled preparation cannot produce a game session");
}
void managed_copy_and_plan_boundaries() {
    Tree tree("path-boundaries"); auto services = synthetic_services(); Cancellation cancel;
    const auto source = tree.root / "copy-source", destination = tree.root / "copy-destination";
    put(source, "new-content"); put(destination, "old-content");
    copy_managed(services, {source, destination, {}}, cancellation_check(services, cancel));
    require(get(destination) == "new-content", "Unhashed same-size copies must refresh actual content");
    const auto clean = processor_plan(tree);
    for (const auto scenario : {"runtime", "archive", "generated", "mapping", "classpath"}) {
        auto plan = clean; const auto outside = tree.root / "outside-authorized-roots";
        if (std::string_view(scenario) == "runtime" || std::string_view(scenario) == "archive") {
            RuntimePlan runtime; runtime.root = plan.root / "runtime"; runtime.executable = runtime.root / "bin/java.exe";
            if (std::string_view(scenario) == "runtime") runtime.root = outside;
            else runtime.archive = DownloadFile{outside / "runtime.zip", "http://127.0.0.1/runtime.zip"};
            plan.runtime = runtime;
        } else {
            ProcessorPlan processor; processor.process.workingDirectory = plan.root / "processor";
            if (std::string_view(scenario) == "generated") processor.generatedMinecraft = outside;
            if (std::string_view(scenario) == "mapping") processor.managedDownload = DownloadFile{outside / "map.txt", "http://127.0.0.1/map.txt"};
            if (std::string_view(scenario) == "classpath") processor.classpath = {outside / "processor.jar"};
            plan.processors.push_back(processor);
        }
        rejects([&] { Installer(services).execute(plan, cancel); }, "minecraft.path");
        require(!fs::exists(outside), "Plan path validation precedes all writes and processes");
    }
}
void binding_failure_cleanup() {
    for (const bool launch : {false, true}) {
        Tree tree("binding-failure"); auto request = launch_request(tree); auto services = synthetic_services();
        const auto bound = services; unsigned attempts = 0;
        services.forOperation = [&](const Cancellation&) -> Services {
            if (++attempts == 1) throw ErrorException(Error{"minecraft.services", "CONTROLLED_SECRET binding failure"});
            return bound;
        };
        Launcher launcher(services); std::vector<LaunchEvent> events;
        const auto callback = [&](const LaunchEvent& event) { events.push_back(event); };
        if (launch) rejects([&] { launcher.launch(request, Cancellation{}, callback); }, "minecraft.services");
        else rejects([&] { launcher.prepare(request, Cancellation{}, callback); }, "minecraft.services");
        require(!launcher.cancel_prepare(request.launchId), "Binding exception releases preparation ownership");
        require(events.size() == 1 && events.front().kind == "failed" && events.front().message.find("CONTROLLED_SECRET") == std::string::npos, "Public binding failure never exposes raw secrets");
        if (launch) require(launcher.launch(request, Cancellation{})->wait() == 0, "Failed binding can retry same launchId");
        else launcher.prepare(request, Cancellation{});
    }
}
void optifine_headless_plan() {
    Tree tree("optifine-headless"); auto services = synthetic_services(); auto request = install_request(tree);
    services.archiveEntries = [](const fs::path&, const CancelCheck& check) {
        check(); return std::vector<std::string>{"optifine/Installer.class", "optifine/OptiFineTweaker.class"};
    };
    const auto vanilla = resolve_version(request.versionId, request.root, [&](auto) { return request.profile; });
    LoaderBuild build; build.kind = LoaderKind::OptiFine; build.minecraftVersion = request.versionId;
    build.version = "1.20.1_HD_U_I6"; build.artifact.filename = "OptiFine_1.20.1_HD_U_I6.jar";
    build.artifact.url = "http://127.0.0.1/OptiFine_1.20.1_HD_U_I6.jar";
    const auto plan = plan_loader(build, request, vanilla, {}, services, cancellation_check(services, Cancellation{}));
    require(plan.processors.size() == 1, "Modern standalone OptiFine keeps its native owned installer plan");
    const auto& processor = plan.processors.front();
    require(std::count(processor.process.arguments.begin(), processor.process.arguments.end(), "-Djava.awt.headless=true") == 1,
            "OptiFine errors fail without a Swing dialog or GUI fallback");
    require(processor.process.purpose == ProcessKind::Preparation && processor.process.terminateOnDrop &&
            std::find(processor.process.arguments.begin(), processor.process.arguments.end(), "optifine.Installer") != processor.process.arguments.end(),
            "OptiFine headless mode preserves exact preparation ownership and installer entry point");
    require(path_is_inside(request.root, processor.generatedMinecraft) && processor.copiesBefore.size() == 2,
            "OptiFine writes remain inside its managed seeded workspace");
}

void bounded_game_wait() {
    struct TimedChild final : OwnedProcess {
        bool alive = false, timeout = true, drained = false, stopped = false;
        unsigned waits = 0;
        std::chrono::milliseconds drainBudget{};
        std::uint64_t pid() const noexcept override { return 9001; }
        bool running() const override { return alive; }
        int wait(const CancelCheck&, bool terminateOnCancellation, std::chrono::milliseconds budget) override {
            ++waits;
            require(!terminateOnCancellation && budget != std::chrono::milliseconds::max(), "Game session always uses a finite non-killing process wait");
            if (timeout) throw ErrorException(Error{"minecraft.process.timeout", "Controlled descendant-held output pipe"});
            std::this_thread::sleep_for(std::min(budget, std::chrono::milliseconds(20)));
            return 7;
        }
        void terminate_owned() override { stopped = true; alive = false; }
        bool drain_owned(std::chrono::milliseconds budget) override { drainBudget = budget; return drained; }
    };
    auto child = std::make_shared<TimedChild>();
    unsigned flushed = 0;
    std::vector<LaunchEvent> events;
    GameSession session("bounded-game", child, [&](const auto& event) { events.push_back(event); }, [&] { ++flushed; });
    rejects([&] { session.wait(std::chrono::milliseconds(-1)); }, "minecraft.process.timeout");
    rejects([&] { session.wait(std::chrono::milliseconds::max()); }, "minecraft.process.timeout");
    rejects([&] { session.wait(std::chrono::milliseconds(40)); }, "minecraft.process.timeout");
    require(events.empty() && !flushed && !child->stopped, "Timeout does not invent EOF, exit or implicit game stopping");
    const auto waits = child->waits;
    rejects([&] { session.wait(); }, "minecraft.process.timeout");
    require(child->waits == waits + 1, "Already-exited game has one bounded default drain attempt");
    child->timeout = false;
    rejects([&] { session.wait(std::chrono::milliseconds(80)); }, "minecraft.process.drain");
    require(child->drainBudget <= std::chrono::milliseconds(60) && events.empty() && !flushed,
            "Exit wait and stream drain consume one shared budget; missing EOF cannot publish exit");
    child->drained = true;
    require(session.wait(std::chrono::milliseconds(80)) == 7 && flushed == 1 && events.size() == 1 && events.front().kind == "exited",
            "Only successful dual-stream drain publishes the final exit");
    require(session.wait(std::chrono::milliseconds(0)) == 7 && flushed == 1 && events.size() == 1, "Bounded successful wait remains idempotent");
}


}
int main() {
    try { launch_ids(); frozen_install(); processors(); processor_budget_preflight(); processor_aggregate_and_reuse(); cancel_and_game_handoff(); bounded_game_wait(); cancel_inflight(); managed_copy_and_plan_boundaries(); binding_failure_cleanup(); optifine_headless_plan(); std::cout << "Minecraft orchestration synthetic lifecycle matrix passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
