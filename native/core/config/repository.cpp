#include "core/config/repository.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"
#include "core/common/error.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace nickel {
namespace {
using namespace foundation_detail;
constexpr std::string_view lock_name = ".nickel-config.lock";
constexpr std::string_view journal_prefix = ".nickel-transaction-";
constexpr std::string_view stage_prefix = ".nickel-stage-";
constexpr std::size_t transaction_entries_limit = 64;

struct PendingWrite {
    std::string name;
    std::string text;
    std::optional<std::string> original_hash;
    std::uint64_t original_size = 0;
    std::uint64_t limit = 0;
};
struct Transaction {
    std::filesystem::path root;
    const Cancellation* cancellation = nullptr;
    std::map<std::string, PendingWrite> writes;
    std::map<std::string, std::optional<std::string>> observed;
};
thread_local Transaction* active_transaction = nullptr;

bool same_root(const std::filesystem::path& a, const std::filesystem::path& b) {
#ifdef _WIN32
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}
Transaction* transaction_for(const std::filesystem::path& root) {
    return active_transaction && same_root(active_transaction->root, root) ? active_transaction : nullptr;
}

bool valid_document(std::string_view name) {
    if (name.empty() || name.size() > 128 || name.starts_with('.') || !name.ends_with(".json") ||
        name.find("..") != std::string_view::npos) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.';
    });
}

std::uint64_t document_limit(std::string_view name, std::uint64_t requested) {
    const auto hard_limit = name == "accounts.json" ? accounts_max_bytes : settings_max_bytes;
    return std::min(requested, hard_limit);
}

bool shape_valid(std::string_view name, const Json& value) {
    if (name == "accounts.json") return value.is_array();
    if (name == "settings.json" || name == "instances.json" || name == "active_account.json") return value.is_object();
    return !value.is_discarded();
}

void validate_text(std::string_view name, std::string_view text, std::uint64_t limit) {
    if (!valid_document(name)) fail("InvalidArgument", "Configuration document name is invalid");
    if (text.size() > document_limit(name, limit)) fail("LimitExceeded", "Configuration document exceeds its size limit");
    if (!valid_utf8(text)) fail("CorruptData", "Configuration text is not valid UTF-8");
    const auto value = Json::parse(text, nullptr, false);
    if (value.is_discarded()) fail("CorruptData", "Configuration contains invalid JSON");
    if (!shape_valid(name, value)) fail("InvalidArgument", "Configuration JSON has an incompatible shape");
}

std::optional<std::string> actual_text(const std::filesystem::path& path) {
    reject_path_links(path);
    if (path_missing(path)) return std::nullopt;
    const auto bytes = read_file_bounded(path, accounts_max_bytes);
    return std::string(bytes.begin(), bytes.end());
}

std::optional<std::string> actual_hash(const std::filesystem::path& path) {
    if (const auto text = actual_text(path)) return sha256_text(*text);
    return std::nullopt;
}

