#include "test_support.hpp"
#include "services_native.hpp"
#include "core/network/hash.hpp"
#include "core/tasks/tasks.hpp"
#include "allocation_fault.hpp"

#include <future>

using namespace test;
namespace net = nickel::transport;
namespace {

NativeServiceOptions controlled() {
    NativeServiceOptions options;
    options.trust = net::TrustPolicy::ControlledLoopback;
    options.useMirrors = false;
    return options;
}

net::NetworkSnapshot snapshot(std::string agent) {
    net::NetworkSnapshot value;
    value.user_agent = std::move(agent);
    value.total_timeout = std::chrono::seconds(5);
    return value;
}

void same_client(const std::string& base) {
    auto http = std::make_shared<net::HttpClient>(snapshot("Pd-shared-owned"));
    const std::weak_ptr<net::HttpClient> original = http;
    const auto count = http.use_count();
    auto factory = make_native_services(http, controlled());
    require(http.use_count() > count, "Factory must retain the original HttpClient, not only its budget");
    const auto beforeBinding = http.use_count();
    auto operation = factory.forOperation(Cancellation{});
    require(http.use_count() > beforeBinding, "Each operation must retain the same original HttpClient");
    factory = {};
    http.reset();
    require(!original.expired(), "Bound operation must keep the original client alive");
    require(operation.fetch(base + "/snapshot", 128, [] {}) == "Pd-shared-owned", "Original client remains usable after factory release");
    operation = {};
    require(original.expired(), "No hidden cloned client or original-client ownership leak remains");
}

void download_agent(const Services& services, const std::string& base, const fs::path& destination,
                    const std::string& agent) {
    auto file = artifact_download(Json{{"url", base + "/snapshot"}, {"size", agent.size()}}, destination, FileKind::Library);
    net::detail::IncrementalHash hash(net::HashAlgorithm::Sha256);
    hash.update({reinterpret_cast<const std::byte*>(agent.data()), agent.size()});
    file.hashes.sha256 = hash.finish();
    ensure_file(services, file, [] {});
    require(get(destination) == agent, "Download must use its bound immutable snapshot on the shared client");
}

void isolation(const std::string& base) {
    Tree tree("shared-http-snapshots");
    auto http = std::make_shared<net::HttpClient>(snapshot("Pd-snapshot-A"));
    auto factory = make_native_services(http, controlled());
    auto first = factory.forOperation(Cancellation{});
    http->set_snapshot(snapshot("Pd-snapshot-B"));
    auto second = factory.forOperation(Cancellation{});
    auto fixedOptions = controlled(); fixedOptions.snapshot = snapshot("Pd-snapshot-fixed");
    auto fixedFactory = make_native_services(http, fixedOptions);
    auto fixed = fixedFactory.forOperation(Cancellation{});
    fixedOptions.snapshot->user_agent = "Pd-mutated-caller-copy";
    http->set_snapshot(snapshot("Pd-snapshot-current"));
    net::HttpRequest cached; cached.url = base + "/hello.txt"; cached.trust = net::TrustPolicy::ControlledLoopback;
    cached.max_response_bytes = 128;
    require(http->get_cached(cached, Cancellation{}).text() == "controlled-body", "Original shared cache can be primed");
    const auto primed = http->cache_stats();
    auto a = std::async(std::launch::async, [&] { return first.fetch(base + "/snapshot-slow", 128, [] {}); });
    auto b = std::async(std::launch::async, [&] { return second.fetch(base + "/snapshot-slow", 128, [] {}); });
    require(a.get() == "Pd-snapshot-A" && b.get() == "Pd-snapshot-B", "Concurrent operations must not share mutable snapshot selection");
    require(fixed.fetch(base + "/snapshot", 128, [] {}) == "Pd-snapshot-fixed", "Explicit option snapshot is captured by value");
    require(fixedFactory.forOperation(Cancellation{}).fetch(base + "/snapshot", 128, [] {}) == "Pd-snapshot-fixed", "Later fixed bindings retain the original options snapshot");
    download_agent(first, base, tree.root / "first.txt", "Pd-snapshot-A");
    download_agent(second, base, tree.root / "second.txt", "Pd-snapshot-B");
    download_agent(fixed, base, tree.root / "fixed.txt", "Pd-snapshot-fixed");
    require(http->snapshot().user_agent == "Pd-snapshot-current", "Minecraft binding must never call the shared mutable setter");
    require(http->get_cached(cached, Cancellation{}).text() == "controlled-body", "Unrelated cache entry remains available");
    require(http->cache_stats().hits > primed.hits && http->cache_stats().entries == primed.entries,
            "Operation snapshots must not invalidate the original shared cache");
    Cancellation cancelled; cancelled.cancel();
    auto stopped = factory.forOperation(cancelled);
    rejects([&] { stopped.fetch(base + "/snapshot", 128, [] {}); }, "cancelled");
    require(first.fetch(base + "/snapshot", 128, [] {}) == "Pd-snapshot-A", "One operation's cancellation cannot cancel another");
    auto registry = std::make_shared<nickel::tasks::Registry>();
    nickel::tasks::TaskSpec spec; spec.target = tree.root / "task-target"; spec.network = snapshot("Pd-snapshot-task");
    auto task = registry->begin(spec);
    auto taskOptions = fixedOptions; taskOptions.task = task;
    auto taskFactory = make_native_services(http, taskOptions);
    auto taskOperation = taskFactory.forOperation(Cancellation{});
    require(taskOperation.fetch(base + "/snapshot", 128, [] {}) == "Pd-snapshot-task", "Immutable Task snapshot wins over explicit options and current settings");
    download_agent(taskOperation, base, tree.root / "task.txt", "Pd-snapshot-task");
    task->complete_work();
    auto invalid = controlled(); invalid.parallelRanges = 9;
    rejects([&] { make_native_services(http, invalid); }, "minecraft.services");
    invalid.parallelRanges = 0;
    rejects([&] { make_native_services(http, invalid); }, "minecraft.services");
}

void allocation_capture(const std::shared_ptr<net::HttpClient>& http, const NativeServiceOptions& options) {
    const auto outer = options.snapshot;
    const auto taskSnapshot = options.task ? std::optional<net::NetworkSnapshot>(options.task->network()) : std::nullopt;
    unsigned failures = 0;
    for (std::ptrdiff_t at = 0; at < 128; ++at) {
        Services captured;
        allocation_fault::arm(at);
        try { captured = make_native_services(http, options); }
        catch (...) { if (!allocation_fault::triggered()) { allocation_fault::disarm(); throw; } }
        allocation_fault::disarm();
        const bool injected = allocation_fault::triggered();
        if (injected) ++failures;
        require(options.snapshot.has_value() == outer.has_value(), "Factory must not clear caller options");
        if (outer) require(options.snapshot->proxy == outer->proxy && options.snapshot->user_agent == outer->user_agent,
                           "Factory success/failure cannot erase or modify caller credentials");
        if (taskSnapshot) require(options.task->network().proxy == taskSnapshot->proxy && options.task->network().user_agent == taskSnapshot->user_agent,
                                  "Factory success/failure cannot erase the immutable Task snapshot");
        if (!injected) { require(captured.forOperation != nullptr, "Factory eventually completes after every allocation checkpoint"); break; }
        require(at < 127, "Factory allocation sweep must reach success");
    }
    require(failures > 0, "Factory capture must be exercised through allocation failures");
    auto factory = make_native_services(http, options);
    failures = 0;
    for (std::ptrdiff_t at = 0; at < 128; ++at) {
        Services operation;
        allocation_fault::arm(at);
        try { operation = factory.forOperation(Cancellation{}); }
        catch (...) { if (!allocation_fault::triggered()) { allocation_fault::disarm(); throw; } }
        allocation_fault::disarm();
        if (allocation_fault::triggered()) ++failures;
        else { require(operation.fetch != nullptr, "Bound operation eventually completes after each capture failure"); break; }
        require(at < 127, "Operation allocation sweep must reach success");
    }
    require(failures > 0, "Immutable operation copies must be exercised through allocation failures");
}

void credential_lifecycle() {
    auto original = snapshot("Pd-credentials-original");
    original.proxy.username = std::string(64, 'u'); original.proxy.password = std::string(64, 'p');
    auto http = std::make_shared<net::HttpClient>(original);
    auto options = controlled(); options.snapshot = original;
    allocation_capture(http, options);
    allocation_capture(http, controlled());
    Tree tree("snapshot-credentials");
    auto registry = std::make_shared<nickel::tasks::Registry>();
    nickel::tasks::TaskSpec spec; spec.target = tree.root / "task-target"; spec.network = original;
    auto task = registry->begin(spec); options.task = task;
    options.snapshot->proxy.password = std::string(64, 'i');
    allocation_capture(http, options);
    require(options.snapshot->proxy.password == std::string(64, 'i'), "Ignored explicit snapshot remains caller-owned");
    require(http->snapshot().proxy == original.proxy, "Captured defaults cannot erase the original shared client snapshot");
    task->complete_work();
}


}

int main(int argc, char** argv) {
    try {
        require(argc == 4 && std::string_view(argv[3]) == "shared-http", "Expected controlled HTTP shared-client test arguments");
        same_client(argv[1]);
        isolation(argv[1]);
        credential_lifecycle();
        std::cout << "shared original HttpClient, immutable operation/task snapshots, credential capture failures and download isolation PASS (controlled)\n";
        return 0;
    } catch (const ErrorException& error) {
        allocation_fault::disarm();
        std::cerr << error.error().code << ": " << error.what() << '\n'; return 1;
    } catch (const std::exception& error) {
        allocation_fault::disarm();
        std::cerr << error.what() << '\n'; return 1;
    }
}
