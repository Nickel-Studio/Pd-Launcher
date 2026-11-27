#include "app/service/operation_queue.hpp"
#include "core/tasks/tasks.hpp"
#include "core/tasks/test_hooks.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {
namespace fs = std::filesystem;
namespace io = nickel::tasks::detail;
namespace task_api = nickel::tasks;
using nickel::Cancellation;
using nickel::Error;
using nickel::ErrorException;
using nickel::Json;
using Status = task_api::TaskStatus;

void check(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

template<class Function>
void error_code(Function&& function, std::string_view expected) {
    try { std::invoke(std::forward<Function>(function)); }
    catch (const ErrorException& failure) {
        check(failure.error().code == expected, "Unexpected public error code");
        return;
    }
    throw std::runtime_error("Required failure did not occur");
}

bool present(const fs::path& path) { return fs::exists(io::io_path(path)); }

struct Scenario {
    std::shared_ptr<task_api::Task> task;
    fs::path source;
    fs::path destination;
    io::FileIdentity identity;
};

Scenario prepare(task_api::Registry& registry, const fs::path& root, std::string_view name,
                 std::optional<Cancellation> cancellation = {}) {
    std::cout << "CASE " << name << '\n' << std::flush;
    task_api::TaskSpec specification;
    specification.kind = task_api::TaskKind::Version;
    specification.target = root / io::utf8_path(name);
    specification.label = std::string(name);
    specification.cancellation = std::move(cancellation);
    auto task = registry.begin(specification);
    task->stage("preparing");
    const auto source = task->staging() / "descriptor.json";
    const auto text = Json{{"id", name}, {"counter", std::numeric_limits<std::uint64_t>::max()}}.dump();
    auto output = io::SecureFile::create_new(source);
    const auto identity = output.identity();
    output.write(std::as_bytes(std::span(text.data(), text.size())));
    output.flush();
    output.close();
    return {std::move(task), source, specification.target, identity};
}

void publish(const Scenario& scenario) {
    auto file = io::SecureFile::open_publication(scenario.source, scenario.identity,
                                                io::GuardMode::NamespaceMutation);
    file.publish_no_replace(scenario.destination);
}

void terminal_once(const task_api::Registry& registry, const Scenario& scenario, Status status) {
    const auto snapshot = scenario.task->snapshot();
    check(snapshot.status == status, "Task has the wrong terminal status");
    const auto delta = registry.since(0);
    const auto terminals = std::count_if(delta.events.begin(), delta.events.end(), [&](const auto& event) {
        return event.kind == task_api::EventKind::Terminal && event.snapshot.id == scenario.task->id();
    });
    check(terminals == 1, "Task terminal event is missing or duplicated");
}

void published_success(task_api::Registry& registry, const Scenario& scenario) {
    terminal_once(registry, scenario, Status::Succeeded);
    check(present(scenario.destination), "Durable external publication is missing");
    const auto bytes = io::read_bounded(scenario.destination, 4096);
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(Json::parse(text).at("counter") == std::numeric_limits<std::uint64_t>::max(),
          "Durable publication loses UINT64");
    check(!registry.cancel(scenario.task->id()) && !scenario.task->cancel(), "Terminal success accepts cancellation");
    scenario.task->rollback("fixture", "Late rollback must not touch a publication");
    check(present(scenario.destination), "Late rollback removes committed domain output");
    terminal_once(registry, scenario, Status::Succeeded);
}

#if defined(NICKEL_TRANSPORT_TEST_HOOKS) && !defined(NICKEL_INSTANCE_PRODUCTION_CONTROLS)
using Hook = std::function<void(std::string_view, const fs::path&)>;
struct CheckpointScope {
    explicit CheckpointScope(Hook hook) : previous(std::move(io::testing::checkpoint)) {
        io::testing::checkpoint = std::move(hook);
    }
    ~CheckpointScope() { io::testing::checkpoint = std::move(previous); }
    Hook previous;
};
#endif

void cancelled_before_commit(task_api::Registry& registry, const fs::path& root) {
    auto scenario = prepare(registry, root, "cancel-before-commit");
    Cancellation request;
    std::stop_callback link(request.stop_token(), [task = scenario.task] { (void)task->cancel(); });
    check(request.stop_token() != scenario.task->cancellation().stop_token(), "Request shares the raw task token");
    request.cancel();
    bool invoked = false;
    error_code([&] {
        scenario.task->commit_external([&] { invoked = true; publish(scenario); });
    }, "cancelled");
    check(!invoked && !present(scenario.destination), "Cancelled preparation enters publication");
    terminal_once(registry, scenario, Status::Cancelled);
    check(!present(scenario.task->workspace()), "Cancelled task leaves owned staging without a failure");
}

void prepublication_failures(task_api::Registry& registry, const fs::path& root) {
#if defined(NICKEL_TRANSPORT_TEST_HOOKS) && !defined(NICKEL_INSTANCE_PRODUCTION_CONTROLS)
    auto intent = prepare(registry, root, "intent-failure");
    bool invoked = false;
    {
        CheckpointScope hook([](std::string_view point, const fs::path&) {
            if (point == "external.intent") throw ErrorException(Error{"fixture_intent", "Injected prepublication failure"});
        });
        error_code([&] { intent.task->commit_external([&] { invoked = true; publish(intent); }); }, "fixture_intent");
    }
    check(!invoked && !present(intent.destination), "Failed commit intent publishes domain output");
    terminal_once(registry, intent, Status::Failed);
#endif
    auto callback = prepare(registry, root, "callback-failure");
    error_code([&] {
        callback.task->commit_external([&] {
            throw ErrorException(Error{"fixture_callback", "Injected failure before publication"});
        });
    }, "fixture_callback");
    check(!present(callback.destination), "Failed callback publishes output");
    terminal_once(registry, callback, Status::Failed);
}

void cancellation_during_publication(task_api::Registry& registry, const fs::path& root) {
    auto scenario = prepare(registry, root, "cancel-during-publication");
    Cancellation request;
    std::atomic<bool> registry_accepted{true};
    std::atomic<bool> task_accepted{true};
    std::atomic<bool> link_called{false};
    std::stop_callback link(request.stop_token(), [task = scenario.task, &link_called] {
        link_called.store(true);
        (void)task->cancel();
    });
    scenario.task->commit_external([&] {
        check(scenario.task->snapshot().status == Status::Committing, "Publication has no commit admission");
        std::jthread cancel([&] {
            request.cancel();
            registry_accepted.store(registry.cancel(scenario.task->id()));
            task_accepted.store(scenario.task->cancel());
        });
        cancel.join();
        check(link_called.load() && !registry_accepted.load() && !task_accepted.load(),
              "Admitted publication accepts a late cancellation");
        check(!scenario.task->cancellation().cancelled(), "Request directly stops the admitted task token");
        publish(scenario);
    });
    published_success(registry, scenario);
    check(!present(scenario.task->workspace()), "Successful external task does not clean its own workspace");
}

#if defined(NICKEL_TRANSPORT_TEST_HOOKS) && !defined(NICKEL_INSTANCE_PRODUCTION_CONTROLS)
void durable_failure_does_not_relabel(task_api::Registry& registry, const fs::path& root,
        std::string_view name, std::string_view checkpoint, std::string_view expected_code) {
    auto scenario = prepare(registry, root, name);
    {
        CheckpointScope hook([checkpoint](std::string_view point, const fs::path&) {
            if (point == checkpoint) throw ErrorException(Error{"fixture_after_publish", "Injected completion failure"});
        });
        scenario.task->commit_external([&] { publish(scenario); });
    }
    published_success(registry, scenario);
    check(scenario.task->snapshot().error_code == expected_code, "Durable completion loses its recovery status");
    check(present(scenario.task->workspace()), "Failure evidence workspace is not retained");
    const auto bytes = io::read_bounded(scenario.task->workspace() / "journal.json", 64 * 1024);
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto journal = Json::parse(text);
    check(journal.at("externalPublication").get<bool>(), "Retained journal loses external publication ownership");
    std::cout << "RETAINED " << io::path_utf8(scenario.task->workspace()) << '\n';
}
#endif

void queue_close_during_commit(task_api::Registry& registry, const fs::path& root) {
    auto scenario = prepare(registry, root, "close-during-publication");
    auto queue = std::make_shared<nickel::app::OperationQueue>();
    std::optional<nickel::app::OperationQueue::Work> dispatched;
    queue->enqueue([&](const Cancellation& cancellation) {
        scenario.task->commit_external([&] {
            std::jthread close([queue] { queue->close(); });
            close.join();
            check(!cancellation.cancelled(), "Queue shutdown bypasses the task commit gate");
            publish(scenario);
        });
    }, scenario.task->cancellation(), [&](nickel::app::OperationQueue::Work work) {
        dispatched = std::move(work);
        return true;
    }, [task = scenario.task] { (void)task->cancel(); });
    check(dispatched.has_value(), "Queue did not dispatch its owner work");
    (*dispatched)(Cancellation{});
    published_success(registry, scenario);
    check(queue->pending() == 0, "Closed queue retains completed active work");
}

void registry_cancel_before_commit(task_api::Registry& registry, const fs::path& root) {
    auto scenario = prepare(registry, root, "registry-cancel-before-commit");
    check(registry.cancel(scenario.task->id()), "Live registry task refuses cancellation");
    error_code([&] { scenario.task->commit_external([&] { publish(scenario); }); }, "cancelled");
    check(!present(scenario.destination), "Cancelled registry task publishes domain output");
    terminal_once(registry, scenario, Status::Cancelled);
}

#if !defined(NICKEL_TRANSPORT_TEST_HOOKS) || defined(NICKEL_INSTANCE_PRODUCTION_CONTROLS)
void accepted_token_cancellation(task_api::Registry& registry, const fs::path& root) {
    Cancellation accepted;
    auto scenario = prepare(registry, root, "accepted-token-cancellation", accepted);
    check(scenario.task->cancellation().stop_token() == accepted.stop_token(),
          "Task does not retain the accepted operation token");
    auto registry_token = registry.cancellation_for(scenario.task->id());
    check(registry_token && registry_token->stop_token() == accepted.stop_token(),
          "Registry cancellation lookup substitutes a different token");
    registry_token->cancel();
    bool invoked = false;
    error_code([&] {
        scenario.task->commit_external([&] { invoked = true; publish(scenario); });
    }, "cancelled");
    check(accepted.cancelled() && !invoked && !present(scenario.destination),
          "Accepted cancellation reaches an irreversible publication");
    terminal_once(registry, scenario, Status::Cancelled);
    check(!registry.cancellation_for(scenario.task->id()), "Terminal task exposes a live cancellation token");
}

void terminal_publication_is_unique(task_api::Registry& registry, const fs::path& root) {
    auto scenario = prepare(registry, root, "terminal-publication-is-unique");
    scenario.task->commit_external([&] { publish(scenario); });
    published_success(registry, scenario);
    bool invoked = false;
    error_code([&] {
        scenario.task->commit_external([&] { invoked = true; publish(scenario); });
    }, "task_terminal");
    error_code([&] { scenario.task->complete_work(); }, "task_terminal");
    check(!invoked, "Terminal task admits a second publication");
    published_success(registry, scenario);
    const auto snapshots = registry.snapshots();
    check(std::count_if(snapshots.begin(), snapshots.end(), [&](const auto& snapshot) {
        return snapshot.id == scenario.task->id();
    }) == 1, "A single publication produces duplicate registry tasks");
}

void guarded_task_creation(task_api::Registry& registry, const fs::path& root) {
    const auto directory = root / "outer-guard";
    io::ParentGuard parent(directory, true, io::GuardMode::NamespaceMutation);
    Cancellation accepted;
    auto scenario = prepare(registry, directory, "guarded-task-creation", accepted);
    parent.verify();
    scenario.task->commit_external([&] {
        parent.verify();
        publish(scenario);
        parent.verify();
    });
    published_success(registry, scenario);
    parent.verify();
    check(!present(scenario.task->workspace()), "Outer creation guard prevents owned task cleanup");
}
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "An exact owned fixture path is required");
        const auto root = io::checked_absolute(io::utf8_path(argv[1]));
        check(!present(root), "Fixture path already exists");
        io::create_directory_new(root);
        task_api::Registry registry;
        cancelled_before_commit(registry, root);
        prepublication_failures(registry, root);
        cancellation_during_publication(registry, root);
