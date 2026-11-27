#include "launcher.hpp"
#include "maven.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

using nickel::Json;
using namespace nickel::minecraft;

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

Json vanilla() {
    return parse_metadata(R"({"id":"1.20.1","mainClass":"net.minecraft.client.main.Main","type":"release","assets":"5","javaVersion":{"component":"java-runtime-gamma","majorVersion":17},"downloads":{"client":{"url":"http://127.0.0.1/client.jar","sha1":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size":1}},"assetIndex":{"id":"5","url":"http://127.0.0.1/5.json","sha1":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","size":1},"arguments":{"jvm":["-Djava.library.path=${natives_directory}/java","-cp","${classpath}",{"rules":[{"action":"allow","os":{"name":"windows","arch":"amd64"}}],"value":"-Dos.correct=yes"}],"game":["--username","${auth_player_name}","--version","${version_name}","--accessToken","${auth_access_token}","--gameDir","${game_directory}","--assetsDir","${assets_root}","--assetIndex","${assets_index_name}","--uuid","${auth_uuid}","--userType","${user_type}",{"rules":[{"action":"allow","features":{"has_custom_resolution":true}}],"value":["--width","${resolution_width}","--height","${resolution_height}"]}]},"libraries":[{"name":"org.test:regular:1.0","url":"http://127.0.0.1/maven/"},{"name":"org.lwjgl:lwjgl:3.4.1","url":"http://127.0.0.1/maven/"},{"name":"org.lwjgl:lwjgl:3.4.1:natives-windows","rules":[{"action":"allow","os":{"name":"windows"}}],"url":"http://127.0.0.1/maven/"},{"name":"org.lwjgl:lwjgl:3.4.1:natives-windows-x86","rules":[{"action":"allow","os":{"name":"windows"}}],"url":"http://127.0.0.1/maven/"},{"name":"org.lwjgl:lwjgl:3.4.1:natives-windows-arm64","rules":[{"action":"allow","os":{"name":"windows"}}],"url":"http://127.0.0.1/maven/"}]} )");
}

LaunchRequest request(Json profile) {
    LaunchRequest result;
    result.launchId = "test-launch-123";
    result.version.root = std::filesystem::absolute("native/tests/minecraft/synthetic/planning/root");
    result.version.gameDirectory = result.version.root / "versions" / "pack";
    result.version.versionId = profile.value("id", "1.20.1");
    result.version.minecraftVersion = "1.20.1";
    result.version.profile = std::move(profile);
    result.identity.name = "Synthetic Player";
    result.identity.uuid = "01234567890123456789012345678901";
    result.identity.accessToken = "SYNTHETIC_SECRET_TOKEN";
    result.identity.userType = "legacy";
    result.platform = {"windows", "amd64", "10.0.19045", {}};
    result.java = JavaCandidate{result.version.root / "mock-java.exe", "21.0.5", "mock", "amd64", 21, 64, true};
    result.settings.width = 1000; result.settings.height = 600;
    return result;
}

}

