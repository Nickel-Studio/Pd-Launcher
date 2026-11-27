#include "test_support.hpp"
#include "allocation_fault.hpp"
#include "services_native.hpp"

#include <array>
#include <cstdio>
#include <new>
#include <algorithm>
#include <charconv>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace test;
namespace net = nickel::transport;
namespace {
struct PreparedAllocationEvents {
    std::ptrdiff_t failure;
    bool* prepared;
    bool* armed;
    PreparedAllocationEvents(std::ptrdiff_t limit, bool& ready, bool& active) noexcept
        : failure(limit), prepared(&ready), armed(&active) {}
    PreparedAllocationEvents(const PreparedAllocationEvents& other) noexcept
        : failure(other.failure), prepared(other.prepared), armed(other.armed) {
        // Arm on the relay's callback copy, after prepare_checked has destroyed its JSON request.
        if (*prepared && !*armed) { *armed = true; allocation_fault::arm(failure); }
    }
    void operator()(const LaunchEvent& event) const noexcept {
        if (event.kind == "prepared") *prepared = true;
    }
};

Services make_services() {
    NativeServiceOptions options;
    options.trust = net::TrustPolicy::ControlledLoopback;
    options.useMirrors = false;
    net::NetworkSnapshot snapshot;
    snapshot.total_timeout = std::chrono::seconds(10);
    return make_native_services(std::make_shared<net::HttpClient>(snapshot), options);
}

LaunchRequest installed_request(const std::string& base, const fs::path& executable, const Tree& tree, Services services) {
    Cancellation cancellation;
    auto operation = services.forOperation(cancellation);
    auto check = cancellation_check(operation, cancellation);
    InstallRequest request;
    request.root = tree.root / "managed";
    request.gameDirectory = tree.root / "game";
    request.versionId = "1.20.1";
    request.instanceName = "handoff-fixture";
    request.profile = parse_metadata(operation.fetch(base + "/profile.json", 1024 * 1024, check));
    request.manualJava = executable;
    request.installJava = false;
    request.resourcesBase = base + "/assets";
    request.platform = {"windows", "amd64", "10.0.19045", {}};
    auto installed = Installer(services).install(request, cancellation);
    LaunchRequest launch;
    launch.version = std::move(installed);
    launch.java = java(executable);
    launch.identity = {"offline", "Controlled Player", "01234567890123456789012345678901", "CONTROLLED_SECRET"};
    launch.assetIndex = parse_metadata(operation.fetch(base + "/index.json", 1024 * 1024, check));
    launch.platform = request.platform;
    launch.settings.gameArguments = {"--sleep", "250"};
    return launch;
}

void handoff_matrix(const std::string& base, const fs::path& executable) {
    Tree tree("allocation-handoff");
    auto services = make_services();
    auto request = installed_request(base, executable, tree, services);
    const auto native_spawn = services.spawn;
    // Keep the actual native adapters, while retaining this test's spawn decoration through operation binding.
    services.forOperation = {};
    std::weak_ptr<OwnedProcess> spawned;
    bool reject_spawn = false, fault_after_spawn = false, allocation_sweep = false;
    unsigned game_spawns = 0;
    services.spawn = [&](const ProcessSpec& spec, const OutputCallback& output) -> std::shared_ptr<OwnedProcess> {
        if (spec.purpose == ProcessKind::Game && reject_spawn) throw std::bad_alloc();
        // The sweep targets Launcher allocations, not foundation's internal thread creation.
        if (spec.purpose == ProcessKind::Game && allocation_sweep) allocation_fault::disarm();
        auto process = native_spawn(spec, output);
        if (spec.purpose == ProcessKind::Game) {
            spawned = process;
            ++game_spawns;
            if (fault_after_spawn) allocation_fault::arm(0);
        }
        return process;
    };
    Launcher launcher(services);

    unsigned allocation_failures = 0;
    bool reached_spawn = false;
    allocation_sweep = true;
    for (std::ptrdiff_t failure = 0; failure < 64; ++failure) {
        request.launchId = "allocation-before-handoff-" + std::to_string(failure);
        std::fprintf(stderr, "allocation sweep checkpoint %td\n", failure);
        std::shared_ptr<GameSession> session;
        bool prepared = false, armed = false;
        const LaunchEventCallback callback = PreparedAllocationEvents(failure, prepared, armed);
        bool rejected = false;
        try { session = launcher.launch(request, Cancellation{}, callback); }
        catch (const std::exception&) { rejected = true; }
        allocation_fault::disarm();
        const bool injected = allocation_fault::triggered();
        auto retained = launcher.session(request.launchId);
        auto process = spawned.lock();
        if (rejected) {
            require(injected && !retained && !process && game_spawns == 0,
                    "Allocation failure before spawn leaves no process or session to orphan");
            require(!launcher.cancel_prepare(request.launchId), "Failed allocation releases the exact preparation");
            ++allocation_failures;
        } else {
            require(session && retained == session && process && process->pid() == session->pid(),
                    "Successful actual game retains its preallocated registered owner");
            require(session->wait(std::chrono::seconds(3)) == 0 && !process->running() && process->drain_owned(std::chrono::milliseconds(0)),
                    "Actual process completes naturally with both EOF streams after the allocation sweep");
            require(!injected, "No allocation is attempted after the actual game is returned");
            reached_spawn = true;
            break;
        }
    }
    allocation_sweep = false;
    require(reached_spawn && allocation_failures >= 6, "Allocation sweep covers session, callbacks, event and map-node registration");
    std::cout << "PASS allocation sweep before actual owned game: failures=" << allocation_failures << "\n";

    request.launchId = "spawn-failure-retry";
    reject_spawn = true;
    const auto spawns_before_failure = game_spawns;
    rejects([&] { launcher.launch(request, Cancellation{}); }, "minecraft.launch");
    require(game_spawns == spawns_before_failure && !launcher.session(request.launchId) && !launcher.cancel_prepare(request.launchId),
            "Spawn failure removes the empty registered owner and releases its launchId");
    reject_spawn = false;
    auto retry = launcher.launch(request, Cancellation{});
    require(launcher.session(request.launchId) == retry && retry->wait(std::chrono::seconds(3)) == 0,
            "The same launchId can retry only when no actual game was spawned");
    std::cout << "PASS actual spawn failure cleanup and same-launchId retry\n";

    for (const bool callback_throw : {false, true}) {
        request.launchId = callback_throw ? "throwing-started-notification" : "allocation-free-postspawn";
        Cancellation cancellation;
        fault_after_spawn = true;
        std::array<std::atomic<unsigned>, 4> events{};
        bool started_fault_pending = false;
        const LaunchEventCallback callback = [&](const LaunchEvent& event) {
            if (event.kind == "started") {
                ++events[0];
                started_fault_pending = !allocation_fault::triggered();
                if (callback_throw) throw std::bad_alloc();
            } else if (event.kind == "stdout-eof") ++events[1];
            else if (event.kind == "stderr-eof") ++events[2];
            else if (event.kind == "exited") ++events[3];
        };
        std::shared_ptr<GameSession> session;
        try { session = launcher.launch(request, cancellation, callback); }
        catch (...) {
            allocation_fault::disarm();
            auto retained = launcher.session(request.launchId);
            if (retained) (void)retained->wait(std::chrono::seconds(3));
            throw;
        }
        allocation_fault::disarm();
        fault_after_spawn = false;
        auto process = spawned.lock();
        require(!allocation_fault::triggered() && started_fault_pending && session && process && session->pid() == process->pid() &&
                launcher.session(request.launchId) == session && events[0] == 1,
                "Postspawn handoff and throwing notification cannot allocate, lose ownership or turn success into failure");
        cancellation.cancel();
        require(!launcher.cancel_prepare(request.launchId) && session->running(), "Late preparation cancellation never stops the retained actual game");
        require(session->wait(std::chrono::seconds(3)) == 0 && !process->running() && process->drain_owned(std::chrono::milliseconds(0)),
                "Retained actual game exits naturally without compensating game stop");
        require(events[1] == 1 && events[2] == 1 && events[3] == 1 && session->wait(std::chrono::milliseconds(0)) == 0,
                "Retained output relay still publishes both authoritative EOF events and one idempotent exit");
        std::cout << "PASS actual owned process allocation-free handoff and started callback throw=" << callback_throw << "\n";
    }

    request.launchId = "throwing-raw-output-callback";
    auto raw_services = services;
    std::atomic<unsigned> raw_calls{0}, terminal_events{0};
    raw_services.spawn = [native_spawn, &raw_calls](const ProcessSpec& spec, const OutputCallback& output) {
        if (spec.purpose != ProcessKind::Game) return native_spawn(spec, output);
        OutputCallback failed_output = [&raw_calls](bool, std::string_view) { ++raw_calls; throw std::bad_alloc(); };
        return native_spawn(spec, failed_output);
    };
    Launcher raw_launcher(raw_services);
    auto raw_session = raw_launcher.launch(request, Cancellation{}, [&](const LaunchEvent& event) {
        if (event.kind == "stdout-eof" || event.kind == "stderr-eof" || event.kind == "exited") ++terminal_events;
    });
    require(raw_session->pid() && raw_launcher.session(request.launchId) == raw_session,
            "Throwing raw output callback cannot lose the registered actual process owner");
    rejects([&] { raw_session->wait(std::chrono::seconds(3)); }, "IoError");
    require(raw_calls > 0 && !raw_session->running() && raw_launcher.session(request.launchId) == raw_session && terminal_events == 0,
            "Raw callback failure preserves ownership through natural exit and reports error without fabricated EOF or compensating game stop");
    std::cout << "PASS actual owned raw callback failure retains session; IoError is not fabricated EOF/exit\n";
}


#ifdef _WIN32
struct HeldPipeTrace {
    mutable std::mutex mutex;
    std::vector<LaunchEvent> events;
    void append(const LaunchEvent& event) {
        std::lock_guard lock(mutex);
        events.push_back(event);
    }
    std::vector<LaunchEvent> snapshot() const {
        std::lock_guard lock(mutex);
        return events;
    }
    unsigned count(std::string_view kind) const {
        const auto current = snapshot();
        return static_cast<unsigned>(std::count_if(current.begin(), current.end(), [&](const auto& event) { return event.kind == kind; }));
    }
    DWORD pid_after(std::string_view prefix) const {
        for (const auto& event : snapshot()) {
            if (event.kind != "stdout" || !event.message.starts_with(prefix)) continue;
            const auto number = std::string_view(event.message).substr(prefix.size());
            DWORD pid = 0;
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), pid);
            require(parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() && pid, "Invalid actual descendant PID receipt");
            return pid;
        }
        return 0;
    }
};