#if defined(NICKEL_TRANSPORT_TEST_HOOKS) && !defined(NICKEL_INSTANCE_PRODUCTION_CONTROLS)
        durable_failure_does_not_relabel(registry, root, "marker-failure", "external.committed", "persistence_pending");
        durable_failure_does_not_relabel(registry, root, "cleanup-failure", "cleanup", "cleanup_pending");
#endif
        queue_close_during_commit(registry, root);
        registry_cancel_before_commit(registry, root);
#if defined(NICKEL_TRANSPORT_TEST_HOOKS) && !defined(NICKEL_INSTANCE_PRODUCTION_CONTROLS)
        std::cout << "PASS eight real task publication controls including injected persistence/cleanup failures\n";
#else
        accepted_token_cancellation(registry, root);
        terminal_publication_is_unique(registry, root);
        guarded_task_creation(registry, root);
        std::cout << "PASS eight hook-free production task publication/cancellation/token/guard/queue controls\n";
#endif
        return 0;
    } catch (const ErrorException& failure) {
#if defined(NICKEL_TRANSPORT_TEST_HOOKS) && !defined(NICKEL_INSTANCE_PRODUCTION_CONTROLS)
        io::testing::checkpoint = {};
#endif
        std::cerr << "FAIL task publication fixture: " << failure.error().code << ": " << failure.what()
                  << "; details=" << failure.error().details.dump() << '\n';
        if (argc == 2) {
            try {
                const auto probe = io::checked_absolute(io::utf8_path(argv[1])) / "begin-primitives";
                io::create_directory_new(probe);
                io::ParentGuard parent(probe);
                std::cerr << "PROBE ParentGuard created\n";
                const auto staging = probe / "staging";
                io::create_directory_new(staging);
                std::cerr << "PROBE staging created\n";
                const auto identity = io::directory_identity(staging);
                std::cerr << "PROBE staging identity " << identity.volume << ':' << identity.index << '\n';
                auto lock = io::SecureFile::lock_workspace(probe / "owner.lock", true);
                std::cerr << "PROBE workspace locked\n";
                io::write_atomic(probe / "journal.json", "{\"schema\":1}");
                std::cerr << "PROBE journal written\n";
                parent.verify();
            } catch (const std::exception& probe_failure) {
                std::cerr << "FAIL begin primitives: " << probe_failure.what() << '\n';
            }
        }
        return 1;
    } catch (const std::exception& failure) {
#if defined(NICKEL_TRANSPORT_TEST_HOOKS) && !defined(NICKEL_INSTANCE_PRODUCTION_CONTROLS)
        io::testing::checkpoint = {};
#endif
        std::cerr << "FAIL task publication fixture: " << failure.what() << '\n';
        return 1;
    }
}
