#include "core/archive/archive.hpp"
#include "core/common/cancellation.hpp"
#include "core/diagnostics/diagnostics.hpp"
#include "core/instances/domain_io.hpp"
#include "core/instances/startup.hpp"
#include "core/minecraft/launcher.hpp"
#include "core/packs/native_services.hpp"
#include "core/tasks/tasks.hpp"
#include "platform/process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace diagnostics = nickel::diagnostics;
namespace minecraft = nickel::minecraft;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}

struct Fixture {
    fs::path root;
    explicit Fixture(const fs::path& parent, std::string_view name)
        : root(parent / (std::string(name) + '-' + nickel::instances::detail::random_id())) {
        fs::create_directories(parent);
        require(fs::create_directory(root), "Fixture path was not unused");
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

class RealProcess final : public minecraft::OwnedProcess {
public:
    explicit RealProcess(nickel::OwnedProcess process) : process_(std::move(process)) {}
    std::uint64_t pid() const noexcept override { return process_.pid(); }
    bool running() const override { return process_.running(); }
    int wait(const minecraft::CancelCheck& check, bool terminate_on_cancel,
             std::chrono::milliseconds timeout) override {
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            try { if (check) check(); }
            catch (...) {
                if (terminate_on_cancel && process_.purpose() == nickel::ProcessPurpose::Preparation)
                    process_.terminate();
                throw;
            }
            auto interval = 20ms;
            if (timeout != std::chrono::milliseconds::max()) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                interval = std::max(0ms, std::min(interval, timeout - elapsed));
            }
            if (auto result = process_.wait(interval)) {
                require(result->stdout_eof && result->stderr_eof, "Foundation wait missed dual EOF");
                return result->exit_code;
            }
            if (timeout != std::chrono::milliseconds::max()
                && std::chrono::steady_clock::now() - started >= timeout)
                throw nickel::ErrorException({"minecraft.process.timeout", "Controlled child timed out"});
        }
    }
    void terminate_owned() override {
        if (process_.purpose() == nickel::ProcessPurpose::Game) process_.stop_owned_game();
        else process_.terminate();
    }
    bool drain_owned(std::chrono::milliseconds timeout) override {
        const auto result = process_.drain(timeout);
        return result && result->stdout_eof && result->stderr_eof;
    }
private:
    mutable nickel::OwnedProcess process_;
};

struct Child {
    std::shared_ptr<diagnostics::OutputSession> output;
    std::shared_ptr<RealProcess> process;
    std::shared_ptr<std::array<std::atomic<bool>, 2>> eof;
};

Child spawn(const fs::path& executable, const fs::path& root, std::string mode,
            nickel::ProcessPurpose purpose = nickel::ProcessPurpose::Game, std::string delay = "40",
            std::string_view known_secret = {}) {
    auto output = std::make_shared<diagnostics::OutputSession>(diagnostics::LaunchLogStore(root));
    if (!known_secret.empty()) output->add_secret(known_secret);
    auto eof = std::make_shared<std::array<std::atomic<bool>, 2>>();
    nickel::ProcessOptions options;
    options.executable = executable;
    options.cwd = root;
    options.arguments = {"--fixture-mode", std::move(mode), "--fixture-delay", std::move(delay)};
    options.purpose = purpose;
    options.capture_limit = 0;
    options.output = [output, eof](nickel::ProcessStream stream, std::string_view bytes, bool closed) {
        const bool error = stream == nickel::ProcessStream::Stderr;
        const auto target = error ? diagnostics::ProcessStream::stderr_stream : diagnostics::ProcessStream::stdout_stream;
        if (closed) {
            output->eof(target);
            (*eof)[error ? 1 : 0].store(true);
        } else output->bytes(target, bytes);
    };
    auto process = std::make_shared<RealProcess>(nickel::OwnedProcess::spawn(options));
    require(process->pid() != 0, "Controlled game has no owned PID");
    return {output, process, eof};
}

void ids_are_ascii_and_bounded() {
    minecraft::validate_launch_id("A");
    minecraft::validate_launch_id(std::string(128, 'a'));
    for (const auto& id : {std::string{}, std::string(129, 'a'), std::string("has space"),
                          std::string("../escape"), std::string("bad\xC3\xA9")})
        rejects([&] { minecraft::validate_launch_id(id); }, "Unsafe launch ID was accepted");
}

