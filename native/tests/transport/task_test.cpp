#include "core/tasks/task.hpp"
#include "core/tasks/secure_fs.hpp"
#include "core/network/control.hpp"
#include "core/tasks/test_hooks.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unordered_map>

namespace {
namespace fs = std::filesystem;
namespace td = nickel::tasks::detail;
using namespace nickel::tasks;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Function> void rejects(Function&& function, const char* message) {
    bool rejected{};
    try { function(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
std::string read(const fs::path& path) {
    const auto bytes = td::read_bounded(path, 1024 * 1024);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
struct OwnedRoot {
    fs::path path;
    td::FileIdentity identity;
    explicit OwnedRoot(const fs::path& parent) {
        td::ParentGuard pin(parent, true);
        path = fs::absolute(parent) / td::utf8_path("task-fixture-" + td::unique_id());
        td::create_directory_new(path); identity = td::directory_identity(path);
    }
    ~OwnedRoot() {
        try { td::remove_owned_tree(path, identity); }
        catch (const std::exception& error) { std::cerr << "fixture retained: " << error.what() << '\n'; }
    }
};
bool path_exists(const fs::path& path) { return fs::exists(td::io_path(path)); }
TaskSpec spec(const fs::path& target) { TaskSpec value; value.target = target; value.label = "fixture"; return value; }
void single_terminal(const Registry& registry, const std::string& id) {
    const auto delta = registry.since(0);
    require(!delta.reset_required, "terminal assertion needs retained events");
    const auto count = std::count_if(delta.events.begin(), delta.events.end(), [&](const Event& event) {
        return event.snapshot.id == id && event.kind == EventKind::Terminal;
    });
    require(count == 1, "task must emit exactly one terminal");
}
void replacement_tests(const fs::path& parent) {
    OwnedRoot root(parent);
    td::create_directory_new(root.path / "mods");
    const auto mods = root.path / "mods";
    Registry registry; nickel::Cancellation cancel;
    for (const auto suffix : {"", ".disabled", ".old"}) {
        const auto original = mods / td::utf8_path(std::string("same.jar") + suffix);
        td::write_atomic(original, "previous");
        const auto lease = lease_file(original, cancel);
        auto task = registry.begin(spec(mods));
        td::write_atomic(task->staging() / "next", "updated");
        task->commit_replacement(original, lease.sha256, "next", original);
        require(read(original) == "updated" && task->snapshot().status == TaskStatus::Succeeded, "same-name replacement failed");
        require(!path_exists(task->workspace()), "replacement cleanup pending unexpectedly");
    }
    for (const auto suffix : {".disabled", ".old"}) {
        const auto original = mods / td::utf8_path(std::string("old-version.jar") + suffix);
        const auto destination = mods / td::utf8_path(std::string("new-version.jar") + suffix);
        const auto retained = mods / td::utf8_path(std::string("saved-version.jar") + suffix);
        td::write_atomic(original, "original bytes");
        const auto previous = lease_file(original, cancel);
        auto reserved = spec(original); reserved.additional_targets = {destination, retained};
        auto task = registry.begin(reserved);
        require(registry.conflicts(destination) && registry.conflicts(retained), "additional replacement targets not reserved");
        td::write_atomic(task->staging() / "next", "new bytes");
        task->commit_files({PublishFile{"next", destination, previous, retained}});
        require(!path_exists(original) && read(destination) == "new bytes" && read(retained) == "original bytes", "renamed retained replacement failed");
    }
    {
        const auto original = mods / "changed.jar"; td::write_atomic(original, "identified");
        const auto previous = lease_file(original, cancel);
        { auto file = td::SecureFile::open_owned(original, previous.identity); file.truncate(0); file.write(std::as_bytes(std::span("mutated", 7))); file.flush(); }
        auto task = registry.begin(spec(mods)); td::write_atomic(task->staging() / "next", "must not publish");
        rejects([&] { task->commit_replacement(original, previous.sha256, "next", mods / "changed-next.jar"); }, "identification hash was replaced with current hash");
        require(read(original) == "mutated" && !path_exists(mods / "changed-next.jar"), "changed original was overwritten");
        task->rollback();
    }
    {
        const auto original = mods / "substituted.jar"; td::write_atomic(original, "same-content");
        const auto previous = lease_file(original, cancel);
        td::write_atomic(original, "same-content");
        auto task = registry.begin(spec(mods)); td::write_atomic(task->staging() / "next", "must not publish");
        rejects([&] { task->commit_files({PublishFile{"next", original, previous, {}}}); }, "substituted original identity was accepted");
        require(read(original) == "same-content", "substituted original was touched");
    }
    {
        const auto original = mods / "conflict.jar", destination = mods / "foreign.jar", retained = mods / "retained.jar";
        td::write_atomic(original, "keep original"); td::write_atomic(destination, "keep foreign");
        const auto previous = lease_file(original, cancel);
        auto task = registry.begin(spec(mods)); td::write_atomic(task->staging() / "next", "must not publish");
        rejects([&] { task->commit_files({PublishFile{"next", destination, previous, retained}}); }, "renamed target conflict was overwritten");
        require(read(original) == "keep original" && read(destination) == "keep foreign" && !path_exists(retained), "preflight conflict moved original");
    }
    {
        const auto original = mods / "unreserved.jar"; td::write_atomic(original, "keep");
        const auto previous = lease_file(original, cancel);
        auto task = registry.begin(spec(original)); td::write_atomic(task->staging() / "next", "must not publish");
        rejects([&] { task->commit_files({PublishFile{"next", mods / "outside-claim.jar", previous, {}}}); }, "unreserved renamed destination accepted");
        require(read(original) == "keep", "unreserved replacement changed original");
    }
    {
        const auto original = mods / "cancel.jar"; td::write_atomic(original, "keep");
        const auto previous = lease_file(original, cancel);
        nickel::Cancellation shared; auto reserved = spec(mods); reserved.cancellation = shared;
        auto task = registry.begin(reserved); td::write_atomic(task->staging() / "next", "must not publish");
        Registry independent;
        const auto active = independent.recover(root.path, cancel);
        require(active.size() == 1 && !active.front().recovered && path_exists(task->workspace()), "other registry removed locked active workspace");
        shared.cancel();
        rejects([&] { task->commit_replacement(original, previous.sha256, "next", original); }, "shared cancellation published replacement");
        require(read(original) == "keep", "cancelled replacement changed original");
        task->rollback();
    }
    std::cout << "PASS hash-guarded same/renamed replacement/disabled-old/retention/additional claims/stale hash+identity/conflict/shared cancel/active workspace lock\n";
}
bool same_receipt(const FileCommitReceipt& left, const FileCommitReceipt& right) {
    return left.step_id == right.step_id && left.published.destination == right.published.destination &&
        left.published.identity == right.published.identity && left.published.size == right.published.size &&
        left.published.sha256 == right.published.sha256 && left.final == right.final &&
        left.durability_pending == right.durability_pending;
}
void file_step_tests(const fs::path& parent) {
    OwnedRoot root(parent);
    Registry registry(128, 8192); nickel::Cancellation cancel;
    {
        const auto mods = root.path / "two-steps"; td::create_directory_new(mods);
        const auto original = mods / "first.jar"; td::write_atomic(original, "first old");
        const auto previous = lease_file(original, cancel);
        auto task = registry.begin(spec(mods)); const auto id = task->id();
        td::write_atomic(task->staging() / "first", "first next");
        const PublishFile first{"first", original, previous, {}};
        const auto receipt = task->commit_file_step("first", first, false);
        require(!receipt.final && !receipt.durability_pending && receipt.published.identity != previous.identity &&
            read(original) == "first next" && receipt.published.size == 10, "nonfinal real replacement receipt is incorrect");
        require(task->snapshot().status == TaskStatus::Running && task->snapshot().files_done == 1 &&
            registry.conflicts(mods) && path_exists(task->workspace()), "nonfinal step released task claims or became terminal");
        require(same_receipt(task->commit_file_step("first", first, false), receipt) &&
            task->committed_files().size() == 1, "identical nonfinal replay published twice");
        rejects([&] { task->commit_file_step("first", first, true); }, "nonfinal replay was promoted without a final real publication");
        auto changed = first; changed.destination = mods / "other.jar";
        rejects([&] { task->commit_file_step("first", changed, false); }, "changed replay target reused a stable step ID");
        changed = first; changed.previous->size += 1;
        rejects([&] { task->commit_file_step("first", changed, false); }, "changed replay lease reused a stable step ID");
        changed = first; changed.retain_previous = mods / "first.old";
        rejects([&] { task->commit_file_step("first", changed, false); }, "changed replay retention reused a stable step ID");
        rejects([&] { task->commit_files({}); }, "empty terminal commit bypassed incremental final publication");
        rejects([&] { task->complete_work(); }, "late work completion bypassed incremental final publication");
        rejects([&] { task->commit_directory(); }, "incremental and directory modes were mixed");
        rejects([&] { task->commit_external([] {}); }, "incremental and external modes were mixed");
        task->stage("download next"); task->files(900, 2);
        require(task->snapshot().files_done == 1 && task->snapshot().files_total == 2,
            "stage or producer progress erased/invented the actual committed prefix");
        td::write_atomic(task->staging() / "second", "second next");
        const PublishFile last{"second", mods / "nested" / "last.jar", {}, {}};
        const auto final_receipt = task->commit_file_step("second", last, true);
        require(final_receipt.final && !final_receipt.durability_pending && read(last.destination) == "second next" &&
            task->snapshot().status == TaskStatus::Succeeded && task->snapshot().files_done == 2,
            "last real publication did not succeed inside the same task");
        const auto committed_progress = [&] {
            const auto gate = nickel::transport::detail::cancellation_gate(task->cancellation());
            std::unique_lock lock(*gate, std::try_to_lock);
            require(lock.owns_lock(), "final committed reporting ran before gate release"); lock.unlock();
            require(!task->cancel() && !registry.cancel(id), "final committed progress cancellation was accepted");
            require(task->snapshot().status == TaskStatus::Succeeded, "last-progress cancellation relabeled durable success");
        };
        committed_progress();
        rejects([&] { committed_progress(); throw std::runtime_error("reporting callback failure"); }, "test reporting failure was not reached");
        task->rollback("report_failed");
        require(task->snapshot().status == TaskStatus::Succeeded && task->snapshot().files_done == 2 &&
            task->committed_files().size() == 2 && !path_exists(task->workspace()), "reporting rollback changed durable final outcome");
        require(same_receipt(task->commit_file_step("second", last, true), final_receipt), "final replay lost its historical receipt");
        single_terminal(registry, id); task.reset();
        require(read(original) == "first next" && read(last.destination) == "second next", "destructor restored a committed prefix");
    }
    for (bool direct : {false, true}) {
        const auto mods = root.path / (direct ? "task-cancel" : "ui-cancel"); td::create_directory_new(mods);
        auto task = registry.begin(spec(mods));
        td::write_atomic(task->staging() / "prefix", "durable prefix");
        const PublishFile first{"prefix", mods / "nested" / "prefix.jar", {}, {}};
        const auto receipt = task->commit_file_step("prefix", first, false);
        td::write_atomic(task->staging() / "suffix", "must not publish");
        const PublishFile last{"suffix", mods / "suffix.jar", {}, {}};
        require(direct ? task->cancel() : registry.cancel(task->id()), "nonfinal task cancellation was rejected");
        require(same_receipt(task->commit_file_step("prefix", first, false), receipt), "cancelled replay lost durable facts");
        rejects([&] { task->commit_file_step("suffix", last, true); }, "cancelled suffix published");
        require(read(first.destination) == "durable prefix" && !path_exists(last.destination) &&
            task->snapshot().status == TaskStatus::Cancelled && task->snapshot().files_done == 1 &&
            task->committed_files().size() == 1, "suffix cancellation restored or erased a committed prefix");
        task->rollback(); single_terminal(registry, task->id());
    }
    {
        const auto mods = root.path / "later-failure"; td::create_directory_new(mods);
        auto task = registry.begin(spec(mods)); td::write_atomic(task->staging() / "prefix", "committed");
        const auto receipt = task->commit_file_step("prefix", {"prefix", mods / "prefix.jar", {}, {}}, false);
        td::write_atomic(mods / "original.jar", "old"); const auto previous = lease_file(mods / "original.jar", cancel);
        td::write_atomic(mods / "original.jar", "externally replaced"); td::write_atomic(task->staging() / "suffix", "never");
        rejects([&] { task->commit_file_step("suffix", {"suffix", mods / "original.jar", previous, {}}, true); },
            "stale suffix revision was accepted");
        require(read(receipt.published.destination) == "committed" && read(mods / "original.jar") == "externally replaced" &&
            task->snapshot().status == TaskStatus::Failed && task->snapshot().files_done == 1,
            "later revision failure changed the committed prefix or foreign suffix");
        single_terminal(registry, task->id());
    }
    {
        const auto mods = root.path / "abandoned"; td::create_directory_new(mods);
        std::string id;
        {
            auto task = registry.begin(spec(mods)); id = task->id(); td::write_atomic(task->staging() / "prefix", "keep");
            task->commit_file_step("prefix", {"prefix", mods / "deep" / "keep.jar", {}, {}}, false);
        }
        require(read(mods / "deep" / "keep.jar") == "keep", "abandonment removed committed prefix directories");
        const auto snapshots = registry.snapshots();
        require(std::any_of(snapshots.begin(), snapshots.end(), [&](const Snapshot& snapshot) {
            return snapshot.id == id && snapshot.status == TaskStatus::Failed && snapshot.files_done == 1;
        }), "abandoned step task lost its truthful partial terminal snapshot");
        single_terminal(registry, id);
    }
    {
        auto task = registry.begin(spec(root.path / "legacy.jar")); td::write_atomic(task->staging() / "next", "legacy");
        task->commit_files({{"next", root.path / "legacy.jar", {}, {}}});
        require(task->snapshot().status == TaskStatus::Succeeded && task->committed_files().empty(), "legacy file commit became incremental");
        rejects([&] { task->commit_files({}); }, "legacy terminal commit became repeatable"); single_terminal(registry, task->id());
    }
    std::cout << "PASS same-Task file steps/two real publications/typed receipts/replay conflicts/durable prefix/cancelled suffix/final progress gate+cancel rejection/destructor/later revision failure/unchanged legacy terminal commit\n";
}
void completion_tests(const fs::path& parent) {
    OwnedRoot root(parent);
    Registry registry;
    {
        const auto target = root.path / "prepared-runtime";
        auto task = registry.begin(spec(target));
        task->stage("prepare"); task->files(1, 1);
        td::write_atomic(task->staging() / "scratch", "owned preparation bytes");
        task->complete_work();
        require(task->snapshot().status == TaskStatus::Succeeded && !path_exists(target) &&
                !path_exists(task->workspace()), "non-file completion published a fake target or leaked workspace");
        require(!registry.cancellation_for(task->id()), "terminal task exposed a cancellation token");
        rejects([&] { task->complete_work(); }, "work completed twice"); single_terminal(registry, task->id());
    }
    {
        const auto target = root.path / "domain-committed";
        td::create_directory_new(target); td::write_atomic(target / "keep", "domain data");
        auto task = registry.begin(spec(target));
        task->complete_work();
        require(read(target / "keep") == "domain data", "work completion changed domain publication");
        single_terminal(registry, task->id());
    }
    for (bool direct : {false, true}) {
        auto task = registry.begin(spec(root.path / (direct ? "direct-cancel" : "registry-cancel")));
        auto token = registry.cancellation_for(task->id());
        require(token && token->stop_token() == task->cancellation().stop_token(), "canonical cancellation token was not shared");
        bool callback{};
        std::stop_callback stop(token->stop_token(), [&] {
            const auto state = task->snapshot();
            require(state.status == TaskStatus::Cancelling, "cancel callback saw unmarked task");
            (void)registry.snapshots();
            require(registry.cancellation_for(task->id()).has_value(), "callback could not reenter token lookup");
            const auto gate = nickel::transport::detail::cancellation_gate(task->cancellation());
            std::unique_lock lock(*gate, std::try_to_lock);
            require(lock.owns_lock(), "stop callback ran under commit gate"); callback = true;
        });
        require(direct ? task->cancel() : registry.cancel(task->id()), "live task cancellation failed");
        require(callback && token->cancelled(), "stop callback did not run");
        rejects([&] { task->complete_work(); }, "cancelled work reported success");
        require(task->snapshot().status == TaskStatus::Cancelled, "cancelled work did not terminate cancelled");
        single_terminal(registry, task->id());
    }
    {
        auto task = registry.begin(spec(root.path / "queued-preflight"));
        auto token = registry.cancellation_for(task->id());
        require(token.has_value(), "active token lookup failed");
        token->cancel();
        require(task->cancellation().cancelled(), "preflight cancellation waited on queued command");
        rejects([&] { task->complete_work(); }, "preflight-cancelled work reported success");
        single_terminal(registry, task->id());
    }
    {
        auto task = registry.begin(spec(root.path / "busy-gate"));
        const auto gate = nickel::transport::detail::cancellation_gate(task->cancellation());
        std::unique_lock lock(*gate);
        std::jthread worker([&] {
            const auto start = std::chrono::steady_clock::now();
            require(registry.cancellation_for(task->id()).has_value(), "lookup waited on commit gate");
            require(!registry.cancel(task->id()), "busy gate cancellation did not fail without blocking");
            require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "preflight cancellation blocked");
        });
        worker.join(); lock.unlock(); task->complete_work(); single_terminal(registry, task->id());
    }
    require(!registry.cancellation_for("unknown"), "unknown id exposed a token");
    std::cout << "PASS real work completion/no fake publication/domain preservation/canonical preflight token/reentrant callbacks/outside locks/nonblocking gate\n";
}
void external_publication_tests(const fs::path& parent) {
    OwnedRoot root(parent);
    Registry registry;
    {
        const auto target = root.path / "real-domain";
        auto task = registry.begin(spec(target));
        const auto id = task->id();
        auto late_cancel = registry.cancellation_for(id);
        require(late_cancel.has_value(), "external commit did not expose its preparation token");
        task->commit_external([&] {
            require(task->snapshot().status == TaskStatus::Committing, "external callback ran before commit boundary");
            require(!registry.cancellation_for(id), "committing task exposed new cancellation token");
            (void)registry.snapshots();
            std::atomic<bool> rejected{};
            std::jthread contender([&] {
                const auto start = std::chrono::steady_clock::now();
                rejected = !registry.cancel(id) && !task->cancel() &&
                    std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250);
            });
            contender.join(); require(rejected, "publication cancellation blocked or was accepted");
            td::create_directory_new(target); td::write_atomic(target / "published", "durable domain bytes");
            late_cancel->cancel();
        });
        require(task->snapshot().status == TaskStatus::Succeeded && read(target / "published") == "durable domain bytes",
                "late cancellation relabeled durable external publication");
        require(!path_exists(task->workspace()) && !registry.cancel(id), "external commit leaked workspace or stayed cancellable");
        task->rollback("consumer_cleanup_failed", "must not override durable success");
        require(task->snapshot().status == TaskStatus::Succeeded, "rollback relabeled durable external publication");
        rejects([&] { task->complete_work(); }, "externally committed task completed twice");
        task.reset();
        const auto history = registry.snapshots();
        require(std::any_of(history.begin(), history.end(), [&](const auto& entry) {
            return entry.id == id && entry.status == TaskStatus::Succeeded;
        }) && read(target / "published") == "durable domain bytes", "destructor changed durable domain success");
        single_terminal(registry, id);
    }
    for (bool raw : {false, true}) {
        auto task = registry.begin(spec(root.path / (raw ? "raw-pre-cancelled-domain" : "pre-cancelled-domain")));
        if (raw) { auto token = registry.cancellation_for(task->id()); require(token.has_value(), "missing canonical token"); token->cancel(); }
        else require(task->cancel(), "pre-publication cancellation failed");
        bool called{};
        rejects([&] { task->commit_external([&] { called = true; }); }, "cancelled external callback ran");
        require(!called && task->snapshot().status == TaskStatus::Cancelled && !path_exists(task->snapshot().target),
                "cancelled external publication was not terminal");
        single_terminal(registry, task->id());
    }
    {
        auto task = registry.begin(spec(root.path / "failed-domain"));
        rejects([&] { task->commit_external([] { throw std::runtime_error("domain failed before publication"); }); }, "external failure was swallowed");
        require(task->snapshot().status == TaskStatus::Failed && !path_exists(task->snapshot().target), "external failure invented success or target");
        single_terminal(registry, task->id());
    }
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
    for (const auto* point : {"completion.journal", "external.intent", "external.committed", "cleanup"}) {
        const auto target = root.path / point;
        auto task = registry.begin(spec(target));
        const auto workspace = task->workspace();
        bool published{};
        td::testing::checkpoint = [point](auto reached, const auto&) {
            if (reached == point) throw std::runtime_error("injected completion I/O failure");
        };
        try {
            if (std::string_view(point) == "completion.journal")
                rejects([&] { task->complete_work(); }, "failed completion marker was reported as success");
            else if (std::string_view(point) == "external.intent")
                rejects([&] { task->commit_external([&] { published = true; }); }, "failed external intent allowed publication");
            else task->commit_external([&] {
                td::create_directory_new(target); td::write_atomic(target / "published", "durable"); published = true;
            });
        } catch (...) { td::testing::checkpoint = {}; throw; }
        td::testing::checkpoint = {};
        const auto state = task->snapshot();
        if (std::string_view(point) == "completion.journal" || std::string_view(point) == "external.intent") {
            require(!published && state.status == TaskStatus::Failed && state.error_code != "cleanup_pending",
                    "pre-durable failure was confused with committed cleanup");
        } else {
            require(published && state.status == TaskStatus::Succeeded && read(target / "published") == "durable",
                    "post-publication maintenance failure changed domain facts");
            task->rollback();
            require(task->snapshot().status == TaskStatus::Succeeded && path_exists(workspace), "rollback removed pending committed evidence");
            nickel::Cancellation recover_cancel;
            const auto recovery = registry.recover(root.path, recover_cancel);
            require(recovery.size() == 1, "post-publication workspace recovery missing");
            if (std::string_view(point) == "external.committed") {
                require(state.error_code == "persistence_pending" && !recovery.front().recovered && path_exists(workspace) &&
                        recovery.front().error == "Domain owner must resolve the external publication outcome",
                        "ambiguous external outcome was assumed recovered");
                td::remove_owned_tree(workspace, td::directory_identity(workspace));
            } else require(state.error_code == "cleanup_pending" && recovery.front().recovered && recovery.front().committed,
                           "durable cleanup-only failure was not recoverable");
        }
        single_terminal(registry, task->id());
    }
#endif
    std::cout << "PASS external pre-cancel/raw-token/commit-gate nonblocking registry+task cancel/late cancellation/rollback+destructor/one terminal/pre-durable failure/persistence versus cleanup/ambiguous recovery\n";
}
void run(const fs::path& parent) {
    OwnedRoot root(parent);
    nickel::Cancellation token;
    const auto copy = token;
    require(nickel::transport::detail::cancellation_gate(token) ==
            nickel::transport::detail::cancellation_gate(copy), "copied cancellation gates differ");
    token.cancel(); require(copy.cancelled(), "common cancellation copy does not share state");
    rejects([&] { copy.check(); }, "cancelled check did not throw");
    nickel::Cancellation alive;
    Registry registry(128, 65536);
    {
        auto task = registry.begin(spec(root.path / "new-dir"));
        td::write_atomic(task->staging() / "entry.txt", "new data");
        require(registry.conflicts(root.path / "new-dir" / "child"), "ancestor claim is not exclusive");
        // Case-distinct descendant claims conflict exactly when the host volume folds
        // names (Windows NTFS, macOS APFS default); byte-exact volumes (Linux ext4,
        // case-sensitive APFS) keep them distinct.
        const bool volume_folds = !td::volume_case_sensitive(root.path);
        require(registry.conflicts(root.path / "NEW-DIR" / "child") == volume_folds,
                "case-distinct descendant claim exclusivity must follow volume semantics");
        rejects([&] { registry.begin(spec(root.path / "new-dir")); }, "duplicate target claim accepted");
        const auto active_recovery = registry.recover(root.path, alive);
        require(active_recovery.size() == 1 && !active_recovery.front().recovered, "recovery deleted an active task");
        task->commit_directory();
        require(task->snapshot().status == TaskStatus::Succeeded, "directory commit not successful");
        require(read(root.path / "new-dir" / "entry.txt") == "new data", "directory commit lost data");
        require(!path_exists(task->workspace()), "successful transaction workspace retained");
        require(!task->cancel() && !registry.cancel(task->id()), "terminal task accepted cancellation");
        rejects([&] { task->commit_directory(); }, "terminal task committed twice");
        single_terminal(registry, task->id());
    }
    {
        td::create_directory_new(root.path / "existing-dir");
        td::write_atomic(root.path / "existing-dir" / "keep.txt", "foreign data");
        auto task = registry.begin(spec(root.path / "existing-dir"));
        td::write_atomic(task->staging() / "new.txt", "new data");
        rejects([&] { task->commit_directory(); }, "directory commit overwrote foreign target");
        require(read(root.path / "existing-dir" / "keep.txt") == "foreign data", "foreign directory changed");
        require(task->snapshot().status == TaskStatus::Failed, "conflict task not terminal failed");
        single_terminal(registry, task->id());
    }
    {
        auto task = registry.begin(spec(root.path / "cancelled-dir"));
        td::write_atomic(task->staging() / "new.txt", "cancelled data");
        require(registry.cancel(task->id()), "registry cancel failed");
        rejects([&] { task->commit_directory(); }, "cancelled task published");
        require(!path_exists(root.path / "cancelled-dir"), "cancelled directory exists");
        require(task->snapshot().status == TaskStatus::Cancelled, "cancelled task has wrong terminal");
        single_terminal(registry, task->id());
    }
    {
        td::create_directory_new(root.path / "files");
        auto task = registry.begin(spec(root.path / "files"));
        td::write_atomic(task->staging() / "one.bin", "one");
        td::write_atomic(task->staging() / "two.bin", "two");
        task->commit_files({{"one.bin", root.path / "files" / "one.bin"}, {"two.bin", root.path / "files" / "two.bin"}});
        require(read(root.path / "files" / "one.bin") == "one", "file commit missing first");
        require(read(root.path / "files" / "two.bin") == "two", "file commit missing second");
        single_terminal(registry, task->id());
    }
    {
        auto task = registry.begin(spec(root.path / "files"));
        td::write_atomic(task->staging() / "new.bin", "must not publish");
        td::write_atomic(task->staging() / "old.bin", "must not replace");
        rejects([&] { task->commit_files({{"new.bin", root.path / "files" / "new.bin"},
            {"old.bin", root.path / "files" / "one.bin"}}); }, "file batch overwrote existing destination");
        require(!path_exists(root.path / "files" / "new.bin"), "preflight conflict partially published");
        require(read(root.path / "files" / "one.bin") == "one", "existing file replaced");
    }
    {
        auto task = registry.begin(spec(root.path / "events"));
        task->file({"a", 1, 10, false}); task->file({"b", 2, 10, false}); task->file({"a", 3, 10, false});
        const auto delta = registry.since(0);
        require(delta.events.back().kind == EventKind::File, "last event not file delta");
        require(delta.events.back().snapshot.recent_files.size() == 1 &&
            delta.events.back().snapshot.recent_files.front().name == "a" &&
            delta.events.back().snapshot.recent_files.front().completed_bytes == 3, "file event repeated wrong file");
        std::vector<std::jthread> threads;
        for (int i = 0; i < 4; ++i) threads.emplace_back([task] {
            for (int item = 0; item < 100; ++item) task->files(static_cast<std::uint64_t>(item));
        });
        threads.clear(); task->rollback();
        std::uint64_t revision{};
        bool terminal_seen{};
        for (const auto& event : registry.since(0).events) {
            if (event.snapshot.id != task->id()) continue;
            require(!terminal_seen && event.snapshot.revision > revision, "event revision regressed or followed terminal");
            revision = event.snapshot.revision;
            terminal_seen = event.kind == EventKind::Terminal;
        }
        require(terminal_seen, "no terminal after concurrent progress");
    }
    for (int i = 0; i < 40; ++i) {
        auto task = registry.begin(spec(root.path / td::utf8_path("race-" + std::to_string(i))));
        td::write_atomic(task->staging() / "data", "race data");
        std::barrier start(3);
        std::atomic<bool> cancelled{}, committed{};
        std::jthread cancel_thread([&] { start.arrive_and_wait(); cancelled = task->cancel(); });
        std::jthread commit_thread([&] {
            start.arrive_and_wait();
            try { task->commit_directory(); committed = true; } catch (const std::exception&) { }
        });
        start.arrive_and_wait(); cancel_thread.join(); commit_thread.join();
        require(cancelled.load() != committed.load(), "cancel and commit both won or both lost");
        require(path_exists(task->snapshot().target) == committed.load(), "race publication differs from terminal result");
        single_terminal(registry, task->id());
    }
    {
        Registry small(2, 4);
        auto task = small.begin(spec(root.path / "bounded-events"));
        for (int i = 0; i < 20; ++i) task->files(static_cast<std::uint64_t>(i));
        const auto delta = small.since(0);
        require(delta.reset_required && delta.snapshots.size() == 1, "wrapped event cursor did not reset");
        task->rollback();
    }
    std::cout << "PASS common copy/claim exclusivity/no-overwrite/directory+file commit/cancel-before-publish/40 cancel races/incremental revisions/bounded events\n";
}
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
void crash_commit(const fs::path& root, std::string point, bool renamed) {
    Registry registry; nickel::Cancellation cancel;
    const auto original = root / "original.jar", destination = renamed ? root / "updated.jar" : original;
    const auto retained = root / "original.jar.old";
    td::write_atomic(original, "original data");
    const auto previous = lease_file(original, cancel);
    auto reserved = spec(original); reserved.additional_targets = {retained};
    if (renamed) reserved.additional_targets.push_back(destination);
    auto task = registry.begin(reserved);
    td::write_atomic(task->staging() / "next", "updated data");
    td::testing::checkpoint = [point](std::string_view reached, const fs::path&) {
        if (reached == point) std::_Exit(0);
        if (point.starts_with("rollback.") && reached == "files.retained") throw std::runtime_error("test rollback");
    };
    task->commit_files({PublishFile{"next", destination, previous, retained}});
    throw std::runtime_error("requested crash checkpoint not reached");
}
void recover_commit(const fs::path& root, bool committed, bool renamed) {
    Registry registry; nickel::Cancellation cancel;
    const auto entries = registry.recover(root, cancel);
    require(entries.size() == 1 && entries.front().recovered && entries.front().committed == committed, "half-commit journal recovery failed");
    const auto original = root / "original.jar", destination = renamed ? root / "updated.jar" : original;
    if (committed) {
        require(read(destination) == "updated data" && read(root / "original.jar.old") == "original data", "committed recovery lost data");
        if (renamed) require(!path_exists(original), "committed renamed recovery restored original unexpectedly");
    } else {
        require(read(original) == "original data" && !path_exists(root / "original.jar.old"), "uncommitted recovery did not restore original");
        if (renamed) require(!path_exists(destination), "uncommitted renamed recovery retained publication");
    }
    std::cout << "PASS actual half-commit/backup/retain/rollback crash recovery\n";
}
void failure_tests(const fs::path& parent) {
    OwnedRoot root(parent);
    Registry registry; nickel::Cancellation cancel;
    for (const auto point : {"files.backed_up", "files.published", "files.retained"}) {
        const auto original = root.path / "original.jar";
        td::write_atomic(original, "original data");
        const auto previous = lease_file(original, cancel);
        auto reserved = spec(root.path / "updated.jar"); reserved.additional_targets = {original, root.path / "retained.old"};
        auto task = registry.begin(reserved); td::write_atomic(task->staging() / "next", "updated data");
        td::testing::checkpoint = [point](std::string_view reached, const fs::path&) {
            if (reached == point) throw std::runtime_error("injected test I/O failure");
        };
        rejects([&] { task->commit_files({PublishFile{"next", root.path / "updated.jar", previous, root.path / "retained.old"}}); }, "injected failure returned success");
        td::testing::checkpoint = {};
        require(read(original) == "original data" && !path_exists(root.path / "updated.jar") && !path_exists(root.path / "retained.old"), "failed replacement did not reverse publication");
        require(!path_exists(task->workspace()), "reversed replacement did not clean workspace");
    }
    {
        const auto destination = root.path / "cleanup.bin";
        auto task = registry.begin(spec(destination)); td::write_atomic(task->staging() / "next", "committed");
        td::testing::checkpoint = [](std::string_view reached, const fs::path&) {
            if (reached == "cleanup") throw std::runtime_error("injected cleanup failure");
        };
        task->commit_files({PublishFile{"next", destination, {}, {}}});
        td::testing::checkpoint = {};
        require(task->snapshot().status == TaskStatus::Succeeded && task->snapshot().error_code == "cleanup_pending", "cleanup failure erased commit");
        const auto entries = registry.recover(root.path, cancel);
        require(entries.size() == 1 && entries.front().committed && entries.front().recovered && read(destination) == "committed", "committed cleanup recovery failed");
    }
    td::testing::checkpoint = {};
    std::cout << "PASS actual injected backup/publish/retain failure reversal/committed cleanup recovery\n";
}
struct HookReset {
    ~HookReset() { td::testing::checkpoint = {}; }
};
void file_step_fault_tests(const fs::path& parent) {
    OwnedRoot root(parent); Registry registry(128, 8192); nickel::Cancellation cancel; HookReset reset;
    for (bool direct : {false, true}) {
        const auto mods = root.path / (direct ? "prepare-direct" : "prepare-ui"); td::create_directory_new(mods);
        auto task = registry.begin(spec(mods)); td::write_atomic(task->staging() / "next", "never published");
        bool prepared{};
        td::testing::checkpoint = [&](std::string_view reached, const fs::path&) {
            if (reached != "files.prepared") return;
            const auto gate = nickel::transport::detail::cancellation_gate(task->cancellation());
            std::unique_lock lock(*gate, std::try_to_lock);
            require(lock.owns_lock(), "preparation and rehash ran under the commit gate"); lock.unlock();
            require(task->snapshot().status == TaskStatus::Running, "preparation exposed premature Committing");
            std::jthread worker([&] { prepared = direct ? task->cancel() : registry.cancel(task->id()); }); worker.join();
        };
        rejects([&] { task->commit_file_step("next", {"next", mods / "next.jar", {}, {}}, true); }, "prepare cancellation published");
        td::testing::checkpoint = {};
        require(prepared && task->snapshot().status == TaskStatus::Cancelled && task->committed_files().empty() &&
            !path_exists(mods / "next.jar"), "pre-gate cancellation did not stop a real file step");
        single_terminal(registry, task->id());
    }
    for (bool missing : {false, true}) {
        const auto mods = root.path / (missing ? "first-missing" : "first-cancel"); td::create_directory_new(mods);
        auto task = registry.begin(spec(mods));
        if (!missing) td::write_atomic(task->staging() / "next", "never published");
        td::testing::checkpoint = [&](std::string_view reached, const fs::path&) {
            if (reached == "files.prepared") require(task->cancel(), "first-step cancellation failed");
            if (reached == "cleanup") throw std::runtime_error("retain failed preparation journal");
        };
        rejects([&] { task->commit_file_step("next", {"next", mods / "next.jar", {}, {}}, true); },
            "first-step preparation failure returned success");
        td::testing::checkpoint = {};
        require(task->committed_files().empty() && task->snapshot().files_done == 0 && path_exists(task->workspace()),
            "preparation-only failure invented a committed prefix or lost its retained journal");
        const auto entries = registry.recover(root.path, cancel);
        require(entries.size() == 1 && entries.front().recovered && !entries.front().committed &&
            entries.front().committed_files.empty() && !path_exists(mods / "next.jar"),
            "first-step retained preparation journal could not recover without a committed prefix");
        single_terminal(registry, task->id());
    }
    {
        const auto mods = root.path / "final-gate"; td::create_directory_new(mods);
        auto task = registry.begin(spec(mods)); auto raw = registry.cancellation_for(task->id());
        require(raw.has_value(), "preparation token missing"); td::write_atomic(task->staging() / "next", "durable");
        bool reached{};
        td::testing::checkpoint = [&](std::string_view point, const fs::path&) {
            if (point != "files.published") return;
            require(task->snapshot().status == TaskStatus::Committing && !registry.cancellation_for(task->id()),
                "publication did not hold its canonical Task boundary");
            std::jthread worker([&] { reached = !task->cancel() && !registry.cancel(task->id()); }); worker.join();
            raw->cancel();
        };
        const auto receipt = task->commit_file_step("next", {"next", mods / "next.jar", {}, {}}, true);
        td::testing::checkpoint = {};
        require(reached && !receipt.durability_pending && task->snapshot().status == TaskStatus::Succeeded &&
            read(receipt.published.destination) == "durable", "late stop undid final actual publication");
        single_terminal(registry, task->id());
    }
    for (const auto point : {"files.planned", "files.backed_up", "files.published"}) {
        const auto mods = root.path / td::utf8_path(std::string("suffix-") + point); td::create_directory_new(mods);
        auto task = registry.begin(spec(mods)); td::write_atomic(task->staging() / "prefix", "prefix");
        task->commit_file_step("prefix", {"prefix", mods / "prefix" / "keep.jar", {}, {}}, false);
        const auto original = mods / "original.jar"; td::write_atomic(original, "old"); const auto previous = lease_file(original, cancel);
        td::write_atomic(task->staging() / "next", "next");
        td::testing::checkpoint = [point](std::string_view reached, const fs::path&) {
            if (reached == point) throw 7;
        };
        bool rejected{};
        try { task->commit_file_step("next", {"next", mods / "next.jar", previous, mods / "retained.jar"}, true); }
        catch (const nickel::ErrorException& error) { rejected = error.error().code == "transaction_failed"; }
        td::testing::checkpoint = {};
        require(rejected && task->snapshot().status == TaskStatus::Failed && task->snapshot().files_done == 1 &&
            read(mods / "prefix" / "keep.jar") == "prefix" && read(original) == "old" &&
            !path_exists(mods / "next.jar") && !path_exists(mods / "retained.jar"),
            "uncommitted suffix failure did not reverse only its own namespace changes");
        single_terminal(registry, task->id());
    }
    for (bool final : {false, true}) for (const auto point : {"files.published", "step.marker", "step.committed", "cleanup"}) {
        if (!final && std::string_view(point) == "cleanup") continue;
        const auto mods = root.path / td::utf8_path(std::string(final ? "final-" : "partial-") + point);
        td::create_directory_new(mods); auto task = registry.begin(spec(mods));
        td::write_atomic(task->staging() / "prefix", "prefix");
        task->commit_file_step("prefix", {"prefix", mods / "prefix.jar", {}, {}}, false);
        td::write_atomic(task->staging() / "next", "durable next");
        const PublishFile next{"next", mods / "next.jar", {}, {}};
        td::testing::checkpoint = [point](std::string_view reached, const fs::path&) {
            if (reached == point) throw 9;
        };
        const auto receipt = task->commit_file_step("next", next, final);
        td::testing::checkpoint = {};
        const bool pending = std::string_view(point) == "files.published" || std::string_view(point) == "step.marker";
        require(receipt.durability_pending == pending && receipt.final == final && task->committed_files().size() == 2 &&
            task->snapshot().files_done == 2 && read(mods / "next.jar") == "durable next" && read(mods / "prefix.jar") == "prefix",
            "post-publication fault erased durable facts or receipt");
        require(task->snapshot().status == (final ? TaskStatus::Succeeded : (pending ? TaskStatus::Failed : TaskStatus::Running)),
            "post-publication fault assigned the wrong whole-task outcome");
        require(same_receipt(task->commit_file_step("next", next, final), receipt), "pending/reported step replay changed facts");
        if (pending) {
            require(task->snapshot().error_code == "persistence_pending", "post-publication persistence fault became cleanup-only");
            if (!final) rejects([&] { task->commit_file_step("suffix", {"absent", mods / "absent.jar", {}, {}}, true); },
                "durability-pending nonfinal task allowed the suffix");
        }
        if (pending || std::string_view(point) == "cleanup") {
            const auto entries = registry.recover(root.path, cancel);
            require(entries.size() == 1 && entries.front().recovered && entries.front().committed == final &&
                entries.front().committed_files.size() == 2 && read(mods / "next.jar") == "durable next",
                "recovery restored a post-publication file step or lost its durable prefix");
        } else if (!final) task->rollback();
        single_terminal(registry, task->id());
    }
    std::cout << "PASS real file-step preparation outside gate/Task+UI cancellation/gate late-stop rejection/opaque suffix reversal/marker+cleanup faults/idempotent pending receipts/recovery prefix\n";
}
void crash_file_step(const fs::path& root, const std::string& point, std::string_view mode) {
    Registry registry; nickel::Cancellation cancel;
    const auto mods = root / "mods"; td::create_directory_new(mods);
    const auto original = mods / "original.jar", destination = mode == "renamed" ? mods / "updated.jar" : original;
    const auto retained = mode == "discard" ? std::optional<fs::path>{} : std::optional<fs::path>{mods / "retained.jar"};
    td::write_atomic(original, "old"); const auto previous = lease_file(original, cancel);
    auto task = registry.begin(spec(mods)); td::write_atomic(task->staging() / "prefix", "committed prefix");
    task->commit_file_step("prefix", {"prefix", mods / "prefix" / "keep.jar", {}, {}}, false);
    if (point == "step.first") std::_Exit(0);
    td::write_atomic(task->staging() / "next", "committed next");
    td::testing::checkpoint = [point](std::string_view reached, const fs::path&) {
        if (reached == point) std::_Exit(0);
        if (point == "rollback.next" && reached == "files.published") throw std::runtime_error("rollback incomplete suffix");
        if (point == "rollback.previous" && reached == "files.backed_up") throw std::runtime_error("rollback backed-up suffix");
    };
    task->commit_file_step("next", {"next", destination, previous, retained}, point != "step.nonfinal");
    if (point == "step.nonfinal") std::_Exit(0);
    throw std::runtime_error("requested file-step crash checkpoint not reached");
}
void recover_file_step(const fs::path& root, std::string_view point, std::string_view mode) {
    Registry registry; nickel::Cancellation cancel; const auto entries = registry.recover(root, cancel);
    const bool next_committed = point == "files.retained" || point == "step.marker" || point == "step.committed" ||
        point == "cleanup" || point == "step.nonfinal" || (point == "files.published" && mode == "discard");
    const bool final = next_committed && point != "step.nonfinal";
    require(entries.size() == 1 && entries.front().recovered && entries.front().committed == final &&
        entries.front().committed_files.size() == (next_committed ? 2U : 1U), "crashed file-step recovery lost partial/final receipts");
    const auto mods = root / "mods", original = mods / "original.jar";
    const auto destination = mode == "renamed" ? mods / "updated.jar" : original;
    require(read(mods / "prefix" / "keep.jar") == "committed prefix", "crash recovery restored or removed a durable prefix");
    const auto& prefix = entries.front().committed_files.front();
    require(prefix.step_id == "prefix" && !prefix.final && prefix.published.destination == mods / "prefix" / "keep.jar",
        "recovered prefix receipt mismatches its actual publication");
    if (next_committed) {
        require(read(destination) == "committed next", "completed next step was restored after a crash");
        if (mode != "discard") require(read(mods / "retained.jar") == "old", "completed retention was lost after a crash");
        if (mode == "renamed") require(!path_exists(original), "renamed completed next step restored original");
        require(entries.front().committed_files.back().final == final, "recovered final step marker differs from actual task outcome");
    } else {
        require(read(original) == "old" && !path_exists(mods / "retained.jar"), "incomplete next step was not restored after a crash");
        if (mode == "renamed") require(!path_exists(destination), "incomplete renamed next step remained published");
    }
    std::cout << "PASS actual file-step crash recovery prefix=" << entries.front().committed_files.size() << " final=" << final << '\n';
}
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--steps") {
            const auto parent = td::utf8_path(argv[2]); file_step_tests(parent);
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
            file_step_fault_tests(parent);
#endif
            return 0;
        }
        if (argc == 3 && (std::string_view(argv[1]) == "--completion" ||
                          std::string_view(argv[1]) == "--completion-only")) {
            const auto parent = td::utf8_path(argv[2]);
            completion_tests(parent); external_publication_tests(parent); return 0;
        }
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
        if (argc == 5 && (std::string_view(argv[1]) == "--crash-step" || std::string_view(argv[1]) == "--recover-step")) {
            const auto root = td::checked_absolute(td::utf8_path(argv[2])); const std::string_view mode = argv[4];
            require(mode == "same" || mode == "renamed" || mode == "discard", "invalid file-step crash mode");
            if (std::string_view(argv[1]) == "--crash-step") crash_file_step(root, argv[3], mode);
            else recover_file_step(root, argv[3], mode);
            return 0;
        }
        if (argc == 5 && std::string_view(argv[1]) == "--crash-commit") {
            crash_commit(td::checked_absolute(td::utf8_path(argv[2])), argv[3], std::string_view(argv[4]) == "renamed");
            return 0;
        }
        if (argc == 5 && std::string_view(argv[1]) == "--recover-commit") {
            recover_commit(td::checked_absolute(td::utf8_path(argv[2])), std::string_view(argv[3]) == "committed", std::string_view(argv[4]) == "renamed");
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--recovery-conflict") {
            Registry registry; nickel::Cancellation cancel;
            const auto root = td::checked_absolute(td::utf8_path(argv[2]));
            const auto entries = registry.recover(root, cancel);
            require(entries.size() == 1 && !entries.front().recovered && path_exists(entries.front().workspace), "recovery touched externally changed file");
            std::cout << "PASS externally modified/substituted recovery is retained\n"; return 0;
        }
#endif
        if (argc == 3 && std::string_view(argv[1]) == "--crash") {
            Registry registry;
            const auto root = td::checked_absolute(td::utf8_path(argv[2]));
            auto task = registry.begin(spec(root / "crash-target"));
            td::write_atomic(task->staging() / "uncommitted", "interrupted data");
            std::_Exit(0);
        }
        if (argc == 3 && std::string_view(argv[1]) == "--recover") {
            Registry registry; nickel::Cancellation cancel;
            const auto root = td::checked_absolute(td::utf8_path(argv[2]));
            const auto entries = registry.recover(root, cancel);
            require(entries.size() == 1 && entries.front().recovered && !entries.front().committed, "crash journal recovery failed");
            require(!path_exists(root / "crash-target"), "uncommitted crash target published");
            std::cout << "PASS process interruption recovery\n"; return 0;
        }
        if (argc > 1 && std::string_view(argv[1]).starts_with("--"))
            throw std::runtime_error("unknown task test mode");
        const auto parent = argc > 1 ? td::utf8_path(argv[1]) : fs::path("native/tests/transport/build-probe");
        run(parent); replacement_tests(parent); completion_tests(parent); external_publication_tests(parent);
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
        failure_tests(parent);
#endif
        return 0;
    } catch (const nickel::ErrorException& failure) {
        const auto& error = failure.error();
        std::cerr << "FAIL task runtime: " << error.code << ": " << error.message;
        const auto native = error.details.find("nativeError");
        if (native != error.details.end() && native->is_number_integer())
            std::cerr << " nativeError=" << native->get<std::int64_t>();
        std::cerr << '\n'; return 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL task runtime: " << error.what() << '\n'; return 1;
    }
}
