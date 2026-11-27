#include "core/tasks/tasks.hpp"
#include "core/tasks/secure_fs.hpp"
#include "core/network/control.hpp"
#include "core/network/errors.hpp"
#include "core/tasks/test_hooks.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace nickel::tasks {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t recent_file_limit = 128;
constexpr std::size_t max_publish_files = 100000;
constexpr std::uint64_t journal_read_limit = 64ULL * 1024 * 1024;
std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
bool terminal(TaskStatus status) {
    return status == TaskStatus::Succeeded || status == TaskStatus::Cancelled || status == TaskStatus::Failed;
}
Json identity_json(detail::FileIdentity value) { return Json{{"volume", value.volume}, {"index", value.index}}; }
detail::FileIdentity read_identity(const Json& value) {
    return {value.at("volume").get<std::uint64_t>(), value.at("index").get<std::uint64_t>()};
}
std::string bytes_text(const std::vector<std::byte>& value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
void write_json(const fs::path& path, const Json& value) {
    const auto text = value.dump();
    if (text.size() > journal_read_limit) throw std::runtime_error("transaction journal exceeds limit");
    detail::write_atomic(path, text);
}
fs::path target_path(const fs::path& input) {
    const auto path = detail::checked_absolute(input);
    const auto relative = path.lexically_relative(path.root_path());
    (void)detail::validate_relative(detail::path_utf8(relative));
    return path;
}
struct Record {
    mutable std::mutex mutex;
    std::shared_ptr<Cancellation> cancel;
    std::shared_ptr<std::mutex> commit_gate;
    Snapshot snapshot;
    std::vector<fs::path> targets;
    std::vector<FileCommitReceipt> committed_files;
    bool incremental_files{};
};
std::optional<Cancellation> prepare_cancel(const std::shared_ptr<Record>& record, Snapshot& snapshot) {
    std::unique_lock gate(*record->commit_gate, std::try_to_lock);
    if (!gate.owns_lock()) return {};
    std::lock_guard lock(record->mutex);
    if (terminal(record->snapshot.status) || record->snapshot.status == TaskStatus::Committing) return {};
    snapshot = record->snapshot;
    snapshot.cancel_requested = true; snapshot.status = TaskStatus::Cancelling; ++snapshot.revision;
    record->snapshot = snapshot;
    return *record->cancel;
}
struct Published {
    fs::path source;
    fs::path destination;
    detail::FileIdentity identity;
    std::uint64_t size{};
    std::string sha256;
    bool moved{};
    std::optional<FileLease> previous;
    fs::path backup;
    bool backed_up{};
    std::optional<fs::path> retained;
    bool retained_moved{};
    std::string step_id;
    bool step_final{};
    bool step_committed{};
    bool durability_pending{};
};
struct CreatedDirectory {
    fs::path path;
    std::optional<detail::FileIdentity> identity;
    std::string step_id;
    bool step_committed{};
};
bool valid_step_id(std::string_view id) {
    return !id.empty() && id.size() <= 128 && id != "." && id != ".." &&
        std::all_of(id.begin(), id.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        });
}
FileCommitReceipt receipt_of(const Published& file) {
    return {file.step_id, {file.destination, file.identity, file.size, file.sha256},
        file.step_final, file.durability_pending};
}
void publish_locked(detail::SecureFile& file, const fs::path& destination, bool& moved) {
    try { file.publish_no_replace(destination); }
    catch (...) {
        // The retained handle changes its own path only after native rename succeeds.
        moved = file.path() == destination;
        throw;
    }
    moved = true;
}
std::string hash_handle(detail::SecureFile& file, const transport::detail::CancelCheck& cancelled) {
    transport::detail::IncrementalHash digest(transport::HashAlgorithm::Sha256);
    std::array<std::byte, 64 * 1024> buffer;
    file.seek(0);
    for (;;) {
        transport::detail::check_cancel(cancelled);
        const auto count = file.read(buffer);
        if (!count) break;
        digest.update(std::span(buffer).first(count));
    }
    file.seek(0);
    return digest.finish();
}
void check_lease(detail::SecureFile& file, const FileLease& lease,
                 const transport::detail::CancelCheck& cancelled) {
    if (file.identity() != lease.identity || file.size() != lease.size ||
        !transport::detail::digest_equal(hash_handle(file, cancelled), lease.sha256))
        transport::detail::fail("revision_conflict", "File changed since its replacement lease was acquired");
}
void verify_tree(const fs::path& root, const transport::detail::CancelCheck& cancelled) {
    detail::ParentGuard pin(root);
    std::uint64_t count{};
    for (const auto& entry : fs::recursive_directory_iterator(detail::io_path(root))) {
        transport::detail::check_cancel(cancelled);
        if (++count > 200000) throw std::runtime_error("staged tree entry limit exceeded");
        const auto path = detail::logical_path(entry.path());
        (void)detail::validate_relative(detail::path_utf8(path.lexically_relative(root)));
        detail::reject_links(path);
        if (entry.is_regular_file()) {
            auto file = detail::SecureFile::open_read(path);
            (void)file.identity();
        } else if (!entry.is_directory()) throw std::runtime_error("staged tree contains special files");
    }
}
} // namespace

struct Registry::Impl {
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<Record>> active;
    std::deque<Snapshot> history;
    std::deque<Event> events;
    std::uint32_t history_limit, event_limit;
    std::uint64_t sequence{};

    void event(EventKind kind, const Snapshot& snapshot) {
        std::lock_guard lock(mutex);
        if (!active.contains(snapshot.id)) return;
        for (auto item = events.rbegin(); item != events.rend(); ++item) {
            if (item->snapshot.id != snapshot.id) continue;
            if (item->snapshot.revision >= snapshot.revision) return;
            break;
        }
        auto incremental = snapshot;
        if (kind == EventKind::File && !incremental.recent_files.empty()) {
            auto changed = incremental.recent_files.back();
            incremental.recent_files.assign(1, std::move(changed));
        } else incremental.recent_files.clear();
        events.push_back({++sequence, kind, std::move(incremental)});
        while (events.size() > event_limit) events.pop_front();
    }
    void finished(const std::string& id, Snapshot snapshot) {
        std::lock_guard lock(mutex);
        history.push_back(snapshot);
        while (history.size() > history_limit) history.pop_front();
        active.erase(id);
        events.push_back({++sequence, EventKind::Terminal, std::move(snapshot)});
        while (events.size() > event_limit) events.pop_front();
    }
};
struct Task::Impl {
    std::shared_ptr<Registry::Impl> registry;
    std::shared_ptr<Record> record;
    transport::NetworkSnapshot network;
    std::string id;
    fs::path workspace, staging, journal_path;
    detail::FileIdentity workspace_identity, staging_identity;
    std::unique_ptr<detail::ParentGuard> parent_pin;
    std::optional<detail::SecureFile> workspace_lock;
    Json journal;
    std::vector<Published> published;
    std::vector<CreatedDirectory> directories;
    std::mutex publication_mutex;
    bool finalized{};