void worker_wait_drains_dual_eof(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(parent, "dual-eof");
    auto child = spawn(executable, fixture.root, "dual-eof");
    std::atomic<bool> exited{false};
    const auto caller_thread = std::this_thread::get_id();
    minecraft::GameSession game("dual-eof-worker", child.process,
        [&](const minecraft::LaunchEvent& event) {
            if (event.kind == "exited") {
                require(std::this_thread::get_id() != caller_thread, "Game wait emitted on caller thread");
                require((*child.eof)[0].load() && (*child.eof)[1].load(), "Exited preceded dual EOF");
                require(child.output->finished(), "Exited preceded OutputSession finish");
                exited.store(true);
            }
        }, [&] { require(child.output->finish(0ms), "Safe output failed drain"); });
    auto waited = std::async(std::launch::async, [&] { return game.wait(); });
    require(waited.get() == 17 && exited.load(), "Owned GameSession did not report actual exit");
    require(game.wait() == 17, "Game wait was not idempotent");
    const auto capture = child.output->capture_text();
    require(capture.find("stdout-final-without-newline") != std::string::npos, "Lost stdout final tail");
    require(capture.find("stderr-final-without-newline") != std::string::npos, "Lost stderr final tail");
    require(capture.find("fixture-private-value") == std::string::npos, "Split secret leaked to capture");
    const auto log = diagnostics::LaunchLogStore(fixture.root).tail();
    require(log && log->text.find("fixture-private-value") == std::string::npos, "Split secret leaked to disk");
}

void descendant_exit_does_not_mean_eof(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(parent, "descendant-pipes");
    auto child = spawn(executable, fixture.root, "descendant-pipes", nickel::ProcessPurpose::Game, "2000");
    std::atomic<bool> exited{false};
    minecraft::GameSession game("descendant-pipes-worker", child.process,
        [&](const minecraft::LaunchEvent& event) { if (event.kind == "exited") exited.store(true); },
        [&] { require(child.output->finish(0ms), "Natural descendant output failed dual EOF"); });
    const auto exit_deadline = std::chrono::steady_clock::now() + 1s;
    while (game.running() && std::chrono::steady_clock::now() < exit_deadline)
        std::this_thread::sleep_for(5ms);
    require(!game.running(), "Descendant fixture root did not exit");
    require(!(*child.eof)[0].load() && !(*child.eof)[1].load(), "Descendant pipes were not held open");
    const auto started = std::chrono::steady_clock::now();
    rejects([&] { (void)game.wait(150ms); }, "Root exit falsely completed held pipes");
    require(std::chrono::steady_clock::now() - started < 750ms, "Finite GameSession wait exceeded its deadline");
    require(!(*child.eof)[0].load() && !(*child.eof)[1].load(), "Timeout synthesized EOF");
    require(!child.output->finished() && !exited.load(), "Timeout falsely completed the output or game session");
    require(game.wait(3s) == 17 && exited.load(), "Natural descendant EOF lost the root exit code");
    require(game.wait(0ms) == 17 && child.output->finished(), "Completed bounded wait was not idempotent");
    const auto text = child.output->capture_text();
    require(text.find("descendant-stdout-tail") != std::string::npos
        && text.find("descendant-stderr-tail") != std::string::npos, "Natural descendant tails were lost");
}

void registered_secrets_are_redacted(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(parent, "registered-secret");
    auto child = spawn(executable, fixture.root, "registered-secret", nickel::ProcessPurpose::Game,
                       "40", "registered-fixture-secret");
    require(child.process->wait({}, false, 3s) == 0 && child.output->finish(0ms), "Registered-secret child failed");
    const auto text = child.output->capture_text();
    require(text.find("registered-fixture-secret") == std::string::npos, "Raw registered secret crossed capture");
    require(text.find("literal-prefix") != std::string::npos && text.find("literal-suffix") != std::string::npos,
            "Registered-secret redaction removed unrelated output");
    const auto log = diagnostics::LaunchLogStore(fixture.root).tail();
    require(log && log->text.find("registered-fixture-secret") == std::string::npos, "Registered secret crossed disk");
    rejects([&] { child.output->add_secret("too-late"); }, "Late secret registration was accepted");
}

