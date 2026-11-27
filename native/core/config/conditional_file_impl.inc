#ifndef NICKEL_CONDITIONAL_TEST_HOOK
#define NICKEL_CONDITIONAL_TEST_HOOK(point, target, stage, backup, receipt) ((void)0)
#define NICKEL_UNDEFINE_CONDITIONAL_HOOK
#endif
#ifndef _WIN32
#include <dirent.h>
#endif

namespace nickel {
namespace {
constexpr std::uint64_t conditional_limit = 8 * 1024 * 1024;
constexpr std::string_view conditional_prefix = ".nickel-conditional-";

void conditional_bound(std::uint64_t limit) {
    if (limit > conditional_limit) fail("LimitExceeded", "Conditional configuration files are limited to 8 MiB");
}
void conditional_target(const std::filesystem::path& path) {
    const auto name = path_to_utf8(path.filename());
    if (name.empty() || name == ".nickel-config.lock" || name.starts_with(conditional_prefix) ||
        name.starts_with(".nickel-dir-anchor-")) fail("InvalidArgument", "Conditional target uses a reserved configuration name");
}
Json conditional_object(const FileIdentity& info) {
#ifdef _WIN32
    return Json::array({std::uint64_t(info.dwVolumeSerialNumber), std::uint64_t(info.nFileIndexHigh),
        std::uint64_t(info.nFileIndexLow), std::uint64_t(info.ftCreationTime.dwHighDateTime),
        std::uint64_t(info.ftCreationTime.dwLowDateTime)});
#else
    return Json::array({std::uint64_t(info.st_dev), std::uint64_t(info.st_ino)});
#endif
}
Json conditional_parent(const DirectoryGuard& parent) {
#ifdef _WIN32
    return conditional_object(file_identity(reinterpret_cast<HANDLE>(parent.native_handle()), parent.path(), true));
#else
    return conditional_object(file_identity(static_cast<int>(parent.native_handle()), parent.path(), true));
#endif
}
void conditional_rewind(const Handle& handle, const std::filesystem::path& path) {
#ifdef _WIN32
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(handle.value, beginning, nullptr, FILE_BEGIN)) io_fail("rewind conditional file", path, static_cast<int>(GetLastError()));
#else
    if (::lseek(handle.value, 0, SEEK_SET) < 0) io_fail("rewind conditional file", path, errno);
#endif
}
Json conditional_revision(const Handle& handle, const FileIdentity& info, const std::filesystem::path& path) {
#ifdef _WIN32
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(handle.value, FileBasicInfo, &basic, sizeof(basic))) {
        io_fail("inspect conditional revision", path, static_cast<int>(GetLastError()));
    }
    return Json{{"write", Json::array({std::uint64_t(info.ftLastWriteTime.dwHighDateTime), std::uint64_t(info.ftLastWriteTime.dwLowDateTime)})},
                {"change", Json::array({std::int64_t(basic.ChangeTime.QuadPart)})}};
#else
    (void)handle; (void)path;
#if defined(__APPLE__)
    return Json{{"write", Json::array({std::int64_t(info.st_mtimespec.tv_sec), std::int64_t(info.st_mtimespec.tv_nsec)})},
                {"change", Json::array({std::int64_t(info.st_ctimespec.tv_sec), std::int64_t(info.st_ctimespec.tv_nsec)})}};
#else
    return Json{{"write", Json::array({std::int64_t(info.st_mtim.tv_sec), std::int64_t(info.st_mtim.tv_nsec)})},
                {"change", Json::array({std::int64_t(info.st_ctim.tv_sec), std::int64_t(info.st_ctim.tv_nsec)})}};
#endif
#endif
}
struct ConditionalMeasurement {
    FileIdentity identity{};
    Json record;
    std::vector<std::uint8_t> bytes;
    ConditionalMeasurement() = default;
    ConditionalMeasurement(ConditionalMeasurement&&) = default;
    ~ConditionalMeasurement() { erase_bytes(bytes); }
};
ConditionalMeasurement conditional_measure(const Handle& handle, const std::filesystem::path& path,
    std::uint64_t limit, const Cancellation* cancellation = nullptr) {
    check_cancel(cancellation);
    conditional_rewind(handle, path);
    const auto before = file_identity(handle.value, path);
    const auto revision = conditional_revision(handle, before, path);
    ConditionalMeasurement value;
    value.bytes = read_contents(handle, path, limit);
    check_cancel(cancellation);
    value.identity = file_identity(handle.value, path);
    if (!same_revision(before, value.identity) || conditional_revision(handle, value.identity, path) != revision) {
        fail("Conflict", "Conditional file changed during its observation");
    }
    Sha256 digest;
    digest.update(value.bytes);
    value.record = revision;
    value.record["object"] = conditional_object(value.identity);
    value.record["size"] = std::uint64_t(value.bytes.size());
    value.record["hash"] = digest.finish();
    return value;
}
bool conditional_record_matches(const Json& actual, const Json& expected, bool after_rename) {
    return actual["object"] == expected["object"] && actual["size"] == expected["size"] &&
        actual["write"] == expected["write"] && actual["hash"] == expected["hash"] &&
        (after_rename || actual["change"] == expected["change"]);
}
struct ConditionalRetainedFile {
    const DirectoryGuard& parent;
    std::filesystem::path path;
    Handle handle;
    bool exists = false;
    FileIdentity identity{};
    ConditionalRetainedFile(const DirectoryGuard& directory, const std::filesystem::path& file)
        : parent(directory), path(file), handle([&] {
#ifdef _WIN32
            return reinterpret_cast<HANDLE>(windows_open_relative(parent, path.filename(),
                GENERIC_READ | DELETE, FILE_SHARE_READ, 1, 0x40));
#else
            return ::openat(static_cast<int>(parent.native_handle()), path.filename().c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
#endif
        }()) {
#ifdef _WIN32
        if (handle.value == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) return;
            io_fail("retain conditional file", path, static_cast<int>(error));
        }
#else
        if (handle.value < 0) {
            if (errno == ENOENT) return;
            io_fail("retain conditional file", path, errno);
        }
#endif
        exists = true;
        identity = file_identity(handle.value, path);
        verify_name();
    }
    void verify_name() const {
        parent.verify();
        if (!exists) return;
        if (!same_identity(identity, file_identity(handle.value, path))) fail("Conflict", "Retained conditional object changed identity");
#ifndef _WIN32
        struct stat named{};
        if (::fstatat(static_cast<int>(parent.native_handle()), path.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(named.st_mode) || named.st_nlink != 1 || !same_identity(identity, named)) {
            fail("Conflict", "Retained conditional name no longer identifies the observed object");
        }
#endif
    }
};
bool conditional_move(const Handle& handle, const DirectoryGuard& parent,
    const std::filesystem::path& source, const std::filesystem::path& destination) {
    parent.verify();
#ifdef _WIN32
    (void)source;
    return rename_handle(handle.value, parent, parent, destination, false);
#else
    (void)handle;
    return reserve_relative(parent, source, parent, destination, false);
#endif
}
void conditional_discard(const Handle& handle, const DirectoryGuard& parent,
    const std::filesystem::path& path, const FileIdentity& expected) {
    parent.verify();
    if (!same_identity(expected, file_identity(handle.value, path))) fail("Conflict", "Conditional cleanup object changed identity");
#ifdef _WIN32
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(handle.value, FileDispositionInfo, &disposition, sizeof(disposition))) {
        io_fail("remove conditional owned file", path, static_cast<int>(GetLastError()));
    }
#else
    struct stat named{};
    if (::fstatat(static_cast<int>(parent.native_handle()), path.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(named.st_mode) || named.st_nlink != 1 || !same_identity(expected, named)) {
        fail("Conflict", "Conditional cleanup name changed identity");
    }
    if (::unlinkat(static_cast<int>(parent.native_handle()), path.filename().c_str(), 0) != 0) {
        io_fail("remove conditional owned file", path, errno);
    }
#endif
}
[[noreturn]] void conditional_recovery_required(const std::filesystem::path& receipt,
    const std::filesystem::path& target, const std::filesystem::path& backup, std::string_view reason,
    bool committed = false) {
    throw ErrorException(Error{"RecoveryRequired", "Conditional replacement preserved files requiring recovery",
        Json{{"receipt", path_to_utf8(receipt)}, {"target", path_to_utf8(target)},
             {"backup", path_to_utf8(backup)}, {"reason", reason}, {"committed", committed}}});
}
std::vector<std::filesystem::path> conditional_receipts(const DirectoryGuard& parent) {
    std::vector<std::filesystem::path> paths;
    const auto collect = [&](const std::filesystem::path& leaf) {
        const auto name = path_to_utf8(leaf);
        if (name.starts_with(conditional_prefix) && name.ends_with(".json")) {
            if (paths.size() == 128) fail("LimitExceeded", "Too many conditional recovery receipts");
            paths.push_back(parent.path() / leaf);
        }
    };
#ifdef _WIN32
    std::vector<std::uint64_t> storage(8192);
    bool restart = true;
    for (;;) {
        if (!GetFileInformationByHandleEx(reinterpret_cast<HANDLE>(parent.native_handle()),
            restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
            storage.data(), static_cast<DWORD>(storage.size() * sizeof(std::uint64_t)))) {
            const auto error = GetLastError();
            if (error == ERROR_NO_MORE_FILES) break;
            io_fail("scan conditional receipts", parent.path(), static_cast<int>(error));
        }
        restart = false;
        std::size_t offset = 0;
        for (;;) {
            const auto capacity = storage.size() * sizeof(std::uint64_t);
            if (offset > capacity - sizeof(FILE_ID_BOTH_DIR_INFO)) fail("CorruptData", "Directory enumeration record is invalid");
            const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<const std::uint8_t*>(storage.data()) + offset);
            if (entry->FileNameLength % sizeof(wchar_t) || entry->FileNameLength > capacity - offset - offsetof(FILE_ID_BOTH_DIR_INFO, FileName)) {
                fail("CorruptData", "Directory enumeration filename is invalid");
            }
            collect(std::filesystem::path(std::wstring(entry->FileName, entry->FileNameLength / sizeof(wchar_t))));
            if (!entry->NextEntryOffset) break;
            if (entry->NextEntryOffset < sizeof(FILE_ID_BOTH_DIR_INFO) || entry->NextEntryOffset > capacity - offset) {
                fail("CorruptData", "Directory enumeration offset is invalid");
            }
            offset += entry->NextEntryOffset;
        }
    }
#else
    const auto fd = ::dup(static_cast<int>(parent.native_handle()));
    if (fd < 0) io_fail("duplicate conditional directory", parent.path(), errno);
    DIR* raw = ::fdopendir(fd);
    if (!raw) { const auto error = errno; ::close(fd); io_fail("scan conditional receipts", parent.path(), error); }
    struct CloseDirectory { DIR* value; ~CloseDirectory() { ::closedir(value); } } directory{raw};
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory.value);
        if (!entry) { if (errno) io_fail("scan conditional receipts", parent.path(), errno); break; }
        collect(std::filesystem::path(entry->d_name));
    }
