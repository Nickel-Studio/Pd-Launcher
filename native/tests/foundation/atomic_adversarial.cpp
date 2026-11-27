#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"
#include "core/config/repository.hpp"
#include "core/config/vault.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace nickel;
using namespace nickel::foundation_detail;
namespace fs = std::filesystem;
void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
std::string failure(const auto& callback) {
    try { callback(); } catch (const ErrorException& error) { return error.error().code; }
    throw std::runtime_error("Unsafe operation unexpectedly succeeded");
}
struct Root {
    fs::path path = fs::current_path() / path_from_utf8("foundation-adversarial-" + unique_token());
    Root() { fs::create_directories(path); }
    ~Root() { std::error_code error; fs::remove_all(path, error); }
};
void raw_write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(output), "Synthetic raw fixture write failed");
}
std::string raw_read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Synthetic raw fixture read failed");
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
void no_owned_leftovers(const fs::path& root) {
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto name = path_to_utf8(entry.path().filename());
        require(!name.starts_with(".nickel-stage-") && !name.starts_with(".nickel-backup-") &&
            !name.starts_with(".nickel-transaction-") && name.find(".tmp-") == std::string::npos,
            "Successful operation left stages, backups or journals");
    }
}

void test_pinning(const fs::path& root) {
    const auto parent = root / "pinned";
    fs::create_directories(parent / "child");
    raw_write(parent / "child/value", "untouched");
    {
        DirectoryGuard guard(parent / "child");
#ifdef _WIN32
        require(!MoveFileExW(parent.c_str(), (root / "moved").c_str(), 0), "Pinned ancestor could be renamed");
        const auto writer = CreateFileW(parent.c_str(), GENERIC_WRITE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        require(writer == INVALID_HANDLE_VALUE, "Pinned ancestor could acquire reparse mutation rights");
        guard.verify();
#else
        fs::rename(parent, root / "moved");
        require(failure([&] { guard.verify(); }) == "NotFound", "Ancestor identity change went unnoticed");
        fs::rename(root / "moved", parent);
#endif
    }
    fs::rename(parent, root / "moved");
    fs::rename(root / "moved", parent);
    require(raw_read(parent / "child/value") == "untouched", "Pinning test altered child data");
#ifdef _WIN32
    const auto hostile = CreateFileW(parent.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(hostile != INVALID_HANDLE_VALUE, "Synthetic writable ancestor handle unavailable");
    (void)failure([&] { DirectoryGuard guard(parent / "child"); });
    CloseHandle(hostile);
#endif
    std::cout << "PASS pinning: ancestor rename/reparse mutation refused, pre-existing mutator fails closed\n";
}

void test_links_and_cleanup(const fs::path& root) {
    const auto external = root / "synthetic-external.json";
    raw_write(external, "{\"external\":true}");
    const auto repository_root = root / "hardlinks";
    fs::create_directories(repository_root);
    const auto target = repository_root / "settings.json";
    fs::create_hard_link(external, target);
    require(failure([&] { (void)read_file_bounded(target, 64); }) == "UnsafePath", "Hard-linked read accepted");
    require(failure([&] { atomic_write_text(target, "replacement"); }) == "UnsafePath", "Hard-linked replacement accepted");
    require(failure([&] { (void)JsonRepository(repository_root).read("settings.json"); }) == "UnsafePath", "Repository disguised unsafe link as empty settings");
    require(failure([&] { (void)rename_no_replace(target, repository_root / "fresh.json"); }) == "UnsafePath", "Hard-linked reservation accepted");
    require(failure([&] { (void)remove_file_if_matches(target, sha256_text(raw_read(external))); }) == "UnsafePath", "Cleanup deleted hard-linked external data");
    require(raw_read(external) == "{\"external\":true}" && fs::exists(target), "Unsafe link refusal changed fixtures");
    fs::remove(target);
    fs::remove(repository_root / ".nickel-config.lock");
    fs::create_hard_link(external, repository_root / ".nickel-config.lock");
    require(failure([&] { FileLock lock(repository_root / ".nickel-config.lock"); }) == "UnsafePath", "Hard-linked lock accepted");
    fs::remove(repository_root / ".nickel-config.lock");
    const auto key_root = root / "hard-key";
    fs::create_directories(key_root);
    raw_write(root / "synthetic-external.key", std::string(32, 'k'));
    fs::create_hard_link(root / "synthetic-external.key", key_root / "credentials.key");
    require(failure([&] { (void)Vault(key_root).load_or_create_key(); }) == "UnsafePath", "Hard-linked credential key accepted");
    require(raw_read(root / "synthetic-external.key") == std::string(32, 'k'), "Unsafe key was changed");

    const auto owned = root / "owned.stage";
    raw_write(owned, "owned");
    const auto hash = sha256_text("owned");
    require(!remove_file_if_matches(owned, sha256_text("wrong")), "Cleanup ignored content identity");
    raw_write(owned, "external");
    require(!remove_file_if_matches(owned, hash) && raw_read(owned) == "external", "Cleanup removed replacement data");
    require(remove_file_if_matches(owned, sha256_text("external")) && !fs::exists(owned), "Verified handle cleanup failed");
    raw_write(owned, "owned");
    raw_write(root / "occupied.stage", "external");
    require(!move_file_if_matches(owned, root / "occupied.stage", hash), "Conditional move overwrote occupied destination");
    require(raw_read(owned) == "owned" && raw_read(root / "occupied.stage") == "external", "Move conflict consumed a fixture");
    require(!move_file_if_matches(owned, root / "new.stage", sha256_text("wrong")), "Conditional move ignored source hash");
    require(move_file_if_matches(owned, root / "new.stage", hash), "Conditional move failed");
    require(!fs::exists(owned) && raw_read(root / "new.stage") == "owned", "Conditional move lost original content");

    const auto cross_source = root / "cross-source" / "source.stage";
    const auto cross_destination = root / "cross-destination" / "destination.stage";
    raw_write(cross_source, "owned");
    fs::create_directories(cross_destination.parent_path());
#ifdef _WIN32
    require(failure([&] { (void)rename_no_replace(cross_source, cross_destination); }) == "Unsupported",
        "Cross-parent reservation weakened Windows directory pinning");
    require(failure([&] { (void)move_file_if_matches(cross_source, cross_destination, hash); }) == "Unsupported",
        "Cross-parent conditional move weakened Windows directory pinning");
    require(raw_read(cross_source) == "owned" && !fs::exists(cross_destination), "Unsupported move changed fixtures");
#else
    require(rename_no_replace(cross_source, cross_destination), "Cross-parent reservation failed");
    require(!fs::exists(cross_source) && raw_read(cross_destination) == "owned", "Cross-parent reservation lost content");
    raw_write(cross_source, "owned");
    require(move_file_if_matches(cross_source, cross_destination.parent_path() / "matched.stage", hash),
        "Cross-parent conditional move failed");
    require(!fs::exists(cross_source) && raw_read(cross_destination.parent_path() / "matched.stage") == "owned",
        "Cross-parent conditional move lost content");
#endif
    std::cout << "PASS links/cleanup: hard-linked keys/config/locks refused, replacement content preserved, no-replace conditional move, cross-parent contract\n";
}

struct Journal {
    fs::path root;
    std::string token = "0123456789abcdef0123456789abcdef";
    fs::path file() const { return root / (".nickel-transaction-" + token + ".json"); }
    fs::path stage(unsigned i) const { return root / (".nickel-stage-" + token + "-" + std::to_string(i)); }
    fs::path backup(unsigned i) const { return root / (".nickel-backup-" + token + "-" + std::to_string(i)); }
    Json value;
    explicit Journal(fs::path directory, bool two = false) : root(std::move(directory)) {
        value = Json{{"schemaVersion", 1}, {"transactionId", token}, {"entries", Json::array()}};
        append("settings.json", "{\"before\":true}", "{\"after\":true}", 0);
        if (two) append("active_account.json", "{\"id\":\"old\"}", "{\"id\":\"new\"}", 1);
        save();
    }
    void append(std::string_view name, std::string_view before, std::string_view after, unsigned i) {
        raw_write(root / path_from_utf8(name), before);
        raw_write(stage(i), after);
        value["entries"].push_back(Json{{"name", name}, {"stage", path_to_utf8(stage(i).filename())},
            {"originalHash", sha256_text(before)}, {"newHash", sha256_text(after)},
            {"originalSize", static_cast<std::uint64_t>(before.size())}, {"newSize", static_cast<std::uint64_t>(after.size())},
            {"limit", static_cast<std::uint64_t>(settings_max_bytes)}});
    }
    void save() { raw_write(file(), value.dump()); }
};

void test_recovery_boundaries(const fs::path& root) {
    Journal moved(root / "crash-after-backup", true);
    fs::rename(moved.root / "settings.json", moved.backup(0));
    const auto restored = JsonRepository(moved.root).recover();
    require(restored.restored == 2 && !restored.conflicts && !restored.rejected, "Crash after backup did not roll forward");
    require(JsonRepository(moved.root).read("settings.json")["after"] == true, "Backup recovery produced wrong data");
    no_owned_leftovers(moved.root);

    Journal committed(root / "crash-after-publish");
    fs::rename(committed.root / "settings.json", committed.backup(0));
    raw_write(committed.root / "settings.json", "{\"after\":true}");
    fs::remove(committed.stage(0));
    const auto cleaned = JsonRepository(committed.root).recover();
    require(cleaned.restored == 0 && !cleaned.conflicts && !cleaned.rejected, "Committed entry with absent stage did not finish cleanup");
    no_owned_leftovers(committed.root);

    Journal read_recovery(root / "read-recovers");
    fs::rename(read_recovery.root / "settings.json", read_recovery.backup(0));
    require(JsonRepository(read_recovery.root).read("settings.json")["after"] == true, "Read observed partial transaction rather than recovering");
    no_owned_leftovers(read_recovery.root);

    Journal external(root / "external-after-backup");
    fs::rename(external.root / "settings.json", external.backup(0));
    raw_write(external.root / "settings.json", "{\"external\":true}");
    const auto conflict = JsonRepository(external.root).recover();
    require(conflict.conflicts == 1 && !conflict.restored, "External replacement after backup was not a conflict");
    require(raw_read(external.root / "settings.json") == "{\"external\":true}" && fs::exists(external.backup(0)) &&
        fs::exists(external.stage(0)) && fs::exists(external.file()), "External conflict destroyed recovery evidence");
    require(failure([&] { (void)JsonRepository(external.root).read("settings.json"); }) == "Conflict", "Conflicted recovery allowed ordinary read");
    require(failure([&] { JsonRepository(external.root).write("settings.json", Json::object()); }) == "Conflict", "Conflicted recovery allowed ordinary write");

    Journal damaged(root / "damaged-second-stage", true);
    raw_write(damaged.stage(1), "damaged");
    const auto rejected = JsonRepository(damaged.root).recover();
    require(rejected.rejected && !rejected.restored && !fs::exists(damaged.backup(0)), "Preflight moved a valid first entry before rejecting second");
    require(raw_read(damaged.root / "settings.json") == "{\"before\":true}", "Rejected transaction altered first target");
    require(failure([&] { (void)JsonRepository(damaged.root).read("settings.json"); }) == "RecoveryRequired", "Damaged journal turned into settings fallback");

    Journal hard_stage(root / "hard-stage");
    fs::remove(hard_stage.stage(0));
    raw_write(root / "synthetic-stage-external", "{\"after\":true}");
    fs::create_hard_link(root / "synthetic-stage-external", hard_stage.stage(0));
    require(JsonRepository(hard_stage.root).recover().rejected != 0, "Hard-linked stage passed hash checks");
    require(fs::exists(hard_stage.stage(0)) && raw_read(root / "synthetic-stage-external") == "{\"after\":true}", "Unsafe stage cleanup touched external object");

    Journal bad_schema(root / "float-schema");
    bad_schema.value["schemaVersion"] = 1.0;
    bad_schema.save();
    require(JsonRepository(bad_schema.root).recover().rejected == 1, "Floating journal schema accepted");
    Journal tampered_backup(root / "tampered-backup");
    fs::rename(tampered_backup.root / "settings.json", tampered_backup.backup(0));
    raw_write(tampered_backup.backup(0), "{\"external\":true}");
    require(JsonRepository(tampered_backup.root).recover().conflicts == 1, "Tampered backup was trusted");
    require(!fs::exists(tampered_backup.root / "settings.json"), "Tampered backup recreated a target");

    const auto excess = root / "excess-journals";
    for (unsigned i = 0; i < 129; ++i) raw_write(excess / (".nickel-transaction-" + std::to_string(i) + ".json"), "{}");
    require(failure([&] { (void)JsonRepository(excess).recover(); }) == "LimitExceeded", "Unbounded recovery journal scan accepted");
    std::cout << "PASS recovery boundaries: backup/publish/cleanup crashes, read gate, external conflict, all-entry preflight, hostile stages/schema/backups, journal cap\n";
}

void test_paths_and_reads(const fs::path& root) {
    require(failure([&] { (void)checked_absolute_path({}); }) == "InvalidArgument", "Empty path accepted");
    require(failure([&] { atomic_write_text(root / "folder/../value", "unsafe"); }) == "UnsafePath", "Traversal normalized before rejection");
#ifdef _WIN32
    for (const auto* name : {"CON", "NUL.json", "COM1.json", "CLOCK$", "tail.", "tail ", "settings.json:stream"}) {
        require(failure([&] { atomic_write_text(root / name, "unsafe"); }) == "UnsafePath", "Reserved/ADS Windows path accepted");
    }
#endif
    const auto config = root / "read-contract";
    JsonRepository repository(config);
    raw_write(config / "settings.json", std::string("\xFF", 1));
    require(failure([&] { (void)repository.read("settings.json"); }) == "CorruptData", "Invalid UTF8 read error silently fell back");
    raw_write(config / "settings.json", std::string(settings_max_bytes + 1, 'x'));
    repository.update("settings.json", Json::object(), [](Json& value) { value["reset"] = true; });
    require(repository.read("settings.json")["reset"] == true, "Oversized settings snapshot could not be safely replaced");
    repository.transaction([&] {
        repository.write_text("accounts.json", "[]", accounts_max_bytes);
        require(failure([&] { (void)repository.read_text("accounts.json", 1); }) == "LimitExceeded", "Staged read ignored requested byte limit");
    });
    no_owned_leftovers(config);
#ifdef _WIN32
    const auto key_root = root / "locked-key";
    Vault vault(key_root);
    (void)vault.load_or_create_key();
    const auto before = raw_read(vault.key_path());
    const auto blocker = CreateFileW(vault.key_path().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(blocker != INVALID_HANDLE_VALUE, "Synthetic key lock unavailable");
    (void)failure([&] { (void)vault.load_or_create_key(); });
    CloseHandle(blocker);
    require(raw_read(vault.key_path()) == before, "Unreadable existing key was replaced");
    no_owned_leftovers(key_root);
#endif
    std::cout << "PASS read/path contracts: traversal/ADS/devices, UTF8 errors, oversized snapshots, staged limits, unreadable key preservation\n";
}
}

int main() {
    try {
        Root root;
        test_pinning(root.path);
        test_links_and_cleanup(root.path);
        test_recovery_boundaries(root.path);
        test_paths_and_reads(root.path);
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL " << error.error().code << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
