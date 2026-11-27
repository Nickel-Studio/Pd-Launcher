#include "core/diagnostics/junk.hpp"
#include "core/common/error.hpp"
#include "core/config/path_guard.hpp"
#include "core/tasks/test_hooks.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace nickel::diagnostics;
namespace safety = nickel::foundation_detail;

fs::path fixtureBase;
std::uint64_t fixtureNumber{};
std::size_t passed{};
std::size_t failed{};
std::size_t skipped{};

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class F>
void rejects(F&& action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "expected a rejected operation");
}

fs::path nativePath(const fs::path& path) {
    return safety::native_filesystem_path(path);
}

std::string readText(const fs::path& path) {
    require(fs::file_size(nativePath(path)) <= 64U * 1024U, "fixture text exceeds its read budget");
    std::ifstream input(nativePath(path), std::ios::binary);
    require(static_cast<bool>(input), "fixture text open failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

#ifndef NICKEL_TRANSPORT_TEST_HOOKS
bool completedLeafMissing(const fs::path& first, const fs::path& second) {
    if (fs::exists(nativePath(first)) && fs::exists(nativePath(second))) return false;
    for (const auto& entry : fs::directory_iterator(nativePath(first.parent_path()))) {
        if (entry.path().filename().string().starts_with(".nickel-delete-") && entry.is_directory()) return false;
    }
    return true;
}
#endif

#ifdef NICKEL_TRANSPORT_TEST_HOOKS
class TransportCheckpoint final {
public:
    explicit TransportCheckpoint(std::function<void(std::string_view, const fs::path&)> action) {
        require(!nickel::tasks::detail::testing::checkpoint, "nested transport test checkpoints");
        nickel::tasks::detail::testing::checkpoint = std::move(action);
    }
    ~TransportCheckpoint() { nickel::tasks::detail::testing::checkpoint = {}; }
    TransportCheckpoint(const TransportCheckpoint&) = delete;
    TransportCheckpoint& operator=(const TransportCheckpoint&) = delete;
};
#endif

bool canWrite(const fs::path& parent, std::uint64_t bytes) {
    std::error_code error;
    const auto space = fs::space(nativePath(parent), error);
    constexpr auto reserve = 256ULL * 1024ULL * 1024ULL;
    return !error && space.available > reserve && bytes <= space.available - reserve;
}

void write(const fs::path& path, const std::string& text) {
    auto parent = path.parent_path();
    while (!fs::exists(nativePath(parent)) && parent.has_parent_path()) parent = parent.parent_path();
    require(canWrite(parent, text.size()), "beforewrite: insufficient owned fixture volume capacity");
    fs::create_directories(nativePath(path.parent_path()));
    std::ofstream output(nativePath(path), std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "fixture file open failed");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    require(static_cast<bool>(output), "fixture file write failed");
}

struct Fixture {
    fs::path root;
    std::string owner;
    std::vector<fs::path> links;

    explicit Fixture(const char* tag) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        owner = "cleanup-fixture-" + std::to_string(tick) + "-" + std::to_string(++fixtureNumber);
        root = fixtureBase / (owner + "-" + tag);
        require(!fs::exists(nativePath(root)), "fixture collision; existing material is not owned");
        require(canWrite(fixtureBase, 4096), "beforewrite: insufficient fixture creation capacity");
        fs::create_directory(nativePath(root));
        write(root / ".cleanup-test-owner", owner);
    }

    ~Fixture() {
        try {
            std::ifstream marker(nativePath(root / ".cleanup-test-owner"), std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(marker)), {});
            marker.close();
            if (content != owner || root.parent_path() != fixtureBase) return;
            for (const auto& link : links) {
                std::error_code error;
                fs::remove(nativePath(link), error);
            }
            std::error_code error;
            fs::remove_all(nativePath(root), error);
            if (error) std::cerr << "FIXTURE_RETAINED " << root.generic_string() << '\n';
        } catch (...) {
            std::cerr << "FIXTURE_RETAINED (destructor exception)\n";
        }
    }

    fs::path game(const char* name = "game") {
        auto result = root / name;
        fs::create_directories(nativePath(result / "versions"));
        return result;
    }

    bool link(const fs::path& target, const fs::path& path, bool directory) {
        std::error_code error;
        if (directory) fs::create_directory_symlink(nativePath(target), nativePath(path), error);
        else fs::create_symlink(nativePath(target), nativePath(path), error);
        if (error) {
            ++skipped;
            std::cout << "SKIP symlink unavailable: " << error.message() << '\n';
            return false;
        }
        links.push_back(path);
        return true;
    }
};

JunkLocations locations(const fs::path& game, std::optional<fs::path> config = {}) {
    JunkLocations result;
    result.gameRoots.push_back({game, {}});
    result.configRoot = std::move(config);
    return result;
}

bool contains(const JunkPreview& preview, const fs::path& path) {
    return std::any_of(preview.items.begin(), preview.items.end(), [&](const auto& item) {
        return item.path == path;
    });
}

void run(const char* name, const std::function<void()>& test) {
    try {
        const auto skippedBefore = skipped;
        test();
        if (skipped != skippedBefore) {
            std::cout << "SKIP_CASE " << name << '\n';
            return;
        }
        ++passed;
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
        ++failed;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    } catch (...) {
        ++failed;
        std::cerr << "FAIL " << name << ": non-standard exception\n";
    }
}