    void save(std::string state) {
        journal["state"] = std::move(state);
        journal["files"] = Json::array();
        for (const auto& file : published) {
            Json row{{"source", detail::path_utf8(file.source.lexically_relative(workspace))},
                {"destination", detail::path_utf8(file.destination)}, {"identity", identity_json(file.identity)},
                {"size", file.size}, {"sha256", file.sha256},
                {"moved", file.moved}, {"backedUp", file.backed_up}, {"retainedMoved", file.retained_moved}};
            if (!file.step_id.empty()) {
                row["stepId"] = file.step_id; row["stepFinal"] = file.step_final;
                row["stepCommitted"] = file.step_committed; row["durabilityPending"] = file.durability_pending;
            }
            if (file.previous) {
                row["previous"] = {{"destination", detail::path_utf8(file.previous->destination)},
                    {"identity", identity_json(file.previous->identity)},
                    {"size", file.previous->size}, {"sha256", file.previous->sha256}};
                row["backup"] = detail::path_utf8(file.backup.lexically_relative(workspace));
                if (file.retained) row["retained"] = detail::path_utf8(*file.retained);
            }
            journal["files"].push_back(std::move(row));
        }
        journal["directories"] = Json::array();
        for (const auto& directory : directories) {
            Json row{{"path", detail::path_utf8(directory.path)}};
            if (directory.identity) row["identity"] = identity_json(*directory.identity);
            if (!directory.step_id.empty()) {
                row["stepId"] = directory.step_id; row["stepCommitted"] = directory.step_committed;
            }
            journal["directories"].push_back(std::move(row));
        }
        write_json(journal_path, journal);
    }
    bool reserved(const fs::path& destination) const {
        const auto child = detail::case_key(detail::path_utf8(destination));
        for (const auto& target : record->targets) {
            const auto parent = detail::case_key(detail::path_utf8(target));
            if (child == parent || child.starts_with(parent + "/")) return true;
        }
        return false;
    }
    void cleanup() {
        NICKEL_TRANSPORT_CHECKPOINT("cleanup", workspace);
        workspace_lock.reset();
        if (fs::exists(detail::io_path(workspace))) detail::remove_owned_tree(workspace, workspace_identity);
    }
    void reverse_publications() {
        for (auto item = published.rbegin(); item != published.rend(); ++item) {
            if (item->step_committed) continue;
            if (item->moved) {
                detail::ParentGuard source_parent(item->source.parent_path(), true, detail::GuardMode::NamespaceMutation);
                auto next = detail::SecureFile::open_publication(item->destination, item->identity, detail::GuardMode::NamespaceMutation);
                check_lease(next, FileLease{item->destination, item->identity, item->size, item->sha256}, {});
                next.publish_no_replace(item->source);
                NICKEL_TRANSPORT_CHECKPOINT("rollback.next", item->source);
                item->moved = false;
            }
            if (item->previous && item->backed_up) {
                const auto& saved = item->retained_moved ? *item->retained : item->backup;
                auto previous = detail::SecureFile::open_publication(saved, item->previous->identity, detail::GuardMode::NamespaceMutation);
                check_lease(previous, *item->previous, {});
                previous.publish_no_replace(item->previous->destination);
                NICKEL_TRANSPORT_CHECKPOINT("rollback.previous", item->previous->destination);
                item->backed_up = false; item->retained_moved = false;
            }
        }
        for (auto item = directories.rbegin(); item != directories.rend(); ++item) {
            if (item->step_committed || !item->identity || !fs::exists(detail::io_path(item->path))) continue;
            if (detail::directory_identity(item->path) != *item->identity || !fs::is_empty(detail::io_path(item->path)))
                transport::detail::fail("recovery_conflict", "Created directory changed during rollback");
            if (!fs::remove(detail::io_path(item->path))) transport::detail::fail("rollback_incomplete", "Cannot remove owned empty directory");
        }
    }
    void finish(TaskStatus status, std::string code = {}, std::string message = {}) {
        {
            std::lock_guard lock(record->mutex);
            record->snapshot.status = status; record->snapshot.ended_ms = now_ms();
            record->snapshot.error_code = std::move(code); record->snapshot.message = std::move(message);
            record->snapshot.speed_bytes_per_second.reset(); record->snapshot.remaining_ms.reset();
            if (record->incremental_files) record->snapshot.files_done = record->committed_files.size();
            if (status == TaskStatus::Succeeded) record->snapshot.percent = 1.0;
            ++record->snapshot.revision;
        }
        finalized = true;
        workspace_lock.reset(); parent_pin.reset();
        registry->finished(id, snapshot_copy());
    }
    Snapshot snapshot_copy() const {
        std::lock_guard lock(record->mutex); return record->snapshot;
    }
    void failed_cleanup(std::string message) {
        try { save("rolling_back"); reverse_publications(); cleanup(); }
        catch (...) { message += "; owned recovery workspace retained"; }
        const auto cancelled = snapshot_copy().cancel_requested || record->cancel->cancelled();
        finish(cancelled ? TaskStatus::Cancelled : TaskStatus::Failed,
               cancelled ? "cancelled" : "transaction_failed", std::move(message));
    }
};

Registry::Registry(std::uint32_t history_limit, std::uint32_t event_limit)
    : impl_(std::make_shared<Impl>()) {
    if (!history_limit || history_limit > 1024 || !event_limit || event_limit > 65536)
        throw std::runtime_error("invalid task registry bounds");
    impl_->history_limit = history_limit; impl_->event_limit = event_limit;
}
std::shared_ptr<Task> Registry::begin(const TaskSpec& spec) {
    return transport::detail::boundary("task_begin_failed", "Cannot begin task transaction", [&] {
        auto task = std::shared_ptr<Task>(new Task(nullptr));
        auto value = std::make_unique<Task::Impl>();
        value->registry = impl_; value->record = std::make_shared<Record>();
        value->record->cancel = spec.cancellation ? std::make_shared<Cancellation>(*spec.cancellation)
                                                  : transport::detail::make_cancellation();
        value->record->commit_gate = transport::detail::cancellation_gate(*value->record->cancel);
        value->id = detail::unique_id(); value->network = spec.network;
        value->record->targets.push_back(target_path(spec.target));
        if (spec.additional_targets.size() > 128)
            transport::detail::fail("task_limits", "Too many task targets");
        for (const auto& target : spec.additional_targets) value->record->targets.push_back(target_path(target));
        const auto& target = value->record->targets.front();
        value->parent_pin = std::make_unique<detail::ParentGuard>(target.parent_path(), false, detail::GuardMode::NamespaceMutation);
        std::unique_lock lock(impl_->mutex);
        if (impl_->active.size() >= 64) transport::detail::fail("task_limits", "Active task budget exceeded");
        for (const auto& [id, other] : impl_->active) {
            (void)id;
            for (const auto& desired : value->record->targets)
                for (const auto& occupied : other->targets)
                    if (detail::paths_overlap(desired, occupied))
                        transport::detail::fail("target_conflict", "Task target is already reserved");
        }
        value->workspace = target.parent_path() / detail::utf8_path(".nickel-task-" + value->id);
        detail::create_directory_new(value->workspace);
        value->workspace_identity = detail::directory_identity(value->workspace);
        value->staging = value->workspace / "staging";
        try {
            value->workspace_lock.emplace(detail::SecureFile::lock_workspace(value->workspace / "owner.lock", true));
            detail::create_directory_new(value->staging);
            value->staging_identity = detail::directory_identity(value->staging);
            value->journal_path = value->workspace / "journal.json";
            value->journal = Json{{"schema", 1}, {"taskId", value->id},
                {"target", detail::path_utf8(target)}, {"workspaceIdentity", identity_json(value->workspace_identity)},
                {"stagingIdentity", identity_json(value->staging_identity)}, {"createdMs", now_ms()},
                {"targets", Json::array()}, {"directoryPublication", false}};
            for (const auto& item : value->record->targets) value->journal["targets"].push_back(detail::path_utf8(item));
            value->save("running");
            auto& snapshot = value->record->snapshot;
            snapshot.id = value->id; snapshot.kind = spec.kind; snapshot.target = target;
            snapshot.label = spec.label.substr(0, 512); snapshot.started_ms = now_ms(); snapshot.revision = 1;
            impl_->active.emplace(value->id, value->record);
            impl_->events.push_back({++impl_->sequence, EventKind::Began, snapshot});
            while (impl_->events.size() > impl_->event_limit) impl_->events.pop_front();
            task->impl_ = std::move(value);
            return task;
        } catch (...) {
            impl_->active.erase(value->id);
            value->workspace_lock.reset();
            detail::remove_owned_tree(value->workspace, value->workspace_identity);
            throw;
        }
    });
}
Registry::~Registry() = default;
std::optional<Cancellation> Registry::cancellation_for(std::string_view id) const {
    std::shared_ptr<Record> record;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->active.find(std::string(id));
        if (found == impl_->active.end()) return {};
        record = found->second;
    }
    std::lock_guard lock(record->mutex);
    if (terminal(record->snapshot.status) || record->snapshot.status == TaskStatus::Committing) return {};
    return *record->cancel;
}
bool Registry::cancel(std::string_view id) {
    std::shared_ptr<Record> record;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->active.find(std::string(id));
        if (found == impl_->active.end()) return false;
        record = found->second;
    }
    Snapshot snapshot;
    auto token = prepare_cancel(record, snapshot);
    if (!token) return false;
    impl_->event(EventKind::CancelRequested, snapshot);
    token->cancel();
    return true;
}
bool Registry::conflicts(const fs::path& input) const {
    const auto target = target_path(input);
    std::lock_guard lock(impl_->mutex);
    for (const auto& [id, record] : impl_->active) {
        (void)id; for (const auto& path : record->targets) if (detail::paths_overlap(path, target)) return true;
    }
    return false;
}
std::vector<Snapshot> Registry::snapshots() const {
    std::vector<std::shared_ptr<Record>> active;
    std::vector<Snapshot> out;
    {
        std::lock_guard lock(impl_->mutex);
        out.assign(impl_->history.begin(), impl_->history.end());
        for (const auto& [id, record] : impl_->active) { (void)id; active.push_back(record); }
    }
    for (const auto& record : active) { std::lock_guard lock(record->mutex); out.push_back(record->snapshot); }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.started_ms < b.started_ms; });
    return out;
}
Delta Registry::since(std::uint64_t cursor) const {
    Delta result;
    {
        std::lock_guard lock(impl_->mutex);
        result.cursor = impl_->sequence;
        result.reset_required = cursor > impl_->sequence ||
            (!impl_->events.empty() && cursor < impl_->events.front().sequence - 1);
        if (!result.reset_required)
            for (const auto& event : impl_->events) if (event.sequence > cursor) result.events.push_back(event);
    }
    if (result.reset_required) result.snapshots = snapshots();
    return result;
}
std::shared_ptr<Registry> Registry::process() { static auto registry = std::make_shared<Registry>(); return registry; }

