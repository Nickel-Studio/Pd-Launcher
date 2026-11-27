#include "fixtures.hpp"
#include "core/tasks/tasks.hpp"
#include "core/tasks/test_hooks.hpp"

#include <array>
#include <cstdlib>
#include <functional>
#include <string_view>

namespace {
using namespace nickel;
using namespace nickel::tests;
using Hook = std::function<void(std::string_view, const Path&)>;

struct HookScope {
    explicit HookScope(Hook hook) : previous(std::move(tasks::detail::testing::checkpoint)) {
        tasks::detail::testing::checkpoint = std::move(hook);
    }
    ~HookScope() { tasks::detail::testing::checkpoint = std::move(previous); }
    Hook previous;
};

struct Layout {
    Layout(const Path& root, bool renamed) : root(root), mods(root / "mods"),
        original(mods / "original.jar.disabled"),
        destination(renamed ? mods / "renamed.jar.disabled" : original),
        retained(renamed ? std::optional<Path>(mods / "retained.jar.disabled") : std::nullopt),
        side(mods / "side.jar") {}
    Path root, mods, original, destination;
    std::optional<Path> retained;
    Path side;
};

std::string read(const Path& path) {
    const auto value = tasks::detail::read_bounded(path, 4096);
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::vector<tasks::PublishFile> publications(const Layout& paths, const std::shared_ptr<tasks::Task>& task) {
    std::filesystem::create_directory(paths.mods);
    write(paths.original, "original revision");
    write(paths.side, "side revision");
    write(task->staging() / "next", "updated revision");
    write(task->staging() / "side-next", "updated side revision");
    return {{"next", paths.destination, tasks::lease_file(paths.original, {}), paths.retained},
        {"side-next", paths.side, tasks::lease_file(paths.side, {}), {}}};
}

std::shared_ptr<tasks::Task> begin(tasks::Registry& registry, const Layout& paths, const Cancellation& cancel = {}) {
    tasks::TaskSpec specification;
    specification.kind = tasks::TaskKind::Resource;
    specification.target = paths.mods;
    specification.cancellation = cancel;
    return registry.begin(specification);
}

void originalState(const Layout& paths) {
    require(read(paths.original) == "original revision" && read(paths.side) == "side revision",
        "Interrupted transaction loses an original revision");
    if (paths.destination != paths.original)
        require(!std::filesystem::exists(paths.destination), "Rolled-back rename leaves a published revision");
    if (paths.retained) require(!std::filesystem::exists(*paths.retained), "Rollback leaves a duplicate retained original");
}

void committedState(const Layout& paths) {
    require(read(paths.destination) == "updated revision" && read(paths.side) == "updated side revision",
        "Committed transaction recovery loses a published revision");
    if (paths.destination != paths.original)
        require(!std::filesystem::exists(paths.original), "Committed rename leaves the previous active name");
    if (paths.retained) require(read(*paths.retained) == "original revision", "Committed recovery loses the retained original");
}

void terminal(const tasks::Registry& registry, const std::shared_ptr<tasks::Task>& task, tasks::TaskStatus status) {
    require(task->snapshot().status == status && !std::filesystem::exists(task->workspace()),
        "Fault injection leaves a nonterminal Task or uncleaned owned workspace");
    unsigned terminals = 0;
    for (const auto& event : registry.since(0).events)
        if (event.snapshot.id == task->id() && event.kind == tasks::EventKind::Terminal) ++terminals;
    require(terminals == 1, "Interrupted transaction emits a missing or repeated terminal event");
}

void exceptionAndCancellationBoundaries() {
    constexpr std::array<std::pair<std::string_view, unsigned>, 6> points{{
        {"files.planned", 1}, {"files.backed_up", 1}, {"files.published", 1},
        {"files.backed_up", 2}, {"files.published", 2}, {"files.retained", 1}}};
    unsigned cases = 0;
    for (const bool renamed : {false, true}) {
        for (const auto& [point, ordinal] : points) {
            if (!renamed && point == "files.retained") continue;
            for (const bool cancelling : {false, true}) {
                TemporaryRoot root;
                const Layout paths(root.path, renamed);
                tasks::Registry registry;
                Cancellation cancel;
                const auto task = begin(registry, paths, cancel);
                const auto files = publications(paths, task);
                unsigned reached = 0;
                {
                    HookScope hook([&](std::string_view current, const Path&) {
                        if (current != point || ++reached != ordinal) return;
                        if (cancelling) cancel.cancel();
                        else throw std::runtime_error("Injected transaction boundary failure");
                    });
                    expectError(cancelling ? "cancelled" : "transaction_failed", [&] { task->commit_files(files); });
                }
                require(reached >= ordinal, "Required real transaction checkpoint was not reached");
                originalState(paths);
                terminal(registry, task, cancelling ? tasks::TaskStatus::Cancelled : tasks::TaskStatus::Failed);
                require(registry.recover(root.path, {}).empty(), "Successful rollback retains a recovery journal");
                ++cases;
            }
        }
    }
    std::cout << "PASS " << cases << " real exception/cancellation transaction boundaries\n";
}

void cleanupRecovery() {
    for (const bool renamed : {false, true}) {
        TemporaryRoot root;
        const Layout paths(root.path, renamed);
        tasks::Registry registry;
        auto task = begin(registry, paths);
        const auto files = publications(paths, task);
        const auto workspace = task->workspace();
        {
            HookScope hook([](std::string_view point, const Path&) {
                if (point == "cleanup") throw std::runtime_error("Injected owned cleanup failure");
            });
            task->commit_files(files);
        }
        require(task->snapshot().status == tasks::TaskStatus::Succeeded &&
            task->snapshot().error_code == "cleanup_pending" && std::filesystem::exists(workspace),
            "Cleanup fault changes committed success or removes its recovery journal");
        committedState(paths);
        task.reset();
        const auto recovered = registry.recover(root.path, {});
        require(recovered.size() == 1 && recovered.front().recovered && recovered.front().committed,
            "Committed cleanup journal is not recovered");
        committedState(paths);
        require(!std::filesystem::exists(workspace) && registry.recover(root.path, {}).empty(),
            "Cleanup recovery is not clean and idempotent");
    }
    std::cout << "PASS committed cleanup failure and idempotent recovery\n";
}

void crashed(const Path& root, std::string_view point, unsigned ordinal, bool renamed) {
    const Layout paths(root, renamed);
    tasks::Registry registry;
    const auto task = begin(registry, paths);
    const auto files = publications(paths, task);
    unsigned reached = 0;
    HookScope hook([&](std::string_view current, const Path&) {
        if (current == point && ++reached == ordinal) std::_Exit(86);
    });
    task->commit_files(files);
    throw std::runtime_error("Process interruption did not reach the requested checkpoint");
}

void recoverCrashed(const Path& root, bool renamed, bool committed, bool foreignConflict) {
    const Layout paths(root, renamed);
    if (foreignConflict) write(paths.destination, "external mutation after interruption");
    tasks::Registry registry;
    const auto recovered = registry.recover(root, {});
    require(recovered.size() == 1, "Crash fixture does not contain exactly one journal");
    if (foreignConflict) {
        require(!recovered.front().recovered && !recovered.front().error.empty() &&
            std::filesystem::exists(recovered.front().workspace), "Conflicted recovery discards its journal");
        require(read(paths.destination) == "external mutation after interruption", "Recovery overwrites an external revision");
        const auto repeated = registry.recover(root, {});
        require(repeated.size() == 1 && !repeated.front().recovered &&
            read(paths.destination) == "external mutation after interruption", "Repeated recovery consumes an external revision");
        std::cout << "PASS interrupted publication retains external conflict and recovery journal\n";
        return;
    }
    require(recovered.front().recovered && recovered.front().committed == committed,
        "Crash recovery mistakes pre-commit and committed states");
    if (committed) committedState(paths);
    else originalState(paths);
    require(!std::filesystem::exists(recovered.front().workspace) && registry.recover(root, {}).empty(),
        "Crash recovery leaves its workspace or is not idempotent");
    std::cout << "PASS process interruption recovery " << (renamed ? "renamed" : "in-place")
        << (committed ? " committed" : " rolled back") << '\n';
}
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    try {
        require(argc >= 2, "Supply a transaction fault mode");
        const std::string_view mode(argv[1]);
        if (mode == "--run") {
            require(argc == 2, "Unexpected transaction fault arguments");
            exceptionAndCancellationBoundaries();
            cleanupRecovery();
        } else if (mode == "--crash") {
            require(argc == 6, "Supply crash root, checkpoint, ordinal and rename flag");
            crashed(pathFromUtf8(argv[2]), argv[3], static_cast<unsigned>(std::stoul(argv[4])), std::string_view(argv[5]) == "1");
        } else if (mode == "--recover") {
            require(argc == 6, "Supply recovery root, rename, committed and conflict flags");
            recoverCrashed(pathFromUtf8(argv[2]), std::string_view(argv[3]) == "1",
                std::string_view(argv[4]) == "1", std::string_view(argv[5]) == "1");
        } else throw std::runtime_error("Unknown transaction fault mode");
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL transaction faults: " << error.error().code << ": " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL transaction faults: " << error.what() << '\n';
    }
    return 1;
}