struct ProcessObservation {
    HANDLE handle;
    explicit ProcessObservation(DWORD pid)
        : handle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        require(handle != nullptr, "Cannot open an actual owned process for read-only exit observation");
    }
    ~ProcessObservation() { CloseHandle(handle); }
    ProcessObservation(const ProcessObservation&) = delete;
    ProcessObservation& operator=(const ProcessObservation&) = delete;
    bool active() const { return WaitForSingleObject(handle, 0) == WAIT_TIMEOUT; }
    DWORD exit_code(DWORD observationTimeout = 0) const {
        DWORD result = 0;
        require(WaitForSingleObject(handle, observationTimeout) == WAIT_OBJECT_0 && GetExitCodeProcess(handle, &result),
                "Actual process has not exited authoritatively within its separate observation budget");
        return result;
    }
};

struct HeldPipeCleanup {
    std::shared_ptr<GameSession> session;
    bool completed = false;
    ~HeldPipeCleanup() {
        if (!completed) {
            // Failed fixtures use the exact retained session for explicit cleanup, never a PID/name kill.
            try { session->stop_owned(); (void)session->wait(std::chrono::seconds(3)); } catch (...) {}
        }
    }
};

DWORD await_held_descendant(const std::shared_ptr<GameSession>& session, const HeldPipeTrace& trace) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const auto spawned = trace.pid_after("held-pipe-parent-child ");
        const auto ready = trace.pid_after("held-pipe-child-ready ");
        if (spawned && spawned == ready && !session->running()) return spawned;
        require(std::chrono::steady_clock::now() < deadline, "Actual root did not exit with a ready descendant holding both output pipes");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void require_no_held_terminal(const HeldPipeTrace& trace) {
    require(trace.count("stdout-eof") == 0 && trace.count("stderr-eof") == 0 && trace.count("exited") == 0,
            "Exited root with live inherited pipes must not fabricate either EOF or terminal Game exit");
}