void scopeTests() {
    run("registration_required", [] {
        JunkService service;
        rejects([&] { service.preview(); });
    });
    run("empty_preview_is_one_shot", [] {
        JunkService service;
        service.registerLocations({});
        const auto preview = service.preview();
        require(!preview.previewId.empty() && preview.fileCount == 0, "empty authorization missing");
        require(service.clean(preview.previewId).removedCount == 0, "empty clean removed data");
        rejects([&] { service.clean(preview.previewId); });
    });
    run("only_known_game_markers", [] {
        Fixture fixture("markers");
        const auto project = fixture.root / "project";
        const auto options = fixture.root / "options";
        const auto world = fixture.root / "world";
        const auto versioned = fixture.game();
        write(project / "logs/latest.log", "project");
        write(options / "options.txt", "options");
        write(options / "logs/latest.log", "options-log");
        write(world / "level.dat", "world");
        write(world / "logs/latest.log", "world-log");
        write(versioned / "logs/latest.log", "version-log");
        JunkLocations input;
        for (const auto& path : {project, options, world, versioned, fixture.root / "missing"}) {
            input.gameRoots.push_back({path, {}});
        }
        JunkService service;
        const auto registered = service.registerLocations(input);
        require(registered.acceptedRootCount == 3, "game-like root filtering changed");
        require(registered.skippedRootCount == 2, "invalid root count incorrect");
        const auto preview = service.preview();
        require(preview.fileCount == 3 && !contains(preview, project / "logs/latest.log"), "project log authorized");
        require(service.clean(preview.previewId).removedCount == 3, "known roots not cleaned");
        require(fs::exists(project / "logs/latest.log"), "project log deleted");
    });
    run("legacy_exact_scope_and_current_log", [] {
        Fixture fixture("scope");
        const auto game = fixture.game();
        const auto config = fixture.root / "config";
        const std::vector<std::pair<fs::path, std::string>> junk{
            {game / "logs/latest.log", "latest"},
            {game / "logs/old.log.gz", "archive"},
            {game / "logs/arbitrary-regular-file", "still-log-scope"},
            {game / "crash-reports/crash.txt", "crash"},
            {game / "hs_err_pid123.log", "jvm"},
            {game / "replay_pid9.log", "replay"},
            {game / "authlib-injector.log", "authlib"},
            {config / "logs/nickel-launcher.log.1", "old-launcher"},
            {config / ".nickel-export.part", "part"},
            {config / "instances.json.tmp-token", "temp"}
        };
        const std::vector<fs::path> preserved{
            game / "logs/nickel-launcher.log", game / "logs/nested/keep.log",
            game / "crash-reports/nested/keep.txt", game / "hs_err_pid123.txt",
            game / "xhs_err_pid123.log", game / "replay_pid123.LOG", game / "latest.log",
            game / "options.txt", game / "mods/keep.jar", game / "saves/keep.dat",
            game / "versions/unregistered/logs/keep.log", config / "logs/nickel-launcher.log",
            config / "settings.json", config / "accounts.json", config / "credentials.key",
            config / "nickel.settings.json", config / ".nickel-x.tmp", config / "nested/.nickel-x.part"
        };
        std::uint64_t bytes{};
        for (const auto& [path, text] : junk) { write(path, text); bytes += text.size(); }
        for (const auto& path : preserved) write(path, "KEEP");
        JunkService service;
        service.registerLocations(locations(game, config));
        const auto preview = service.preview();
        require(preview.fileCount == junk.size() && preview.totalBytes == bytes, "scope count/bytes differ from oracle");
        for (const auto& [path, text] : junk) { (void)text; require(contains(preview, path), "oracle item missing"); }
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == junk.size() && result.freedBytes == bytes && result.failedCount == 0, "exact scope clean failed");
        for (const auto& path : preserved) require(fs::exists(path), "non-junk data deleted");
    });
    run("root_patterns_preserve_legacy_empty_suffix_rules", [] {
        Fixture fixture("patterns");
        const auto game = fixture.game();
        const auto config = fixture.root / "config";
        for (const auto& path : {game / "hs_err_pid.log", game / "replay_pid.log", config / ".nickel-.part", config / "instances.json.tmp-"}) write(path, "x");
        JunkService service;
        service.registerLocations(locations(game, config));
        require(service.preview().fileCount == 4, "literal legacy patterns changed");
    });
    run("only_explicit_effective_isolation", [] {
        Fixture fixture("isolation");
        const auto game = fixture.game();
        const auto isolated = game / "versions/Named Instance";
        const auto shared = game / "versions/shared";
        const auto unregistered = game / "versions/unregistered";
        write(isolated / "logs/latest.log", "isolated");
        write(shared / "logs/latest.log", "not-effective");
        write(unregistered / "logs/latest.log", "not-registered");
        write(game / "logs/latest.log", "shared-root");
        auto input = locations(game);
        input.gameRoots.front().instances = {{"Named Instance", isolated}, {"shared", game}};
        JunkService service;
        const auto registration = service.registerLocations(input);
        require(registration.effectiveDirectoryCount == 2, "effective directories not deduplicated");
        const auto preview = service.preview();
        require(preview.fileCount == 2 && contains(preview, isolated / "logs/latest.log"), "effective isolated log missing");
        service.clean(preview.previewId);
        require(fs::exists(shared / "logs/latest.log") && fs::exists(unregistered / "logs/latest.log"), "cleanup guessed recursive version paths");
    });
    run("pending_instances_never_authorized", [] {
        Fixture fixture("pending");
        const auto game = fixture.game();
        const auto pending = game / "versions/pending";
        write(pending / ".nickel-pack.pending", "owned-by-installer");
        write(pending / "logs/latest.log", "pending-log");
        auto input = locations(game);
        input.gameRoots.front().instances = {{"pending", pending}};
        JunkService service;
        service.registerLocations(input);
        require(service.preview().fileCount == 0, "pending instance cleanup authorized");
    });
    run("outside_mismatched_nested_and_traversal_bindings", [] {
        Fixture fixture("bad-bindings");
        const auto game = fixture.game();
        const auto outside = fixture.root / "outside";
        write(outside / "logs/latest.log", "outside");
        write(game / "versions/good/logs/latest.log", "good");
        write(game / "versions/good/nested/logs/latest.log", "nested");
        auto input = locations(game);
        input.gameRoots.front().instances = {
            {"outside", outside}, {"wrong", game / "versions/good"},
            {"good", game / "versions/good/nested"}, {"../outside", outside},
            {"good", game / "versions/good"}
        };
        JunkService service;
        service.registerLocations(input);
        const auto preview = service.preview();
        require(preview.fileCount == 1 && contains(preview, game / "versions/good/logs/latest.log"), "invalid isolation binding authorized");
        service.clean(preview.previewId);
        require(fs::exists(outside / "logs/latest.log"), "outside log removed");
    });
    run("duplicates_and_config_overlap_count_once", [] {
        Fixture fixture("dedup");
        const auto game = fixture.game();
        write(game / "logs/latest.log", "once");
        auto input = locations(game, game);
        input.gameRoots.push_back({game, {}});
        input.gameRoots.push_back({game / ".", {}});
        JunkService service;
        service.registerLocations(input);
        const auto preview = service.preview();
        require(preview.fileCount == 1 && preview.totalBytes == 4, "duplicate authorization");
        require(service.clean(preview.previewId).removedCount == 1, "duplicate deletion");
    });
    run("relative_root_is_not_registered", [] {
        JunkService service;
        const auto result = service.registerLocations(locations(fs::path("relative/game")));
        require(result.acceptedRootCount == 0 && result.skippedRootCount == 1, "relative root accepted");
        require(service.preview().fileCount == 0, "relative root produced candidates");
    });
}