void complete_report_zip_is_frozen(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(parent, "complete-frozen-report");
    const auto root = fixture.root / "game";
    const auto resource = root / "versions" / "OuterID";
    fs::create_directories(resource);
    auto child = spawn(executable, resource, "crash", nickel::ProcessPurpose::Game, "40", "registered-fixture-secret");
    const auto code = child.process->wait({}, false, 3s);
    require(child.output->finish(0ms), "Full-report child failed dual EOF");
    diagnostics::DiagnosticInput context;
    context.exit_code = code;
    context.minecraft_version = "1.20.6";
    context.loader = "vanilla";
    context.java_major = 21;
    context.java_vendor = "ControlledFixture";
    context.stdout_tail = child.output->capture_text();
    const auto store = child.output->log_store();
    const auto snapshot = diagnostics::capture_error_report({root, "OuterID", "full-report-worker"}, store, context);
    require(snapshot.target().root == root && snapshot.target().version_id == "OuterID"
        && snapshot.target().launch_id == "full-report-worker", "Full snapshot lost captured scope");
    require(snapshot.diagnosis().category == "outOfMemory", "Complete log source lost crash diagnosis");
    const auto latest = store.latest();
    require(latest && latest->text.find("registered-fixture-secret") == std::string::npos,
            "Store copy lost this launch's registered secrets");
    std::ofstream(resource / "logs/latest.log", std::ios::binary) << "route-changed-after-failure\n";
    auto archive = nickel::packs::native_archive_backend();
    require(static_cast<bool>(archive), "Production ZIP archive backend is unavailable");
    const auto destination = fixture.root / "complete-report.zip";
    require(diagnostics::export_error_report_zip(destination, snapshot, *archive) == destination,
            "Full frozen ZIP was not published at the exact destination");
    nickel::Cancellation cancellation;
    const auto bytes = nickel::archive::Archive::read_entry(destination, "logs/latest.log", 2 * 1024 * 1024, cancellation);
    const auto text = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    require(text.find("crash-source-begin") != std::string::npos && text.find("crash-source-end") != std::string::npos
        && text.find("complete-source-evidence-19999") != std::string::npos, "ZIP included a tail instead of complete source");
    require(text.find("fixture-private-value") == std::string::npos
        && text.find("registered-fixture-secret") == std::string::npos
        && text.find("route-changed-after-failure") == std::string::npos, "ZIP leaked secrets or re-read live selection");
    rejects([&] { (void)diagnostics::export_error_report_zip(destination, snapshot, *archive); },
            "Complete report overwrote an existing destination");
}

void preparation_cancel_never_stops_game(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(parent, "running-cancel");
    auto child = spawn(executable, fixture.root, "hold", nickel::ProcessPurpose::Game, "500");
    nickel::Cancellation preparation;
    preparation.cancel();
    rejects([&] { child.process->wait([&] { preparation.check(); }, true, 1s); }, "Cancelled prepare wait accepted");
    require(child.process->running(), "Preparation cancellation killed owned running game");
    require(child.process->wait({}, false, 2s) == 0, "Game did not exit naturally");
    require(child.process->drain_owned(1s) && child.output->finish(0ms), "Natural game EOF failed");

    Fixture preparation_fixture(parent, "prepare-cancel");
    auto prepared = spawn(executable, preparation_fixture.root, "hold", nickel::ProcessPurpose::Preparation, "5000");
    rejects([&] { prepared.process->wait([&] { preparation.check(); }, true, 1s); }, "Preparation cancellation accepted");
    require(prepared.process->drain_owned(2s), "Cancelled preparation did not drain owned child");
    require(!prepared.process->running(), "Cancelled preparation kept running");
    (void)prepared.output->finish(0ms);
}