Task::Task(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Task::~Task() {
    if (impl_ && !impl_->finalized) {
        try { rollback("abandoned", "task handle abandoned before commit"); }
        catch (...) { }
    }
}
const std::string& Task::id() const noexcept { return impl_->id; }
const Cancellation& Task::cancellation() const noexcept { return *impl_->record->cancel; }
const transport::NetworkSnapshot& Task::network() const noexcept { return impl_->network; }
const fs::path& Task::workspace() const noexcept { return impl_->workspace; }
const fs::path& Task::staging() const noexcept { return impl_->staging; }
Snapshot Task::snapshot() const { return impl_->snapshot_copy(); }
bool Task::cancel() {
    Snapshot snapshot;
    auto token = prepare_cancel(impl_->record, snapshot);
    if (!token) return false;
    impl_->registry->event(EventKind::CancelRequested, snapshot);
    token->cancel();
    return true;
}
void Task::stage(std::string name, std::string detail_text) {
    Snapshot snapshot;
    {
        std::lock_guard lock(impl_->record->mutex);
        if (terminal(impl_->record->snapshot.status)) return;
        auto& state = impl_->record->snapshot;
        state.stage = name.substr(0, 256); state.detail = detail_text.substr(0, 1024);
        state.percent.reset(); state.total_bytes.reset(); state.completed_bytes = 0;
        state.files_done = impl_->record->incremental_files ? impl_->record->committed_files.size() : 0;
        state.files_total.reset(); ++state.revision;
        snapshot = state;
    }
    impl_->registry->event(EventKind::Stage, snapshot);
}
void Task::progress(const transport::TransferProgress& progress) {
    Snapshot snapshot;
    {
        std::lock_guard lock(impl_->record->mutex);
        auto& state = impl_->record->snapshot; if (terminal(state.status)) return;
        state.transferred_bytes = std::max(state.transferred_bytes, progress.transferred_bytes);
        state.completed_bytes = progress.completed_bytes; state.total_bytes = progress.total_bytes;
        if (progress.total_bytes && *progress.total_bytes)
            state.percent = std::clamp(static_cast<double>(progress.completed_bytes) / *progress.total_bytes, 0.0, 1.0);
        else state.percent.reset();
        state.speed_bytes_per_second = std::isfinite(progress.speed_bytes_per_second) && progress.speed_bytes_per_second > 0
            ? std::optional<double>(progress.speed_bytes_per_second) : std::nullopt;
        state.remaining_ms = progress.remaining && progress.remaining->count() >= 0
            ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(progress.remaining->count())) : std::nullopt;
        ++state.revision; snapshot = state;
    }
    impl_->registry->event(EventKind::Progress, snapshot);
}
void Task::file(FileProgress progress) {
    progress.name = progress.name.substr(0, 4096);
    Snapshot snapshot;
    {
        std::lock_guard lock(impl_->record->mutex);
        auto& state = impl_->record->snapshot; if (terminal(state.status)) return;
        const auto found = std::find_if(state.recent_files.begin(), state.recent_files.end(),
            [&](const auto& old) { return old.name == progress.name; });
        if (found != state.recent_files.end()) state.recent_files.erase(found);
        else if (state.recent_files.size() == recent_file_limit) state.recent_files.erase(state.recent_files.begin());
        state.recent_files.push_back(std::move(progress));
        ++state.revision; snapshot = state;
    }
    impl_->registry->event(EventKind::File, snapshot);
}
void Task::files(std::uint64_t done, std::optional<std::uint64_t> total) {
    Snapshot snapshot;
    {
        std::lock_guard lock(impl_->record->mutex);
        auto& state = impl_->record->snapshot; if (terminal(state.status)) return;
        state.files_done = impl_->record->incremental_files ? impl_->record->committed_files.size() : done;
        state.files_total = total;
        if (!state.total_bytes) {
            if (total && *total) state.percent = std::clamp(static_cast<double>(state.files_done) / *total, 0.0, 1.0);
            else state.percent.reset();
        }
        ++state.revision; snapshot = state;
    }
    impl_->registry->event(EventKind::Progress, snapshot);
}
void Task::complete_work() {
    transport::detail::boundary("task_completion_failed", "Cannot complete task work", [&] {
        std::lock_guard publication(impl_->publication_mutex);
        std::lock_guard gate(*impl_->record->commit_gate);
        if (impl_->finalized) transport::detail::fail("task_terminal", "Task is already terminal");
        if (!impl_->published.empty() || !impl_->directories.empty() || impl_->journal.value("incrementalFiles", false) ||
            impl_->journal.value("directoryPublication", false) || impl_->journal.value("externalPublication", false))
            transport::detail::fail("task_publication_pending", "Publication requires its transaction commit");
        try {
            {
                std::lock_guard lock(impl_->record->mutex);
                if (impl_->record->snapshot.cancel_requested || cancellation().cancelled())
                    transport::detail::fail("cancelled", "Operation cancelled");
                impl_->record->snapshot.status = TaskStatus::Committing;
            }
            NICKEL_TRANSPORT_CHECKPOINT("completion.journal", impl_->workspace);
            impl_->save("committed");
        } catch (...) {
            impl_->failed_cleanup("Task work completion failed before its durable marker");
            throw;
        }
        try { impl_->cleanup(); }
        catch (...) {
            impl_->finish(TaskStatus::Succeeded, "cleanup_pending", "Work completed; owned cleanup is pending");
            return;
        }
        impl_->finish(TaskStatus::Succeeded);
    });
}
void Task::commit_external(const std::function<void()>& publication) {
    transport::detail::boundary("task_publication_failed", "Cannot publish task domain work", [&] {
        std::lock_guard serialization(impl_->publication_mutex);
        std::lock_guard gate(*impl_->record->commit_gate);
        if (impl_->finalized) transport::detail::fail("task_terminal", "Task is already terminal");
        if (!publication) transport::detail::fail("invalid_publication", "Publication callback is required");
        if (!impl_->published.empty() || !impl_->directories.empty() || impl_->journal.value("incrementalFiles", false) ||
            impl_->journal.value("directoryPublication", false))
            transport::detail::fail("task_publication_pending", "File publication requires its transaction commit");
        try {
            {
                std::lock_guard lock(impl_->record->mutex);
                if (impl_->record->snapshot.cancel_requested || cancellation().cancelled())
                    transport::detail::fail("cancelled", "Operation cancelled");
                impl_->record->snapshot.status = TaskStatus::Committing;
            }
            impl_->journal["externalPublication"] = true;
            NICKEL_TRANSPORT_CHECKPOINT("external.intent", impl_->workspace);
            impl_->save("committing");
            transport::detail::check_cancel(transport::detail::cancellation_check(cancellation()));
            publication();
        } catch (...) {
            impl_->failed_cleanup("Domain publication did not complete");
            throw;
        }
        // The external owner has crossed its irreversible boundary. Cancellation
        // and our journal/cleanup failures must not relabel or undo that fact.
        try {
            NICKEL_TRANSPORT_CHECKPOINT("external.committed", impl_->workspace);
            impl_->save("committed");
        } catch (...) {
            impl_->finish(TaskStatus::Succeeded, "persistence_pending", "Domain committed; task completion marker is pending");
            return;
        }
        try { impl_->cleanup(); }
        catch (...) {
            impl_->finish(TaskStatus::Succeeded, "cleanup_pending", "Domain committed; owned cleanup is pending");
            return;
        }
        impl_->finish(TaskStatus::Succeeded);
    });
}
void Task::commit_directory() try {
    std::lock_guard publication(impl_->publication_mutex);
    std::lock_guard gate(*impl_->record->commit_gate);
    if (impl_->finalized) transport::detail::fail("task_terminal", "Task is already terminal");
    if (!impl_->published.empty() || !impl_->directories.empty() || impl_->journal.value("incrementalFiles", false) ||
        impl_->journal.value("externalPublication", false))
        transport::detail::fail("task_publication_pending", "Task publication modes cannot be mixed");
    const auto cancel = [record = impl_->record] {
        std::lock_guard lock(record->mutex);
        return record->snapshot.cancel_requested || record->cancel->cancelled();
    };
    try {
        transport::detail::check_cancel(cancel);
        verify_tree(impl_->staging, cancel);
        const auto target = impl_->record->targets.front();
        if (fs::exists(detail::io_path(target))) transport::detail::fail("target_conflict", "Directory target already exists");
        impl_->journal["directoryPublication"] = true;
        impl_->save("committing");
        {
            std::lock_guard lock(impl_->record->mutex);
            impl_->record->snapshot.status = TaskStatus::Committing;
        }
        transport::detail::check_cancel(cancel);
        detail::rename_owned_no_replace(impl_->staging, target, impl_->staging_identity);
        // The destination identity proves commit if the final journal write is interrupted.
        impl_->journal["publishedDirectoryIdentity"] = identity_json(impl_->staging_identity);
        try { impl_->save("committed"); impl_->cleanup(); }
        catch (const std::exception&) {
            impl_->finish(TaskStatus::Succeeded, "cleanup_pending", "Committed; owned cleanup is pending");
            return;
        }
        impl_->finish(TaskStatus::Succeeded);
    } catch (const std::exception&) {
        impl_->failed_cleanup("Directory transaction failed");
        throw;
    }
} catch (const ErrorException&) {
    throw;
} catch (const std::exception&) {
    transport::detail::fail("transaction_failed", "Directory transaction failed");
}
FileLease lease_file(const fs::path& path, const Cancellation& cancel) {
    return transport::detail::boundary("lease_failed", "Cannot acquire file revision lease", [&] {
        const auto destination = target_path(path);
        auto file = detail::SecureFile::open_read(destination);
        const auto identity = file.identity();
        const auto size = file.size();
        const auto digest = hash_handle(file, transport::detail::cancellation_check(cancel));
        if (file.size() != size) transport::detail::fail("revision_conflict", "File changed while leasing");
        return FileLease{destination, identity, size, digest};
    });
}
void Task::commit_replacement(const fs::path& original, std::string_view expected_sha256,
                              std::string_view source_relative, const fs::path& destination,
                              std::optional<fs::path> retain_original) {
    transport::detail::boundary("transaction_failed", "File replacement transaction failed", [&] {
        if (expected_sha256.size() != 64 || !std::all_of(expected_sha256.begin(), expected_sha256.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        })) transport::detail::fail("revision_conflict", "Expected original SHA256 is invalid");
        const auto previous = lease_file(original, cancellation());
        if (!transport::detail::digest_equal(previous.sha256, expected_sha256))
            transport::detail::fail("revision_conflict", "Original file changed before replacement");
        commit_files({PublishFile{std::string(source_relative), destination, previous, std::move(retain_original)}});
    });
}
void Task::adopt_file(const fs::path& source, std::string_view relative, detail::FileIdentity identity) {
    transport::detail::boundary("transaction_failed", "Cannot adopt verified task file", [&] {
        std::lock_guard publication(impl_->publication_mutex);
        std::lock_guard gate(*impl_->record->commit_gate);
        if (impl_->finalized) transport::detail::fail("task_terminal", "Task is already terminal");
        cancellation().check();
        const auto destination = impl_->staging / detail::utf8_path(detail::validate_relative(relative));
        detail::ParentGuard parents(destination.parent_path(), true, detail::GuardMode::NamespaceMutation);
        auto file = detail::SecureFile::open_publication(source, identity, detail::GuardMode::NamespaceMutation);
        impl_->save("running");
        file.publish_no_replace(destination);
    });
}
void Task::commit_files(const std::vector<PublishFile>& files, const std::vector<fs::path>& directories) {
    (void)publish_files(files, directories, {}, true);
}
FileCommitReceipt Task::commit_file_step(std::string_view step_id, const PublishFile& file, bool final) {
    if (!valid_step_id(step_id)) transport::detail::fail("invalid_step", "A bounded stable file step ID is required");
    auto receipt = publish_files({file}, {}, std::string(step_id), final);
    return std::move(*receipt);
}
std::vector<FileCommitReceipt> Task::committed_files() const {
    std::lock_guard lock(impl_->record->mutex);
    return impl_->record->committed_files;
}
std::optional<FileCommitReceipt> Task::publish_files(const std::vector<PublishFile>& files,
    const std::vector<fs::path>& requested_directories, std::string step_id, bool final) {
    return transport::detail::boundary("transaction_failed", "File transaction failed", [&]() -> std::optional<FileCommitReceipt> {
        std::lock_guard serialization(impl_->publication_mutex);
        std::unique_lock gate(*impl_->record->commit_gate);
        const bool incremental = !step_id.empty();
        if (incremental) {
            if (files.size() != 1 || !requested_directories.empty())
                transport::detail::fail("invalid_step", "A step must publish exactly one real file");
            const auto found = std::find_if(impl_->published.begin(), impl_->published.end(),
                [&](const Published& file) { return file.step_id == step_id; });
            if (found != impl_->published.end()) {
                const auto& request = files.front();
                const auto source = impl_->staging / detail::utf8_path(detail::validate_relative(request.source_relative));
                const auto same_previous = [&] {
                    if (found->previous.has_value() != request.previous.has_value()) return false;
                    if (!found->previous) return true;
                    const auto& left = *found->previous; const auto& right = *request.previous;
                    return left.destination == target_path(right.destination) && left.identity == right.identity &&
                        left.size == right.size && transport::detail::digest_equal(left.sha256, right.sha256);
                };
                const auto retained = request.retain_previous ? std::optional<fs::path>(target_path(*request.retain_previous)) : std::nullopt;
                if (!found->step_committed || found->source != source || found->destination != target_path(request.destination) ||
                    found->step_final != final || found->retained != retained || !same_previous())
                    transport::detail::fail("step_conflict", "File step ID was used for a different publication");
                return receipt_of(*found);
            }
        }
        if (impl_->finalized) transport::detail::fail("task_terminal", "Task is already terminal");
        if (impl_->journal.value("directoryPublication", false) || impl_->journal.value("externalPublication", false) ||
            (!incremental && impl_->journal.value("incrementalFiles", false)))
            transport::detail::fail("task_publication_pending", "Task publication modes cannot be mixed");
        const auto cancel = [record = impl_->record] {
            std::lock_guard lock(record->mutex);
            return record->snapshot.cancel_requested || record->cancel->cancelled();
        };
        const auto first_file = impl_->published.size(), first_directory = impl_->directories.size();
        bool legacy_committed{}, step_durability_pending{};
        std::optional<FileCommitReceipt> receipt, record_receipt;
        const auto step_complete = [&] {
            if (!incremental || impl_->published.size() != first_file + 1) return false;
            const auto& file = impl_->published.back();
            return file.moved && (!file.retained || file.retained_moved);
        };
        const auto mark_step = [&] {
            if (!step_complete()) return;
            impl_->published.back().step_committed = true;
            for (std::size_t at = first_directory; at < impl_->directories.size(); ++at)
                impl_->directories[at].step_committed = true;
        };
        const auto finish_step = [&]() -> std::optional<FileCommitReceipt> {
            mark_step();
            auto& file = impl_->published.back();
            file.durability_pending = step_durability_pending;
            bool marker_saved{};
            try {
                NICKEL_TRANSPORT_CHECKPOINT("step.marker", impl_->workspace);
                impl_->save(final ? "committed" : "running"); marker_saved = true;
            } catch (...) { file.durability_pending = true; }
            receipt->durability_pending = record_receipt->durability_pending = file.durability_pending;
            {
                std::lock_guard lock(impl_->record->mutex);
                impl_->record->committed_files.push_back(std::move(*record_receipt));
                impl_->record->snapshot.files_done = impl_->record->committed_files.size();
                ++impl_->record->snapshot.revision;
            }
            // Reporting cannot undo an actual completed namespace publication.
            try { NICKEL_TRANSPORT_CHECKPOINT("step.committed", file.destination); } catch (...) { }
            if (!marker_saved || file.durability_pending) {
                impl_->finish(final ? TaskStatus::Succeeded : TaskStatus::Failed, "persistence_pending",
                    "File step committed; durability or its completion marker is pending");
                return std::move(receipt);
            }
            if (final) {
                try { impl_->cleanup(); }
                catch (...) {
                    impl_->finish(TaskStatus::Succeeded, "cleanup_pending", "File steps committed; owned cleanup is pending");
                    return std::move(receipt);
                }
                impl_->finish(TaskStatus::Succeeded);
            } else {
                {
                    std::lock_guard lock(impl_->record->mutex);
                    impl_->record->snapshot.status = impl_->record->snapshot.cancel_requested || cancellation().cancelled()
                        ? TaskStatus::Cancelling : TaskStatus::Running;
                    ++impl_->record->snapshot.revision;
                }
                try { impl_->registry->event(EventKind::Progress, impl_->snapshot_copy()); } catch (...) { }
            }
            return std::move(receipt);
        };
        // Serialize mutation methods, but allow Task/UI cancellation throughout preparation and hashing.
        gate.unlock();
        try {
            transport::detail::check_cancel(cancel);
            if (incremental) {
                impl_->journal["incrementalFiles"] = true;
                std::lock_guard lock(impl_->record->mutex);
                impl_->record->incremental_files = true;
            }
            if (files.size() > max_publish_files - first_file || requested_directories.size() > max_publish_files - first_directory)
                transport::detail::fail("task_limits", "Invalid publication count");
            if (incremental && std::any_of(impl_->published.begin(), impl_->published.end(),
                    [](const Published& file) { return file.step_id.empty() || !file.step_committed; }))
                transport::detail::fail("task_publication_pending", "An unfinished file publication already exists");
            struct Handles {
                detail::SecureFile next;
                std::optional<detail::SecureFile> previous;
                detail::FileRevision next_revision;
                std::optional<detail::FileRevision> previous_revision;
            };
            std::vector<Handles> handles;
            std::vector<std::unique_ptr<detail::ParentGuard>> parent_pins;
            std::unordered_set<std::string> sources, destinations, planned;
            for (const auto& file : impl_->published) {
                sources.insert(detail::case_key(detail::path_utf8(file.source.lexically_relative(impl_->staging))));
                destinations.insert(detail::case_key(detail::path_utf8(file.destination)));
                if (file.previous) destinations.insert(detail::case_key(detail::path_utf8(file.previous->destination)));
                if (file.retained) destinations.insert(detail::case_key(detail::path_utf8(*file.retained)));
            }
            std::vector<fs::path> needed;
            auto plan_directory = [&](const fs::path& input) {
                auto path = target_path(input);
                std::vector<fs::path> missing;
                for (; !fs::exists(detail::io_path(path)); path = path.parent_path()) {
                    if (!impl_->reserved(path)) transport::detail::fail("target_conflict", "Missing parent is outside reserved task targets");
                    missing.push_back(path);
                }
                parent_pins.push_back(std::make_unique<detail::ParentGuard>(path, false, detail::GuardMode::NamespaceMutation));
                for (auto item = missing.rbegin(); item != missing.rend(); ++item)
                    if (planned.insert(detail::case_key(detail::path_utf8(*item))).second) needed.push_back(*item);
            };
            for (const auto& file : files) {
                transport::detail::check_cancel(cancel);
                const auto name = detail::validate_relative(file.source_relative);
                const auto source = impl_->staging / detail::utf8_path(name);
                const auto destination = target_path(file.destination);
                const auto destination_key = detail::case_key(detail::path_utf8(destination));
                if (!sources.insert(detail::case_key(name)).second ||
                    !destinations.insert(destination_key).second || !impl_->reserved(destination))
                    transport::detail::fail("target_conflict", "Duplicate or unreserved file publication");
                auto input = detail::SecureFile::open_read(source);
                Published publication;
                publication.source = source; publication.destination = destination; publication.identity = input.identity();
                publication.step_id = step_id; publication.step_final = incremental && final;
                input.close();
                Handles locked{detail::SecureFile::open_publication(source, publication.identity, detail::GuardMode::NamespaceMutation), {}, {}, {}};
                locked.next_revision = locked.next.revision();
                publication.size = locked.next.size(); publication.sha256 = hash_handle(locked.next, cancel);
                if (locked.next.size() != publication.size || locked.next.revision() != locked.next_revision)
                    transport::detail::fail("revision_conflict", "Staged file changed during publication preparation");
                if (file.previous) {
                    auto previous = *file.previous;
                    previous.destination = target_path(previous.destination);
                    const auto original_key = detail::case_key(detail::path_utf8(previous.destination));
                    if (!impl_->reserved(previous.destination) || previous.sha256.size() != 64 ||
                        (original_key != destination_key && !destinations.insert(original_key).second))
                        transport::detail::fail("revision_conflict", "Replacement lease does not match reserved original");
                    if (original_key != destination_key && fs::exists(detail::io_path(destination)))
                        transport::detail::fail("target_conflict", "Renamed replacement target already exists");
                    locked.previous.emplace(transport::detail::boundary("revision_conflict", "Original file changed before replacement", [&] {
                        return detail::SecureFile::open_publication(previous.destination, previous.identity, detail::GuardMode::NamespaceMutation);
                    }));
                    locked.previous_revision = locked.previous->revision();
                    check_lease(*locked.previous, previous, cancel);
                    if (locked.previous->revision() != *locked.previous_revision)
                        transport::detail::fail("revision_conflict", "Original file changed during publication preparation");
                    publication.previous = std::move(previous);
                    publication.backup = impl_->workspace / "backup" / detail::utf8_path(std::to_string(impl_->published.size()));
                    if (file.retain_previous) {
                        const auto retained = target_path(*file.retain_previous);
                        if (!impl_->reserved(retained) || fs::exists(detail::io_path(retained)) ||
                            !destinations.insert(detail::case_key(detail::path_utf8(retained))).second)
                            transport::detail::fail("target_conflict", "Old revision retention target conflicts");
                        publication.retained = retained;
                        plan_directory(retained.parent_path());
                    }
                } else if (fs::exists(detail::io_path(destination)) || file.retain_previous) {
                    transport::detail::fail("target_conflict", "Existing target requires an explicit revision lease");
                }
                plan_directory(destination.parent_path());
                impl_->published.push_back(std::move(publication)); handles.push_back(std::move(locked));
            }
            for (const auto& directory : requested_directories) {
                const auto path = target_path(directory);
                if (!impl_->reserved(path) || destinations.contains(detail::case_key(detail::path_utf8(path))))
                    transport::detail::fail("target_conflict", "Directory publication conflicts with file");
                plan_directory(path);
            }
            if (needed.size() > max_publish_files - first_directory)
                transport::detail::fail("task_limits", "Invalid publication directory count");
            std::sort(needed.begin(), needed.end(), [](const auto& a, const auto& b) {
                const auto left = detail::path_utf8(a), right = detail::path_utf8(b);
                return std::count(left.begin(), left.end(), '/') < std::count(right.begin(), right.end(), '/');
            });
            for (const auto& path : needed) impl_->directories.push_back({path, {}, step_id, false});
            if (incremental) {
                receipt.emplace(receipt_of(impl_->published.back())); record_receipt = receipt;
                {
                    std::lock_guard lock(impl_->record->mutex);
                    impl_->record->committed_files.reserve(first_file + 1); impl_->record->incremental_files = true;
                }
                impl_->journal["incrementalFiles"] = true;
            }
            NICKEL_TRANSPORT_CHECKPOINT("files.prepared", impl_->workspace);
            gate.lock();
            transport::detail::check_cancel(cancel);
            for (std::size_t at = 0; at < handles.size(); ++at) {
                const auto& file = impl_->published[first_file + at]; auto& locked = handles[at];
                if (locked.next.identity() != file.identity || locked.next.size() != file.size ||
                    locked.next.revision() != locked.next_revision ||
                    (locked.previous && (locked.previous->identity() != file.previous->identity ||
                     locked.previous->size() != file.previous->size || locked.previous->revision() != *locked.previous_revision)))
                    transport::detail::fail("revision_conflict", "Prepared file revision changed before publication");
            }
            impl_->save("committing");
            NICKEL_TRANSPORT_CHECKPOINT("files.planned", impl_->workspace);
            {
                std::lock_guard lock(impl_->record->mutex);
                impl_->record->snapshot.status = TaskStatus::Committing;
            }
            for (std::size_t at = first_directory; at < impl_->directories.size(); ++at) {
                auto& directory = impl_->directories[at];
                transport::detail::check_cancel(cancel);
                detail::create_directory_new(directory.path);
                directory.identity = detail::directory_identity(directory.path);
                impl_->save("committing");
                parent_pins.push_back(std::make_unique<detail::ParentGuard>(directory.path, false, detail::GuardMode::NamespaceMutation));
            }
            if (std::any_of(handles.begin(), handles.end(), [](const Handles& file) { return file.previous.has_value(); }) &&
                !fs::exists(detail::io_path(impl_->workspace / "backup")))
                detail::create_directory_new(impl_->workspace / "backup");
            for (std::size_t at = first_file; at < impl_->published.size(); ++at) {
                auto& file = impl_->published[at]; auto& locked = handles[at - first_file];
                transport::detail::check_cancel(cancel);
                if (locked.previous) {
                    publish_locked(*locked.previous, file.backup, file.backed_up);
                    NICKEL_TRANSPORT_CHECKPOINT("files.backed_up", file.backup);
                    impl_->save("committing");
                }
                publish_locked(locked.next, file.destination, file.moved); mark_step();
                NICKEL_TRANSPORT_CHECKPOINT("files.published", file.destination);
                impl_->save("committing");
            }
            for (std::size_t at = first_file; at < impl_->published.size(); ++at) {
                auto& file = impl_->published[at];
                if (file.retained) {
                    if (!incremental) transport::detail::check_cancel(cancel);
                    publish_locked(*handles[at - first_file].previous, *file.retained, file.retained_moved); mark_step();
                    NICKEL_TRANSPORT_CHECKPOINT("files.retained", *file.retained);
                    impl_->save("committing");
                }
            }
            if (incremental) {
                const auto& file = impl_->published.back();
                detail::sync_directory(file.destination.parent_path());
                if (file.previous) detail::sync_directory(file.previous->destination.parent_path());
                if (file.retained) detail::sync_directory(file.retained->parent_path());
            } else {
                transport::detail::check_cancel(cancel);
                impl_->save("committed"); legacy_committed = true;
                NICKEL_TRANSPORT_CHECKPOINT("files.committed", impl_->workspace);
            }
            handles.clear(); parent_pins.clear();
        } catch (...) {
            if (!gate.owns_lock()) gate.lock();
            if (step_complete()) step_durability_pending = true;
            else if (!legacy_committed) {
                impl_->failed_cleanup("File transaction failed");
                try { throw; }
                catch (const ErrorException&) { throw; }
                catch (const std::exception&) { throw; }
                catch (...) { transport::detail::fail("transaction_failed", "File transaction failed"); }
            }
        }
        if (incremental) return finish_step();
        try { impl_->cleanup(); }
        catch (...) {
            impl_->finish(TaskStatus::Succeeded, "cleanup_pending", "Committed; owned cleanup is pending"); return {};
        }
        impl_->finish(TaskStatus::Succeeded);
        return {};
    });
}
void Task::rollback(std::string error_code, std::string message) {
    std::lock_guard publication(impl_->publication_mutex);
    std::lock_guard gate(*impl_->record->commit_gate);
    if (impl_->finalized) return;
    try { impl_->save("rolling_back"); impl_->reverse_publications(); impl_->cleanup(); }
    catch (...) {
        if (!message.empty()) message += "; "; message += "cleanup retained owned journal";
        if (error_code.empty()) error_code = "rollback_incomplete";
    }
    const auto cancelled = impl_->snapshot_copy().cancel_requested || cancellation().cancelled();
    impl_->finish(cancelled ? TaskStatus::Cancelled : TaskStatus::Failed,
                  cancelled ? "cancelled" : (error_code.empty() ? "rolled_back" : std::move(error_code)), std::move(message));
}
std::vector<RecoveryEntry> Registry::recover(const fs::path& parent, const Cancellation& cancel) {
    return transport::detail::boundary("recovery_failed", "Cannot inspect task recovery journals", [&] {
        const auto root = detail::checked_absolute(parent);
        const auto cancelled = transport::detail::cancellation_check(cancel);
        detail::ParentGuard pin(root, false, detail::GuardMode::NamespaceMutation);
        std::vector<RecoveryEntry> out;
        for (const auto& entry : fs::directory_iterator(detail::io_path(root))) {
            transport::detail::check_cancel(cancelled);
            const auto workspace = detail::logical_path(entry.path());
            const auto name = detail::path_utf8(entry.path().filename());
            if (!name.starts_with(".nickel-task-")) continue;
            RecoveryEntry result; result.workspace = workspace; result.task_id = name.substr(13);
            if (result.task_id.size() != 32 || !std::all_of(result.task_id.begin(), result.task_id.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) continue;
            try {
                std::unique_lock registry_lock(impl_->mutex);
                if (impl_->active.contains(result.task_id)) throw std::runtime_error("transaction is still active");
                detail::reject_links(workspace);
                auto owner_lock = detail::SecureFile::lock_workspace(workspace / "owner.lock", false);
                const auto journal = Json::parse(bytes_text(detail::read_bounded(workspace / "journal.json", journal_read_limit)));
                if (journal.at("schema") != 1 || journal.at("taskId").get<std::string>() != result.task_id)
                    throw std::runtime_error("foreign transaction journal");
                const auto identity = read_identity(journal.at("workspaceIdentity"));
                if (detail::directory_identity(workspace) != identity) throw std::runtime_error("transaction workspace identity changed");
                const auto target = target_path(detail::utf8_path(journal.at("target").get<std::string>()));
                if (target.parent_path() != root) throw std::runtime_error("journal target outside recovery parent");
                std::vector<fs::path> targets;
                const auto& reserved_targets = journal.at("targets");
                if (!reserved_targets.is_array() || reserved_targets.empty() || reserved_targets.size() > 129)
                    throw std::runtime_error("invalid journal target claims");
                for (const auto& item : reserved_targets) targets.push_back(target_path(detail::utf8_path(item.get<std::string>())));
                if (targets.front() != target) throw std::runtime_error("journal primary target changed");
                for (const auto& [id, record] : impl_->active) {
                    (void)id;
                    for (const auto& occupied : record->targets) for (const auto& desired : targets)
                        if (detail::paths_overlap(occupied, desired)) throw std::runtime_error("recovery target is reserved");
                }
                const auto reserved = [&](const fs::path& path) {
                    const auto child = detail::case_key(detail::path_utf8(path));
                    return std::any_of(targets.begin(), targets.end(), [&](const auto& allowed) {
                        const auto key = detail::case_key(detail::path_utf8(allowed));
                        return child == key || child.starts_with(key + "/");
                    });
                };
                const auto state = journal.at("state").get<std::string>();
                if (state != "running" && state != "committing" && state != "committed" && state != "rolling_back")
                    throw std::runtime_error("unknown journal state");
                if (journal.value("externalPublication", false) && state == "committing")
                    transport::detail::fail("external_publication_pending", "Domain owner must resolve the external publication outcome");
                const bool incremental = journal.value("incrementalFiles", false);
                if (incremental && (journal.value("directoryPublication", false) || journal.value("externalPublication", false)))
                    throw std::runtime_error("journal publication modes are mixed");
                result.committed = state == "committed";
                if (journal.value("directoryPublication", false)) {
                    const auto staging = workspace / "staging";
                    if (!fs::exists(detail::io_path(staging)) && fs::exists(detail::io_path(target)) &&
                        detail::directory_identity(target) == read_identity(journal.at("stagingIdentity"))) result.committed = true;
                    else if (!fs::exists(detail::io_path(staging)) || result.committed)
                        throw std::runtime_error("directory publication identity is ambiguous");
                } else {
                    Task::Impl recovery;
                    recovery.workspace = workspace; recovery.workspace_identity = identity;
                    recovery.journal = journal; recovery.journal_path = workspace / "journal.json";
                    const auto& files = journal.at("files");
                    if (!files.is_array() || files.size() > max_publish_files ||
                        (incremental && files.empty() && (state == "committed" || state == "committing")))
                        throw std::runtime_error("journal file count invalid");
                    std::unordered_set<std::string> destinations, sources, backups, step_ids;
                    std::unordered_map<std::string, std::size_t> steps;
                    bool pending_step{}, final_step{};
                    const auto identity_at = [](const fs::path& path) -> std::optional<detail::FileIdentity> {
                        if (!fs::exists(detail::io_path(path))) return {};
                        return detail::SecureFile::open_read(path).identity();
                    };
                    for (const auto& row : files) {
                        transport::detail::check_cancel(cancelled);
                        const auto relative = detail::validate_relative(row.at("source").get<std::string>());
                        if (!relative.starts_with("staging/") || !sources.insert(detail::case_key(relative)).second)
                            throw std::runtime_error("journal source outside staging or duplicate");
                        Published file;
                        file.source = workspace / detail::utf8_path(relative);
                        file.destination = target_path(detail::utf8_path(row.at("destination").get<std::string>()));
                        file.identity = read_identity(row.at("identity"));
                        file.size = row.at("size").get<std::uint64_t>(); file.sha256 = row.at("sha256").get<std::string>();
                        bool marked_step{};
                        if (incremental) {
                            file.step_id = row.at("stepId").get<std::string>();
                            file.step_final = row.at("stepFinal").get<bool>();
                            marked_step = row.at("stepCommitted").get<bool>();
                            file.durability_pending = row.at("durabilityPending").get<bool>();
                            if (!valid_step_id(file.step_id) || !step_ids.insert(file.step_id).second || pending_step || final_step)
                                throw std::runtime_error("journal file steps are not a unique contiguous prefix");
                            final_step = file.step_final;
                        } else if (row.contains("stepId") || row.contains("stepCommitted") || row.contains("stepFinal")) {
                            throw std::runtime_error("journal step metadata requires incremental publication mode");
                        }
                        const auto destination_key = detail::case_key(detail::path_utf8(file.destination));
                        if (file.sha256.size() != 64 || !reserved(file.destination) || !destinations.insert(destination_key).second)
                            throw std::runtime_error("journal destination or content revision is invalid");
                        if (row.contains("previous")) {
                            const auto& previous = row.at("previous");
                            const auto original = target_path(detail::utf8_path(previous.at("destination").get<std::string>()));
                            const auto original_key = detail::case_key(detail::path_utf8(original));
                            if (!reserved(original) || (original_key != destination_key && !destinations.insert(original_key).second))
                                throw std::runtime_error("journal original revision is not exclusively reserved");
                            file.previous = FileLease{original, read_identity(previous.at("identity")),
                                previous.at("size").get<std::uint64_t>(), previous.at("sha256").get<std::string>()};
                            if (file.previous->sha256.size() != 64) throw std::runtime_error("journal original digest invalid");
                            const auto backup = detail::validate_relative(row.at("backup").get<std::string>());
                            if (!backup.starts_with("backup/") || !backups.insert(detail::case_key(backup)).second)
                                throw std::runtime_error("journal backup outside workspace or duplicate");
                            file.backup = workspace / detail::utf8_path(backup);
                            if (row.contains("retained")) {
                                file.retained = target_path(detail::utf8_path(row.at("retained").get<std::string>()));
                                if (!reserved(*file.retained) || !destinations.insert(detail::case_key(detail::path_utf8(*file.retained))).second)
                                    throw std::runtime_error("journal retained revision is not exclusively reserved");
                            }
                        } else if (row.contains("backup") || row.contains("retained")) {
                            throw std::runtime_error("journal retention requires an original revision");
                        }
                        const auto destination_identity = identity_at(file.destination);
                        const auto source_identity = identity_at(file.source);
                        if (destination_identity == file.identity) {
                            if (source_identity) throw std::runtime_error("published identity appears in both paths");
                            file.moved = true;
                        } else if (source_identity != file.identity) {
                            throw std::runtime_error("recovery refuses missing or foreign publication");
                        }
                        if ((result.committed || marked_step) && !file.moved)
                            throw std::runtime_error("committed publication was replaced");
                        if (file.moved) {
                            auto next = detail::SecureFile::open_publication(file.destination, file.identity, detail::GuardMode::NamespaceMutation);
                            check_lease(next, FileLease{file.destination, file.identity, file.size, file.sha256}, cancelled);
                        }
                        if (file.previous) {
                            const auto original_identity = identity_at(file.previous->destination);
                            const auto backup_identity = identity_at(file.backup);
                            const auto retained_identity = file.retained ? identity_at(*file.retained) : std::optional<detail::FileIdentity>{};
                            if ((backup_identity && *backup_identity != file.previous->identity) ||
                                (retained_identity && *retained_identity != file.previous->identity) ||
                                (backup_identity && retained_identity)) throw std::runtime_error("old revision backup changed or duplicated");
                            file.backed_up = backup_identity.has_value() || retained_identity.has_value();
                            file.retained_moved = retained_identity.has_value();
                            if (file.backed_up) {
                                if (original_identity && !(file.previous->destination == file.destination && file.moved))
                                    throw std::runtime_error("original path was replaced during recovery");
                                auto previous = detail::SecureFile::open_publication(file.retained_moved ? *file.retained : file.backup,
                                    file.previous->identity, detail::GuardMode::NamespaceMutation);
                                check_lease(previous, *file.previous, cancelled);
                            } else if (!(result.committed || marked_step || (incremental && file.moved && !file.retained))) {
                                if (original_identity != file.previous->identity || file.moved)
                                    throw std::runtime_error("old revision is missing during recovery");
                                auto previous = detail::SecureFile::open_read(file.previous->destination);
                                check_lease(previous, *file.previous, cancelled);
                            } else if (file.retained || (file.previous->destination != file.destination && original_identity)) {
                                throw std::runtime_error("committed old revision retention is incomplete");
                            }
                            if (!file.moved && file.destination != file.previous->destination && destination_identity)
                                throw std::runtime_error("renamed recovery target contains foreign file");
                            if ((result.committed || marked_step) && file.retained && !file.retained_moved)
                                throw std::runtime_error("retained revision publication incomplete");
                        } else if (!file.moved && destination_identity) {
                            throw std::runtime_error("recovery target contains foreign file");
                        }
                        if (incremental) {
                            // Retained identities and content, not stale move bits, establish a completed step.
                            file.step_committed = file.moved && (!file.retained || file.retained_moved);
                            if (marked_step && !file.step_committed) throw std::runtime_error("committed file step is incomplete");
                            pending_step = !file.step_committed;
                            if (file.step_committed) {
                                file.durability_pending = file.durability_pending || !marked_step;
                                result.committed_files.push_back(receipt_of(file));
                            }
                            steps.emplace(file.step_id, recovery.published.size());
                        }
                        recovery.published.push_back(std::move(file));
                    }
                    if (incremental && !recovery.published.empty()) {
                        const auto& last = recovery.published.back();
                        if (result.committed && (!last.step_final || !last.step_committed))
                            throw std::runtime_error("terminal file-step journal lacks a final real publication");
                        result.committed = last.step_final && last.step_committed;
                    }
                    if (journal.contains("directories")) {
                        const auto& directories = journal.at("directories");
                        if (!directories.is_array() || directories.size() > max_publish_files)
                            throw std::runtime_error("journal directory count invalid");
                        std::unordered_set<std::string> unique;
                        for (const auto& row : directories) {
                            const auto path = target_path(detail::utf8_path(row.at("path").get<std::string>()));
                            if (!reserved(path) || !unique.insert(detail::case_key(detail::path_utf8(path))).second)
                                throw std::runtime_error("journal directory not reserved or duplicate");
                            CreatedDirectory directory; directory.path = path;
                            if (incremental) {
                                directory.step_id = row.at("stepId").get<std::string>();
                                const auto step = steps.find(directory.step_id);
                                if (step == steps.end()) throw std::runtime_error("journal directory has no file step");
                                directory.step_committed = recovery.published[step->second].step_committed;
                                if (row.at("stepCommitted").get<bool>() && !directory.step_committed)
                                    throw std::runtime_error("journal directory claims an uncommitted file step");
                            } else if (row.contains("stepId") || row.contains("stepCommitted")) {
                                throw std::runtime_error("journal directory step metadata requires incremental mode");
                            }
                            if (!row.contains("identity")) {
                                if (directory.step_committed || (fs::exists(detail::io_path(path)) && !result.committed))
                                    throw std::runtime_error("created directory identity is ambiguous");
                                continue;
                            }
                            directory.identity = read_identity(row.at("identity"));
                            if (directory.step_committed && detail::directory_identity(path) != *directory.identity)
                                throw std::runtime_error("committed step directory identity changed");
                            recovery.directories.push_back(std::move(directory));
                        }
                    }
                    transport::detail::check_cancel(cancelled);
                    if (incremental) recovery.save(result.committed ? "committed" : "rolling_back");
                    if (!result.committed) recovery.reverse_publications();
                }
                transport::detail::check_cancel(cancelled);
                owner_lock.close();
                detail::remove_owned_tree(workspace, identity); result.recovered = true;
            } catch (const ErrorException& error) {
                if (error.error().code == "cancelled") throw;
                result.error = error.error().message;
            } catch (const std::exception&) { result.error = "Recovery conflict; owned journal retained"; }
            out.push_back(std::move(result));
        }
        return out;
    });
}
nlohmann::json to_json(const Snapshot& snapshot) {
    const auto optional = [](const auto& value) -> Json { return value ? Json(*value) : Json(nullptr); };
    Json files = Json::array();
    for (const auto& file : snapshot.recent_files)
        files.push_back({{"name", file.name}, {"completedBytes", file.completed_bytes},
                         {"totalBytes", optional(file.total_bytes)}, {"done", file.done}});
    return Json{{"taskId", snapshot.id}, {"kind", static_cast<int>(snapshot.kind)},
        {"status", static_cast<int>(snapshot.status)}, {"revision", snapshot.revision},
        {"target", detail::path_utf8(snapshot.target)}, {"label", snapshot.label},
        {"stage", snapshot.stage}, {"detail", snapshot.detail}, {"cancelRequested", snapshot.cancel_requested},
        {"filesDone", snapshot.files_done}, {"filesTotal", optional(snapshot.files_total)},
        {"transferredBytes", snapshot.transferred_bytes}, {"completedBytes", snapshot.completed_bytes},
        {"totalBytes", optional(snapshot.total_bytes)}, {"percent", optional(snapshot.percent)},
        {"speedBps", optional(snapshot.speed_bytes_per_second)}, {"remainingMs", optional(snapshot.remaining_ms)},
        {"startedAt", snapshot.started_ms}, {"endedAt", optional(snapshot.ended_ms)}, {"files", files},
        {"errorCode", snapshot.error_code}, {"message", snapshot.message}};
}
} // namespace nickel::tasks