void authorizationTests() {
    run("public_copy_does_not_change_authorization", [] {
        Fixture fixture("public-copy");
        const auto game = fixture.game();
        const auto junk = game / "logs/latest.log";
        const auto save = game / "saves/keep.dat";
        write(junk, "junk"); write(save, "save");
        JunkService service;
        service.registerLocations(locations(game));
        auto preview = service.preview();
        preview.items.front().path = save;
        preview.items.front().size = 999;
        preview.items.front().revision = "invented";
        preview.totalBytes = 999;
        require(service.clean(preview.previewId).freedBytes == 4, "public DTO mutated private authorization");
        require(fs::exists(save), "DTO path injection deleted save");
    });
    run("new_files_not_in_preview_survive", [] {
        Fixture fixture("new-files");
        const auto game = fixture.game();
        write(game / "logs/old.log", "old");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        write(game / "logs/new.log", "new");
        write(game / "hs_err_pid777.log", "new-jvm");
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 1 && fs::exists(game / "logs/new.log") && fs::exists(game / "hs_err_pid777.log"), "execute rescanned enlarged scope");
    });
    run("same_path_replacement_is_conflict", [] {
        Fixture fixture("replace");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "original");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        fs::rename(path, path.parent_path() / "old-object");
        write(path, "original");
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 0 && result.conflictCount == 1 && fs::exists(path), "replacement identity accepted");
    });
    run("same_size_write_and_restored_mtime_is_conflict", [] {
        Fixture fixture("revision");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "AAAA");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        const auto oldTime = fs::last_write_time(path);
        write(path, "BBBB");
        fs::last_write_time(path, oldTime);
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 0 && result.conflictCount == 1 && fs::exists(path), "same-size external write accepted");
    });
    run("external_append_is_conflict", [] {
        Fixture fixture("append");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "old");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        write(path, "old-and-new");
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 0 && result.conflictCount == 1 && fs::exists(path), "appended data deleted");
    });
    run("parent_replacement_is_conflict", [] {
        Fixture fixture("parent");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "old");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        fs::rename(game / "logs", game / "retained-old-logs");
        write(path, "new");
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 0 && result.conflictCount == 1, "replaced parent accepted");
        require(fs::exists(path) && fs::exists(game / "retained-old-logs/latest.log"), "cleanup followed parent replacement");
    });
    run("root_replacement_is_conflict", [] {
        Fixture fixture("root");
        const auto game = fixture.game();
        write(game / "logs/latest.log", "old");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        fs::rename(game, fixture.root / "old-game");
        fs::create_directories(game / "versions");
        write(game / "logs/latest.log", "new");
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 0 && result.conflictCount == 1 && fs::exists(game / "logs/latest.log"), "root replacement accepted");
    });
    run("missing_item_does_not_fail_unrelated_authorization", [] {
        Fixture fixture("missing");
        const auto game = fixture.game();
        write(game / "logs/a.log", "a"); write(game / "logs/b.log", "b");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        fs::remove(game / "logs/a.log");
        const auto result = service.clean(preview.previewId);
        require(result.missingCount == 1 && result.removedCount == 1 && result.freedBytes == 1, "missing item accounting incorrect");
    });
    run("reregister_invalidates_preview", [] {
        Fixture fixture("reregister");
        const auto game = fixture.game();
        write(game / "logs/latest.log", "old");
        JunkService service;
        const auto first = service.registerLocations(locations(game));
        const auto preview = service.preview();
        const auto second = service.registerLocations({});
        require(second.revision > first.revision, "registration revision not monotonic");
        rejects([&] { service.clean(preview.previewId); });
        require(fs::exists(game / "logs/latest.log"), "stale location authorization used");
    });
    run("preview_retention_eviction", [] {
        JunkLimits limits; limits.maxRetainedPreviews = 1;
        JunkService service(limits);
        service.registerLocations({});
        const auto first = service.preview();
        const auto second = service.preview();
        require(first.previewId != second.previewId, "preview identity reused");
        rejects([&] { service.clean(first.previewId); });
        service.clean(second.previewId);
    });
    run("expired_and_discarded_previews", [] {
        JunkLimits limits; limits.previewLifetime = std::chrono::seconds{1};
        JunkService service(limits);
        service.registerLocations({});
        const auto expired = service.preview();
        std::this_thread::sleep_for(std::chrono::milliseconds{1100});
        rejects([&] { service.clean(expired.previewId); });
        const auto discarded = service.preview();
        service.discardPreview(discarded.previewId);
        rejects([&] { service.clean(discarded.previewId); });
        rejects([&] { service.clean(""); });
        rejects([&] { service.clean("unissued-preview"); });
    });
    run("cancelled_preview_does_not_authorize", [] {
        JunkService service;
        service.registerLocations({});
        std::stop_source cancellation;
        cancellation.request_stop();
        rejects([&] { service.preview(cancellation.get_token()); });
    });
    run("cancelled_clean_is_consumed_without_deletion", [] {
        Fixture fixture("cancel-clean");
        const auto game = fixture.game();
        write(game / "logs/latest.log", "keep");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::stop_source cancellation;
        cancellation.request_stop();
        const auto result = service.clean(preview.previewId, cancellation.get_token());
        require(result.cancelledCount == 1 && result.removedCount == 0 && fs::exists(game / "logs/latest.log"), "pre-cancelled clean deleted data");
        rejects([&] { service.clean(preview.previewId); });
    });
    run("concurrent_double_clean_consumes_once", [] {
        Fixture fixture("concurrent");
        const auto game = fixture.game();
        write(game / "logs/latest.log", "once");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::atomic<std::size_t> removed{}, rejected{};
        auto execute = [&] {
            try { removed.fetch_add(service.clean(preview.previewId).removedCount); }
            catch (const std::exception&) { rejected.fetch_add(1); }
        };
        std::thread first(execute), second(execute);
        first.join(); second.join();
        require(removed == 1 && rejected == 1, "double execution was not linearized");
    });
}

void linkTests() {
    run("hardlinked_candidate_not_authorized", [] {
        Fixture fixture("hardlink");
        const auto game = fixture.game();
        const auto target = fixture.root / "outside-file";
        write(target, "outside");
        fs::create_directories(game / "logs");
        fs::create_hard_link(target, game / "logs/hard.log");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        require(preview.fileCount == 0 && fs::exists(target), "hardlink candidate authorized");
    });
    run("hardlink_added_after_preview_is_conflict", [] {
        Fixture fixture("hardlink-after");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "keep");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        fs::create_hard_link(path, fixture.root / "external-alias");
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 0 && result.conflictCount == 1 && fs::exists(path), "post-preview hardlink accepted");
    });
    run("linked_logs_and_crash_directories_are_not_followed", [] {
        Fixture fixture("linked-dirs");
        const auto game = fixture.game();
        const auto outside = fixture.root / "outside";
        write(outside / "keep.log", "outside");
        if (!fixture.link(outside, game / "logs", true)) return;
        if (!fixture.link(outside, game / "crash-reports", true)) return;
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        require(preview.fileCount == 0, "linked directory scanned");
        service.clean(preview.previewId);
        require(fs::exists(outside / "keep.log"), "linked outside file removed");
    });
    run("linked_file_and_root_are_not_authorized", [] {
        Fixture fixture("linked-root");
        const auto game = fixture.game();
        write(game / "logs/latest.log", "real");
        write(fixture.root / "outside-file", "outside");
        if (!fixture.link(fixture.root / "outside-file", game / "logs/linked.log", false)) return;
        if (!fixture.link(game, fixture.root / "linked-game", true)) return;
        JunkService service;
        service.registerLocations(locations(fixture.root / "linked-game"));
        require(service.preview().fileCount == 0, "linked root accepted");
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        require(preview.fileCount == 1 && !contains(preview, game / "logs/linked.log"), "linked file accepted");
        service.clean(preview.previewId);
        require(fs::exists(fixture.root / "outside-file"), "linked file target removed");
    });
    run("symlink_replacement_after_preview_is_conflict", [] {
        Fixture fixture("link-replace");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "original");
        write(fixture.root / "outside-file", "outside");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        fs::remove(path);
        if (!fixture.link(fixture.root / "outside-file", path, false)) return;
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 0 && result.conflictCount == 1 && fs::exists(fixture.root / "outside-file"), "linked replacement accepted");
    });
    run("versions_redirected_outside_is_not_effective", [] {
        Fixture fixture("versions-link");
        const auto game = fixture.root / "game";
        const auto outside = fixture.root / "outside";
        write(game / "options.txt", "options");
        write(outside / "good/logs/latest.log", "outside");
        if (!fixture.link(outside, game / "versions", true)) return;
        auto input = locations(game);
        input.gameRoots.front().instances = {{"good", game / "versions/good"}};
        JunkService service;
        service.registerLocations(input);
        require(service.preview().fileCount == 0, "outside versions redirect authorized");
        require(fs::exists(outside / "good/logs/latest.log"), "outside isolated log removed");
    });
#ifdef _WIN32
    run("open_non_delete_shared_file_is_not_unlinked", [] {
        Fixture fixture("occupied");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "occupied");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        HANDLE handle = CreateFileW(nativePath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "failed to create occupied fixture");
        const auto result = service.clean(preview.previewId);
        CloseHandle(handle);
        require(result.removedCount == 0 && result.freedBytes == 0 && fs::exists(nativePath(path)), "occupied file was removed");
        require(result.conflictCount == 1 && result.failedCount == 0, "publication-open rejection did not preserve transport revision_conflict");
        rejects([&] { service.clean(preview.previewId); });
    });
#endif
}