void external_commit_is_the_game_boundary(const fs::path& executable, const fs::path& parent) {
    Fixture before(parent, "cancel-before-spawn");
    auto registry = std::make_shared<nickel::tasks::Registry>();
    nickel::Cancellation cancelled;
    nickel::tasks::TaskSpec before_spec;
    before_spec.target = before.root;
    before_spec.kind = nickel::tasks::TaskKind::Runtime;
    before_spec.cancellation = cancelled;
    auto before_task = registry->begin(before_spec);
    require(before_task->cancel(), "Task did not accept preparation cancellation");
    bool spawned = false;
    rejects([&] { before_task->commit_external([&] { spawned = true; }); },
            "Cancelled preparation crossed external publication");
    require(!spawned && before_task->snapshot().status == nickel::tasks::TaskStatus::Cancelled,
            "Cancelled preparation did not preserve a truthful terminal state");

    Fixture after(parent, "cancel-at-spawn");
    nickel::Cancellation late;
    nickel::tasks::TaskSpec after_spec;
    after_spec.target = after.root;
    after_spec.kind = nickel::tasks::TaskKind::Runtime;
    after_spec.cancellation = late;
    auto task = registry->begin(after_spec);
    std::promise<void> entered, release;
    auto boundary = entered.get_future();
    const auto resume = release.get_future().share();
    Child child;
    auto publication = std::async(std::launch::async, [&] {
        task->commit_external([&] {
            child = spawn(executable, after.root, "hold", nickel::ProcessPurpose::Game, "500");
            entered.set_value();
            (void)resume.wait_for(2s);
        });
    });
    const bool reached = boundary.wait_for(2s) == std::future_status::ready;
    const bool accepted = reached && task->cancel();
    if (reached) late.cancel();
    release.set_value();
    publication.get();
    require(reached && !accepted, "Task cancellation crossed the Game spawn commit guard");
    require(task->snapshot().status == nickel::tasks::TaskStatus::Succeeded && !task->cancel(),
            "Late cancellation relabelled an externally published game");
    require(child.process && child.process->running(), "Late preparation cancellation killed the game");
    require(child.process->wait({}, false, 2s) == 0 && child.output->finish(0ms),
            "Published game lost its natural exit and dual EOF");
}


void explicit_stop_is_independent(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(parent, "explicit-owned-stop");
    Fixture unrelated(parent, "unrelated-owned-process");
    auto other = spawn(executable, unrelated.root, "hold", nickel::ProcessPurpose::Game, "5000");
    auto child = spawn(executable, fixture.root, "hold", nickel::ProcessPurpose::Game, "5000");
    require(other.process->pid() != child.process->pid(), "Stop isolation fixture did not create separate processes");
    minecraft::GameSession game("explicit-stop-worker", child.process);
    game.stop_owned();
    (void)game.wait();
    require(!game.running() && child.process->drain_owned(1s), "Explicit owned game stop failed");
    (void)child.output->finish(0ms);
    require(other.process->running(), "Stopping one owned game affected an unrelated process");
    require(other.process->wait({}, false, 6s) == 0 && other.output->finish(0ms),
            "Unrelated process did not retain its natural exit and dual EOF");
}

void unsafe_stream_never_exports(const fs::path& executable, const fs::path& parent) {
    for (const auto& mode : {"unsafe", "oversized"}) {
        Fixture fixture(parent, mode);
        auto child = spawn(executable, fixture.root, mode);
        (void)child.process->wait({}, false, 3s);
        require(child.process->drain_owned(1s), "Unsafe stream did not drain");
        require(!child.output->finish(0ms) && !child.output->safe(), "Unsafe EOF was presented as success");
        const auto capture = child.output->capture_text();
        require(capture.find("fixture-private-value") == std::string::npos, "Unsafe stream exposed sensitive bytes");
        require(capture.find("independent-stderr-visible") != std::string::npos, "Unsafe stdout quarantined stderr");
        rejects([&] { (void)diagnostics::safe_report(diagnostics::LaunchLogStore(fixture.root), {}); },
                "Unsafe stream exported a success report");
    }
}