int main() {
    try {
        rejects([] { parse_metadata("{\"a\":1,\"a\":2}"); }, "Duplicate JSON keys must reject");
        rejects([] { parse_metadata("{\"a\":{\"b\":1,\"b\":2}}"); }, "Nested duplicate JSON keys must reject");
        check(parse_metadata("{\"a\":{\"b\":1},\"c\":{\"b\":2}}").size() == 2, "Sibling JSON keys must pass");
        auto base = vanilla();
        auto child = parse_metadata(R"({"id":"fabric-pack","inheritsFrom":"1.20.1","mainClass":"net.fabricmc.loader.impl.launch.knot.KnotClient","arguments":{"game":["--tweakClass","first.Tweak","--tweakClass","second.Tweak"]},"libraries":[{"name":"org.test:regular:2.0","url":"http://127.0.0.1/loader/"},{"name":"org.test:regular:2.0:special","url":"http://127.0.0.1/loader/"}],"nickelCustom":{"preserve":true}})");
        const auto root = std::filesystem::absolute("native/tests/minecraft/synthetic/planning/root");
        auto resolved = resolve_version("fabric-pack", root, [&](std::string_view id) { return id == "fabric-pack" ? child : base; });
        check(resolved.baseVersionId == "1.20.1" && resolved.clientVersionId == "1.20.1", "Inherited base client jar");
        check(resolved.metadata["mainClass"] == child["mainClass"], "Inherited main class override");
        check(resolved.metadata["nickelCustom"]["preserve"] == true, "Unknown profile fields preserved");
        auto libraries = plan_libraries(resolved.metadata, root, {"windows", "amd64", "10", {}}, root / "natives");
        check(libraries.natives.size() == 1, "Only matching native architecture extracted");
        check(libraries.natives[0].destination == root / "natives" / "java", "Native extraction agrees with java property suffix");
        check(std::count_if(libraries.downloads.begin(), libraries.downloads.end(), [](const auto& file) { return file.coordinate.starts_with("org.test:regular:"); }) == 2, "Inherited classifier stays distinct");
        check(libraries.downloads[0].coordinate == "org.test:regular:2.0", "Loader library precedence");
        auto cycle = child; cycle["inheritsFrom"] = "fabric-pack";
        rejects([&] { resolve_version("fabric-pack", root, [&](auto) { return cycle; }); }, "Inheritance cycle rejects");
        rejects([&] { resolve_version("fabric-pack", root, [&](auto) { return base; }); }, "Profile identity mismatch rejects");
        Json legacy = base; legacy["assets"] = "legacy";
        Json index = parse_metadata(R"({"virtual":true,"map_to_resources":true,"objects":{"sounds/test.ogg":{"hash":"cccccccccccccccccccccccccccccccccccccccc","size":3},"sounds/duplicate.ogg":{"hash":"cccccccccccccccccccccccccccccccccccccccc","size":3}}})");
        auto assets = plan_assets(legacy, index, root, root / "game");
        check(assets.downloads.size() == 1 && assets.copies.size() == 4, "Assets deduplicate downloads and map virtual/resources");
        check(assets.assetsRoot == root / "game" / "resources", "Legacy asset root uses isolated game directory");
        index["objects"]["../escape"] = {{"hash", std::string(40, 'c')}, {"size", 1}};
        rejects([&] { plan_assets(legacy, index, root, root / "game"); }, "Asset path traversal rejects");
        Json runtime = parse_metadata(R"({"files":{"bin/javaw.exe":{"type":"file","executable":true,"downloads":{"raw":{"url":"http://127.0.0.1/java.exe","sha1":"dddddddddddddddddddddddddddddddddddddddd","size":3}}},"lib":{"type":"directory"},"bin/link":{"type":"link","target":"../lib"}}})");
        auto runtimePlan = plan_mojang_runtime(runtime, root / "runtime", {"windows", "amd64", "10", {}});
        check(runtimePlan.executable.filename() == "javaw.exe" && runtimePlan.links.size() == 1, "Runtime files directories and internal links");
        runtime["files"]["bin/link"]["target"] = "../../../../escape";
        rejects([&] { plan_mojang_runtime(runtime, root / "runtime", {"windows", "amd64", "10", {}}); }, "Runtime link escape rejects");
        auto vanillaRequest = request(base);
        auto vanillaPlan = build_launch_plan(vanillaRequest);
        check(vanillaPlan.mainClass == "net.minecraft.client.main.Main", "Real vanilla main class");
        check(!contains(vanillaPlan.process.arguments, "-jar"), "Vanilla is not simplified java -jar");
        check(contains(vanillaPlan.process.arguments, "Synthetic Player"), "Spaces remain one argv token");
        check(contains(vanillaPlan.process.arguments, "-Dos.correct=yes"), "Conditional JVM OS and architecture rules");
        check(std::count(vanillaPlan.process.arguments.begin(), vanillaPlan.process.arguments.end(), "-cp") == 1, "Exactly one classpath pair");
        check(std::count(vanillaPlan.process.arguments.begin(), vanillaPlan.process.arguments.end(), "--width") == 1, "Conditional and explicit dimensions do not duplicate");
        check(vanillaPlan.nativesDirectory.filename() == "java", "Native path no duplicated java suffix");
        check(launch_plan_json(vanillaPlan).dump().find("SYNTHETIC_SECRET_TOKEN") == std::string::npos, "CLI plan output redacts secret");
        auto modded = request(resolved.metadata);
        modded.version.loaders = {{LoaderKind::Fabric, "0.15.1"}};
        auto fabricPlan = build_launch_plan(modded);
        check(fabricPlan.mainClass.find("KnotClient") != std::string::npos, "Fabric actual launch main class");
        check(std::count(fabricPlan.process.arguments.begin(), fabricPlan.process.arguments.end(), "--tweakClass") == 2, "Repeated tweakClass key preserves distinct values");
        auto forge = request(base);
        const auto moduleJar = root / "libraries" / "org" / "test" / "regular" / "1.0" / "regular-1.0.jar";
        forge.version.loaders = {{LoaderKind::Forge, "47.1.0"}};
        forge.version.profile["mainClass"] = "cpw.mods.bootstraplauncher.BootstrapLauncher";
        forge.settings.jvmArguments = {"--module-path", path_utf8(moduleJar), "--add-opens=java.base/java.lang=ALL-UNNAMED"};
        auto forgePlan = build_launch_plan(forge);
        check(std::none_of(forgePlan.classpath.begin(), forgePlan.classpath.end(), [&](const auto& path) { return path == moduleJar || path.filename() == "1.20.1.jar"; }), "Forge Windows module/classpath split excludes module and vanilla jars");
        auto custom = vanillaRequest; custom.settings.memoryManagement = "custom"; custom.settings.jvmArguments = {"-XX:+UseParallelGC", "-Xmx128M"};
        auto customPlan = build_launch_plan(custom);
        check(contains(customPlan.process.arguments, "-XX:+UseParallelGC") && !contains(customPlan.process.arguments, "-Xmx128M") && contains(customPlan.process.arguments, "-Xmx4096M"), "GC custom preserved but heap ownership enforced");
        auto online = vanillaRequest; online.identity.type = "yggdrasil"; online.identity.yggdrasilApi = "https://synthetic.invalid/api/yggdrasil";
        auto authPlan = build_launch_plan(online);
        check(authPlan.agents.size() == 1 && authPlan.agents[0].entryClass == "moe.yushi.authlibinjector.Premain", "Yggdrasil authlib agent plan");
        online.identity.yggdrasilApi = "https://name:password@synthetic.invalid/api/yggdrasil";
        rejects([&] { build_launch_plan(online); }, "Auth API credentials URL rejects");
        auto lwjgl = vanillaRequest; lwjgl.java->major = 25;
        auto lwjglPlan = build_launch_plan(lwjgl);
        check(lwjglPlan.agents.size() == 1 && lwjglPlan.agents[0].entryClass == "org.glavo.lwjgl.UnsafeAgent", "LWJGL 3.4 Java25 agent");
        auto pending = vanillaRequest; pending.settings.launchable = false;
        rejects([&] { build_launch_plan(pending); }, "Pending instance launch rejects");
        for (auto id : {"", "a b", "a/b", "a\\b", "a.b"}) rejects([&] { validate_launch_id(id); }, "Invalid launchId rejects");
        validate_launch_id("current-launch_123");
        check(manifest_attribute("Manifest-Version: 1.0\r\nMain-Class: example.\r\n Main\r\n\r\nName: ignored\r\n", "Main-Class") == "example.Main", "JAR manifest folded entry class");
        rejects([] { manifest_attribute("Main-Class: a\nMain-Class: b\n", "Main-Class"); }, "Duplicate manifest main class rejects");
        check(split_arguments("-Dpath=\"C:\\Game Path\" --name 'Player Name' \"\"").size() == 4, "Legacy quoting spaces and empty token");
        check(redact_output("Authorization=secret Bearer opaque password=hunter2", {}) == "Authorization=[redacted] Bearer [redacted] password=[redacted]", "Sensitive output patterns redact");
        std::cout << "Minecraft metadata, inheritance, download and launch-plan synthetic matrices passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
