#include "fixtures.hpp"
#include "core/common/error.hpp"
#include "core/diagnostics/junk.hpp"
#include "core/tasks/secure_fs.hpp"
#include "core/tasks/test_hooks.hpp"

#include <chrono>
#include <cstdarg>
#include <functional>
#include <initializer_list>
#include <stop_token>
#include <sys/syscall.h>
#include <thread>

namespace pa = posix_acceptance;
namespace fs = std::filesystem;
namespace secure = nickel::tasks::detail;
using nickel::diagnostics::JunkCleanResult;
using nickel::diagnostics::JunkGameRoot;
using nickel::diagnostics::JunkLimits;
using nickel::diagnostics::JunkLocations;
using nickel::diagnostics::JunkPreview;
using nickel::diagnostics::JunkService;
using pa::require;

namespace {
unsigned passed{}, failed{};
std::string_view case_filter;


JunkLocations locations(const fs::path& game) { return {{JunkGameRoot{game, {}}}, {}}; }
fs::path game(pa::Fixture& fixture, const fs::path& relative = "game") {
    const auto path = fixture.path(relative);
    fixture.write(path / "options.txt", "protected-options");
    return path;
}
void log_result(const JunkCleanResult& value) {
    std::cout << "RESULT removed=" << value.removedCount << " freed=" << value.freedBytes << " conflicts=" << value.conflictCount
              << " failed=" << value.failedCount << " missing=" << value.missingCount << " cancelled=" << value.cancelled
              << " deferred=" << value.cancelledCount << " location_conflict=" << value.locationConflict << '\n';
}
void successful(const JunkCleanResult& value, std::size_t count, std::uint64_t bytes) {
    log_result(value);
    require(value.removedCount == count && value.freedBytes == bytes && value.failedCount == 0 && value.conflictCount == 0 &&
            value.missingCount == 0 && !value.cancelled && !value.locationConflict, "authorized deletion/count mismatch");
}
void conflict(const JunkCleanResult& value, std::size_t count = 1) {
    log_result(value);
    require(value.removedCount == 0 && value.freedBytes == 0 && value.conflictCount == count && value.failedCount == 0,
            "changed object was not reported as a zero-delete conflict");
}
bool contains(const JunkPreview& preview, const fs::path& path) {
    return std::any_of(preview.items.begin(), preview.items.end(), [&](const auto& item) { return item.path == path; });
}
std::string rejects(const std::function<void()>& action, std::initializer_list<std::string_view> allowed) {
    try { action(); }
    catch (const nickel::ErrorException& error) {
        std::cout << "REJECTION code=" << error.error().code << " message=" << error.error().message << '\n';
        require(std::find(allowed.begin(), allowed.end(), error.error().code) != allowed.end(), "unexpected rejection code");
        return error.error().code;
    }
    throw std::runtime_error("operation unexpectedly accepted invalid authorization");
}
void log_identity(const secure::RegularFileToken& token) {
    std::cout << "TOKEN root=" << token.root.string() << " file=" << token.path.string() << " root_dev=" << token.root_identity.volume
              << " root_ino=" << token.root_identity.index << " parent_ino=" << token.parent_identity.index
              << " ino=" << token.file_identity.index << " size=" << token.size << " mtime=" << token.revision.modified
              << " ctime=" << token.revision.changed << " sha256=" << token.sha256 << '\n';
}
void restore_mtime(const fs::path& path, const struct stat& before) {
    const struct timespec times[2]{before.st_atim, before.st_mtim};
    pa::os_check(::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0, "restore original mtime");
    const auto after = pa::status(path);
    require(after.st_size == before.st_size && after.st_mtim.tv_sec == before.st_mtim.tv_sec &&
            after.st_mtim.tv_nsec == before.st_mtim.tv_nsec, "same-size restored-mtime fixture not established");
    std::cout << "RESTORED_MTIME ino=" << after.st_ino << " size=" << after.st_size << " mtime=" << after.st_mtim.tv_sec << ':'
              << after.st_mtim.tv_nsec << " ctime_before=" << before.st_ctim.tv_sec << ':' << before.st_ctim.tv_nsec
              << " ctime_after=" << after.st_ctim.tv_sec << ':' << after.st_ctim.tv_nsec << '\n';
}

#ifdef POSIX_ACCEPTANCE_OBSERVERS
struct ReadTarget { dev_t dev{}; ino_t ino{}; std::uint64_t bytes{}; unsigned calls{}; };
struct Observers {
    bool reading{};
    bool in_hook{};
    std::vector<ReadTarget> targets;
    std::function<void()> after_read;
    std::function<void(int, const char*, int, const char*, unsigned, bool, long)> rename;
    std::function<void(int, const char*, int)> unlink;
    std::string error;
};
thread_local Observers observers;
struct Observe {
    Observe() { observers = {}; }
    ~Observe() { observers = {}; }
    void watch(const fs::path& path) {
        const auto value = pa::status(path);
        observers.targets.push_back({value.st_dev, value.st_ino, 0, 0}); observers.reading = true;
    }
    void verify() const { require(observers.error.empty(), "syscall injection/observer failed: " + observers.error); }
    std::uint64_t bytes(std::size_t index) const { return observers.targets.at(index).bytes; }
    void stop_reading() { observers.reading = false; }
};
class Checkpoint {
public:
    explicit Checkpoint(std::function<void(std::string_view, const fs::path&)> action) {
        require(!secure::testing::checkpoint, "nested production checkpoint"); secure::testing::checkpoint = std::move(action);
    }
    ~Checkpoint() { secure::testing::checkpoint = {}; }
};
fs::path descriptor_path(int fd) {
    const std::string name = "/proc/self/fd/" + std::to_string(fd);
    char buffer[4096];
    const auto count = ::readlink(name.c_str(), buffer, sizeof(buffer));
    pa::os_check(count > 0 && count < static_cast<ssize_t>(sizeof(buffer)), "resolve controlled descriptor");
    return fs::path(std::string(buffer, static_cast<std::size_t>(count)));
}
#endif

template<class F>
void test(std::string_view name, F action) {
    if (!case_filter.empty() && case_filter != name) return;
    std::cout << "TEST_BEGIN " << name << '\n';
    const auto cleanup_before = pa::fixture_cleanup_failures;
    try {
        action();
        require(pa::fixture_cleanup_failures == cleanup_before, "owned fixture cleanup did not complete");
        ++passed; std::cout << "PASS " << name << '\n';
    } catch (const nickel::ErrorException& error) {
        ++failed; std::cout << "FAIL " << name << " code=" << error.error().code << " reason=" << error.what() << '\n';
    } catch (const std::exception& error) {
        ++failed; std::cout << "FAIL " << name << " reason=" << error.what() << '\n';
    }
}

void baseline_tests() {
    test("cleanup-normal-exact-scope-and-one-shot", [] {
        pa::Fixture fixture("normal"); const auto root = game(fixture);
        const auto log = root / "logs/old.log", crash = root / "crash-reports/old.txt", jvm = root / "hs_err_pid17.log";
        const auto current = root / "logs/nickel-launcher.log", save = root / "saves/world/level.dat";
        fixture.write(log, "junk-log"); fixture.write(crash, "crash"); fixture.write(jvm, "jvm");
        fixture.write(current, "CURRENT"); fixture.write(save, "SAVE");
        fixture.write(root / "logs/nested/not-recursive.log", "NESTED");
        fixture.write(root / "logs/.nickel-delete-decoy/leaf", "QUARANTINE-PROTECTED");
        JunkService service; const auto registration = service.registerLocations(locations(root));
        require(registration.acceptedRootCount == 1 && registration.issues.empty(), "normal root must not be Unsupported");
        const auto preview = service.preview();
        require(preview.fileCount == 3 && preview.totalBytes == 16 && contains(preview, log) && contains(preview, crash) && contains(preview, jvm),
                "preview changed exact legacy scope");
        const auto newcomer = root / "logs/new-after-preview.log"; fixture.write(newcomer, "NEW");
        successful(service.clean(preview.previewId), 3, 16);
        require(!pa::exists(log) && !pa::exists(crash) && !pa::exists(jvm), "authorized regular files remained");
        require(fixture.read(current) == "CURRENT" && fixture.read(save) == "SAVE" && fixture.read(newcomer) == "NEW" &&
                fixture.read(root / "logs/nested/not-recursive.log") == "NESTED" &&
                fixture.read(root / "logs/.nickel-delete-decoy/leaf") == "QUARANTINE-PROTECTED", "cleanup expanded authorization");
        rejects([&] { service.clean(preview.previewId); }, {"cleanup_invalid_preview"});
    });
    test("cleanup-empty-file-real-unlink", [] {
        pa::Fixture fixture("empty"); const auto root = game(fixture); const auto leaf = root / "logs/empty.log";
        fixture.write(leaf, ""); JunkService service; service.registerLocations(locations(root));
        const auto preview = service.preview(); require(preview.fileCount == 1 && preview.totalBytes == 0, "empty preview missing");
        successful(service.clean(preview.previewId), 1, 0); require(!pa::exists(leaf), "empty file not removed");
    });
    test("cleanup-unicode-deep-root-isolated-and-config", [] {
        pa::Fixture fixture("unicode-deep"); auto root = fixture.root();
        for (unsigned i = 0; i < 7; ++i) root /= "deep-" + std::to_string(i) + "-" + std::string(130, 'x');
        root /= fs::path(u8"\u6e38\u620f-\U0001f6e0");
        fixture.write(root / "options.txt", "protected");
        const std::u8string version_name = u8"\u5b9e\u4f8b-\u6d4b\u8bd5";
        const auto version = root / "versions" / fs::path(version_name);
        const auto config = fixture.path(fs::path(u8"\u914d\u7f6e"));
        const std::vector<fs::path> leaves{root / "logs" / fs::path(u8"\u65e5\u5fd7.log"), version / "logs" / fs::path(u8"\u5f52\u6863.log.gz"),
                                         version / "crash-reports" / fs::path(u8"\u5d29\u6e83.txt"), config / fs::path(u8".nickel-\u6d4b\u8bd5.part")};
        for (const auto& leaf : leaves) fixture.write(leaf, "junk");
        const auto protected_leaf = root / "versions/unregistered/logs/old.log"; fixture.write(protected_leaf, "KEEP");
        auto input = locations(root); input.configRoot = config;
        input.gameRoots.front().instances = {{std::string(version_name.begin(), version_name.end()), version}};
        JunkService service; const auto registration = service.registerLocations(input);
        std::cout << "DEEP_PATH bytes=" << root.string().size() << '\n';
        require(root.string().size() > 1000 && registration.acceptedRootCount == 1 && registration.effectiveDirectoryCount == 2 &&
                registration.issues.empty(), "Unicode/deep registration failed");
        const auto preview = service.preview(); require(preview.fileCount == 4 && preview.totalBytes == 16, "Unicode/deep preview failed");
        for (const auto& leaf : leaves) require(contains(preview, leaf), "logical UTF-8 DTO changed");
        successful(service.clean(preview.previewId), 4, 16);
        for (const auto& leaf : leaves) require(!pa::exists(leaf), "deep authorized leaf remained");
        require(fixture.read(protected_leaf) == "KEEP", "unregistered instance was cleaned");
    });
    test("token-normal-empty-and-reuse-rejected", [] {
        pa::Fixture fixture("tokens"); const auto root = game(fixture);
        for (const auto& pair : std::vector<std::pair<std::string, std::string>>{{"data.log", "real-token"}, {"empty.log", ""}}) {
            const auto leaf = root / "logs" / pair.first; fixture.write(leaf, pair.second);
            const auto token = secure::capture_regular_file(root, leaf); log_identity(token);
            require(token.sha256.size() == 64 && token.size == pair.second.size(), "token hash/size invalid");
            secure::remove_regular_file(token); require(!pa::exists(leaf), "real token removal failed");
            rejects([&] { secure::remove_regular_file(token); }, {"revision_conflict"});
        }
    });
    test("preview-invalid-discard-and-reregister-consume", [] {
        pa::Fixture fixture("oneshot"); const auto root = game(fixture); fixture.write(root / "logs/a.log", "KEEP");
        JunkService service; service.registerLocations(locations(root));
        rejects([&] { service.clean(""); }, {"cleanup_invalid_preview"});
        rejects([&] { service.clean(std::string(129, 'x')); }, {"cleanup_invalid_preview"});
        rejects([&] { service.clean("unknown"); }, {"cleanup_invalid_preview"});
        auto preview = service.preview(); service.discardPreview(preview.previewId);
        rejects([&] { service.clean(preview.previewId); }, {"cleanup_invalid_preview"});
        preview = service.preview(); service.registerLocations(locations(root));
        rejects([&] { service.clean(preview.previewId); }, {"cleanup_invalid_preview"});
        require(fixture.read(root / "logs/a.log") == "KEEP", "invalid preview touched leaf");
    });
    test("same-size-restored-mtime-cleanup-conflict", [] {
        pa::Fixture fixture("mtime"); const auto root = game(fixture); const auto leaf = root / "logs/a.log";
        fixture.write(leaf, "AAAA"); const auto before = pa::status(leaf);
        JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview();
        std::this_thread::sleep_for(std::chrono::milliseconds(2)); fixture.write(leaf, "BBBB"); restore_mtime(leaf, before);
        require(pa::same_inode(before, pa::status(leaf)), "mtime fixture must retain inode");
        conflict(service.clean(preview.previewId)); require(fixture.read(leaf) == "BBBB", "changed content removed");
        rejects([&] { service.clean(preview.previewId); }, {"cleanup_invalid_preview"});
    });
    test("token-content-hash-independent-of-size-mtime-revision", [] {
        pa::Fixture fixture("hash-recheck"); const auto root = game(fixture); const auto leaf = root / "logs/a.log";
        fixture.write(leaf, "AAAA"); auto token = secure::capture_regular_file(root, leaf); log_identity(token);
        const auto before = pa::status(leaf); fixture.write(leaf, "BBBB"); restore_mtime(leaf, before);
        auto current = secure::SecureFile::open_read(leaf); token.revision = current.revision(); current.close();
        std::cout << "HASH_ORACLE token revision deliberately refreshed; original content SHA retained\n";
        rejects([&] { secure::remove_regular_file(token); }, {"revision_conflict"});
        require(fixture.read(leaf) == "BBBB", "independent digest recheck accepted new content");
    });
    test("replacement-inode-identical-bytes-and-mtime-conflict", [] {
        pa::Fixture fixture("identity"); const auto root = game(fixture); const auto leaf = root / "logs/a.log";
        fixture.write(leaf, "SAME"); const auto before = pa::status(leaf);
        JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview();
        fs::rename(leaf, root / "original-retained"); fixture.write(leaf, "SAME", true); restore_mtime(leaf, before);
        require(!pa::same_inode(before, pa::status(leaf)), "replacement must change inode");
        conflict(service.clean(preview.previewId));
        require(fixture.read(leaf) == "SAME" && fixture.read(root / "original-retained") == "SAME", "identity replacement deleted another object");
    });
    test("leaf-symlink-before-preview-never-authorized", [] {
        pa::Fixture fixture("leaf-link-before"); const auto root = game(fixture); const auto sentinel = fixture.path("protected/a.log");
        fixture.write(sentinel, "SENTINEL"); fixture.directory(root / "logs");
        pa::os_check(::symlink(sentinel.c_str(), (root / "logs/a.log").c_str()) == 0, "create leaf symlink");
        JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview();
        require(preview.fileCount == 0 && preview.skippedCount >= 1, "symlink was authorized");
        successful(service.clean(preview.previewId), 0, 0); require(fixture.read(sentinel) == "SENTINEL", "symlink target touched");
    });
    test("leaf-symlink-after-preview-refuses-target", [] {
        pa::Fixture fixture("leaf-link-after"); const auto root = game(fixture); const auto leaf = root / "logs/a.log";
        const auto sentinel = fixture.path("protected/a.log"); fixture.write(leaf, "OLD"); fixture.write(sentinel, "SENTINEL");
        JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview();
        fs::rename(leaf, root / "retained"); pa::os_check(::symlink(sentinel.c_str(), leaf.c_str()) == 0, "swap leaf symlink");
        conflict(service.clean(preview.previewId)); require(fixture.read(sentinel) == "SENTINEL" && S_ISLNK(pa::status(leaf).st_mode), "link target or link removed");
    });
    test("parent-symlink-before-preview-refuses-target", [] {
        pa::Fixture fixture("parent-link-before"); const auto root = game(fixture); const auto sentinel = fixture.path("protected/a.log");
        fixture.write(sentinel, "SENTINEL"); pa::os_check(::symlink(sentinel.parent_path().c_str(), (root / "logs").c_str()) == 0, "create parent link");
        JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview();
        require(preview.fileCount == 0, "linked parent authorized target"); successful(service.clean(preview.previewId), 0, 0);
        require(fixture.read(sentinel) == "SENTINEL", "linked parent target removed");
    });
    test("parent-replacement-and-symlink-after-preview", [] {
        pa::Fixture fixture("parent-link-after"); const auto root = game(fixture); const auto leaf = root / "logs/a.log";
        const auto sentinel = fixture.path("protected/a.log"); fixture.write(leaf, "OLD"); fixture.write(sentinel, "SENTINEL");
        JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview();
        fs::rename(root / "logs", root / "logs-retained"); pa::os_check(::symlink(sentinel.parent_path().c_str(), (root / "logs").c_str()) == 0, "swap parent link");
        conflict(service.clean(preview.previewId));
        require(fixture.read(sentinel) == "SENTINEL" && fixture.read(root / "logs-retained/a.log") == "OLD", "replacement parent followed");
    });
    test("hardlink-before-and-after-preview-fail-closed", [] {
        pa::Fixture fixture("hardlink"); const auto root = game(fixture); const auto leaf = root / "logs/a.log";
        const auto sentinel = fixture.path("protected/a.log"); fixture.write(leaf, "SHARED"); fixture.directory(sentinel.parent_path());
        pa::os_check(::link(leaf.c_str(), sentinel.c_str()) == 0, "create hardlink");
        JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview();
        require(preview.fileCount == 0 && preview.skippedCount >= 1, "multiply linked inode was authorized");
        rejects([&] { secure::capture_regular_file(root, leaf); }, {"revision_conflict"});
        pa::os_check(::unlink(sentinel.c_str()) == 0, "remove exact test hardlink");
        const auto approved = service.preview(); require(approved.fileCount == 1, "single-link authorization missing");
        pa::os_check(::link(leaf.c_str(), sentinel.c_str()) == 0, "add hardlink after preview");
        conflict(service.clean(approved.previewId)); require(fixture.read(sentinel) == "SHARED" && fixture.read(leaf) == "SHARED", "hardlink sentinel damaged");
    });
    test("token-outside-root-and-forged-root-denied", [] {
        pa::Fixture fixture("outside-root"); const auto root = game(fixture); const auto leaf = root / "logs/a.log";
        const auto outside = fixture.path("protected/a.log"); fixture.write(leaf, "OLD"); fixture.write(outside, "SENTINEL");
        rejects([&] { secure::capture_regular_file(root, outside); }, {"outside_root"});
        auto token = secure::capture_regular_file(root, leaf); token.path = outside;
        rejects([&] { secure::remove_regular_file(token); }, {"outside_root"});
        token = secure::capture_regular_file(root, leaf); ++token.root_identity.index;
        rejects([&] { secure::remove_regular_file(token); }, {"revision_conflict"});
        require(fixture.read(outside) == "SENTINEL" && fixture.read(leaf) == "OLD", "forged root touched object");
    });
    test("isolated-root-exact-binding-pending-and-filesystem-root-protected", [] {
        pa::Fixture fixture("isolation"); const auto root = game(fixture); const auto version = root / "versions/isolated";
        const auto leaf = version / "logs/a.log"; fixture.write(leaf, "OLD");
        const auto outside = fixture.path("protected/a.log"); fixture.write(outside, "SENTINEL");
        auto input = locations(root); input.gameRoots.push_back({fs::path("/"), {}});
        input.gameRoots.front().instances = {{"isolated", version}, {"invalid", outside.parent_path()}};
        fixture.directory(root / "versions/invalid");
        JunkService service; const auto registration = service.registerLocations(input);
        require(registration.acceptedRootCount == 1 && registration.skippedRootCount == 1 && registration.effectiveDirectoryCount == 2 &&
                !registration.issues.empty(), "root or isolation protection failed");
        const auto preview = service.preview(); require(preview.fileCount == 1 && contains(preview, leaf), "exact isolated leaf missing");
        fixture.write(version / ".nickel-pack.pending", "OWNER-INSTALLER");
        conflict(service.clean(preview.previewId)); require(fixture.read(leaf) == "OLD" && fixture.read(outside) == "SENTINEL", "pending isolated root cleaned");
    });
    test("root-replacement-after-preview-is-conflict", [] {
        pa::Fixture fixture("root-replace"); const auto root = game(fixture); fixture.write(root / "logs/a.log", "OLD");
        JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview();
        fs::rename(root, fixture.path("retained-game")); game(fixture); fixture.write(root / "logs/a.log", "NEW");
        conflict(service.clean(preview.previewId)); require(fixture.read(root / "logs/a.log") == "NEW" &&
                fixture.read(fixture.path("retained-game/logs/a.log")) == "OLD", "replacement root cleaned");
    });
    test("cancel-preview-and-clean-consumes-one-shot", [] {
        pa::Fixture fixture("cancel-before"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, "OLD");
        JunkService service; service.registerLocations(locations(root)); std::stop_source stop; stop.request_stop();
        rejects([&] { service.preview(stop.get_token()); }, {"Cancelled"});
        const auto preview = service.preview(); const auto result = service.clean(preview.previewId, stop.get_token()); log_result(result);
        require(result.cancelled && result.cancelledCount == 1 && result.removedCount == 0 && fixture.read(leaf) == "OLD", "pre-cancel removed data");
        rejects([&] { service.clean(preview.previewId); }, {"cleanup_invalid_preview"});
    });
    test("token-cancel-during-real-capture-and-before-remove", [] {
        pa::Fixture fixture("token-cancel"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, "OLD");
        unsigned checks{};
        rejects([&] { secure::capture_regular_file(root, leaf, [&] { return ++checks >= 3; }); }, {"cancelled"});
        require(checks >= 3, "capture cancellation did not reach real hash checks");
        const auto token = secure::capture_regular_file(root, leaf);
        rejects([&] { secure::remove_regular_file(token, [] { return true; }); }, {"cancelled"});
        require(fixture.read(leaf) == "OLD", "cancelled token lost data");
    });
    test("cleanup-byte-and-count-bounds-throw-not-authorize", [] {
        pa::Fixture fixture("bounds-public"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, std::string(64, 'x'));
        JunkLimits limits; limits.maxFileBytes = 32; JunkService service(limits); service.registerLocations(locations(root));
        rejects([&] { service.preview(); }, {"LimitExceeded"});
        rejects([&] { secure::capture_regular_file(root, leaf, 32); }, {"file_limits"});
        require(fixture.read(leaf) == std::string(64, 'x'), "bounded preview mutated data");
    });
}

#ifdef POSIX_ACCEPTANCE_OBSERVERS
void observed_tests() {
    test("preview-max-file-bytes-before-any-read", [] {
        pa::Fixture fixture("bounds-file-read"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, std::string(256, 'x'));
        JunkLimits limits; limits.maxFileBytes = 128; JunkService service(limits); service.registerLocations(locations(root));
        Observe observed; observed.watch(leaf); rejects([&] { service.preview(); }, {"LimitExceeded"}); observed.verify();
        std::cout << "BOUNDS_READ bytes=" << observed.bytes(0) << '\n'; require(observed.bytes(0) == 0, "oversize preview hashed before bound");
    });
    test("token-max-bytes-before-any-read", [] {
        pa::Fixture fixture("token-bounds-read"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, std::string(256, 'x'));
        Observe observed; observed.watch(leaf); rejects([&] { secure::capture_regular_file(root, leaf, 128); }, {"file_limits"}); observed.verify();
        require(observed.bytes(0) == 0, "oversize token capture hashed before bound"); std::cout << "BOUNDS_READ bytes=0\n";
    });
    for (const auto mode : {0, 1, 2}) {
        test(mode == 0 ? "preview-total-quota-before-second-hash" : mode == 1 ? "preview-file-count-before-second-hash" : "preview-scan-count-before-second-hash", [mode] {
            pa::Fixture fixture("bounds-second"); const auto root = game(fixture); const auto a = root / "logs/a.log", b = root / "logs/b.log";
            fixture.write(a, "12345678"); fixture.write(b, "87654321"); JunkLimits limits;
            if (mode == 0) limits.maxTotalBytes = 8;
            if (mode == 1) limits.maxFiles = 1;
            if (mode == 2) limits.maxScannedEntries = 1;
            JunkService service(limits); service.registerLocations(locations(root)); Observe observed; observed.watch(a); observed.watch(b);
            rejects([&] { service.preview(); }, {"LimitExceeded"}); observed.verify();
            std::cout << "BOUNDS_READ a=" << observed.bytes(0) << " b=" << observed.bytes(1) << '\n';
            require(observed.bytes(0) + observed.bytes(1) == 8 && (observed.bytes(0) == 0 || observed.bytes(1) == 0), "second file hashed after exhausted bound");
        });
    }
    test("preview-cancel-in-real-read-publishes-no-authorization", [] {
        pa::Fixture fixture("preview-read-cancel"); const auto root = game(fixture); const auto leaf = root / "logs/a.log";
        fixture.write(leaf, std::string(128 * 1024, 'x')); JunkService service; service.registerLocations(locations(root));
        std::stop_source stop; unsigned hits{}; Observe observed; observed.watch(leaf);
        observers.after_read = [&] { ++hits; stop.request_stop(); };
        rejects([&] { service.preview(stop.get_token()); }, {"Cancelled"}); observed.verify();
        require(hits == 1 && observed.bytes(0) == 64 * 1024, "preview cancellation did not intercept the first real production read");
        observed.stop_reading(); require(fixture.read(leaf).size() == 128 * 1024, "cancelled preview lost data");
        std::cout << "HOOK_HIT preview.read=" << hits << '\n';
    });
    test("mid-clean-cancel-real-second-validated-partial-counts", [] {
        pa::Fixture fixture("partial-cancel"); const auto root = game(fixture); const auto a = root / "logs/a.log", b = root / "logs/b.log";
        fixture.write(a, "a"); fixture.write(b, "b"); JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview();
        std::stop_source stop; unsigned hits{};
        Checkpoint checkpoint([&](std::string_view point, const fs::path&) { if (point == "regular.validated" && ++hits == 2) stop.request_stop(); });
        const auto result = service.clean(preview.previewId, stop.get_token()); log_result(result);
        require(hits == 2 && result.cancelled && result.removedCount == 1 && result.freedBytes == 1 && result.cancelledCount == 1 &&
                pa::exists(a) != pa::exists(b), "partial cancellation counts/scope failed");
        rejects([&] { service.clean(preview.previewId); }, {"cleanup_invalid_preview"}); std::cout << "HOOK_HIT regular.validated=" << hits << '\n';
    });
    test("backend-location-change-at-validated-fail-closed", [] {
        pa::Fixture fixture("location-validated"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, "OLD");
        bool valid = true; JunkService service({}, [&](const JunkLocations&) { return valid; }); service.registerLocations(locations(root)); const auto preview = service.preview();
        unsigned hits{}; Checkpoint checkpoint([&](std::string_view point, const fs::path& path) {
            if (point == "regular.validated" && path == leaf) { ++hits; valid = false; }
        });
        const auto result = service.clean(preview.previewId); conflict(result); require(hits == 1 && result.locationConflict && fixture.read(leaf) == "OLD", "backend change ignored");
        std::cout << "HOOK_HIT regular.validated=" << hits << '\n';
    });
    test("parent-swap-at-validated-never-follows-new-parent", [] {
        pa::Fixture fixture("parent-validated"); const auto root = game(fixture); const auto leaf = root / "logs/a.log";
        const auto sentinel = fixture.path("protected/a.log"); fixture.write(leaf, "OLD"); fixture.write(sentinel, "SENTINEL");
        JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview(); unsigned hits{};
        Checkpoint checkpoint([&](std::string_view point, const fs::path& path) {
            if (point == "regular.validated" && path == leaf) {
                ++hits; fs::rename(root / "logs", root / "logs-retained");
                pa::os_check(::symlink(sentinel.parent_path().c_str(), (root / "logs").c_str()) == 0, "controlled parent symlink");
            }
        });
        conflict(service.clean(preview.previewId)); require(hits == 1 && fixture.read(sentinel) == "SENTINEL" && fixture.read(root / "logs-retained/a.log") == "OLD", "controlled parent swap escaped");
        std::cout << "HOOK_HIT regular.validated=" << hits << '\n';
    });
    test("last-pre-quarantine-syscall-leaf-swap-restores-without-delete", [] {
        pa::Fixture fixture("syscall-leaf-swap"); const auto root = game(fixture); const auto leaf = root / "logs/a.log", retained = root / "logs/original-retained";
        fixture.write(leaf, "OLD"); JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview();
        Observe observed; unsigned hits{};
        observers.rename = [&](int from, const char* name, int to, const char* destination, unsigned flags, bool after, long) {
            if (!after && flags == 1 && descriptor_path(from) == leaf.parent_path() && std::string_view(name) == leaf.filename().string() &&
                std::string_view(destination) == "leaf" && descriptor_path(to).filename().string().starts_with(".nickel-delete-")) {
                ++hits; require(hits == 1, "repeat swap hook");
                const auto directory = pa::status(descriptor_path(to)); require(directory.st_uid == ::geteuid() && (directory.st_mode & 077) == 0, "quarantine not private");
                fs::rename(leaf, retained); fixture.write(leaf, "NEW", true);
            }
        };
        conflict(service.clean(preview.previewId)); observed.verify();
        require(hits == 1 && fixture.read(leaf) == "NEW" && fixture.read(retained) == "OLD" && fixture.quarantines(leaf.parent_path()).empty(), "pre-quarantine substitute deleted or not restored");
        std::cout << "HOOK_HIT SYS_renameat2.before=" << hits << " flags=RENAME_NOREPLACE private_mode=0700\n";
    });
    test("new-original-path-after-isolation-is-not-deleted", [] {
        pa::Fixture fixture("new-path-after-isolate"); const auto root = game(fixture); const auto leaf = root / "logs/a.log";
        fixture.write(leaf, "OLD"); JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview(); Observe observed; unsigned hits{};
        observers.rename = [&](int from, const char* name, int to, const char* destination, unsigned flags, bool after, long result) {
            if (after && result == 0 && flags == 1 && descriptor_path(from) == leaf.parent_path() && std::string_view(name) == leaf.filename().string() &&
                std::string_view(destination) == "leaf" && descriptor_path(to).filename().string().starts_with(".nickel-delete-")) {
                ++hits; fixture.write(leaf, "NEW", true);
            }
        };
        successful(service.clean(preview.previewId), 1, 3); observed.verify();
        require(hits == 1 && fixture.read(leaf) == "NEW" && fixture.quarantines(leaf.parent_path()).empty(), "post-isolation new path deleted");
        std::cout << "HOOK_HIT SYS_renameat2.after=" << hits << '\n';
    });
    test("quarantine-cancel-no-replace-restores-original", [] {
        pa::Fixture fixture("quarantine-cancel"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, "OLD");
        const auto token = secure::capture_regular_file(root, leaf); std::stop_source stop; unsigned hits{};
        Checkpoint checkpoint([&](std::string_view point, const fs::path& path) {
            if (point == "regular.quarantined" && path == leaf) { ++hits; require(!pa::exists(leaf), "leaf was not really quarantined"); stop.request_stop(); }
        });
        rejects([&] { secure::remove_regular_file(token, [&] { return stop.stop_requested(); }); }, {"cancelled"});
        require(hits == 1 && fixture.read(leaf) == "OLD" && fixture.quarantines(leaf.parent_path()).empty(), "cancel did not restore exact original");
        std::cout << "HOOK_HIT regular.quarantined=" << hits << '\n';
    });
    test("quarantine-restore-conflict-never-overwrites-new-leaf", [] {
        pa::Fixture fixture("quarantine-no-overwrite"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, "OLD");
        const auto token = secure::capture_regular_file(root, leaf); unsigned hits{}; bool cancel{}; fs::path quarantine;
        Checkpoint checkpoint([&](std::string_view point, const fs::path& path) {
            if (point == "regular.quarantined" && path == leaf) {
                ++hits; const auto found = fixture.quarantines(leaf.parent_path()); require(found.size() == 1, "missing private quarantine"); quarantine = found.front();
                const auto value = pa::status(quarantine); require(value.st_uid == ::geteuid() && (value.st_mode & 077) == 0, "quarantine permissions not private");
                fixture.write(leaf, "NEW", true); cancel = true;
            }
        });
        rejects([&] { secure::remove_regular_file(token, [&] { return cancel; }); }, {"revision_conflict"});
        require(hits == 1 && fixture.read(leaf) == "NEW" && fixture.read(quarantine / "leaf") == "OLD" &&
                static_cast<std::uint64_t>(pa::status(quarantine / "leaf").st_ino) == token.file_identity.index, "no-replace recovery overwrote or lost original");
        std::cout << "HOOK_HIT regular.quarantined=" << hits << " RECOVERY_RETAINED " << quarantine.string() << "/leaf ino=" << token.file_identity.index << " new_leaf=NEW\n";
    });
    test("quarantine-entry-swap-before-final-recheck-fail-closed", [] {
        pa::Fixture fixture("quarantine-entry-swap"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, "OLD");
        const auto token = secure::capture_regular_file(root, leaf); unsigned hits{}; fs::path quarantine;
        Checkpoint checkpoint([&](std::string_view point, const fs::path& path) {
            if (point == "regular.quarantined" && path == leaf) {
                ++hits; quarantine = fixture.quarantines(leaf.parent_path()).at(0);
                fs::rename(quarantine / "leaf", quarantine / "retained-original"); fixture.write(quarantine / "leaf", "NEW", true);
            }
        });
        rejects([&] { secure::remove_regular_file(token); }, {"revision_conflict"});
        require(hits == 1 && fixture.read(leaf) == "NEW" && fixture.read(quarantine / "retained-original") == "OLD", "entry swap deleted substitute or original");
        std::cout << "HOOK_HIT regular.quarantined=" << hits << " retained_original=" << (quarantine / "retained-original").string() << '\n';
    });
    test("quarantine-same-inode-content-change-restored-mtime-conflict", [] {
        pa::Fixture fixture("quarantine-content"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, "OLD");
        const auto token = secure::capture_regular_file(root, leaf); unsigned hits{};
        Checkpoint checkpoint([&](std::string_view point, const fs::path& path) {
            if (point == "regular.quarantined" && path == leaf) {
                ++hits; const auto isolated = fixture.quarantines(leaf.parent_path()).at(0) / "leaf";
                const auto before = pa::status(isolated); fixture.write(isolated, "NEW"); restore_mtime(isolated, before);
                const auto after = pa::status(isolated);
                require(pa::same_inode(before, after), "quarantine content case must keep inode");
                std::cout << "HOOK_HIT regular.quarantined=" << hits << " SAME_INODE_MTIME_CTIME before=" << before.st_mtim.tv_sec << ':'
                          << before.st_mtim.tv_nsec << ',' << before.st_ctim.tv_sec << ':' << before.st_ctim.tv_nsec
                          << " after=" << after.st_mtim.tv_sec << ':' << after.st_mtim.tv_nsec << ',' << after.st_ctim.tv_sec << ':'
                          << after.st_ctim.tv_nsec << " same_ctime=" << (before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec)
                          << " old_content=OLD new_content=NEW size=3 ino=" << after.st_ino << '\n';
            }
        });
        bool rejected{};
        try { secure::remove_regular_file(token); }
        catch (const nickel::ErrorException& error) {
            std::cout << "REJECTION code=" << error.error().code << " message=" << error.error().message << '\n';
            require(error.error().code == "revision_conflict", "unexpected isolated-content rejection"); rejected = true;
        }
        std::cout << "CONTENT_RACE_RESULT accepted=" << !rejected << " original_path_exists=" << pa::exists(leaf)
                  << " quarantine_count=" << fixture.quarantines(leaf.parent_path()).size() << '\n';
        require(rejected, "same-inode isolated changed content accepted and deleted");
        require(hits == 1 && fixture.read(leaf) == "NEW" && fixture.quarantines(leaf.parent_path()).empty(), "isolated content change silently deleted");
    });
    test("cancel-at-first-real-removal-hash-preserves-leaf", [] {
        pa::Fixture fixture("removal-hash-cancel"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, "OLD");
        JunkService service; service.registerLocations(locations(root)); const auto preview = service.preview(); std::stop_source stop; unsigned hits{};
        Checkpoint checkpoint([&](std::string_view point, const fs::path& path) { if (point == "regular.hash" && path == leaf) { ++hits; stop.request_stop(); } });
        const auto result = service.clean(preview.previewId, stop.get_token()); log_result(result);
        require(hits == 1 && result.cancelled && result.cancelledCount == 1 && result.removedCount == 0 && fixture.read(leaf) == "OLD", "hash cancellation ignored");
        std::cout << "HOOK_HIT regular.hash=" << hits << '\n';
    });
}

void residual_boundary() {
    test("documented-same-uid-last-fstatat-to-unlinkat-boundary", [] {
        pa::Fixture fixture("residual-last-unlink"); const auto root = game(fixture); const auto leaf = root / "logs/a.log"; fixture.write(leaf, "OLD");
        const auto token = secure::capture_regular_file(root, leaf); Observe observed; unsigned hits{}; fs::path quarantine;
        observers.unlink = [&](int parent, const char* name, int flags) {
            const auto directory = descriptor_path(parent);
            if (flags == 0 && std::string_view(name) == "leaf" && directory.parent_path() == leaf.parent_path() &&
                directory.filename().string().starts_with(".nickel-delete-")) {
                ++hits; quarantine = directory; require(hits == 1, "duplicate residual boundary injection");
                fs::rename(quarantine / "leaf", quarantine / "attacker-retained-original"); fixture.write(quarantine / "leaf", "NEW", true);
            }
        };
        secure::remove_regular_file(token); observed.verify();
        require(hits == 1 && !pa::exists(quarantine / "leaf") && fixture.read(quarantine / "attacker-retained-original") == "OLD", "declared residual was not exercised");
        std::cout << "RESIDUAL_BOUNDARY_REPRODUCED hook_hit=" << hits << " same_uid=" << ::geteuid()
                  << " substitution_deleted=true authorized_original_retained=true quarantine=" << quarantine.string()
                  << " classification=KNOWN_LIMIT_NOT_SAFETY_PASS\n";
    });
}
#endif
} // namespace

