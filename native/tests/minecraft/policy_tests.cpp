#include "java.hpp"
#include "loader_policy.hpp"
#include "maven.hpp"
#include "memory.hpp"
#include "rules.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace nickel::minecraft;

namespace {

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

}

int main() {
    try {
        for (const auto& [version, major] : std::array<std::pair<const char*, unsigned>, 7>{{{"1.5.2", 8}, {"1.12.2", 8}, {"1.17", 16}, {"1.18", 17}, {"1.20.4", 17}, {"1.20.5", 21}, {"26.1", 21}}}) check(java_requirement(version).minMajor == major, "Java boundary");
        check(java_requirement("1.7.2", "forge", "10.12.2.1121").maxMajor == 7, "Forge Java 7");
        check(java_requirement("1.16.5", "forge", "36.2.24").maxMajor == 8, "Forge old 36 Java 8");
        check(java_requirement("1.16.5", "forge", "36.2.26").maxMajor == 23, "Forge new 36 range");
        const std::array<JavaCandidate, 3> candidates{{{"java8", "1.8.0_412", "OpenJDK", "amd64", 8, 64, true}, {"java25", "25.0.1", "OpenJDK", "amd64", 25, 64, true}, {"java21", "21.0.5", "OpenJDK", "amd64", 21, 64, true}}};
        check(select_java(java_requirement("1.20.5"), candidates).major == 21, "Java exact major preference");
        auto invalid = candidates[2]; invalid.bits = 32;
        rejects([&] { validate_manual_java(java_requirement("1.20.5"), invalid); }, "32-bit Java must reject");
        check(parse_java_major("1.8.0_412") == 8, "Java 8 parse");
        auto probe = parse_java_probe("mock-java", "    java.version = 21.0.5\n    java.vendor = Eclipse Adoptium\n    os.arch = amd64\n    sun.arch.data.model = 64\n");
        check(probe.major == 21 && probe.bits == 64 && probe.executable, "Java property probe");
        check(compatible_adoptium_major(java_requirement("1.17")) == 17, "Java 16 LTS fallback");
        rejects([] { compatible_adoptium_major(java_requirement("1.7.2", "forge")); }, "Exact Java 7 cannot silently become Java 8");
        check(automatic_memory_mb(4096, 8192, 0, VersionKind::Regular) == 2970, "Memory oracle 1");
        check(automatic_memory_mb(8192, 16384, 0, VersionKind::Regular) == 4301, "Memory oracle 2");
        check(automatic_memory_mb(16384, 32768, 200, VersionKind::Moddable) == 10138, "Memory oracle 3");
        check(automatic_memory_mb(32768, 32768, 50, VersionKind::OptiFine) == 8704, "Memory oracle 4");
        check(automatic_memory_mb(4096, 4096, 200, VersionKind::Moddable) == 3994, "Memory oracle 5");
        check(automatic_memory_mb({}, 1024, 0, VersionKind::Regular) == 1024, "Unknown available still respects physical ceiling");
        rejects([] { manual_memory_mb(8192, 4096); }, "Manual heap must not clamp");
        for (auto major : {8U, 14U, 15U, 21U, 22U, 23U, 25U}) {
            const auto arguments = gc_arguments("auto-zgc", major, 17763, true).value();
            check(std::find(arguments.begin(), arguments.end(), "-XX:+UseZGC") != arguments.end() == (major >= 15), "ZGC boundary");
            check(std::find(arguments.begin(), arguments.end(), "-XX:+ZGenerational") != arguments.end() == (major >= 21 && major <= 22), "Generational ZGC boundary");
        }
        check(!gc_arguments("custom", 21, 17763, true), "Custom GC unchanged");
        Platform windows{"windows", "amd64", "10.0.19045", {{"has_custom_resolution", true}}};
        Rule allow; allow.allow = true;
        Rule deny; deny.osName = "windows"; deny.osVersion = "^10\\."; deny.architecture = "amd64";
        check(!rules_allow(std::array{allow, deny}, windows), "Last matching rule wins");
        check(rules_allow(std::array{deny, allow}, windows), "Allow can reenable");
        Rule feature; feature.allow = true; feature.features["has_custom_resolution"] = true;
        check(rules_allow(std::array{feature}, windows), "Feature rule true");
        feature.features["has_custom_resolution"] = false;
        check(!rules_allow(std::array{feature}, windows), "Feature rule false");
        for (const auto& [arch, suffix] : std::array<std::pair<const char*, const char*>, 4>{{{"x86_64", ""}, {"x86", "-x86"}, {"aarch64", "-arm64"}, {"arm", "-arm32"}}}) {
            Platform platform{"windows", arch, "10", {}};
            check(native_classifier_matches(std::string("natives-windows") + suffix, platform), "Native arch selected");
            check(!native_classifier_matches("natives-linux", platform), "Native OS selected");
            if (std::string_view(arch) != "x86_64") check(!native_classifier_matches("natives-windows", platform), "Default native excludes other arch");
        }
        check(MavenCoordinate::parse("net.minecraftforge:forge:1.20.1-47.1.0:client").key() != MavenCoordinate::parse("net.minecraftforge:forge:1.20.1-47.1.0:universal").key(), "Classifier classpath keys");
        check(MavenCoordinate::parse("a.b:c:1.2:slim@zip").relative_path().generic_string() == "a/b/c/1.2/c-1.2-slim.zip", "Maven extension/classifier");
        for (auto path : {"../escape", "C:/escape", "a/../../b", "a//b", "a/NUL.jar", "a/file:stream", "a/file. ", "a/%2e%2e/b"}) rejects([&] { validate_relative_path(path); }, "Unsafe path must reject");
        rejects([] { MavenCoordinate::parse("../g:a:1"); }, "Unsafe Maven coordinate");
        check(compare_versions("1.10", "1.9") > 0, "Natural Maven ordering");
        check(compare_versions("21.1.10", "21.1.10-beta") > 0, "Stable above beta");
        check(java_path_argument("-Dpath=\\\\?\\C:\\Game Path;\\\\?\\UNC\\server\\share", true) == "-Dpath=C:\\Game Path;\\\\server\\share", "Windows long path expansion");
        auto versions = parse_maven_versions("<metadata><versions><version>21.1.2</version><version>21.1.10</version><version>21.1.10-beta</version></versions></metadata>");
        check(versions.back() == "21.1.10", "Maven XML stable ordering");
        rejects([] { parse_maven_versions("<!DOCTYPE x><versions></versions>"); }, "Maven DTD forbidden");
        check(neoforge_matches_minecraft("1.20.1", "1.20.1-47.1.0"), "NeoForge legacy coordinates");
        check(neoforge_matches_minecraft("1.21.1", "21.1.4"), "NeoForge short coordinates");
        check(!neoforge_matches_minecraft("1.21.2", "21.1.4"), "NeoForge mismatched coordinates");
        const std::array<LoaderSelection, 1> forge{{{LoaderKind::Forge, "47.1.0"}}};
        check(optifine_compatibility("1.20.1", std::string("Forge 47.1.0"), forge).route == OptiFineRoute::Mods, "Forge OptiFine mods route");
        check(optifine_compatibility("1.20.1", {}, forge).route == OptiFineRoute::Incompatible, "Unknown Forge compatibility must not guess");
        check(optifine_compatibility("1.12.2", {}, {}).route == OptiFineRoute::LaunchWrapper, "Legacy OptiFine profile");
        const std::array<LoaderSelection, 1> fabric{{{LoaderKind::Fabric, "0.15.1"}}};
        check(optifine_compatibility("1.20.4", {}, fabric, true, true).route == OptiFineRoute::Mods, "Fabric OptiFine complete dependencies");
        check(optifine_compatibility("1.20.5", {}, fabric, true, true).route == OptiFineRoute::Incompatible, "Fabric OptiFine new versions reject");
        rejects([] { validate_loader_combination(std::array{LoaderSelection{LoaderKind::Forge, "47"}, LoaderSelection{LoaderKind::Fabric, "0.15"}}, "1.20.1"); }, "Two primary loaders reject");
        std::cout << "Minecraft policy oracle matrices passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