void quotaTests() {
    run("invalid_limits_rejected", [] {
        JunkLimits limits; limits.maxFiles = 0;
        rejects([&] { JunkService service(limits); });
        limits = {}; limits.maxRetainedPreviews = std::numeric_limits<std::size_t>::max();
        rejects([&] { JunkService service(limits); });
        limits = {}; limits.previewLifetime = std::chrono::seconds{0};
        rejects([&] { JunkService service(limits); });
    });
    run("root_and_instance_registration_quotas", [] {
        Fixture fixture("registration-quota");
        const auto game = fixture.game();
        JunkLimits limits; limits.maxRoots = 1; limits.maxInstances = 1;
        JunkService service(limits);
        service.registerLocations(locations(game));
        const auto stillValid = service.preview();
        auto input = locations(game); input.gameRoots.push_back({game, {}});
        rejects([&] { service.registerLocations(input); });
        service.clean(stillValid.previewId);
        input = locations(game);
        input.gameRoots.front().instances = {{"one", game}, {"two", game}};
        rejects([&] { service.registerLocations(input); });
    });
    run("file_quota_fails_without_partial_preview", [] {
        Fixture fixture("file-quota");
        const auto game = fixture.game();
        write(game / "logs/a.log", "a"); write(game / "logs/b.log", "b");
        JunkLimits limits; limits.maxFiles = 1;
        JunkService service(limits);
        service.registerLocations(locations(game));
        rejects([&] { service.preview(); });
        require(fs::exists(game / "logs/a.log") && fs::exists(game / "logs/b.log"), "failed preview changed files");
    });
    run("per_file_and_total_byte_quotas", [] {
        Fixture fixture("byte-quota");
        const auto game = fixture.game();
        write(game / "logs/a.log", "aaaa"); write(game / "logs/b.log", "bbbb");
        JunkLimits limits; limits.maxFileBytes = 3;
        JunkService perFile(limits);
        perFile.registerLocations(locations(game));
        rejects([&] { perFile.preview(); });
        limits = {}; limits.maxTotalBytes = 7;
        JunkService total(limits);
        total.registerLocations(locations(game));
        rejects([&] { total.preview(); });
    });
    run("scan_entry_quota_counts_non_candidates", [] {
        Fixture fixture("scan-quota");
        const auto game = fixture.game();
        for (std::size_t index = 0; index < 12; ++index) fs::create_directories(game / "logs" / std::to_string(index));
        JunkLimits limits; limits.maxScannedEntries = 10;
        JunkService service(limits);
        service.registerLocations(locations(game));
        rejects([&] { service.preview(); });
    });
    run("beforewrite_capacity_failure_is_non_destructive", [] {
        Fixture fixture("disk-guard");
        const auto path = fixture.root / "keep";
        write(path, "keep");
        require(!canWrite(fixture.root, std::numeric_limits<std::uint64_t>::max()), "synthetic oversize request passed capacity gate");
        require(fs::file_size(path) == 4, "capacity probe modified file");
    });
}

void validatorTests() {
    run("validator_conflict_consumes_without_deleting", [] {
        Fixture fixture("validator-conflict");
        const auto game = fixture.game();
        write(game / "logs/latest.log", "keep");
        bool valid = true;
        JunkService service({}, [&](const JunkLocations& registered) {
            require(registered.gameRoots.front().root == game, "validator did not receive immutable registered locations");
            return valid;
        });
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        valid = false;
        const auto result = service.clean(preview.previewId);
        require(result.locationConflict && result.conflictCount == 1 && result.removedCount == 0, "location change was not an explicit conflict");
        require(fs::exists(game / "logs/latest.log"), "changed backend location deleted a file");
        rejects([&] { service.clean(preview.previewId); });
        rejects([&] { service.preview(); });
    });
    run("validator_exception_fails_closed", [] {
        JunkService service({}, [](const JunkLocations&) -> bool { throw 7; });
        service.registerLocations({});
        rejects([&] { service.preview(); });
    });
    run("cancelled_empty_clean_has_explicit_status", [] {
        JunkService service;
        service.registerLocations({});
        const auto preview = service.preview();
        std::stop_source cancellation;
        cancellation.request_stop();
        const auto result = service.clean(preview.previewId, cancellation.get_token());
        require(result.cancelled && result.cancelledCount == 0 && result.removedCount == 0, "empty pre-cancelled cleanup was reported as success");
        rejects([&] { service.clean(preview.previewId); });
    });
    run("validator_checks_each_leaf_after_hash", [] {
        Fixture fixture("validator-per-leaf");
        const auto game = fixture.game();
        const auto first = game / "logs/a.log", second = game / "logs/b.log";
        write(first, "a"); write(second, "b");
        bool cleaning = false;
        bool valid = true;
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
        std::size_t validated{};
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path&) {
            if (point == "regular.validated" && ++validated == 2) valid = false;
        });
#endif
        JunkService service({}, [&](const JunkLocations&) {
#ifndef NICKEL_TRANSPORT_TEST_HOOKS
            if (cleaning && completedLeafMissing(first, second)) valid = false;
#endif
            return !cleaning || valid;
        });
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        cleaning = true;
        const auto result = service.clean(preview.previewId);
        require(result.locationConflict && result.removedCount == 1 && result.conflictCount == 1 && result.freedBytes == 1, "per-leaf backend change was not partial conflict");
        require(fs::exists(nativePath(first)) != fs::exists(nativePath(second)), "location conflict removed both authorized leaves");
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
        require(validated == 2, "partial conflict did not reach both real commit checkpoints");
#endif
        rejects([&] { service.clean(preview.previewId); });
    });
    run("mid_clean_cancellation_reports_partial_result", [] {
        Fixture fixture("partial-cancel");
        const auto game = fixture.game();
        const auto first = game / "logs/a.log", second = game / "logs/b.log";
        write(first, "a"); write(second, "b");
        std::stop_source cancellation;
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
        std::size_t validated{};
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path&) {
            if (point == "regular.validated" && ++validated == 2) cancellation.request_stop();
        });
        JunkService service;
#else
        bool cleaning = false;
        JunkService service({}, [&](const JunkLocations&) {
            if (cleaning && completedLeafMissing(first, second)) cancellation.request_stop();
            return true;
        });
#endif
        service.registerLocations(locations(game));
        const auto preview = service.preview();
#ifndef NICKEL_TRANSPORT_TEST_HOOKS
        cleaning = true;
#endif
        const auto result = service.clean(preview.previewId, cancellation.get_token());
        require(result.cancelled && result.removedCount == 1 && result.cancelledCount == 1 && result.freedBytes == 1, "partial cancellation lost completed/deferred counts");
        require(fs::exists(nativePath(first)) != fs::exists(nativePath(second)), "partial cancellation removed both leaves");
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
        require(validated == 2, "partial cancellation did not reach both real commit checkpoints");
#endif
        rejects([&] { service.clean(preview.previewId); });
    });
    run("logical_ancestor_link_is_explicitly_unsupported", [] {
        Fixture fixture("ancestor-link");
        const auto parent = fixture.root / "real";
        const auto game = parent / "game";
        write(game / "options.txt", "options");
        write(game / "logs/latest.log", "outside-through-alias");
        if (!fixture.link(parent, fixture.root / "alias", true)) return;
        JunkService service;
        const auto result = service.registerLocations(locations(fixture.root / "alias/game"));
        require(result.acceptedRootCount == 0 && result.skippedRootCount == 1 && !result.issues.empty(), "logical link ancestor was silently canonicalized");
        require(service.preview().fileCount == 0 && fs::exists(game / "logs/latest.log"), "unsupported alias authorized physical target");
    });
    run("windows_current_log_case_alias_is_preserved", [] {
#ifdef _WIN32
        Fixture fixture("case-log");
        const auto game = fixture.game();
        write(game / "logs/NICKEL-LAUNCHER.LOG", "current");
        JunkService service;
        service.registerLocations(locations(game));
        require(service.preview().fileCount == 0, "Windows current launcher case alias was authorized");
#else
        ++skipped;
        std::cout << "SKIP Windows current-log case alias test\n";
#endif
    });
}