void require_held_completion(const HeldPipeTrace& trace, std::string_view launchId, std::uint64_t pid) {
    require(trace.count("stdout-eof") == 1 && trace.count("stderr-eof") == 1 && trace.count("exited") == 1,
            "Successful owned wait/drain must publish both actual EOFs and exactly one exit");
    bool stdout_eof = false, stderr_eof = false;
    for (const auto& event : trace.snapshot()) {
        require(event.launchId == launchId, "Held-pipe lifecycle event lost its launchId");
        if (event.kind == "stdout-eof") stdout_eof = true;
        else if (event.kind == "stderr-eof") stderr_eof = true;
        else if (event.kind == "exited")
            require(stdout_eof && stderr_eof && event.pid == pid && event.exitCode == 0,
                    "Game exit must follow both authoritative stream EOFs and retain actual root PID/exit code");
    }
}

void held_pipe_matrix(const std::string& base, const fs::path& executable) {
    Tree tree("actual-descendant-held-pipes");
    auto services = make_services();
    auto request = installed_request(base, executable, tree, services);
    Launcher launcher(services);

    {
        auto trace = std::make_shared<HeldPipeTrace>();
        request.launchId = "held-pipes-natural-exit";
        request.settings.gameArguments = {"--spawn-pipe-holder", "3000"};
        Cancellation cancellation;
        auto session = launcher.launch(request, cancellation, [trace](const LaunchEvent& event) { trace->append(event); });
        HeldPipeCleanup cleanup{session};
        ProcessObservation root(static_cast<DWORD>(session->pid()));
        const auto child_pid = await_held_descendant(session, *trace);
        ProcessObservation child(child_pid);
        require(session->pid() && session->pid() != child_pid && root.exit_code() == 0 && child.active(),
                "Timed held-pipe fixture requires a genuinely exited root and live owned descendant");
        std::cout << "HELD_PIPE_STARTED " << Json{{"case", "natural"}, {"rootPid", session->pid()}, {"childPid", child_pid}, {"rootExitCode", root.exit_code()}}.dump() << std::endl;
        cancellation.cancel();
        require(!launcher.cancel_prepare(request.launchId), "Late preparation cancellation cannot take a held-pipe Game session");
        rejects([&] { session->wait(std::chrono::milliseconds(0)); }, "minecraft.process.timeout");
        const auto start = std::chrono::steady_clock::now();
        rejects([&] { session->wait(std::chrono::milliseconds(125)); }, "minecraft.process.timeout");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        require(elapsed < std::chrono::seconds(1) && child.active() && launcher.session(request.launchId) == session,
                "Timed wait uses one finite budget and neither loses ownership nor stops the live descendant");
        require_no_held_terminal(*trace);
        require(session->wait(std::chrono::seconds(5)) == 0,
                "Natural descendant pipe closure must permit authoritative owned wait/drain");
        // Windows can close inherited stdio before signalling the descendant process handle.
        const bool child_active_at_drain = child.active();
        const auto observed = std::chrono::steady_clock::now();
        const auto child_exit = child.exit_code(1000);
        const auto observation_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - observed);
        require(child_exit == 0, "Natural descendant exit must succeed within its independent one-second observation budget");
        cleanup.completed = true;
        require_held_completion(*trace, request.launchId, session->pid());
        require(session->wait(std::chrono::milliseconds(0)) == 0, "Natural held-pipe wait must remain idempotent");
        require_held_completion(*trace, request.launchId, session->pid());
        std::cout << "HELD_PIPE_RESULT " << Json{{"case", "natural"}, {"rootPid", session->pid()}, {"childPid", child_pid},
            {"rootExitCode", root.exit_code()}, {"childExitCode", child_exit}, {"timedWaitMs", elapsed.count()},
            {"childActiveAtDrainReturn", child_active_at_drain}, {"childExitObservationMs", observation_elapsed.count()},
            {"implicitStop", false}, {"stdoutEofCount", trace->count("stdout-eof")}, {"stderrEofCount", trace->count("stderr-eof")},
            {"exitEventCount", trace->count("exited")}, {"authoritativeDrainVerified", true}}.dump() << std::endl;
    }

    {
        auto trace = std::make_shared<HeldPipeTrace>();
        request.launchId = "held-pipes-ten-second-drain";
        request.settings.gameArguments = {"--spawn-pipe-holder", "16000"};
        auto session = launcher.launch(request, Cancellation{}, [trace](const LaunchEvent& event) { trace->append(event); });
        HeldPipeCleanup cleanup{session};
        ProcessObservation root(static_cast<DWORD>(session->pid()));
        const auto child_pid = await_held_descendant(session, *trace);
        ProcessObservation child(child_pid);
        require(root.exit_code() == 0 && child.active() && !session->running(),
                "No-argument held-pipe fixture requires a genuinely exited root before its bounded drain attempt");
        std::cout << "HELD_PIPE_STARTED " << Json{{"case", "bounded-drain"}, {"rootPid", session->pid()}, {"childPid", child_pid}, {"rootExitCode", root.exit_code()}}.dump() << std::endl;
        const auto start = std::chrono::steady_clock::now();
        rejects([&] { session->wait(); }, "minecraft.process.timeout");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        require(elapsed >= std::chrono::seconds(9) && elapsed < std::chrono::seconds(13) && child.active(),
                "No-argument wait must report its one ten-second post-root-exit drain timeout without implicit Game stop");
        require_no_held_terminal(*trace);
        require(launcher.session(request.launchId) == session && !launcher.cancel_prepare(request.launchId),
                "Exited-root drain timeout must retain exact launchId recovery and reject preparation cancellation");
        session->stop_owned();
        require(session->wait(std::chrono::seconds(3)) == 0,
                "Explicit retained Game stop must drain both streams after the owned descendant job stop");
        const auto child_exit = child.exit_code(1000);
        require(child_exit == 125, "Explicit owned job stop must authoritatively exit the still-owned descendant with code 125");
        cleanup.completed = true;
        require_held_completion(*trace, request.launchId, session->pid());
        require(session->wait(std::chrono::milliseconds(0)) == 0, "Stopped held-pipe wait must remain idempotent");
        require_held_completion(*trace, request.launchId, session->pid());
        std::cout << "HELD_PIPE_RESULT " << Json{{"case", "bounded-drain"}, {"rootPid", session->pid()}, {"childPid", child_pid},
            {"rootExitCode", root.exit_code()}, {"childExitCode", child_exit}, {"noArgumentWaitMs", elapsed.count()},
            {"implicitStop", false}, {"explicitOwnedStop", true}, {"stdoutEofCount", trace->count("stdout-eof")},
            {"stderrEofCount", trace->count("stderr-eof")}, {"exitEventCount", trace->count("exited")}, {"authoritativeDrainVerified", true}}.dump() << std::endl;
    }
    std::cout << "PASS actual exited-root/descendant-held-pipe native session matrix; controlled mock only, not Java/Game/App acceptance\n";
}
#endif
}

int main(int argc, char** argv) {
    try {
        require(argc == 4, "Expected controlled server, actual mock executable and handoff case");
        const auto executable = fs::absolute(fs::u8path(argv[2]));
        const auto mode = std::string_view(argv[3]);
        if (mode == "handoff") handoff_matrix(argv[1], executable);
#ifdef _WIN32
        else if (mode == "held-pipes") held_pipe_matrix(argv[1], executable);
#endif
        else throw std::runtime_error("Unsupported native handoff case");
        std::cout << "Minecraft handoff regression uses actual HTTP/hash/archive/owned-process adapters and mock Java, not a real Minecraft acceptance\n";
        return 0;
    } catch (const std::exception& error) {
        allocation_fault::disarm();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