#endif
    parent.verify();
    std::sort(paths.begin(), paths.end());
    return paths;
}
bool conditional_hex(std::string_view value, std::size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool conditional_record_valid(const Json& record, std::uint64_t limit) {
    if (!record.is_object() || record.size() != 5 || !record.contains("object") || !record.contains("size") ||
        !record.contains("write") || !record.contains("change") || !record.contains("hash") ||
        !record["size"].is_number_unsigned() || record["size"].get<std::uint64_t>() > limit ||
        !record["hash"].is_string() || !conditional_hex(record["hash"].get<std::string>(), 64) ||
        !record["object"].is_array() || !record["write"].is_array() || !record["change"].is_array()) return false;
#ifdef _WIN32
    if (record["object"].size() != 5 || record["write"].size() != 2 || record["change"].size() != 1) return false;
#else
    if (record["object"].size() != 2 || record["write"].size() != 2 || record["change"].size() != 2) return false;
#endif
    for (const auto& value : record["object"]) if (!value.is_number_unsigned()) return false;
    for (const auto& value : record["write"]) if (!value.is_number_integer()) return false;
    for (const auto& value : record["change"]) if (!value.is_number_integer()) return false;
    return true;
}
#ifdef _WIN32
constexpr std::string_view conditional_platform = "windows";
#else
constexpr std::string_view conditional_platform = "posix";
#endif
Json conditional_parse_receipt(const ConditionalMeasurement& measurement,
    const DirectoryGuard& parent, const std::filesystem::path& path) {
    const auto name = path_to_utf8(path.filename());
    const auto token = std::string_view(name).substr(conditional_prefix.size(), name.size() - conditional_prefix.size() - 5);
    const auto& bytes = measurement.bytes;
    auto value = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!conditional_hex(token, 32) || !value.is_object() || value.size() != 11 ||
        !value.contains("schemaVersion") || !value.contains("transactionId") || !value.contains("platform") ||
        !value.contains("target") || !value.contains("parentObject") || !value.contains("stage") || !value.contains("backup") ||
        !value.contains("originalExists") || !value.contains("original") || !value.contains("replacement") || !value.contains("limit") ||
        !value["schemaVersion"].is_number_unsigned() || value["schemaVersion"] != 1 ||
        !value["transactionId"].is_string() || value["transactionId"].get<std::string>() != token ||
        !value["platform"].is_string() || value["platform"].get<std::string>() != conditional_platform ||
        !value["target"].is_string() || !value["stage"].is_string() || !value["backup"].is_string() ||
        !value["originalExists"].is_boolean() || !value["limit"].is_number_unsigned() ||
        value["limit"].get<std::uint64_t>() > conditional_limit || value["parentObject"] != conditional_parent(parent) ||
        value["stage"].get<std::string>() != ".nickel-conditional-stage-" + std::string(token) ||
        value["backup"].get<std::string>() != ".nickel-conditional-backup-" + std::string(token)) {
        fail("CorruptData", "Conditional receipt schema or parent binding is invalid");
    }
    const auto target = checked_absolute_path(path_from_utf8(value["target"].get<std::string>()));
    conditional_target(target);
    if (target.parent_path() != parent.path() || path_to_utf8(target) != value["target"].get<std::string>() ||
        !conditional_record_valid(value["replacement"], value["limit"].get<std::uint64_t>()) ||
        (value["originalExists"].get<bool>() ? !conditional_record_valid(value["original"], conditional_limit) : !value["original"].is_null())) {
        fail("CorruptData", "Conditional receipt file binding is invalid");
    }
    return value;
}
ConditionalRecoveryReport conditional_recover_unlocked(const DirectoryGuard& parent) {
    ConditionalRecoveryReport report;
    for (const auto& receipt_path : conditional_receipts(parent)) {
        auto target_path = parent.path();
        auto backup_path = parent.path();
        bool committed_fact = false;
        try {
            ConditionalRetainedFile receipt(parent, receipt_path);
            if (!receipt.exists) continue;
            const auto receipt_value = conditional_measure(receipt.handle, receipt.path, 64 * 1024);
            const auto value = conditional_parse_receipt(receipt_value, parent, receipt_path);
            target_path = path_from_utf8(value["target"].get<std::string>());
            const auto stage_path = parent.path() / path_from_utf8(value["stage"].get<std::string>());
            backup_path = parent.path() / path_from_utf8(value["backup"].get<std::string>());
            ConditionalRetainedFile target(parent, target_path), stage(parent, stage_path), backup(parent, backup_path);
            const bool had_original = value["originalExists"].get<bool>();
            const auto matches = [&](const ConditionalRetainedFile& file, const Json& record) {
                if (!file.exists) return false;
                file.verify_name();
                const auto observed = conditional_measure(file.handle, file.path, conditional_limit);
                return conditional_record_matches(observed.record, record, true);
            };
            committed_fact = matches(target, value["replacement"]);
            if (stage.exists && !matches(stage, value["replacement"])) fail("Conflict", "Conditional stage was replaced or changed");
            if (backup.exists && (!had_original || !matches(backup, value["original"]))) fail("Conflict", "Conditional backup was replaced or changed");
            if (committed_fact) {
                if (stage.exists) fail("Conflict", "Published conditional stage also has an unexpected stage name");
                if (backup.exists) conditional_discard(backup.handle, parent, backup.path, backup.identity);
                ++report.committed;
            } else if (had_original && target.exists && !backup.exists && matches(target, value["original"])) {
                if (stage.exists) conditional_discard(stage.handle, parent, stage.path, stage.identity);
                ++report.restored;
            } else if (!target.exists && had_original && backup.exists && stage.exists) {
                if (!conditional_move(backup.handle, parent, backup.path, target_path)) fail("Conflict", "External object occupies the conditional restore name");
                backup.path = target_path;
                backup.verify_name();
                if (!matches(backup, value["original"])) fail("Conflict", "Restored conditional original failed verification");
                conditional_discard(stage.handle, parent, stage.path, stage.identity);
                ++report.restored;
            } else if (!had_original && !target.exists && !backup.exists && stage.exists) {
                conditional_discard(stage.handle, parent, stage.path, stage.identity);
                ++report.restored;
            } else {
                // A receipt alone cannot distinguish an abort from a removed publication.
                fail("Conflict", "Conditional recovery preserved an external occupant or ambiguous publication");
            }
            receipt.verify_name();
            if (!conditional_record_matches(conditional_measure(receipt.handle, receipt.path, 64 * 1024).record, receipt_value.record, false)) {
                fail("Conflict", "Conditional receipt changed during recovery");
            }
            sync_pinned_directory(parent);
            conditional_discard(receipt.handle, parent, receipt.path, receipt.identity);
            sync_pinned_directory(parent);
        } catch (const ErrorException& error) {
            conditional_recovery_required(receipt_path, target_path, backup_path, error.error().code, committed_fact);
        } catch (...) {
            conditional_recovery_required(receipt_path, target_path, backup_path, "UnclassifiedRecoveryFailure", committed_fact);
        }
    }
    return report;
}
} // namespace