void logicalBoundaryTests() {
    run("empty_regular_file_has_verified_deletion", [] {
        Fixture fixture("empty-file");
        const auto game = fixture.game();
        const auto path = game / "logs/empty.log";
        write(path, "");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        require(preview.fileCount == 1 && preview.totalBytes == 0, "empty file authorization missing");
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 1 && result.freedBytes == 0 && result.failedCount == 0 && !fs::exists(nativePath(path)), "empty file was not deleted by its token");
    });
    run("instance_becoming_pending_after_preview_is_conflict", [] {
        Fixture fixture("pending-after-preview");
        const auto game = fixture.game();
        const auto version = game / "versions/isolated";
        const auto path = version / "logs/latest.log";
        write(path, "preserve");
        auto input = locations(game);
        input.gameRoots.front().instances = {{"isolated", version}};
        JunkService service;
        service.registerLocations(input);
        const auto preview = service.preview();
        require(preview.fileCount == 1, "isolated preview missing");
        write(version / ".nickel-pack.pending", "installer-owned");
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 0 && result.conflictCount == 1 && result.freedBytes == 0 && readText(path) == "preserve", "pending-after-preview instance was cleaned");
        rejects([&] { service.clean(preview.previewId); });
    });
    run("unicode_deep_logical_roots_preserve_exact_scope", [] {
        Fixture fixture("unicode-long-path");
        auto game = fixture.root;
        for (std::size_t index = 0; index < 4; ++index) {
            game /= "deep-" + std::to_string(index) + "-" + std::string(55, 'x');
        }
        game /= fs::path(u8"\u6e38\u620f-\U0001f6e0");
        const auto config = fixture.root / fs::path(u8"\u914d\u7f6e");
        const std::u8string versionName = u8"\u547d\u540d\u5b9e\u4f8b-\u6d4b\u8bd5";
        const auto version = game / "versions" / fs::path(versionName);
        const auto rootLog = game / "logs" / fs::path(u8"\u65e5\u5fd7.log");
        const auto isolatedLog = version / "logs" / fs::path(u8"\u5f52\u6863.log.gz");
        const auto crash = version / "crash-reports" / fs::path(u8"\u5d29\u6e83.txt");
        const auto temporary = config / fs::path(u8".nickel-\u6d4b\u8bd5.part");
        const std::vector<fs::path> candidates{rootLog, isolatedLog, crash, temporary};
        const auto current = game / "logs/nickel-launcher.log";
        const auto outside = game / "versions/unregistered/logs/latest.log";
        write(game / "options.txt", "options");
        for (const auto& path : candidates) write(path, "junk");
        write(current, "current"); write(outside, "outside");
        require(game.native().size() > 260, "long-path fixture was silently shortened");
        auto input = locations(game, config);
        input.gameRoots.front().instances = {{std::string(versionName.begin(), versionName.end()), version}};
        JunkService service;
        const auto registered = service.registerLocations(input);
        require(registered.acceptedRootCount == 1 && registered.effectiveDirectoryCount == 2 && registered.issues.empty(), "Unicode/deep logical registration failed");
        const auto preview = service.preview();
        require(preview.fileCount == candidates.size() && preview.totalBytes == 16, "Unicode/deep preview omitted exact leaves");
        for (const auto& path : candidates) require(contains(preview, path), "logical DTO path was rewritten to native I/O syntax");
        const auto newLog = game / "logs" / fs::path(u8"\u65b0\u65e5\u5fd7.log");
        write(newLog, "new");
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == candidates.size() && result.freedBytes == 16 && result.failedCount == 0 && result.conflictCount == 0, "Unicode/deep authorized deletion failed");
        for (const auto& path : candidates) require(!fs::exists(nativePath(path)), "Unicode/deep authorized leaf remained");
        require(readText(current) == "current" && readText(outside) == "outside" && readText(newLog) == "new", "deep cleanup enlarged scope");
    });
}

void leafVerificationTests() {
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
    run("location_conflict_at_verified_commit_preserves_leaf", [] {
        Fixture fixture("validator-at-commit");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "keep");
        bool valid = true;
        bool reached = false;
        JunkService service({}, [&](const JunkLocations&) { return valid; });
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (point == "regular.validated" && leaf == path) { reached = true; valid = false; }
        });
        const auto result = service.clean(preview.previewId);
        require(reached && result.locationConflict && result.conflictCount == 1 && result.removedCount == 0 && result.freedBytes == 0, "commit-time location conflict was ignored");
        require(readText(path) == "keep", "commit-time location conflict lost data");
        rejects([&] { service.clean(preview.previewId); });
    });
    run("cancellation_during_leaf_hash_preserves_all_data", [] {
        Fixture fixture("cancel-during-hash");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        const std::string payload(3U * 64U * 1024U, 'x');
        write(path, payload);
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::stop_source cancellation;
        bool reached = false;
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (point == "regular.hash" && leaf == path) { reached = true; cancellation.request_stop(); }
        });
        const auto result = service.clean(preview.previewId, cancellation.get_token());
        require(reached && result.cancelled && result.cancelledCount == 1 && result.removedCount == 0 && result.freedBytes == 0, "hash-time cancellation was not consumed safely");
        require(fs::file_size(nativePath(path)) == payload.size(), "hash-time cancellation changed the authorized file");
        rejects([&] { service.clean(preview.previewId); });
    });
#ifdef _WIN32
    run("retained_windows_handles_block_swap_and_outside_writer", [] {
        Fixture fixture("retained-win-handles");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        const auto replacement = game / "logs/new-unpreviewed.log";
        write(path, "authorized");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        write(replacement, "outside-new");
        bool reached = false, writerBlocked = false, replacementBlocked = false, parentBlocked = false;
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (point != "regular.validated" || leaf != path) return;
            reached = true;
            std::thread outsider([&] {
                const auto writer = CreateFileW(nativePath(path).c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                writerBlocked = writer == INVALID_HANDLE_VALUE;
                if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
                replacementBlocked = !MoveFileExW(nativePath(replacement).c_str(), nativePath(path).c_str(), MOVEFILE_REPLACE_EXISTING);
                parentBlocked = !MoveFileExW(nativePath(path.parent_path()).c_str(), nativePath(game / "moved-logs").c_str(), 0);
            });
            outsider.join();
        });
        const auto result = service.clean(preview.previewId);
        require(reached && writerBlocked && replacementBlocked && parentBlocked, "retained handles permitted a concurrent writer or namespace replacement");
        require(result.removedCount == 1 && result.freedBytes == 10 && result.failedCount == 0 && !fs::exists(nativePath(path)), "retained-handle token deletion failed");
        require(readText(replacement) == "outside-new", "new unpreviewed replacement was deleted");
    });