void output_capture_is_bounded(const fs::path& parent) {
    Fixture fixture(parent, "capture-quota");
    diagnostics::OutputSession output(diagnostics::LaunchLogStore(fixture.root), {}, false);
    const auto lines = std::string("line\n");
    for (int index = 0; index != 4005; ++index) output.bytes(diagnostics::ProcessStream::stdout_stream, lines);
    output.eof(diagnostics::ProcessStream::stdout_stream);
    output.eof(diagnostics::ProcessStream::stderr_stream);
    require(output.finish(0ms), "Bounded capture failed");
    const auto capture = output.capture_text();
    require(static_cast<std::size_t>(std::count(capture.begin(), capture.end(), '\n')) + 1 <= diagnostics::capture_max_lines,
            "OutputSession exceeded line quota");
    require(capture.size() <= diagnostics::capture_max_bytes, "OutputSession exceeded byte quota");
}

void safe_report_is_a_snapshot(const fs::path& executable, const fs::path& parent) {
    Fixture fixture(parent, "frozen-report");
    auto child = spawn(executable, fixture.root, "crash");
    const auto code = child.process->wait({}, false, 3s);
    require(child.process->drain_owned(1s) && child.output->finish(0ms), "Crash output did not drain");
    diagnostics::DiagnosticInput context;
    context.exit_code = code;
    context.minecraft_version = "1.20.6";
    context.loader = "fabric";
    context.java_major = 21;
    context.java_vendor = "ControlledFixture";
    context.stdout_tail = child.output->capture_text();
    const auto report = diagnostics::safe_report(diagnostics::LaunchLogStore(fixture.root), context);
    const auto text = diagnostics::safe_report_text(report);
    require(report.diagnosis.at("category") == "outOfMemory", "Actual crash was not diagnosed");
    require(report.diagnosis.at("context").at("minecraftVersion") == "1.20.6", "Lost real Minecraft context");
    require(text.find("fixture-private-value") == std::string::npos, "Report leaked sensitive log");
    std::ofstream(fixture.root / "logs/latest.log", std::ios::binary) << "route-changed-after-failure\n";
    require(diagnostics::safe_report_text(report) == text, "Frozen report re-read changed logs");
}

void onboarding_dispatches_finished_once(const fs::path& parent) {
    Fixture fixture(parent, "onboarding-once");
    nickel::instances::OnboardingGate gate(fixture.root, "fixture-version");
    unsigned dispatches = 0;
    rejects([&] { gate.finish([&] { ++dispatches; }); }, "Onboarding dispatched before EULA");
    gate.accept_eula(true, nickel::instances::eula_version, 1);
    rejects([&] { gate.finish([&] { ++dispatches; }); }, "Onboarding dispatched before changelog");
    gate.acknowledge_changelog("fixture-version");
    require(gate.finish([&] { gate.require_finished(); ++dispatches; }), "Finished onboarding failed dispatch");
    require(!gate.finish([&] { ++dispatches; }) && dispatches == 1, "Startup dispatched more than once");
    const std::vector<std::string> arguments{"--launch-instance", "OuterID", "--game-root", nickel::instances::path_to_utf8(fixture.root)};
    const auto parsed = nickel::instances::parse_startup_arguments(arguments);
    require(parsed && parsed->instance_id == "OuterID" && parsed->game_root == fixture.root,
            "CLI lost registered outer-directory identity");
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "Expected controlled child and fixture parent");
        const auto executable = fs::absolute(nickel::instances::path_from_utf8(argv[1]));
        const auto parent = fs::absolute(nickel::instances::path_from_utf8(argv[2]));
        ids_are_ascii_and_bounded();
        worker_wait_drains_dual_eof(executable, parent);
        descendant_exit_does_not_mean_eof(executable, parent);
        registered_secrets_are_redacted(executable, parent);
        preparation_cancel_never_stops_game(executable, parent);
        external_commit_is_the_game_boundary(executable, parent);
        explicit_stop_is_independent(executable, parent);
        unsafe_stream_never_exports(executable, parent);
        output_capture_is_bounded(parent);
        safe_report_is_a_snapshot(executable, parent);
        complete_report_zip_is_frozen(executable, parent);
        onboarding_dispatches_finished_once(parent);
        std::cout << "PASS: Palladium owned process, bounded session wait, external commit cancellation, isolated owned stop, dual EOF, secrets, quotas, full frozen ZIP and finished-once gate\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