struct ConditionalFileSnapshot::Impl {
    std::filesystem::path path;
    Json parent_object;
    Json expected;
    bool exists = false;
    std::vector<std::uint8_t> bytes;
    std::string hash;
    ~Impl() { erase_bytes(bytes); }
};
struct ConditionalFileAccess {
    static const ConditionalFileSnapshot::Impl& get(const ConditionalFileSnapshot& snapshot) {
        if (!snapshot.impl_) fail("InvalidArgument", "Conditional snapshot is empty or moved from");
        return *snapshot.impl_;
    }
};
#ifndef NICKEL_CONDITIONAL_TEST_REPLACE_ONLY
ConditionalFileSnapshot::ConditionalFileSnapshot(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
const std::filesystem::path& ConditionalFileSnapshot::path() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->path : empty;
}
bool ConditionalFileSnapshot::exists() const noexcept { return impl_ && impl_->exists; }
std::span<const std::uint8_t> ConditionalFileSnapshot::bytes() const noexcept {
    return impl_ ? std::span<const std::uint8_t>(impl_->bytes) : std::span<const std::uint8_t>{};
}
std::string_view ConditionalFileSnapshot::sha256() const noexcept { return impl_ ? std::string_view(impl_->hash) : std::string_view{}; }
ConditionalFileSnapshot read_conditional_file(const std::filesystem::path& path, std::uint64_t limit,
    const Cancellation* cancellation) {
    conditional_bound(limit);
    check_cancel(cancellation);
    const auto absolute = checked_absolute_path(path);
    conditional_target(absolute);
    DirectoryGuard parent(absolute.parent_path(), false, DirectoryGuardMode::NamespaceMutation);
    FileLock lock(parent.path() / ".nickel-config.lock", cancellation, std::chrono::seconds(30), DirectoryGuardMode::NamespaceMutation);
    (void)conditional_recover_unlocked(parent);
    auto snapshot = std::make_shared<ConditionalFileSnapshot::Impl>();
    snapshot->path = absolute;
    snapshot->parent_object = conditional_parent(parent);
    ConditionalRetainedFile original(parent, absolute);
    snapshot->exists = original.exists;
    if (original.exists) {
        auto measurement = conditional_measure(original.handle, absolute, limit, cancellation);
        original.verify_name();
        snapshot->expected = std::move(measurement.record);
        snapshot->hash = snapshot->expected["hash"].get<std::string>();
        snapshot->bytes = std::move(measurement.bytes);
    }
    check_cancel(cancellation);
    parent.verify();
    return ConditionalFileSnapshot(std::move(snapshot));
}
#endif
ConditionalReplaceResult replace_file_conditionally(const ConditionalFileSnapshot& expected,
    std::span<const std::uint8_t> bytes, std::uint64_t limit, const AtomicWriteOptions& options) {
    conditional_bound(limit);
    if (bytes.size() > limit) fail("LimitExceeded", "Conditional replacement exceeds its explicit bound");
    const auto& snapshot = ConditionalFileAccess::get(expected);
    check_cancel(options.cancellation);
    DirectoryGuard parent(snapshot.path.parent_path(), options.create_parents, DirectoryGuardMode::NamespaceMutation);
    FileLock lock(parent.path() / ".nickel-config.lock", options.cancellation, std::chrono::seconds(30), DirectoryGuardMode::NamespaceMutation);
    (void)conditional_recover_unlocked(parent);
    if (conditional_parent(parent) != snapshot.parent_object) fail("Conflict", "Conditional snapshot parent changed identity");
    ConditionalRetainedFile original(parent, snapshot.path);
    const auto verify_original = [&](bool after_rename) {
        if (original.exists != snapshot.exists) fail("Conflict", "Conditional file presence changed since observation");
        if (!original.exists) return;
        original.verify_name();
        const auto observed = conditional_measure(original.handle, original.path, conditional_limit);
        if (!conditional_record_matches(observed.record, snapshot.expected, after_rename)) {
            fail("Conflict", "Conditional file identity, revision, size or digest changed since observation");
        }
    };
    verify_original(false);
    const auto token = unique_token();
    const auto stage_path = parent.path() / (".nickel-conditional-stage-" + token);
    const auto backup_path = parent.path() / (".nickel-conditional-backup-" + token);
    const auto receipt_path = parent.path() / (std::string(conditional_prefix) + token + ".json");
    CreatedFile stage(parent, stage_path, true);
    NICKEL_CONDITIONAL_TEST_HOOK("stage-created", snapshot.path, stage_path, backup_path, receipt_path);
    while (!bytes.empty()) {
        check_cancel(options.cancellation);
        const auto chunk = bytes.first(std::min<std::size_t>(bytes.size(), 64 * 1024));
        NICKEL_CONDITIONAL_TEST_HOOK("stage-write", snapshot.path, stage_path, backup_path, receipt_path);
        write_chunks(stage.handle.value, stage.path, chunk);
        bytes = bytes.subspan(chunk.size());
    }
    check_cancel(options.cancellation);
    NICKEL_CONDITIONAL_TEST_HOOK("stage-flush", snapshot.path, stage_path, backup_path, receipt_path);
    flush_file(stage.handle.value, stage.path);
    stage.verify_name();
    const auto replacement = conditional_measure(stage.handle, stage.path, limit, options.cancellation);
    Json value{{"schemaVersion", std::uint64_t(1)}, {"transactionId", token}, {"platform", conditional_platform},
        {"target", path_to_utf8(snapshot.path)}, {"parentObject", snapshot.parent_object},
        {"stage", path_to_utf8(stage_path.filename())}, {"backup", path_to_utf8(backup_path.filename())},
        {"originalExists", snapshot.exists}, {"original", snapshot.exists ? snapshot.expected : Json(nullptr)},
        {"replacement", replacement.record}, {"limit", limit}};
    const auto text = value.dump();
    if (text.size() > 64 * 1024) fail("LimitExceeded", "Conditional receipt exceeds its recovery bound");
    CreatedFile receipt(parent, receipt_path, true);
    write_contents(receipt.handle.value, receipt.path, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    sync_pinned_directory(parent);
    ConditionalReplaceResult result{false, receipt_path};
    stage.owned = false;
    receipt.owned = false;
    bool quarantined = false;
    bool published = false;
    bool committed = false;
    try {
        NICKEL_CONDITIONAL_TEST_HOOK("receipt-flushed", snapshot.path, stage_path, backup_path, receipt_path);
        verify_original(false);
        check_cancel(options.cancellation);
        NICKEL_CONDITIONAL_TEST_HOOK("before-quarantine", snapshot.path, stage_path, backup_path, receipt_path);
        verify_original(false);
        if (original.exists) {
            if (!conditional_move(original.handle, parent, original.path, backup_path)) fail("Conflict", "Conditional backup name is occupied");
            quarantined = true;
            original.path = backup_path;
            NICKEL_CONDITIONAL_TEST_HOOK("after-quarantine", snapshot.path, stage_path, backup_path, receipt_path);
            verify_original(true);
            sync_pinned_directory(parent);
        }
        check_cancel(options.cancellation);
        NICKEL_CONDITIONAL_TEST_HOOK("before-publication", snapshot.path, stage_path, backup_path, receipt_path);
        check_cancel(options.cancellation);
        if (quarantined) verify_original(true);
        stage.verify_name();
        if (!conditional_record_matches(conditional_measure(stage.handle, stage.path, limit).record, replacement.record, false)) {
            fail("Conflict", "Conditional stage changed before publication");
        }
        if (!conditional_move(stage.handle, parent, stage.path, snapshot.path)) fail("Conflict", "Conditional publication name is occupied");
        published = true;
#ifndef _WIN32
        // POSIX no-replace rename is name-based; prove it moved our held stage.
        stage.path = snapshot.path;
        stage.verify_name();
        if (!conditional_record_matches(conditional_measure(stage.handle, stage.path, limit).record, replacement.record, true)) {
            fail("Conflict", "Published conditional stage failed identity or digest verification");
        }
#endif
        // Successful verified no-replace publication is the linearization point.
        committed = true;
        result.committed = true;
        NICKEL_CONDITIONAL_TEST_HOOK("after-publication", snapshot.path, stage_path, backup_path, receipt_path);
        flush_file(stage.handle.value, snapshot.path);
        sync_pinned_directory(parent);
        NICKEL_CONDITIONAL_TEST_HOOK("before-cleanup", snapshot.path, stage_path, backup_path, receipt_path);
        if (quarantined) {
            verify_original(true);
            conditional_discard(original.handle, parent, original.path, original.identity);
        }
        sync_pinned_directory(parent);
        conditional_discard(receipt.handle, parent, receipt.path, receipt.identity);
        sync_pinned_directory(parent);
        result.recovery_receipt.clear();
        return result;
    } catch (...) {
        if (committed) return result;
        if (published) conditional_recovery_required(receipt_path, snapshot.path, backup_path, "AmbiguousStagePublication");
        const auto failure = std::current_exception();
        try {
            if (quarantined) {
                verify_original(true);
                NICKEL_CONDITIONAL_TEST_HOOK("before-restoration", snapshot.path, stage_path, backup_path, receipt_path);
                if (!conditional_move(original.handle, parent, original.path, snapshot.path)) {
                    fail("Conflict", "External object occupies the conditional restoration name");
                }
                original.path = snapshot.path;
                verify_original(true);
                sync_pinned_directory(parent);
            }
            stage.verify_name();
            conditional_discard(stage.handle, parent, stage.path, stage.identity);
            conditional_discard(receipt.handle, parent, receipt.path, receipt.identity);
            sync_pinned_directory(parent);
        } catch (const ErrorException& recovery) {
            conditional_recovery_required(receipt_path, snapshot.path, backup_path, recovery.error().code);
        } catch (...) {
            conditional_recovery_required(receipt_path, snapshot.path, backup_path, "UnclassifiedRestorationFailure");
        }
        std::rethrow_exception(failure);
    }
}
ConditionalReplaceResult replace_text_conditionally(const ConditionalFileSnapshot& expected,
    std::string_view text, std::uint64_t limit, const AtomicWriteOptions& options) {
    return replace_file_conditionally(expected, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, limit, options);
}
#ifndef NICKEL_CONDITIONAL_TEST_REPLACE_ONLY
ConditionalRecoveryReport recover_conditional_replacements(const std::filesystem::path& directory) {
    const auto absolute = checked_absolute_path(directory);
    DirectoryGuard parent(absolute, false, DirectoryGuardMode::NamespaceMutation);
    FileLock lock(parent.path() / ".nickel-config.lock", nullptr, std::chrono::seconds(30), DirectoryGuardMode::NamespaceMutation);
    return conditional_recover_unlocked(parent);
}
#endif
} // namespace nickel

#ifdef NICKEL_UNDEFINE_CONDITIONAL_HOOK
#undef NICKEL_CONDITIONAL_TEST_HOOK
#undef NICKEL_UNDEFINE_CONDITIONAL_HOOK
#endif