#else
    run("posix_swap_after_validation_preserves_both_inodes", [] {
        Fixture fixture("posix-validated-swap");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        const auto retained = fixture.root / "old-inode";
        const auto replacement = fixture.root / "replacement";
        write(path, "original"); write(replacement, "external");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        bool reached = false;
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (point == "regular.validated" && leaf == path) {
                reached = true;
                fs::rename(path, retained);
                fs::rename(replacement, path);
            }
        });
        const auto result = service.clean(preview.previewId);
        require(reached && result.removedCount == 0 && result.conflictCount == 1 && result.freedBytes == 0, "validated POSIX replacement was deleted");
        require(readText(path) == "external" && readText(retained) == "original", "POSIX replacement conflict lost an inode");
        rejects([&] { service.clean(preview.previewId); });
    });
    run("posix_quarantine_writer_conflict_restores_without_deletion", [] {
        Fixture fixture("posix-quarantine-write");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "AAAA");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::fstream writer(path, std::ios::binary | std::ios::in | std::ios::out);
        require(static_cast<bool>(writer), "POSIX retained writer fixture unavailable");
        bool reached = false;
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (point == "regular.quarantined" && leaf == path) {
                reached = true;
                writer.seekp(0); writer.write("BBBB", 4); writer.flush();
                require(static_cast<bool>(writer), "POSIX retained writer did not write the isolated inode");
            }
        });
        const auto result = service.clean(preview.previewId);
        writer.close();
        require(reached && result.removedCount == 0 && result.conflictCount == 1 && result.freedBytes == 0, "quarantined POSIX write was deleted");
        require(readText(path) == "BBBB", "quarantined write conflict was not restored");
    });
    run("posix_quarantine_cancellation_restores_authorized_inode", [] {
        Fixture fixture("posix-quarantine-cancel");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "keep");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::stop_source cancellation;
        bool reached = false;
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (point == "regular.quarantined" && leaf == path) { reached = true; cancellation.request_stop(); }
        });
        const auto result = service.clean(preview.previewId, cancellation.get_token());
        require(reached && result.cancelled && result.cancelledCount == 1 && result.removedCount == 0 && result.freedBytes == 0, "POSIX quarantine cancellation was not recorded");
        require(readText(path) == "keep", "POSIX quarantine cancellation lost the preview inode");
        rejects([&] { service.clean(preview.previewId); });
    });
    run("posix_restore_collision_retains_old_inode_without_overwrite", [] {
        Fixture fixture("posix-restore-collision");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "original");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::stop_source cancellation;
        bool reached = false;
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (point == "regular.quarantined" && leaf == path) {
                reached = true; write(path, "external"); cancellation.request_stop();
            }
        });
        const auto result = service.clean(preview.previewId, cancellation.get_token());
        require(reached && result.removedCount == 0 && result.conflictCount == 1 && result.freedBytes == 0 && readText(path) == "external", "no-replace restoration overwrote a new entry");
        std::size_t retained{};
        for (const auto& entry : fs::directory_iterator(path.parent_path())) {
            if (entry.path().filename().string().starts_with(".nickel-delete-") && entry.is_directory()) {
                require(readText(entry.path() / "leaf") == "original", "retained quarantine contains the wrong inode");
                ++retained;
            }
        }
        require(retained == 1, "conflicted original inode was not retained");
        rejects([&] { service.clean(preview.previewId); });
    });
#endif
#else
    ++skipped;
    std::cout << "SKIP transport verification checkpoints are not enabled in this production target\n";
#endif
}


template<class F>
void rejectsCode(const char* expected, F&& action) {
    bool matched = false;
    try { action(); }
    catch (const nickel::ErrorException& error) { matched = error.error().code == expected; }
    require(matched, "operation did not preserve its public error code");
}

void removalPersistenceTests() {
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
    for (const auto bytes : {std::size_t{0}, std::size_t{17}}) {
        for (int fault = 0; fault < 4; ++fault) {
            const auto name = "committed_persistence_fault_" + std::to_string(bytes) + "_" + std::to_string(fault);
            run(name.c_str(), [bytes, fault] {
                Fixture fixture("persistence-fault");
                const auto game = fixture.game();
                const auto path = game / "logs/latest.log";
                write(path, std::string(bytes, 'x'));
                JunkService service;
                service.registerLocations(locations(game));
                const auto preview = service.preview();
                std::size_t reached{};
                TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
                    if (point != "regular.committed" || leaf != path) return;
                    ++reached;
                    require(!fs::exists(nativePath(leaf)), "persistence fault preceded real deletion");
                    if (fault == 0) throw std::system_error(std::make_error_code(std::errc::io_error));
                    if (fault == 1) throw nickel::ErrorException(nickel::Error{"cancelled", "private test error"});
                    if (fault == 2) throw std::runtime_error("private test runtime error");
                    throw 7;
                });
                const auto result = service.clean(preview.previewId);
                require(reached == 1 && result.removedCount == 1 && result.freedBytes == bytes && result.failedCount == 1 &&
                        result.missingCount == 0 && result.conflictCount == 0 && result.cancelledCount == 0 && !result.cancelled && !result.locationConflict,
                        "committed persistence failure lost applied bytes or was classified as cancellation");
                require(!fs::exists(nativePath(path)), "committed persistence fault recreated the removed file");
                rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
            });
        }
    }
    run("persistence_fault_and_late_cancel_preserve_partial_results", [] {
        Fixture fixture("persistence-partial-cancel");
        const auto game = fixture.game();
        write(game / "logs/a.log", "a"); write(game / "logs/b.log", "b");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::stop_source cancellation;
        std::size_t reached{};
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path&) {
            if (point == "regular.committed") {
                ++reached; cancellation.request_stop();
                throw std::system_error(std::make_error_code(std::errc::io_error));
            }
        });
        const auto result = service.clean(preview.previewId, cancellation.get_token());
        require(reached == 1 && result.removedCount == 1 && result.freedBytes == 1 && result.failedCount == 1 &&
                result.cancelled && result.cancelledCount == 1 && result.conflictCount == 0 && result.missingCount == 0 && !result.locationConflict,
                "persistence error and cancellation discarded committed facts or double-counted a deferred leaf");
        require(fs::exists(nativePath(game / "logs/a.log")) != fs::exists(nativePath(game / "logs/b.log")), "persistence/cancel removed both leaves");
        rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
    });
    run("persistence_fault_and_final_cancel_keep_committed_result", [] {
        Fixture fixture("persistence-final-cancel");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "data");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::stop_source cancellation;
        std::size_t reached{};
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (point == "regular.committed" && leaf == path) {
                ++reached; cancellation.request_stop();
                throw nickel::ErrorException(nickel::Error{"cancelled", "post-commit cancellation"});
            }
        });
        const auto result = service.clean(preview.previewId, cancellation.get_token());
        require(reached == 1 && result.removedCount == 1 && result.freedBytes == 4 && result.failedCount == 1 &&
                !result.cancelled && result.cancelledCount == 0 && result.conflictCount == 0 && result.missingCount == 0 && !result.locationConflict,
                "final committed persistence error was replaced by a zero-effect cancelled result");
        require(!fs::exists(nativePath(path)), "final persistence fault lost real removal");
        rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
    });
    run("persistence_fault_and_backend_conflict_preserve_remaining_leaf", [] {
        Fixture fixture("persistence-partial-conflict");
        const auto game = fixture.game();
        write(game / "logs/a.log", "a"); write(game / "logs/b.log", "b");
        bool valid = true;
        JunkService service({}, [&](const JunkLocations&) { return valid; });
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::size_t reached{};
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path&) {
            if (point == "regular.committed") {
                ++reached; valid = false;
                throw std::system_error(std::make_error_code(std::errc::io_error));
            }
        });
        const auto result = service.clean(preview.previewId);
        require(reached == 1 && result.removedCount == 1 && result.freedBytes == 1 && result.failedCount == 1 &&
                result.locationConflict && result.conflictCount == 1 && result.cancelledCount == 0 && !result.cancelled && result.missingCount == 0,
                "persistence error discarded committed facts or deleted a location-conflicted leaf");
        require(fs::exists(nativePath(game / "logs/a.log")) != fs::exists(nativePath(game / "logs/b.log")), "persistence/conflict removed both leaves");
        rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
    });
    for (int fault = 0; fault < 4; ++fault) {
        const auto name = "committed_publication_fault_" + std::to_string(fault);
        run(name.c_str(), [fault] {
            Fixture fixture("publication-fault");
            const auto game = fixture.game();
            const auto path = game / "logs/latest.log";
            write(path, "data");
            JunkService service;
            service.registerLocations(locations(game));
            const auto preview = service.preview();
            std::size_t reached{};
            TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
                if (point != "cleanup.committed" || leaf != path) return;
                ++reached;
                require(!fs::exists(nativePath(leaf)), "publication fault preceded real deletion");
                if (fault == 0) throw std::system_error(std::make_error_code(std::errc::io_error));
                if (fault == 1) throw nickel::ErrorException(nickel::Error{"cancelled", "post-commit publication fault"});
                if (fault == 2) throw std::runtime_error("post-commit publication fault");
                throw 7;
            });
            const auto result = service.clean(preview.previewId);
            require(reached == 1 && result.removedCount == 1 && result.freedBytes == 4 && result.failedCount == 1 &&
                    result.missingCount == 0 && result.conflictCount == 0 && result.cancelledCount == 0 && !result.cancelled && !result.locationConflict,
                    "publication error lost removal facts or misclassified the committed leaf");
            require(!fs::exists(nativePath(path)), "publication fault recreated the removed file");
            rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
        });
    }
    run("transport_and_publication_faults_count_the_committed_leaf_once", [] {
        Fixture fixture("double-postcommit-fault");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "data");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::size_t transport{}, publication{};
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (leaf != path) return;
            if (point == "regular.committed") {
                ++transport;
                throw std::system_error(std::make_error_code(std::errc::io_error));
            }
            if (point == "cleanup.committed") {
                ++publication;
                throw nickel::ErrorException(nickel::Error{"cancelled", "post-commit publication fault"});
            }
        });
        const auto result = service.clean(preview.previewId);
        require(transport == 1 && publication == 1 && result.removedCount == 1 && result.freedBytes == 4 && result.failedCount == 1 &&
                result.missingCount == 0 && result.conflictCount == 0 && result.cancelledCount == 0 && !result.cancelled && !result.locationConflict,
                "two post-commit errors double-counted one leaf or discarded the applied result");
        require(!fs::exists(nativePath(path)), "double post-commit fault recreated the removed file");
        rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
    });
    const nickel::Json validDetails{{"committed", true}, {"removed", true}, {"removedBytes", std::uint64_t{4}}, {"durabilityPending", true}};
    std::vector<std::pair<std::string, nickel::Json>> malformed{
        {"null", nullptr}, {"array", nickel::Json::array()}, {"string", "not-an-object"}
    };
    for (const auto* key : {"committed", "removed", "durabilityPending"}) {
        auto missing = validDetails;
        missing.erase(key);
        malformed.emplace_back(std::string(key) + "_missing", std::move(missing));
        for (const auto& value : {nickel::Json(false), nickel::Json("true"), nickel::Json(1)}) {
            auto details = validDetails;
            details[key] = value;
            malformed.emplace_back(std::string(key) + "_" + value.dump(), std::move(details));
        }
    }
    auto missingBytes = validDetails;
    missingBytes.erase("removedBytes");
    malformed.emplace_back("bytes_missing", std::move(missingBytes));
    for (const auto& value : {nickel::Json("4"), nickel::Json(4.0), nickel::Json(-1), nickel::Json(5),
                             nickel::Json(std::uint64_t{5}), nickel::Json(std::numeric_limits<std::uint64_t>::max())}) {
        auto details = validDetails;
        details["removedBytes"] = value;
        malformed.emplace_back("bytes_" + value.dump(), std::move(details));
    }
    for (const auto& [shape, details] : malformed) {
        const auto name = "precommit_pending_details_rejected_" + shape;
        run(name.c_str(), [details] {
            Fixture fixture("precommit-pending-claim");
            const auto game = fixture.game();
            const auto path = game / "logs/latest.log";
            write(path, "keep");
            JunkService service;
            service.registerLocations(locations(game));
            const auto preview = service.preview();
            std::size_t reached{};
            TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
                if (point == "regular.validated" && leaf == path) {
                    ++reached;
                    throw nickel::ErrorException(nickel::Error{"remove_persistence_pending", "malformed precommit claim", details});
                }
            });
            const auto result = service.clean(preview.previewId);
            require(reached == 1 && result.removedCount == 0 && result.freedBytes == 0 && result.failedCount == 1 &&
                    result.missingCount == 0 && result.conflictCount == 0 && result.cancelledCount == 0 && !result.cancelled && !result.locationConflict,
                    "malformed pending details claimed an uncommitted removal");
            require(readText(path) == "keep", "precommit pending error changed the file");
            rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
        });
    }
    run("unrelated_error_with_pending_details_does_not_claim_removal", [validDetails] {
        Fixture fixture("precommit-unrelated-code");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "keep");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::size_t reached{};
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (point == "regular.validated" && leaf == path) {
                ++reached;
                throw nickel::ErrorException(nickel::Error{"remove_failed", "unrelated precommit error", validDetails});
            }
        });
        const auto result = service.clean(preview.previewId);
        require(reached == 1 && result.removedCount == 0 && result.freedBytes == 0 && result.failedCount == 1 &&
                result.missingCount == 0 && result.conflictCount == 0 && result.cancelledCount == 0 && !result.cancelled && !result.locationConflict,
                "unrelated error code was interpreted as committed removal");
        require(readText(path) == "keep", "unrelated precommit error changed the file");
        rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
    });
