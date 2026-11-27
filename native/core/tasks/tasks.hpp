#pragma once

#include "core/network/http.hpp"
#include "core/network/hash.hpp"
#include "core/tasks/secure_fs.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nickel::tasks {
enum class TaskKind { Version, Modpack, Resource, Installer, Export, Runtime };
enum class TaskStatus { Running, Cancelling, Committing, Succeeded, Cancelled, Failed };
enum class EventKind { Began, Stage, File, Progress, CancelRequested, Terminal };
struct FileProgress {
    std::string name;
    std::uint64_t completed_bytes{};
    std::optional<std::uint64_t> total_bytes;
    bool done{};
};
struct Snapshot {
    std::string id;
    TaskKind kind{TaskKind::Version};
    TaskStatus status{TaskStatus::Running};
    std::uint64_t revision{};
    std::filesystem::path target;
    std::string label;
    std::string stage;
    std::string detail;
    bool cancel_requested{};
    std::uint64_t files_done{};
    std::optional<std::uint64_t> files_total;
    std::uint64_t transferred_bytes{};
    std::uint64_t completed_bytes{};
    std::optional<std::uint64_t> total_bytes;
    std::optional<double> percent;
    std::optional<double> speed_bytes_per_second;
    std::optional<std::uint64_t> remaining_ms;
    std::uint64_t started_ms{};
    std::optional<std::uint64_t> ended_ms;
    std::vector<FileProgress> recent_files;
    std::string error_code;
    std::string message;
};
struct Event {
    std::uint64_t sequence{};
    EventKind kind{EventKind::Progress};
    Snapshot snapshot;
};
struct Delta {
    std::uint64_t cursor{};
    bool reset_required{};
    std::vector<Event> events;
    std::vector<Snapshot> snapshots;
};
struct TaskSpec {
    TaskKind kind{TaskKind::Version};
    std::filesystem::path target;
    std::vector<std::filesystem::path> additional_targets;
    std::string label;
    transport::NetworkSnapshot network;
    std::optional<Cancellation> cancellation;
};
struct FileLease {
    std::filesystem::path destination;
    detail::FileIdentity identity;
    std::uint64_t size{};
    std::string sha256;
};
struct PublishFile {
    std::string source_relative;
    std::filesystem::path destination;
    std::optional<FileLease> previous;
    std::optional<std::filesystem::path> retain_previous;
};
struct FileCommitReceipt {
    std::string step_id;
    FileLease published;
    bool final{};
    bool durability_pending{};
};
FileLease lease_file(const std::filesystem::path& path, const Cancellation& cancel);
struct RecoveryEntry {
    std::filesystem::path workspace;
    std::string task_id;
    bool committed{};
    bool recovered{};
    std::string error;
    std::vector<FileCommitReceipt> committed_files;
};
class Task;
class Registry {
public:
    explicit Registry(std::uint32_t history_limit = 32, std::uint32_t event_limit = 2048);
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    std::shared_ptr<Task> begin(const TaskSpec& spec);
    bool cancel(std::string_view id);
    // Copies a live non-committing token; stop callbacks must run outside all caller locks.
    std::optional<Cancellation> cancellation_for(std::string_view id) const;
    bool conflicts(const std::filesystem::path& target) const;
    Delta since(std::uint64_t cursor) const;
    std::vector<Snapshot> snapshots() const;
    std::vector<RecoveryEntry> recover(const std::filesystem::path& parent,
                                      const Cancellation& cancel);
    static std::shared_ptr<Registry> process();
private:
    friend class Task;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

class Task {
public:
    ~Task();
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    const std::string& id() const noexcept;
    const Cancellation& cancellation() const noexcept;
    const transport::NetworkSnapshot& network() const noexcept;
    const std::filesystem::path& workspace() const noexcept;
    const std::filesystem::path& staging() const noexcept;
    Snapshot snapshot() const;
    bool cancel();
    void stage(std::string name, std::string detail = {});
    void progress(const transport::TransferProgress& progress);
    void file(FileProgress progress);
    void files(std::uint64_t done, std::optional<std::uint64_t> total = {});
    void commit_directory();
    // Completes non-publication work and removes only this task's owned workspace.
    void complete_work();
    // Only the irreversible domain commit/spawn belongs here, not its preparation.
    // The callback must return after durable publication and throw only before it.
    void commit_external(const std::function<void()>& publication);
    void adopt_file(const std::filesystem::path& source, std::string_view relative,
                    detail::FileIdentity identity);
    void commit_files(const std::vector<PublishFile>& files,
                      const std::vector<std::filesystem::path>& directories = {});
    // One real publication per stable step ID. Committed prefixes survive cancellation/recovery.
    // A final step finishes under the same gate; repeated identical IDs return historical receipts.
    FileCommitReceipt commit_file_step(std::string_view step_id, const PublishFile& file, bool final);
    std::vector<FileCommitReceipt> committed_files() const;
    void commit_replacement(const std::filesystem::path& original, std::string_view expected_sha256,
                            std::string_view source_relative, const std::filesystem::path& destination,
                            std::optional<std::filesystem::path> retain_original = {});
    void rollback(std::string error_code = {}, std::string message = {});
private:
    friend class Registry;
    struct Impl;
    explicit Task(std::unique_ptr<Impl> impl);
    std::optional<FileCommitReceipt> publish_files(const std::vector<PublishFile>& files,
        const std::vector<std::filesystem::path>& directories, std::string step_id, bool final);
    std::unique_ptr<Impl> impl_;
};
nlohmann::json to_json(const Snapshot& snapshot);
} // namespace nickel::tasks