bool hex_string(std::string_view text, std::size_t length) {
    return text.size() == length && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool exact_fields(const Json& value, const std::set<std::string>& names) {
    if (!value.is_object() || value.size() != names.size()) return false;
    return std::all_of(names.begin(), names.end(), [&value](const std::string& name) { return value.contains(name); });
}

struct JournalEntry {
    std::string name;
    std::string stage;
    std::string backup;
    std::optional<std::string> original_hash;
    std::string new_hash;
    std::uint64_t original_size = 0;
    std::uint64_t new_size = 0;
    std::uint64_t limit = 0;
    bool already_committed = false;
};

std::optional<std::vector<JournalEntry>> parse_journal(const Json& value, std::string_view token) {
    if (!exact_fields(value, {"schemaVersion", "transactionId", "entries"}) || !value["schemaVersion"].is_number_unsigned() || value["schemaVersion"] != 1 ||
        !value["transactionId"].is_string() || value["transactionId"].get<std::string>() != token || !hex_string(token, 32) ||
        !value["entries"].is_array() || value["entries"].empty() || value["entries"].size() > transaction_entries_limit) return std::nullopt;
    std::vector<JournalEntry> result;
    std::set<std::string> seen;
    std::size_t index = 0;
    for (const auto& entry : value["entries"]) {
        if (!exact_fields(entry, {"name", "stage", "originalHash", "newHash", "originalSize", "newSize", "limit"}) ||
            !entry["name"].is_string() || !entry["stage"].is_string() || !entry["newHash"].is_string() ||
            (!entry["originalHash"].is_null() && !entry["originalHash"].is_string()) ||
            !entry["originalSize"].is_number_unsigned() || !entry["newSize"].is_number_unsigned() || !entry["limit"].is_number_unsigned()) return std::nullopt;
        JournalEntry item;
        item.name = entry["name"].get<std::string>();
        item.stage = entry["stage"].get<std::string>();
        item.backup = ".nickel-backup-" + std::string(token) + "-" + std::to_string(index);
        item.new_hash = entry["newHash"].get<std::string>();
        item.original_size = entry["originalSize"].get<std::uint64_t>();
        item.new_size = entry["newSize"].get<std::uint64_t>();
        item.limit = entry["limit"].get<std::uint64_t>();
        if (!entry["originalHash"].is_null()) item.original_hash = entry["originalHash"].get<std::string>();
        auto identity = item.name;
#ifdef _WIN32
        for (auto& c : identity) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
        if (!valid_document(item.name) || !seen.insert(identity).second || !hex_string(item.new_hash, 64) ||
            (item.original_hash && !hex_string(*item.original_hash, 64)) || (!item.original_hash && item.original_size != 0) ||
            item.original_size > accounts_max_bytes || item.limit != document_limit(item.name, item.limit) || item.new_size > item.limit ||
            item.stage != std::string(stage_prefix) + std::string(token) + "-" + std::to_string(index)) return std::nullopt;
        result.push_back(std::move(item));
        ++index;
    }
    return result;
}

RecoveryReport recover_unlocked(const std::filesystem::path& root) {
    RecoveryReport report;
    if (path_missing(root)) return report;
    DirectoryGuard pinned(root);
    std::error_code error;
    std::vector<std::filesystem::path> journals;
    for (std::filesystem::directory_iterator iterator(native_filesystem_path(root), error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto name = path_to_utf8(iterator->path().filename());
        if (name.starts_with(journal_prefix) && name.ends_with(".json")) {
            if (journals.size() == 128) fail("LimitExceeded", "Too many pending configuration journals");
            journals.push_back(root / iterator->path().filename());
        }
    }
    if (error) io_fail("scan configuration recovery", root, error.value());
    std::sort(journals.begin(), journals.end());
    for (const auto& path : journals) {
        const auto name = path_to_utf8(path.filename());
        const auto token = std::string_view(name).substr(journal_prefix.size(), name.size() - journal_prefix.size() - 5);
        std::optional<std::string> journal_text;
        std::optional<std::vector<JournalEntry>> entries;
        try {
            journal_text = read_text_bounded(path, 64 * 1024);
            const auto value = Json::parse(*journal_text, nullptr, false);
            entries = parse_journal(value, token);
        } catch (...) { ++report.rejected; continue; }
        if (!entries) { ++report.rejected; continue; }
        bool safe = true;
        for (auto& item : *entries) {
            try {
                const auto target = root / path_from_utf8(item.name);
                const auto current = actual_text(target);
                const auto current_hash = current ? std::optional(sha256_text(*current)) : std::nullopt;
                const auto backup = actual_text(root / path_from_utf8(item.backup));
                const bool backup_valid = backup && item.original_hash &&
                    backup->size() == item.original_size && sha256_text(*backup) == *item.original_hash;
                if (backup && !backup_valid) { ++report.conflicts; safe = false; continue; }
                if (current_hash && *current_hash == item.new_hash && current->size() == item.new_size) {
                    item.already_committed = true;
                } else if (!(current_hash == item.original_hash && (!current || current->size() == item.original_size)) &&
                           !(!current && backup_valid)) {
                    ++report.conflicts; safe = false; continue;
                }
                const auto stage = root / path_from_utf8(item.stage);
                if (!path_missing(stage)) {
                    const auto text = read_text_bounded(stage, item.limit);
                    if (text.size() != item.new_size || sha256_text(text) != item.new_hash) {
                        ++report.rejected; safe = false; continue;
                    }
                    validate_text(item.name, text, item.limit);
                } else if (!item.already_committed) { ++report.rejected; safe = false; }
            } catch (...) { ++report.rejected; safe = false; }
        }
        if (!safe) continue;
        for (auto& item : *entries) {
            if (item.already_committed) continue;
            try {
                if (read_text_bounded(path, 64 * 1024) != *journal_text) { ++report.conflicts; safe = false; break; }
                const auto target = root / path_from_utf8(item.name);
                const auto backup = root / path_from_utf8(item.backup);
                const auto stage = root / path_from_utf8(item.stage);
                const auto text = read_text_bounded(stage, item.limit);
                if (sha256_text(text) != item.new_hash || text.size() != item.new_size) { ++report.rejected; safe = false; break; }
                if (item.original_hash && path_missing(backup)) {
                    if (!move_file_if_matches(target, backup, *item.original_hash)) { ++report.conflicts; safe = false; break; }
                }
                if (item.original_hash && actual_hash(backup) != item.original_hash) { ++report.conflicts; safe = false; break; }
                if (!path_missing(target) || !atomic_create_file(target,
                    {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, true)) {
                    ++report.conflicts; safe = false; break;
                }
                item.already_committed = true;
                ++report.restored;
            } catch (const ErrorException& error) {
                if (error.error().code == "Conflict" || error.error().code == "AlreadyExists") ++report.conflicts;
                else ++report.rejected;
                safe = false; break;
            }
        }
        if (!safe) continue;
        for (const auto& item : *entries) {
            try {
                const auto current = actual_text(root / path_from_utf8(item.name));
                if (!current || current->size() != item.new_size || sha256_text(*current) != item.new_hash) {
                    ++report.conflicts; safe = false; break;
                }
            } catch (...) { ++report.rejected; safe = false; break; }
        }
        if (!safe) continue;
        for (const auto& item : *entries) {
            try {
                if (!remove_file_if_matches(root / path_from_utf8(item.stage), item.new_hash) ||
                    (item.original_hash && !remove_file_if_matches(root / path_from_utf8(item.backup), *item.original_hash))) {
                    ++report.conflicts; safe = false; break;
                }
            } catch (...) { ++report.rejected; safe = false; break; }
        }
        if (!safe) continue;
        if (!remove_file_if_matches(path, sha256_text(*journal_text))) { ++report.conflicts; continue; }
        sync_directory(root);
    }
    pinned.verify();
    return report;
}

void commit_transaction(Transaction& transaction) {
    if (transaction.writes.empty()) return;
    check_cancel(transaction.cancellation);
    if (transaction.writes.size() > transaction_entries_limit) fail("LimitExceeded", "Too many configuration documents in a transaction");
    const auto token = unique_token();
    Json journal{{"schemaVersion", 1}, {"transactionId", token}, {"entries", Json::array()}};
    std::vector<std::pair<std::filesystem::path, std::string>> stages;
    bool committed = false;
    struct Cleanup {
        std::vector<std::pair<std::filesystem::path, std::string>>& stages;
        bool& committed;
        ~Cleanup() {
            if (!committed) for (const auto& [path, hash] : stages) {
                try { (void)remove_file_if_matches(path, hash); } catch (...) {}
            }
        }
    } cleanup{stages, committed};
    std::size_t index = 0;
    for (const auto& [name, item] : transaction.writes) {
        check_cancel(transaction.cancellation);
        if (actual_hash(transaction.root / path_from_utf8(name)) != item.original_hash) {
            fail("Conflict", "Configuration was changed externally; reload before saving");
        }
        const auto stage = std::string(stage_prefix) + token + "-" + std::to_string(index++);
        const auto stage_path = transaction.root / path_from_utf8(stage);
        const auto hash = sha256_text(item.text);
        create_file_exclusive(stage_path, {reinterpret_cast<const std::uint8_t*>(item.text.data()), item.text.size()}, true);
        stages.emplace_back(stage_path, hash);
        journal["entries"].push_back(Json{{"name", name}, {"stage", stage},
            {"originalHash", item.original_hash ? Json(*item.original_hash) : Json(nullptr)},
            {"newHash", hash}, {"originalSize", item.original_size},
            {"newSize", static_cast<std::uint64_t>(item.text.size())}, {"limit", item.limit}});
    }
    sync_directory(transaction.root);
    const auto journal_path = transaction.root / path_from_utf8(std::string(journal_prefix) + token + ".json");
    atomic_write_text(journal_path, journal.dump(), {.private_file = true, .create_parents = false,
                                                   .cancellation = transaction.cancellation});
    // Durable journal is the commit boundary. Cancellation cannot split a
    // multi-document save; crash recovery rolls forward only verified originals.
    committed = true;
    const auto report = recover_unlocked(transaction.root);
    if (report.conflicts) fail("Conflict", "Configuration changed during commit; recovery preserved both versions");
    if (report.rejected) fail("CorruptData", "Configuration transaction could not be recovered safely");
}

} // namespace

JsonRepository::JsonRepository(std::filesystem::path root) : root_(checked_absolute_path(root)) {
    reject_path_links(root_);
}
const std::filesystem::path& JsonRepository::root() const noexcept { return root_; }
std::filesystem::path JsonRepository::document_path(std::string_view name) const {
    if (!valid_document(name)) fail("InvalidArgument", "Configuration document name is invalid");
    return root_ / path_from_utf8(name);
}

std::optional<std::string> JsonRepository::read_text(std::string_view name, std::uint64_t limit) const {
    const auto path = document_path(name);
    const auto bounded = document_limit(name, limit);
    auto transaction = transaction_for(root_);
    if (!transaction) {
        reject_path_links(root_);
        if (path_missing(root_)) return std::nullopt;
        FileLock lock(root_ / path_from_utf8(lock_name));
        const auto recovery = recover_unlocked(root_);
        if (recovery.conflicts) fail("Conflict", "A pending configuration transaction conflicts with external changes");
        if (recovery.rejected) fail("RecoveryRequired", "A pending configuration transaction is damaged; original data was preserved");
        if (path_missing(path)) return std::nullopt;
        return read_text_bounded(path, bounded);
    }
    const auto item = transaction->writes.find(std::string(name));
    if (item != transaction->writes.end()) {
        if (item->second.text.size() > bounded) fail("LimitExceeded", "Configuration document exceeds its size limit");
        return item->second.text;
    }
    const auto observed = transaction->observed.find(std::string(name));
    if (observed != transaction->observed.end()) {
        if (observed->second && observed->second->size() > bounded) fail("LimitExceeded", "Configuration document exceeds its size limit");
        if (observed->second && !valid_utf8(*observed->second)) fail("CorruptData", "Configuration text is not valid UTF-8");
        return observed->second;
    }
    auto text = actual_text(path);
    transaction->observed.emplace(std::string(name), text);
    if (text && text->size() > bounded) fail("LimitExceeded", "Configuration document exceeds its size limit");
    if (text && !valid_utf8(*text)) fail("CorruptData", "Configuration text is not valid UTF-8");
    return text;
}

JsonReadResult JsonRepository::inspect(std::string_view name, const Json& fallback, std::uint64_t limit) const {
    JsonReadResult result;
    result.value = fallback;
    try { result.text = read_text(name, limit); }
    catch (const ErrorException& error) {
        if (error.error().code == "LimitExceeded") { result.state = JsonReadResult::State::Oversized; return result; }
        throw;
    }
    if (!result.text) return result;
    auto value = Json::parse(*result.text, nullptr, false);
    if (value.is_discarded()) { result.state = JsonReadResult::State::Corrupt; return result; }
    if (!shape_valid(name, value)) { result.state = JsonReadResult::State::WrongShape; return result; }
    result.state = JsonReadResult::State::Valid;
    result.value = std::move(value);
    return result;
}
Json JsonRepository::read(std::string_view name, const Json& fallback, std::uint64_t limit) const {
    return inspect(name, fallback, limit).value;
}

void JsonRepository::write_text(std::string_view name, std::string_view text, std::uint64_t limit) {
    validate_text(name, text, limit);
    if (auto transaction = transaction_for(root_)) {
        auto item = transaction->writes.find(std::string(name));
        if (item == transaction->writes.end()) {
            const auto observed = transaction->observed.find(std::string(name));
            const auto original = observed != transaction->observed.end() ? observed->second : actual_text(document_path(name));
            PendingWrite pending;
            pending.name = name;
            pending.text = text;
            pending.limit = document_limit(name, limit);
            if (original) { pending.original_hash = sha256_text(*original); pending.original_size = original->size(); }
            const auto key = pending.name;
            transaction->writes.emplace(key, std::move(pending));
        } else { item->second.text = text; item->second.limit = document_limit(name, limit); }
        return;
    }
    transaction([&] { write_text(name, text, limit); });
}
void JsonRepository::write(std::string_view name, const Json& value, std::uint64_t limit) {
    if (!shape_valid(name, value)) fail("InvalidArgument", "Configuration JSON has an incompatible shape");
    std::string text;
    try { text = value.dump(2); } catch (...) { fail("CorruptData", "Configuration cannot be serialized"); }
    write_text(name, text, limit);
}
Json JsonRepository::update(std::string_view name, const Json& fallback,
                             const std::function<void(Json&)>& mutator, std::uint64_t limit,
                             const Cancellation* cancellation) {
    Json value;
    transaction([&] { value = read(name, fallback, limit); mutator(value); write(name, value, limit); }, cancellation);
    return value;
}
void JsonRepository::transaction(const std::function<void()>& callback, const Cancellation* cancellation) {
    if (active_transaction) {
        if (!same_root(active_transaction->root, root_)) fail("InvalidArgument", "Nested transactions must use the same configuration root");
        check_cancel(cancellation);
        callback();
        return;
    }
    check_cancel(cancellation);
    FileLock lock(root_ / path_from_utf8(lock_name), cancellation);
    const auto recovery = recover_unlocked(root_);
    if (recovery.conflicts) fail("Conflict", "A pending configuration transaction conflicts with external changes");
    if (recovery.rejected) fail("CorruptData", "A pending configuration transaction is damaged; original data was preserved");
    Transaction state{root_, cancellation, {}, {}};
    struct Reset {
        ~Reset() { active_transaction = nullptr; }
    } reset;
    active_transaction = &state;
    callback();
    check_cancel(cancellation);
    commit_transaction(state);
}
RecoveryReport JsonRepository::recover() {
    if (active_transaction) fail("Busy", "Recovery cannot run inside a configuration transaction");
    if (path_missing(root_)) return {};
    FileLock lock(root_ / path_from_utf8(lock_name));
    return recover_unlocked(root_);
}

} // namespace nickel