#else
    ++skipped;
    std::cout << "SKIP committed persistence faults require the controlled transport target\n";
#endif
}


#ifndef _WIN32
class RetainedPosixWriter final {
public:
    explicit RetainedPosixWriter(const fs::path& path) : handle_(::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC)) {
        require(handle_ >= 0, "retained POSIX writer open failed");
    }
    ~RetainedPosixWriter() { if (handle_ >= 0) ::close(handle_); }
    RetainedPosixWriter(const RetainedPosixWriter&) = delete;
    RetainedPosixWriter& operator=(const RetainedPosixWriter&) = delete;

    bool replaceKeepingMtime() {
        struct stat before{}, after{};
        require(::fstat(handle_, &before) == 0, "retained POSIX inode stat failed");
        require(::pwrite(handle_, "NEW", 3, 0) == 3, "retained POSIX inode write failed");
#if defined(__APPLE__)
        const timespec times[2]{before.st_atimespec, before.st_mtimespec};
#else
        const timespec times[2]{before.st_atim, before.st_mtim};
#endif
        require(::futimens(handle_, times) == 0 && ::fstat(handle_, &after) == 0, "retained POSIX mtime restoration failed");
        require(before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_size == after.st_size,
                "POSIX writer changed the fixture identity or size");
#if defined(__APPLE__)
        require(before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec && before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec,
                "POSIX writer did not restore the exact mtime");
        return before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec && before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
        require(before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec,
                "POSIX writer did not restore the exact mtime");
        return before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
    }
private:
    int handle_;
};
#endif