#ifdef POSIX_ACCEPTANCE_OBSERVERS
extern "C" ssize_t __real_read(int, void*, std::size_t);
extern "C" long __real_syscall(long, ...);
extern "C" int __real_unlinkat(int, const char*, int);

extern "C" ssize_t __wrap_read(int fd, void* bytes, std::size_t count) {
    const auto result = __real_read(fd, bytes, count);
    if (result <= 0 || !observers.reading || observers.in_hook) return result;
    struct stat value{};
    if (::fstat(fd, &value)) return result;
    for (auto& target : observers.targets) {
        if (value.st_dev != target.dev || value.st_ino != target.ino) continue;
        target.bytes += static_cast<std::uint64_t>(result); ++target.calls;
        if (observers.after_read) {
            observers.in_hook = true;
            try { observers.after_read(); } catch (const std::exception& error) { observers.error = error.what(); }
            observers.in_hook = false;
        }
    }
    return result;
}
extern "C" long __wrap_syscall(long number, ...) {
    if (number != SYS_renameat2) { observers.error = "unexpected wrapped syscall"; errno = ENOSYS; return -1; }
    va_list arguments; va_start(arguments, number);
    const int from = va_arg(arguments, int); const auto* source = va_arg(arguments, const char*);
    const int to = va_arg(arguments, int); const auto* destination = va_arg(arguments, const char*);
    const unsigned flags = va_arg(arguments, unsigned); va_end(arguments);
    try {
        if (observers.rename && !observers.in_hook) {
            observers.in_hook = true; observers.rename(from, source, to, destination, flags, false, 0); observers.in_hook = false;
        }
        const auto result = __real_syscall(number, from, source, to, destination, flags); const auto saved_errno = errno;
        if (observers.rename && !observers.in_hook) {
            observers.in_hook = true; observers.rename(from, source, to, destination, flags, true, result); observers.in_hook = false;
        }
        errno = saved_errno; return result;
    } catch (const std::exception& error) { observers.in_hook = false; observers.error = error.what(); errno = EIO; return -1; }
}
extern "C" int __wrap_unlinkat(int parent, const char* name, int flags) {
    if (observers.unlink && !observers.in_hook) {
        observers.in_hook = true;
        try { observers.unlink(parent, name, flags); }
        catch (const std::exception& error) { observers.in_hook = false; observers.error = error.what(); errno = EIO; return -1; }
        observers.in_hook = false;
    }
    return __real_unlinkat(parent, name, flags);
}
#endif

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cout << "POSIX_ACCEPTANCE pid=" << ::getpid() << " uid=" << ::geteuid() << " real_linux=true fixture_fs=tmpfs fixture_limit=" << pa::fixture_limit
#ifdef POSIX_ACCEPTANCE_OBSERVERS
              << " mode=CONTROLLED_PRODUCTION_HOOKS_AND_LD_OBSERVERS\n";
#else
              << " mode=PLAIN_PRODUCTION_NO_TEST_HOOKS\n";
#endif
    if (argc == 2 && std::string_view(argv[1]) == "--exercise-residual-boundary") {
#ifdef POSIX_ACCEPTANCE_OBSERVERS
        residual_boundary();
        std::cout << "RESIDUAL_SUMMARY reproduced=" << passed << " failed=" << failed << " fixture_cleanup_failures=" << pa::fixture_cleanup_failures << " NOT_SECURITY_PASS\n";
        return failed == 0 && pa::fixture_cleanup_failures == 0 ? 0 : 1;
#else
        return 2;
#endif
    }
    if (argc == 3 && std::string_view(argv[1]) == "--case") case_filter = argv[2];
    else if (argc != 1) return 2;
    baseline_tests();
#ifdef POSIX_ACCEPTANCE_OBSERVERS
    observed_tests();
#endif
    std::cout << "ACCEPTANCE_SUMMARY passed=" << passed << " failed=" << failed << " fixture_cleanup_failures=" << pa::fixture_cleanup_failures << '\n';
    if (passed + failed == 0) return 2;
    return failed == 0 && pa::fixture_cleanup_failures == 0 ? 0 : 1;
}
