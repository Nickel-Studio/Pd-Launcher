#include "core/instances/instances.hpp"
#include "core/instances/settings.hpp"
#include "core/instances/startup.hpp"
#include "core/instances/domain_io.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace nickel::instances;
namespace {
void require(bool value, const char* description) {
    if (!value) throw std::runtime_error(description);
}
template<class Fn> void rejected(Fn&& fn, std::string_view code = {}) {
    try { fn(); } catch (const Error& error) {
        if (!code.empty() && error.code() != code) throw std::runtime_error("Wrong error: " + error.code() + " expected " + std::string(code));
        return;
    }
    throw std::runtime_error("Expected rejection");
}
struct Fixture {
    fs::path root = fs::current_path() / ("fixture-" + detail::random_id());
    Fixture() {
        require(!detail::path_exists(root), "Fixture already exists");
        detail::create_directories(root);
    }
    ~Fixture() { try { detail::remove_owned_tree(root); } catch (...) {} }
    fs::path game() { auto path = root / ".minecraft"; detail::create_directories(path); return path; }
    fs::path config() { auto path = root / "Nickel"; detail::create_directories(path); return path; }
};
void write(const fs::path& path, std::string_view text) {
    detail::create_directories(path.parent_path());
    detail::atomic_write(path, text);
}
void descriptor(const fs::path& root, std::string_view id, const Json& custom = Json::object(), std::string_view release = "2025-01-01T00:00:00Z") {
    const auto directory = root / "versions" / path_from_utf8(id);
    detail::create_directories(directory);
    Json value{{"id", id}, {"type", "release"}, {"mainClass", "net.minecraft.client.main.Main"}, {"releaseTime", release}, {"nickelCustom", custom}, {"future", Json{{"nested", true}}}};
    write(directory / path_from_utf8(std::string(id) + ".json"), value.dump());
}
InstallIdentity installed(std::string_view minecraft = "1.20.1", std::optional<std::string> loader = {}, std::string_view build = "") {
    return {std::string(minecraft), loader, std::string(build), Json{{"id", minecraft}, {"type", "release"}, {"mainClass", "net.minecraft.client.main.Main"}, {"unknownMinecraft", 42}}, nullptr};
}
void icon_rules() {
    Fixture f;
    const auto game = f.game();
    const auto entry = [&](std::string_view directory_id, Json value) {
        const auto path = game / "versions" / path_from_utf8(directory_id);
        fs::create_directories(path);
        write(path / path_from_utf8(std::string(directory_id) + ".json"), value.dump());
        return instance_detail(game, directory_id);
    };
    for (const auto id : {"15w14a", "3D Shareware v1.34", "1.RV-Pre1", "20w14infinite", "22w13oneBlockAt", "23w13a_or_b", "24w14potato"}) {
        require(entry(id, {{"id", id}, {"type", "snapshot"}}).icon == "gold_block", "April Fools icon changed");
    }
    int number = 0;
    for (const auto& [type, icon] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
        {"release", "grass"}, {"snapshot", "command_block"}, {"old_beta", "cobblestone"}, {"old_alpha", "cobblestone"}, {"custom", "redstone_block"}}) {
        const auto id = "Type" + std::to_string(number++);
        require(entry(id, {{"id", id}, {"type", type}}).icon == icon, "Version type icon changed");
    }
    for (const auto& [loader, icon] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
        {"forge", "anvil"}, {"neoforge", "neoforge"}, {"fabric", "fabric"}, {"optifine", "grass_path"},
        {"liteloader", "egg"}, {"optifabric", "gold_block"}, {"quilt", "gold_block"}}) {
        const auto id = "Profile" + std::to_string(number++);
        require(entry(id, {{"id", id}, {"type", "snapshot"}, {"nickelProfile", {{"loader", loader}, {"minecraftVersion", "24w14potato"}}}}).icon == icon,
            "Loader/type/April Fools priority or OptiFabric badge contract changed");
    }
    const auto quilt = entry("Quilt", {{"id", "1.20.1"}, {"type", "release"}, {"libraries", Json::array({Json{{"name", "org.quiltmc:quilt-loader:0.24.0"}}})}});
    require(quilt.loader == "fabric" && quilt.icon == "fabric" && quilt.loader_version == "0.24.0", "Legacy Quilt classification or actual library version lost");
    require(entry("Mixed", {{"id", "1.20.1-forge-fabric"}, {"type", "release"}}).loader == "fabric", "Legacy loader evidence priority changed");
    require(entry("Beta", {{"id", "b1.7.3"}, {"type", "old_beta"}}).loader == "vanilla", "Old beta incorrectly classified by type alone");
    require(entry("Legacy", {{"id", "1.5.2"}, {"type", "release"}}).loader == "legacy", "Legacy version number classification changed");
    const auto named = game / "versions/MissingForge";
    fs::create_directories(named);
    write(named / profile_file, Json{{"loader", "forge"}}.dump());
    write(named / "PCL/Setup.ini", "VersionForge:47.0.0\n");
    auto missing = instance_detail(game, "MissingForge");
    require(!missing.has_json && !missing.ready && missing.loader == "unknown" && missing.icon == "redstone_block" && missing.named_profile,
        "Missing descriptor imported profile/cache loader evidence");
    write(named / "MissingForge.json", "{\"id\":");
    require(instance_detail(game, "MissingForge").icon == "redstone_block", "Corrupt descriptor fallback changed");
    entry("CustomIcon", {{"id", "24w14potato"}, {"type", "snapshot"}});
    set_version_settings(game, "CustomIcon", Json{{"icon", "egg"}});
    require(instance_detail(game, "CustomIcon").icon == "egg", "Explicit local icon no longer overrides automatic choice");
}
void identity() {
    Fixture f;
    const auto game = f.game();
    for (const auto* invalid : {"", "../escape", "a..b", "a/b", "a\\b", "a:b", "nul", "COM1.txt", "nickel.profile", "gameData", "a.", " a"}) rejected([&] { validate_name(invalid); }, "invalid_name");
    validate_name("My world");
    validate_name("3D Shareware v1.34");
    const auto order = ordered_roots({"B:/new", "C:/default", "D:/old", "B:/new"}, {"D:/old", "C:/default"}, "C:/default");
    require(order == std::vector<std::string>({"C:/default", "D:/old", "B:/new"}), "Root addition order changed");
    auto registry = registry_from_text(R"({"roots":["C:/test"],"active":"C:/test","labels":{"C:/test":"Test"},"future":{"x":1}})");
    require(registry_to_json(registry)["future"]["x"] == 1, "Unknown registry key lost");
    require(registry_from_text(R"({"roots":"broken"})").roots.empty(), "Broken schema accepted");
    descriptor(game, "1.20.1", {{"name", "Survival"}, {"favorite", true}, {"favoriteAt", 200}, {"unknownCustom", 7}});
    descriptor(game, "1.19.4", {{"favorite", true}, {"favoriteAt", 100}});
    descriptor(game, "1.21.4", Json::object(), "2026-01-01T00:00:00Z");
    rename_display(game, "1.20.1", "Renamed", "Description", "Category", true, 300);
    auto entries = scan_instances(game);
    require(entries.size() == 3 && entries[0].id == "1.19.4" && entries[1].id == "1.20.1", "Favorite ordering changed");
    const auto text = detail::read_bounded(game / "versions/1.20.1/1.20.1.json", descriptor_max_bytes);
    const auto value = parse_legacy_json(text);
    require(value["id"] == "1.20.1" && value["nickelCustom"]["name"] == "Renamed", "Display rename changed identity");
    require(value["future"]["nested"] == true && value["nickelCustom"]["unknownCustom"] == 7, "Unknown descriptor fields lost");
    require(value["nickelCustom"]["favoriteAt"] == 200, "Favorite timestamp reordered");
    InstanceRegistry store(f.config(), game);
    store.ensure_default();
    auto second = store.create_root(f.root / "second");
    store.rename_root(second, "Second");
    require(store.get().roots.size() == 2 && store.get().labels[path_to_utf8(second)] == "Second", "Whole root registry failed");
    rejected([&] { store.remove_root(game, true, std::array<fs::path, 1>{game / "protected"}); }, "protected_path");
#ifdef _WIN32
    const auto case_variant = game.parent_path() / ".MINECRAFT" / "missing-protected";
    rejected([&] { store.remove_root(game, true, std::array<fs::path, 1>{case_variant}); }, "protected_path");
    std::wstring short_text(32768, L'\0');
    const auto size = GetShortPathNameW(detail::native_path(game).c_str(), short_text.data(), static_cast<DWORD>(short_text.size()));
    if (size == 0 || size >= short_text.size()) throw std::runtime_error("Cannot inspect protected path alias");
    short_text.resize(size);
    const auto short_game = detail::logical_path(fs::path(short_text));
    require(root_identity(path_to_utf8(detail::canonical_path(short_game))) == root_identity(path_to_utf8(detail::canonical_path(game))),
        "Pinned canonical path lost Windows short-name identity");
    rejected([&] { store.remove_root(game, true, std::array<fs::path, 1>{short_game / "missing-protected"}); }, "protected_path");
    std::cout << (root_identity(path_to_utf8(short_game)) == root_identity(path_to_utf8(game))
        ? "NTFS 8.3 alias unavailable; equivalent canonical-path containment checked\n"
        : "NTFS 8.3 protected path alias checked\n");
#endif
    require(store.get().roots.size() == 2 && detail::path_exists(game), "Protected-path rejection changed registered roots");
    store.remove_root(second, true, {});
    require(!fs::exists(second) && store.get().roots.size() == 1, "Whole root removal failed");
    delete_instance(game, "1.19.4");
    require(fs::exists(game / "versions/1.20.1"), "Delete crossed identity boundary");
    require(parse_legacy_json("\xef\xbb\xbf{\"url\":\"https://example.test/a,\",/* comment */\"x\":1,}")["url"] == "https://example.test/a,", "Lenient parser damaged string");
    rejected([&] { parse_legacy_json(R"({"x":1,"x":2})"); }, "invalid_json");
    icon_rules();
    std::cout << "identity/order/favorites/root deletion/unknown fields/legacy BOM comments/icon and loader priority: PASS\n";
}
void metadata() {
    Fixture fixture;
    const auto root = fixture.game();
    descriptor(root, "1.20.1", {{"name", "Before"}, {"description", "Keep description"}, {"category", "Keep category"},
        {"favorite", true}, {"favoriteAt", 200}, {"futureCustom", Json{{"nested", true}}}});
    const auto directory = root / "versions/1.20.1";
    const auto file = directory / "1.20.1.json";
    const auto read = [&] { return parse_legacy_json(detail::read_bounded(file, descriptor_max_bytes)); };
    patch_instance_metadata(root, "1.20.1", Json{{"name", "After"}});
    auto value = read();
    require(value["id"] == "1.20.1" && value["nickelCustom"]["name"] == "After"
        && value["nickelCustom"]["description"] == "Keep description" && value["nickelCustom"]["category"] == "Keep category"
        && value["nickelCustom"]["futureCustom"]["nested"] == true && value["future"]["nested"] == true,
        "Name-only patch overwrote omitted display or unknown metadata");
    patch_instance_metadata(root, "1.20.1", Json{{"favorite", true}, {"favoriteAt", 900}});
    require(read()["nickelCustom"]["favoriteAt"] == 200, "Favorite retry reordered an existing timestamp");
    patch_instance_metadata(root, "1.20.1", Json{{"favorite", false}});
    require(read()["nickelCustom"]["favoriteAt"].is_null(), "Favorite disable left an enabled timestamp");
    patch_instance_metadata(root, "1.20.1", Json{{"favorite", true}, {"favoriteAt", 300}});
    require(read()["nickelCustom"]["favoriteAt"] == 300, "New favorite did not record its requested safe timestamp");
    patch_instance_metadata(root, "1.20.1", Json{{"favorite", false}});
    patch_instance_metadata(root, "1.20.1", Json{{"favorite", true}});
    require(detail::unsigned_value(read()["nickelCustom"]["favoriteAt"]).has_value(), "New favorite did not record a system timestamp");
    const Json settings{{"windowTitle", "Keep title"}, {"memoryMode", "custom"}, {"memoryMb", 3072},
        {"disableWrapper", true}, {"futureSettings", Json{{"nested", 42}}}, {"icon", "grass"}};
    write(directory / settings_file, settings.dump());
    const auto descriptor_before_icon = detail::read_bounded(file, descriptor_max_bytes);
    for (const auto icon : {"grass", "command_block", "cobblestone", "anvil", "neoforge", "fabric", "grass_path", "egg",
                           "gold_block", "redstone_block", "redstone_lamp_on", "redstone_lamp_off"}) {
        patch_instance_metadata(root, "1.20.1", Json{{"icon", icon}});
        auto expected = settings; expected["icon"] = icon;
        require(read_local_settings(root, "1.20.1").value == expected, "Icon patch replaced another known or unknown setting");
        require(instance_detail(root, "1.20.1").icon == icon, "Built-in icon patch did not reach the instance projection");
    }
    require(detail::read_bounded(file, descriptor_max_bytes) == descriptor_before_icon, "Icon-only patch rewrote the version descriptor");
    for (const Json icon : {Json(nullptr), Json("auto"), Json("")}) {
        patch_instance_metadata(root, "1.20.1", Json{{"icon", icon}});
        require(read_local_settings(root, "1.20.1").value["icon"].is_null(), "Automatic icon did not clear the local selection");
    }
    const auto before = detail::read_bounded(file, descriptor_max_bytes);
    const auto settings_before = detail::read_bounded(directory / settings_file, settings_max_bytes);
    for (const auto icon : {"../outside", "C:/private.png", "https://example.test/icon.png", ":/arbitrary/icon.png", "quilt", "custom"})
        rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"icon", icon}}); }, "invalid_icon");
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"icon", 1}}); }, "invalid_schema");
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"icon", "egg"}, {"name", "Mixed"}}); }, "invalid_schema");
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"id", "Renamed"}}); }, "invalid_schema");
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"favorite", 1}}); }, "invalid_schema");
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"favorite", true}, {"favoriteAt", true}}); }, "invalid_schema");
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"favoriteAt", 1}}); }, "invalid_schema");
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"favorite", true}, {"favoriteAt", 9007199254740992ULL}}); }, "invalid_schema");
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"name", " "}}); }, "invalid_name");
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"description", std::string(16 * 1024 + 1, 'x')}}); }, "limit");
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json::array()); }, "invalid_schema");
    std::stop_source cancelled; cancelled.request_stop();
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"name", "Cancelled"}}, cancelled.get_token()); }, "cancelled");
    require(detail::read_bounded(file, descriptor_max_bytes) == before
        && detail::read_bounded(directory / settings_file, settings_max_bytes) == settings_before, "Rejected metadata patch changed persisted data");
    auto pending = InstanceTransaction::reserve(root, "PendingMetadata", "1.20.1");
    rejected([&] { patch_instance_metadata(root, "PendingMetadata", Json{{"icon", "egg"}}); }, "pending_instance");
    require(!fs::exists(pending.directory() / settings_file), "Pending metadata was published");
    write(directory / settings_file, "{broken settings");
    rejected([&] { patch_instance_metadata(root, "1.20.1", Json{{"icon", "egg"}}); }, "invalid_json");
    require(detail::read_bounded(directory / settings_file, settings_max_bytes) == "{broken settings", "Icon patch replaced corrupt settings silently");
    std::cout << "metadata patch/omitted fields/unknown preservation/built-in icon/favorite timestamp/pending/cancellation: PASS\n";
}
void transaction() {
    Fixture f;
    auto game = f.game();
    auto first = InstanceTransaction::reserve(game, "Pack", "1.20.1");
    require(is_pending(first.directory()) && scan_instances(game).empty(), "Pending target became visible");
    rejected([&] { load_profile(game, "Pack"); }, "pending_instance");
    rejected([&] { read_local_settings(game, "Pack"); }, "pending_instance");
    rejected([&] { resource_directory(game, "Pack", Json::object()); }, "pending_instance");
    require(reserved_resource_directory(first, Json::object()) == first.directory(), "Pending default isolation changed");
    require(reserved_resource_directory(first, Json{{"versionIsolation", false}}) == first.root(), "Pending shared isolation changed");
    write(first.directory() / settings_file, Json{{"versionIsolation", true}, {"windowTitle", "Owned"}}.dump());
    require(reserved_resource_directory(first, Json{{"versionIsolation", false}}) == first.directory(), "Pending local settings did not override global isolation");
    require(read_reserved_local_settings(first).value["windowTitle"] == "Owned", "Pending helper did not read its owned local settings");
    std::stop_source cancelled_read; cancelled_read.request_stop();
    rejected([&] { reserved_resource_directory(first, Json::object(), cancelled_read.get_token()); }, "cancelled");
    rejected([&] { InstanceTransaction::reserve(game, "pack", "1.20.1"); }, "conflict");
    auto second = InstanceTransaction::reserve(game, "Pack", "1.20.1", {}, "", true);
    require(second.profile().name != "Pack", "Suffix conflicted");
    second.cleanup();
    rejected([&] { first.complete(installed("1.21.4")); }, "identity_mismatch");
    first.complete(installed());
    first.commit("Display Pack", Json{{"format", "mrpack"}});
    require(scan_instances(game).size() == 1 && load_profile(game, "Pack")->ready, "Commit not published");
    auto interrupted = InstanceTransaction::reserve(game, "Interrupted", "1.20.1");
    interrupted.leave_for_recovery();
    auto recovered = recover_interrupted_instances(game);
    require(recovered.removed == std::vector<std::string>{"Interrupted"} && !fs::exists(game / "versions/Interrupted"), "Interrupted target was not recovered");
    auto stolen = InstanceTransaction::reserve(game, "Stolen", "1.20.1");
    write(stolen.directory() / pending_file, "00000000000000000000000000000000");
    rejected([&] { reserved_resource_directory(stolen, Json::object()); }, "ownership_changed");
    rejected([&] { stolen.cleanup(); }, "ownership_changed");
    require(fs::is_directory(stolen.directory()), "Ownership conflict deleted target");
    auto cancelled = InstanceTransaction::reserve(game, "Cancelled", "1.20.1");
    cancelled.complete(installed());
    std::stop_source stop;
    stop.request_stop();
    rejected([&] { cancelled.commit("Cancelled", Json::object(), stop.get_token()); }, "cancelled");
    require(is_pending(cancelled.directory()), "Cancelled target published");
    cancelled.cleanup();
    fs::path retained;
    {
        auto undrained = InstanceTransaction::reserve(game, "UndrainedOwner", "1.20.1");
        retained = undrained.directory();
        const auto owner = detail::read_bounded(retained / pending_file, 64);
        write(retained / pending_file, "00000000000000000000000000000000");
        rejected([&] { undrained.leave_for_recovery(false); }, "ownership_changed");
        write(retained / pending_file, owner);
    }
    require(is_pending(retained), "Failed ownership check let an undrained transaction clean up in its destructor");
    const auto running = recover_interrupted_instances(game);
    require(std::find(running.skipped.begin(), running.skipped.end(), "UndrainedOwner") != running.skipped.end(),
        "Recovery ignored the retained live writer PID");
#ifdef _WIN32
    {
        auto undrained = InstanceTransaction::reserve(game, "UndrainedJournal", "1.20.1");
        retained = undrained.directory();
        const auto owner = detail::read_bounded(retained / pending_file, 64);
        const auto journal = game / ".nickel-pack-transactions" / (owner + ".json");
        {
            struct Lease {
                HANDLE handle;
                ~Lease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
            } lease{CreateFileW(journal.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            require(lease.handle != INVALID_HANDLE_VALUE, "Cannot pin the synthetic recovery journal");
            rejected([&] { undrained.leave_for_recovery(false); }, "io");
        }
        require(detail::read_optional_json(journal, 64 * 1024).value()["pid"] == detail::current_pid(),
            "Failed undrained recovery write changed the owning PID");
    }
    require(is_pending(retained), "Failed journal write let an undrained transaction delete a live writer target");
#endif
    std::cout << "pending/no-case-overwrite/identity mismatch/atomic commit/recovery/token ownership/cancel/undrained failure retention: PASS\n";
}
void settings() {
    Fixture f;
    const auto game = f.game();
    descriptor(game, "1.20.1");
    const auto directory = game / "versions/1.20.1";
    write(directory / "PCL/Setup.ini", "VersionArgumentTitle:Legacy\r\nVersionRamType:1\nVersionRamCustom:15\nVersionAdvanceGC:5\nVersionServerAuthServer:https://auth.example.test/path\nFutureLegacy:keep\n");
    write(game / "PCL/Setup.ini", "LaunchAdvanceJvm:-Droot=yes\n");
    const auto legacy = read_local_settings(game, "1.20.1");
    require(legacy.source == "pcl" && legacy.value["memoryMb"] == 3072 && legacy.value["jvmArgs"] == "-Droot=yes", "Legacy setup mapping incorrect");
    require(legacy.value["authServer"] == "https://auth.example.test/path" && legacy.unmapped_legacy_keys == std::vector<std::string>{"FutureLegacy"}, "Legacy colon or unknown mapping incorrect");
    set_version_settings(game, "1.20.1", Json{{"windowTitle", "Local"}, {"future", Json{{"nested", 42}}}, {"settingsSource", "pcl"}, {"memoryMode", "custom"}});
    require(read_local_settings(game, "1.20.1").source == "nickel", "Local source not authoritative");
    auto local = get_version_settings(game, "1.20.1");
    require(local["memoryMode"] == "global", "Invalid custom memory not inherited");
    set_version_settings(game, "1.20.1", Json{{"windowTitle", "Again"}});
    require(get_version_settings(game, "1.20.1")["future"]["nested"] == 42, "Unknown setting erased on save");
    auto effective = resolve_settings(Json{{"memoryMode", "custom"}, {"memoryMb", 2048}, {"versionIsolation", false}}, Json{{"memoryMode", "global"}, {"windowWidth", 100}}, MemoryContext{8192, 4096});
    require(effective.memory_mb == 2048 && !effective.isolation && effective.width == 320, "Effective inheritance failed");
    rejected([&] { resolve_settings(Json{{"memoryMode", "custom"}, {"memoryMb", 16384}}, Json::object(), MemoryContext{8192, 4096}); }, "invalid_memory");
    require(split_arguments(R"(-Dpath=C:\Java "two words" '')") == std::vector<std::string>({R"(-Dpath=C:\Java)", "two words", ""}), "Argument parser modified Windows paths");
    rejected([&] { split_arguments("\"unclosed"); }, "invalid_arguments");
    auto projected = project_portable_settings(Json{{"windowTitle", "Good"}, {"jvmArgs", "-Dpath=C:/users/test"}, {"javaPath", "C:/Java"}, {"authServer", "https://auth.example.test"}, {"gameArgs", "--accessToken synthetic-secret"}});
    require(projected.settings.contains("windowTitle") && projected.settings.contains("authServer") && !projected.settings.contains("jvmArgs") && !projected.settings.contains("javaPath") && !projected.settings.contains("gameArgs"), "Portable projection leaked host data");
    rejected([&] { validate_portable_settings(Json{{"javaPath", "C:/Java"}}); }, "invalid_schema");
    auto merged = merge_portable_settings(Json{{"future", Json{{"keep", true}}}}, Json{{"windowTitle", "Imported"}});
    require(merged["future"]["keep"] == true, "Portable merge erased unknown fields");
    require(mods_directory(directory, "LiteLoader", "1.12.2") == directory / "mods/1.12.2", "LiteLoader root differs");
    auto args = gc_arguments("auto-zgc", 21, 17763);
    require(std::find(args.begin(), args.end(), "-XX:+UseZGC") != args.end(), "ZGC selection failed");
    std::cout << "legacy schema/local precedence/unknown fields/effective memory/argument parser/safe projection/LiteLoader/GC: PASS\n";
}
void startup_layouts(const fs::path& game, const OnboardingGate& gate) {
    for (const bool native_layout : {false, true}) {
        const std::string id = native_layout ? "NativeLayout" : "LegacyLayout";
        auto transaction = InstanceTransaction::reserve(game, id, "1.20.1");
        const auto outer = transaction.directory();
        const auto install_root = native_layout ? outer / "instances" / id : outer;
        descriptor(install_root, "1.20.1");
        const auto exact = install_root / "versions/1.20.1/1.20.1.json";
        const auto raw = detail::read_bounded(exact, descriptor_max_bytes);
        transaction.complete(installed());
        transaction.commit(id, Json::object());
        auto result = resolve_launch_target(game, id, Json::object());
        const auto startup_result = resolve_startup_launch({game, id}, Json::object(), gate);
        require(startup_result.descriptor_path == result.descriptor_path && startup_result.install_root == result.install_root,
            "CLI wrapper diverged from the canonical launch target");
        require(result.entry.id == id && result.entry.named_profile && result.entry.ready, "Canonical target lost its outer entry");
        require(result.install_root == install_root && result.descriptor_path == exact
            && result.installed_version_id == "1.20.1" && result.minecraft_version == "1.20.1"
            && result.descriptor["id"] == "1.20.1" && result.request.instance_id == id,
            "Named layout mixed the outer identity and exact inner Minecraft version");
        require(result.resource_root == outer && result.mods_root == outer / "mods" && result.settings.isolation,
            "Native install root changed effective resource isolation");
        result = resolve_startup_launch({game, id}, Json{{"versionIsolation", false}}, gate);
        require(result.resource_root == game && !result.settings.isolation && result.install_root == install_root,
            "Shared resource isolation changed the selected installation layout");
        set_version_settings(game, id, Json{{"versionIsolation", true}});
        require(resolve_startup_launch({game, id}, Json{{"versionIsolation", false}}, gate).resource_root == outer,
            "Local resource isolation did not override global settings");
        auto changed = parse_legacy_json(raw); changed["id"] = "1.21.4";
        write(exact, changed.dump());
        rejected([&] { resolve_startup_launch({game, id}, Json::object(), gate); }, "identity_mismatch");
        write(exact, raw);
        const auto opposite = native_layout ? outer : outer / "instances" / id;
        descriptor(opposite, "1.20.1");
        rejected([&] { resolve_startup_launch({game, id}, Json::object(), gate); }, "conflict");
        require(detail::read_bounded(exact, descriptor_max_bytes) == raw, "Layout resolution moved or rewrote existing data");
    }
    auto incomplete = InstanceTransaction::reserve(game, "NoInnerDescriptor", "1.20.1");
    incomplete.complete(installed()); incomplete.commit("No inner descriptor", Json::object());
    rejected([&] { resolve_startup_launch({game, "NoInnerDescriptor"}, Json::object(), gate); }, "incomplete_instance");
    descriptor(game, "WrongDirectory");
    const auto wrong = game / "versions/WrongDirectory/WrongDirectory.json";
    auto mismatch = parse_legacy_json(detail::read_bounded(wrong, descriptor_max_bytes));
    mismatch["id"] = "1.20.1"; write(wrong, mismatch.dump());
    rejected([&] { resolve_startup_launch({game, "WrongDirectory"}, Json::object(), gate); }, "identity_mismatch");
}
void startup() {
    Fixture f;
    auto game = f.game();
    descriptor(game, "1.20.1");
    OnboardingGate gate(f.config(), "0.9.0-test");
    require(gate.status().needs_eula && !fs::exists(f.config() / "onboarding.json"), "First run invented consent");
    const std::vector<std::string> args{"--launch-instance", "1.20.1", "--game-root", path_to_utf8(game)};
    auto parsed = parse_startup_arguments(args);
    require(parsed && parsed->instance_id == "1.20.1", "CLI parse failed");
    const auto target = resolve_launch_target(game, "1.20.1", Json::object());
    require(target.entry.id == "1.20.1" && !target.profile && target.install_root == game,
        "Data-only canonical resolution performed onboarding or changed the ordinary layout");
    rejected([&] { resolve_startup_launch(*parsed, Json::object(), gate); }, "eula_required");
    rejected([&] { gate.accept_eula(false, eula_version, 123); }, "eula_required");
    rejected([&] { gate.accept_eula(true, "old", 123); }, "eula_required");
    rejected([&] { gate.accept_eula(true, eula_version, 0); }, "invalid_timestamp");
    gate.accept_eula(true, eula_version, 123);
    gate.accept_eula(true, eula_version, 456);
    require(gate.status().accepted_at == 123, "Retried signature changed timestamp");
    rejected([&] { resolve_startup_launch(*parsed, Json::object(), gate); }, "changelog_required");
    gate.acknowledge_changelog("0.9.0-test");
    int dispatched = 0;
    require(gate.finish([&] {
        ++dispatched;
        const auto launch = resolve_startup_launch(*parsed, Json::object(), gate);
        require(launch.minecraft_version == "1.20.1" && launch.installed_version_id == "1.20.1"
            && launch.install_root == game && launch.descriptor_path == game / "versions/1.20.1/1.20.1.json",
            "CLI resolution mixed Minecraft metadata and directory identity");
    }), "First dispatch missing");
    require(!gate.finish([&] { ++dispatched; }) && dispatched == 1, "Startup dispatched twice");
    startup_layouts(game, gate);
    rejected([&] { parse_startup_arguments(std::vector<std::string>{"--launch-instance", "1.20.1", "--launch-instance", "1.20.1", "--game-root", path_to_utf8(game)}); }, "invalid_cli");
    rejected([&] { parse_startup_arguments(std::vector<std::string>{"--launch-instance", "../x", "--game-root", path_to_utf8(game)}); }, "invalid_name");
    rejected([&] { parse_startup_arguments(std::vector<std::string>{"--launch-instance", "1.20.1", "--game-root", "."}); }, "invalid_cli");
    auto script = launch_script(f.root / "Nickel.exe", *parsed, true);
    require(script.find("--launch-instance") != std::string::npos && script.find("DisableDelayedExpansion") != std::string::npos && script.find("accessToken") == std::string::npos, "Script contains launch secrets or lacks safe quoting");
    auto pending = InstanceTransaction::reserve(game, "Pending", "1.20.1");
    rejected([&] { resolve_startup_launch({game, "Pending"}, Json::object(), gate); }, "pending_instance");
    rejected([&] { resolve_launch_target(game, "Pending", Json::object()); }, "pending_instance");
    gate.reject();
    rejected([&] { gate.require_accepted(); }, "exiting");
    write(f.config() / "onboarding.json", R"({"eulaVersion":"minecraft-eula-v1","eulaAcceptedAt":true,"changelogSeenVersion":"0.9.0-test"})");
    OnboardingGate corrupt(f.config(), "0.9.0-test");
    require(corrupt.status().needs_eula, "Malformed consent bypassed gate");
    std::cout << "first-run/EULA/CLI one-shot/legacy and native exact layout/conflict/resource isolation/pending: PASS\n";
}
void long_paths() {
    Fixture fixture;
    auto base = fixture.root;
    for (unsigned index = 0; base.native().size() < 360; ++index)
        base /= "deep-" + std::to_string(index) + '-' + std::string(48, 'x');
    const auto game = base / ".minecraft";
    InstanceRegistry registry(base / "Nickel", game);
    require(registry.ensure_default() == game, "Deep registered root identity changed");
    const auto other = registry.create_root(base / "second");
    require(detail::directory_entries(other).empty(), "Deep empty directory enumerated unrelated CWD content");
    registry.rename_root(other, "Deep second root");
    require(registry.get().roots.size() == 2, "Deep registry did not persist both roots");
    registry.remove_root(other, true, {});
    require(!detail::path_exists(other), "Deep whole-root removal failed");
    descriptor(game, "1.20.1");
    const auto ordinary = game / "versions/1.20.1";
    write(ordinary / "PCL/Setup.ini", "VersionArgumentTitle:Deep legacy title\n");
    require(read_local_settings(game, "1.20.1").value["windowTitle"] == "Deep legacy title",
        "Deep legacy settings were mistaken for a missing file");
    set_version_settings(game, "1.20.1", Json{{"versionIsolation", false}, {"future", Json{{"keep", true}}}});
    patch_instance_metadata(game, "1.20.1", Json{{"name", "Deep display"}});
    patch_instance_metadata(game, "1.20.1", Json{{"icon", "egg"}});
    write(ordinary / "mods/one.jar", "synthetic mod");
    require(count_mods(ordinary / "mods") == 1, "Deep mod enumeration failed");
    const auto target = resolve_launch_target(game, "1.20.1", Json::object());
    require(target.resource_root == game && target.install_root == game && target.entry.icon == "egg"
        && target.descriptor_path == ordinary / "1.20.1.json", "Deep ordinary target changed logical paths");
    OnboardingGate gate(base / "Nickel", "0.9.0-test");
    gate.accept_eula(true, eula_version, 123);
    gate.acknowledge_changelog("0.9.0-test");
    require(gate.finish([] {}), "Deep onboarding did not complete");
    startup_layouts(game, gate);
    auto transaction = InstanceTransaction::reserve(game, "DeepOwned", "1.20.1");
    transaction.check_owned();
    const auto owner = detail::read_bounded(transaction.directory() / pending_file, 64);
    const auto journal = transaction.root() / ".nickel-pack-transactions" / (owner + ".json");
    require(journal.native().size() > 400, "Deep journal fixture no longer exceeds traditional Windows limits");
    const auto disk = detail::read_optional_json(journal, 64 * 1024);
    require(disk && (*disk)["root"] == path_to_utf8(game), "Deep journal exposed a native prefix or lost ownership");
    require(reserved_resource_directory(transaction, Json{{"versionIsolation", false}}) == game,
        "Deep reservation helper lost its shared resource root");
    apply_portable_settings(transaction.directory(), Json{{"versionIsolation", true}, {"windowTitle", "Deep owned title"}});
    require(read_reserved_local_settings(transaction).value["windowTitle"] == "Deep owned title"
        && reserved_resource_directory(transaction, Json::object()) == transaction.directory(),
        "Deep reservation settings were mistaken for missing data");
    rejected([&] { read_local_settings(game, "DeepOwned"); }, "pending_instance");
    transaction.cleanup();
    require(!detail::path_exists(journal) && !detail::path_exists(game / "versions/DeepOwned"),
        "Deep transaction cleanup did not remove its own marker and journal");
    {
        auto interrupted = InstanceTransaction::reserve(game, "DeepInterrupted", "1.20.1");
        interrupted.leave_for_recovery();
    }
    const auto recovered = recover_interrupted_instances(game);
    require(recovered.errors.empty() && recovered.removed == std::vector<std::string>{"DeepInterrupted"},
        "Deep recovery misclassified the original journal");
    const auto entries = scan_instances(game);
    require(entries.size() == 5, "Deep public enumeration lost completed instances or exposed pending data");
    for (const auto& entry : entries)
        require(!path_to_utf8(entry.directory).starts_with("\\\\?\\"), "Native Windows prefix escaped into an instance DTO");
    delete_instance(game, "1.20.1");
    require(!detail::path_exists(ordinary), "Deep instance retirement/removal failed");
    std::stop_source stopped; stopped.request_stop();
    rejected([&] { detail::directory_entries(game, true, stopped.get_token()); }, "cancelled");
    std::cout << "deep paths/exact native enumeration/owned journal/settings/legacy and native resolve/registry/recovery/logical DTO paths: PASS\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        const std::string group = argc > 1 ? argv[1] : "all";
        if (group == "identity" || group == "all") identity();
        if (group == "metadata" || group == "all") metadata();
        if (group == "transaction" || group == "all") transaction();
        if (group == "settings" || group == "all") settings();
        if (group == "startup" || group == "all") startup();
        if (group == "longpath" || group == "all") long_paths();
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
