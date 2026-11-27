#include "live_environment.hpp"
#include "test_support.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace test;
namespace live = test::live;
namespace {

void provision(const fs::path& root) {
    for (const auto& name : {"managed", "games", "evidence", "temp", "home"})
        fs::create_directories(filesystem_path(root / name));
#ifdef _WIN32
    const auto path = filesystem_path(root / ".nickel-minecraft-live-cache");
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Fixture marker must be created once");
    DWORD bytes = 0;
    const bool written = ::WriteFile(handle, live::markerBytes.data(), static_cast<DWORD>(live::markerBytes.size()), &bytes, nullptr);
    CloseHandle(handle);
    require(written && bytes == live::markerBytes.size(), "Fixture marker bytes must match");
#endif
}

std::string property(const ProcessSpec& spec, std::string_view prefix) {
    const auto found = std::find_if(spec.arguments.begin(), spec.arguments.end(), [&](const auto& value) { return value.starts_with(prefix); });
    require(found != spec.arguments.end(), "Required explicit Java property is missing");
    require(std::count_if(spec.arguments.begin(), spec.arguments.end(), [&](const auto& value) { return value.starts_with(prefix); }) == 1,
            "Java property must occur exactly once");
    return found->substr(prefix.size());
}

void gates() {
    require(live::capacity_sufficient(live::cMinimumBytes, live::dMinimumBytes), "Exact independent capacity limits should pass");
    require(!live::capacity_sufficient(live::cMinimumBytes - 1, 64ULL * live::dMinimumBytes), "Large D cannot compensate for insufficient C");
    require(!live::capacity_sufficient(64ULL * live::cMinimumBytes, live::dMinimumBytes - 1), "Large C cannot compensate for insufficient D");
    require(!live::capacity_sufficient(live::cMinimumBytes - 1, live::runtimeMinimumBytes, live::runtimeMinimumBytes), "Runtime gate must still protect C");
    require(live::capacity_sufficient(live::cMinimumBytes, live::progressMinimumBytes, live::progressMinimumBytes), "Internal progress floor remains independent");
}

void child_contract() {
    Tree tree("live-environment"); provision(tree.root);
    auto environment = std::make_shared<live::Environment>(tree.root);
    const auto layout = environment->layout();
    require(layout.size() == 6, "All six owned write directories need physical-volume evidence");
    for (const auto& entry : layout.items()) require(entry.value()["volumeGuid"] == layout["cache"]["volumeGuid"], "All child paths must be the same physical D volume");
    ProcessSpec spec; spec.executable = fs::path("C:/read-only-java/bin/java.exe");
    spec.workingDirectory = spec.executable.parent_path(); spec.arguments = {"-XshowSettings:properties", "-version"};
    const auto probe = environment->constrain(spec);
    require(probe.workingDirectory == environment->home(), "Read-only C probe directory must not become cwd");
    require(property(probe, "-Djava.io.tmpdir=") == path_utf8(environment->temp()), "Explicit Java temp must be the D fixture temp");
    require(property(probe, "-Duser.home=") == path_utf8(environment->home()), "Explicit Java home must be the D fixture home");
    for (const auto& name : {"TEMP", "TMP", "TMPDIR", "HOME", "USERPROFILE", "APPDATA", "LOCALAPPDATA"})
        require(probe.environment.contains(name), "Every child needs explicit directory environment");
    for (const auto& name : {"JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS"})
        require(std::find(probe.unsetEnvironment.begin(), probe.unsetEnvironment.end(), name) != probe.unsetEnvironment.end(), "Ambient Java write options must not override the fixture");
    const auto optifineHome = tree.root / "managed/loader/optifine/installer-tmp/owned";
    spec.arguments = {"-Djava.awt.headless=true", "-Duser.home=" + path_utf8(optifineHome), "-cp", "installer.jar", "optifine.Installer"};
    spec.workingDirectory = optifineHome;
    spec.environment = {{"HOME", path_utf8(optifineHome)}, {"APPDATA", path_utf8(optifineHome)}};
    const auto optifine = environment->constrain(spec);
    require(property(optifine, "-Duser.home=") == path_utf8(optifineHome), "OptiFine's operation home must be preserved");
    require(optifine.environment.at("HOME") == path_utf8(optifineHome), "OptiFine home environment must agree with its Java property");
    require(std::count(optifine.arguments.begin(), optifine.arguments.end(), "-Djava.awt.headless=true") == 1, "OptiFine must retain one headless option");
    const auto again = environment->constrain(optifine);
    require(again.arguments == optifine.arguments && again.environment == optifine.environment, "Child constraint must be idempotent");
    ProcessSpec game = optifine; game.purpose = ProcessKind::Game;
    game.arguments = {"-cp", "client.jar", "net.minecraft.client.main.Main", "-Duser.home=C:/program-argument"};
    game.environment.clear(); game.workingDirectory = tree.root / "games/test";
    const auto launched = environment->constrain(game);
    require(launched.arguments.back() == game.arguments.back(), "Program arguments after main class must not be rewritten");
    auto invalid = game; invalid.workingDirectory = fs::path("C:/");
    rejects([&] { environment->constrain(invalid); }, "minecraft.live.environment");
    invalid = game; invalid.environment["temp"] = "C:/Windows/Temp";
    rejects([&] { environment->constrain(invalid); }, "minecraft.live.environment");
    invalid = game; invalid.arguments.insert(invalid.arguments.begin(), "-Duser.home=C:/");
    rejects([&] { environment->constrain(invalid); }, "minecraft.live.environment");
    invalid = game; invalid.arguments.insert(invalid.arguments.begin(), "-XX:ErrorFile=C:/error.log");
    rejects([&] { environment->constrain(invalid); }, "minecraft.live.environment");
    invalid = game; invalid.arguments.insert(invalid.arguments.begin(), {"-Djava.io.tmpdir=" + path_utf8(environment->temp()), "-Djava.io.tmpdir=" + path_utf8(environment->temp())});
    rejects([&] { environment->constrain(invalid); }, "minecraft.live.environment");
    unsigned spawned = 0, recorded = 0;
    Services raw; raw.spawn = [&](const ProcessSpec& child, const OutputCallback&) {
        ++spawned; require(child.environment.contains("TMPDIR"), "Bound services must retain the child-wide constraint");
        return std::make_shared<Child>();
    };
    raw.forOperation = [raw](const Cancellation&) { return raw; };
    auto bound = live::constrain_services(raw, environment, [&](const ProcessSpec&) { ++recorded; });
    const auto child = bound.forOperation(Cancellation{}).spawn(game, {});
    require(child && spawned == 1 && recorded == 1, "Operation decoration must happen before one owned spawn");
    auto failed = live::constrain_services(raw, environment, [](const ProcessSpec&) { throw std::bad_alloc(); });
    rejects([&] { failed.spawn(game, {}); });
    require(spawned == 1, "Recorder allocation failure cannot create a Game");
    rejects([&] { bound.discoverJava([] {}); }, "minecraft.live.environment");
}

void ownership() {
    Tree tree("live-invalid-marker"); provision(tree.root);
    put(tree.root / ".nickel-minecraft-live-cache", "not the owned cache schema\n");
    rejects([&] { live::Environment environment(tree.root); }, "minecraft.live.environment");
}

}

int main() {
    try {
        gates();
#ifdef _WIN32
        child_contract(); ownership();
#endif
        std::cout << "live environment capacity and child contract PASS (controlled; not genuine Java acceptance)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