void palladiumTests() {
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
    run("capture_byte_quota_fails_before_any_hash", [] {
        Fixture fixture("capture-byte-limit");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "data");
        JunkLimits limits; limits.maxFileBytes = 3;
        JunkService service(limits);
        service.registerLocations(locations(game));
        std::size_t hashes{};
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path&) { if (point == "regular.hash") ++hashes; });
        rejectsCode("LimitExceeded", [&] { service.preview(); });
        require(hashes == 0 && readText(path) == "data", "oversized capture read data or changed a candidate");
    });
    run("remaining_total_quota_fails_before_second_hash", [] {
        Fixture fixture("capture-total-limit");
        const auto game = fixture.game();
        JunkLimits limits; limits.maxTotalBytes = 7;
        JunkService service(limits);
        service.registerLocations(locations(game));
        const auto retained = service.preview();
        write(game / "logs/a.log", "AAAA"); write(game / "logs/b.log", "BBBB");
        std::size_t hashes{};
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path&) { if (point == "regular.hash") ++hashes; });
        rejectsCode("LimitExceeded", [&] { service.preview(); });
        require(hashes == 1, "remaining total quota was checked after reading the second candidate");
        require(service.clean(retained.previewId).removedCount == 0, "failed preview expanded old authorization");
        require(readText(game / "logs/a.log") == "AAAA" && readText(game / "logs/b.log") == "BBBB", "failed total quota changed data");
    });
    run("zero_remaining_bytes_permit_empty_regular_capture", [] {
        Fixture fixture("zero-remaining");
        const auto game = fixture.game();
        write(game / "logs/a.log", "data"); write(game / "logs/b.log", "");
        JunkLimits limits; limits.maxTotalBytes = 4;
        JunkService service(limits);
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        require(preview.fileCount == 2 && preview.totalBytes == 4, "empty candidate was excluded by an exhausted byte budget");
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 2 && result.freedBytes == 4 && result.failedCount == 0, "bounded empty capture failed verified cleanup");
    });
    run("cancelled_capture_hash_preserves_retained_preview", [] {
        Fixture fixture("capture-hash-cancel");
        const auto game = fixture.game();
        JunkService service;
        service.registerLocations(locations(game));
        const auto retained = service.preview();
        const auto path = game / "logs/latest.log";
        write(path, std::string(64U * 1024U, 'x'));
        std::stop_source cancellation;
        std::size_t hashes{};
        {
            TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
                if (point == "regular.hash" && leaf == path) { ++hashes; cancellation.request_stop(); }
            });
            rejectsCode("Cancelled", [&] { service.preview(cancellation.get_token()); });
        }
        require(hashes == 1 && fs::file_size(nativePath(path)) == 64U * 1024U, "capture cancellation failed to stop safely during hash");
        require(service.clean(retained.previewId).removedCount == 0 && fs::exists(nativePath(path)), "cancelled preview enlarged prior authorization");
        const auto fresh = service.preview();
        require(fresh.fileCount == 1 && service.clean(fresh.previewId).removedCount == 1, "cancelled capture left the service unusable");
    });
    run("mixed_missing_conflict_and_committed_results_are_exact", [] {
        Fixture fixture("mixed-result");
        const auto game = fixture.game();
        for (const auto* name : {"a.log", "b.log", "c.log"}) write(game / "logs" / name, "OLD");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        require(preview.items.size() == 3, "mixed fixture preview is incomplete");
        write(preview.items[1].path, "NEW");
        fs::remove(nativePath(preview.items[2].path));
        const auto unpreviewed = game / "logs/new.log";
        write(unpreviewed, "keep");
        const auto result = service.clean(preview.previewId);
        require(result.removedCount == 1 && result.freedBytes == 3 && result.conflictCount == 1 && result.missingCount == 1 &&
                result.failedCount == 0 && result.cancelledCount == 0 && !result.cancelled && !result.locationConflict,
                "mixed cleanup lost exact per-file outcomes");
        require(!fs::exists(nativePath(preview.items[0].path)) && readText(preview.items[1].path) == "NEW" && readText(unpreviewed) == "keep",
                "mixed cleanup expanded authorization or lost replacement content");
        rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
    });
    run("cancellation_after_first_commit_preserves_partial_counts", [] {
        Fixture fixture("committed-partial-cancel");
        const auto game = fixture.game();
        write(game / "logs/a.log", "a"); write(game / "logs/b.log", "b");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::stop_source cancellation;
        std::size_t committed{};
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (point == "cleanup.committed") {
                require(!fs::exists(nativePath(leaf)), "cleanup commit checkpoint preceded real deletion");
                ++committed; cancellation.request_stop();
            }
        });
        const auto result = service.clean(preview.previewId, cancellation.get_token());
        require(committed == 1 && result.removedCount == 1 && result.freedBytes == 1 && result.cancelled && result.cancelledCount == 1 &&
                result.failedCount == 0 && result.conflictCount == 0 && result.missingCount == 0 && !result.locationConflict,
                "post-commit cancellation discarded applied effects or deleted a deferred leaf");
        rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
    });
    run("late_cancellation_after_final_commit_keeps_applied_result", [] {
        Fixture fixture("committed-final-cancel");
        const auto game = fixture.game();
        const auto path = game / "logs/latest.log";
        write(path, "data");
        JunkService service;
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::stop_source cancellation;
        bool committed = false;
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
            if (point == "cleanup.committed" && leaf == path) { committed = true; cancellation.request_stop(); }
        });
        const auto result = service.clean(preview.previewId, cancellation.get_token());
        require(committed && cancellation.stop_requested() && !fs::exists(nativePath(path)) && result.removedCount == 1 && result.freedBytes == 4 &&
                !result.cancelled && result.cancelledCount == 0 && result.failedCount == 0 && result.conflictCount == 0 && result.missingCount == 0 && !result.locationConflict,
                "late cancellation replaced an already-applied final result");
        rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
    });
    run("backend_change_after_commit_preserves_first_effect", [] {
        Fixture fixture("committed-partial-conflict");
        const auto game = fixture.game();
        write(game / "logs/a.log", "a"); write(game / "logs/b.log", "b");
        bool valid = true;
        JunkService service({}, [&](const JunkLocations&) { return valid; });
        service.registerLocations(locations(game));
        const auto preview = service.preview();
        std::size_t committed{};
        TransportCheckpoint checkpoint([&](std::string_view point, const fs::path&) { if (point == "cleanup.committed") { ++committed; valid = false; } });
        const auto result = service.clean(preview.previewId);
        require(committed == 1 && result.removedCount == 1 && result.freedBytes == 1 && result.locationConflict && result.conflictCount == 1 &&
                result.failedCount == 0 && result.missingCount == 0 && result.cancelledCount == 0 && !result.cancelled,
                "backend post-commit change discarded the first applied effect");
        require(fs::exists(nativePath(game / "logs/a.log")) != fs::exists(nativePath(game / "logs/b.log")), "post-commit conflict removed both leaves");
        rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
    });
#ifndef _WIN32
    for (std::size_t iteration = 0; iteration < 5; ++iteration) {
        const auto name = "posix_quarantine_restored_mtime_no_sleep_" + std::to_string(iteration);
        run(name.c_str(), [iteration] {
            Fixture fixture("posix-restored-mtime");
            const auto game = fixture.game();
            const auto path = game / "logs/latest.log";
            write(path, "OLD");
            JunkService service;
            service.registerLocations(locations(game));
            const auto preview = service.preview();
            RetainedPosixWriter writer(path);
            std::size_t reached{};
            bool sameCtime = false;
            TransportCheckpoint checkpoint([&](std::string_view point, const fs::path& leaf) {
                if (point == "regular.quarantined" && leaf == path) { ++reached; sameCtime = writer.replaceKeepingMtime(); }
            });
            const auto result = service.clean(preview.previewId);
            const bool retained = fs::exists(nativePath(path)) && readText(path) == "NEW";
            std::cout << "POSIX_QUARANTINE_REPRO iteration=" << iteration << " reached=" << reached << " sameCtime=" << sameCtime
                      << " restoredMtime=" << (reached == 1) << " conflict=" << result.conflictCount << " retainedNew=" << retained << '\n';
            require(reached == 1 && result.conflictCount == 1 && retained && result.removedCount == 0 && result.freedBytes == 0 &&
                    result.failedCount == 0 && result.cancelledCount == 0 && !result.cancelled,
                    "no-sleep quarantined content change was deleted or not restored");
            rejectsCode("cleanup_invalid_preview", [&] { service.clean(preview.previewId); });
        });
    }
#endif
#endif
}



} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: palladium_cleanup_tests <owned-absolute-fixture-directory>");
        const std::string argument{argv[1]};
        fixtureBase = fs::path(std::u8string(argument.begin(), argument.end()));
        require(fixtureBase.is_absolute() && fs::is_directory(fixtureBase), "fixture base must be existing owned absolute directory");
        safety::DirectoryGuard guard(fixtureBase);
        guard.verify();
#ifdef _WIN32
        require(fixtureBase.root_name().wstring() != L"C:", "fixtures must not write to constrained C volume");
#endif
        require(canWrite(fixtureBase, 1024ULL * 1024ULL), "beforewrite: insufficient fixture volume capacity");
        scopeTests();
        authorizationTests();
        linkTests();
        quotaTests();
        validatorTests();
        logicalBoundaryTests();
        leafVerificationTests();
        palladiumTests();
        removalPersistenceTests();
        std::cout << "RESULT passed=" << passed << " failed=" << failed << " skipped=" << skipped << '\n';
        return failed == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "PREWRITE_FAILURE " << error.what() << '\n';
        return 2;
    }
}
