#include "java.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <regex>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace nickel::minecraft {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::optional<std::array<unsigned, 3>> version_parts(std::string_view version) {
    std::array<unsigned, 3> result{};
    auto end = version.find_first_of("-+");
    version = version.substr(0, end);
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (version.empty()) return result;
        end = version.find('.');
        auto part = version.substr(0, end);
        auto [pointer, error] = std::from_chars(part.data(), part.data() + part.size(), result[i]);
        if (error != std::errc{} || pointer != part.data() + part.size()) return {};
        if (end == std::string_view::npos) return result;
        version.remove_prefix(end + 1);
    }
    if (!version.empty()) return {};
    return result;
}

JavaRequirement range(unsigned minimum, std::optional<unsigned> maximum,
                      unsigned preferred, std::string reason) {
    return {minimum, maximum, preferred, std::move(reason)};
}

std::string invalid_candidate(const JavaRequirement& requirement, const JavaCandidate& candidate) {
    if (!candidate.executable || candidate.path.empty()) return "Java executable is unavailable";
    if (!candidate.major) return "Java major version could not be determined";
    if (candidate.bits < 64) return "Minecraft requires a 64-bit Java runtime";
    if (!requirement.accepts(candidate.major)) return "Java version is outside the supported compatibility range";
    return {};
}

JavaSelection selection(const JavaRequirement& requirement, const JavaCandidate* candidate,
                        std::string source, std::string reason) {
    JavaSelection result;
    result.requirement = requirement;
    result.source = std::move(source);
    result.reason = std::move(reason);
    if (candidate) {
        result.path = candidate->path;
        result.version = candidate->version;
        result.vendor = candidate->vendor;
        result.major = candidate->major;
        result.bits = candidate->bits;
        result.compatible = true;
        result.needsDownload = false;
    }
    return result;
}

}

bool JavaRequirement::accepts(unsigned major) const noexcept {
    return major >= minMajor && (!maxMajor || major <= *maxMajor);
}

// Requirements come from a release table plus optional metadata: the newest matching
// row wins, and metadata can only raise the floor, never contradict an explicit ceiling.
JavaRequirement java_requirement(std::string_view mc, std::string_view loader,
                                 std::string_view loaderVersion, std::string_view releaseTime,
                                 std::optional<unsigned> metadataMajor) {
    auto result = range(8, {}, 8, "Unknown Minecraft version requires Java 8 or newer");
    const auto parts = version_parts(mc);
    const auto kind = lower(loader);
    if (parts && (*parts)[0] == 1) {
        const auto minor = (*parts)[1];
        if (*parts >= std::array<unsigned, 3>{1, 20, 5}) {
            result = range(21, {}, 21, "Minecraft 1.20.5 or newer requires Java 21");
        } else if (minor >= 18) {
            result = range(17, {}, 17, "Minecraft 1.18 or newer requires Java 17");
        } else if (minor >= 17) {
            result = range(16, {}, 16, "Minecraft 1.17 requires Java 16");
        } else if (minor < 6) {
            result = range(8, 8, 8, "Minecraft 1.5.2 or older uses Java 8");
        } else {
            result = range(8, 17, 8, "Legacy Minecraft supports Java 8 through 17");
        }
        if (kind == "forge" || (kind == "neoforge" && mc == "1.20.1")) {
            const auto patch = (*parts)[2];
            if ((minor == 6 && patch >= 1 && patch <= 4) || (minor == 7 && patch <= 2)) {
                result = range(7, 7, 7, "Forge 1.6.1 through 1.7.2 requires Java 7");
            } else if (minor <= 12) {
                result = range(8, 8, 8, "Legacy Forge requires Java 8");
            } else if (minor <= 14) {
                result = range(8, 10, 8, "Forge 1.13 through 1.14 supports Java 8 through 10");
            } else if (minor == 15) {
                result = range(8, 15, 8, "Forge 1.15 supports Java 8 through 15");
            } else if (minor == 16 && patch <= 5) {
                auto build = loaderVersion;
                const auto prefix = std::string(mc) + '-';
                if (build.starts_with(prefix)) build.remove_prefix(prefix.size());
                const auto forgeParts = version_parts(build);
                if (forgeParts && (*forgeParts)[0] >= 34 && *forgeParts <= std::array<unsigned, 3>{36, 2, 25}) {
                    result = range(8, 8, 8, "Forge through 36.2.25 requires Java 8");
                } else if (forgeParts && (*forgeParts)[0] == 36) {
                    result = range(8, 23, 17, "Forge 36.2.26 or newer supports Java 8 through 23");
                }
            } else if (minor == 17 && patch == 1 && loaderVersion.starts_with("37.0.")) {
                result = range(16, 16, 16, "Minecraft 1.17.1 with Forge 37.0 requires Java 16");
            }
        } else if (kind == "optifine" && (minor <= 6 || (minor >= 8 && minor <= 12))) {
            result = range(8, 8, 8, "Legacy OptiFine requires Java 8");
        } else if (kind == "liteloader" && minor <= 12) {
            result = range(8, 8, 8, "LiteLoader requires Java 8");
        }
    } else if ((parts && (*parts)[0] >= 26) || releaseTime.substr(0, 10) >= "2024-04-23") {
        result = range(21, {}, 21, "Modern Minecraft requires Java 21");
    } else if (releaseTime.substr(0, 10) >= "2021-11-30") {
        result = range(17, {}, 17, "This Minecraft snapshot requires Java 17");
    } else if (releaseTime.substr(0, 10) >= "2021-06-08") {
        result = range(16, {}, 16, "This Minecraft snapshot requires Java 16");
    }
    if (metadataMajor && *metadataMajor > result.minMajor && !(result.maxMajor && *result.maxMajor < *metadataMajor)) {
        result.minMajor = *metadataMajor;
        result.preferredMajor = std::max(result.preferredMajor, *metadataMajor);
        result.reason = "Minecraft javaVersion metadata requires Java " + std::to_string(*metadataMajor);
    }
    return result;
}

// Loader requirements are intersected, not widened: disjoint ranges are an error
// because no single runtime could satisfy the requested stack.
JavaRequirement intersect_java_requirements(std::span<const JavaRequirement> requirements) {
    if (requirements.empty()) return java_requirement({});
    auto result = requirements.front();
    for (const auto& requirement : requirements.subspan(1)) {
        result.minMajor = std::max(result.minMajor, requirement.minMajor);
        if (requirement.maxMajor) result.maxMajor = result.maxMajor ? std::min(*result.maxMajor, *requirement.maxMajor) : requirement.maxMajor;
        result.preferredMajor = std::max(result.preferredMajor, requirement.preferredMajor);
    }
    if (result.maxMajor && result.minMajor > *result.maxMajor) throw std::invalid_argument("Loaders have incompatible Java requirements");
    result.preferredMajor = std::max(result.minMajor, result.preferredMajor);
    if (result.maxMajor) result.preferredMajor = std::min(result.preferredMajor, *result.maxMajor);
    return result;
}

// Deterministic ranking: exact preferred major first, then the lowest compatible
// major, then the path, so enumeration order cannot change the chosen runtime.
JavaSelection select_java(const JavaRequirement& requirement, std::span<const JavaCandidate> candidates) {
    const JavaCandidate* best = nullptr;
    for (const auto& candidate : candidates) {
        if (!invalid_candidate(requirement, candidate).empty()) continue;
        auto rank = [&](const JavaCandidate& value) {
            return std::tuple{value.major != requirement.preferredMajor, value.major, lower(value.path.generic_string())};
        };
        if (!best || rank(candidate) < rank(*best)) best = &candidate;
    }
    return selection(requirement, best, best ? "auto" : "unavailable",
                     best ? "Selected compatible Java " + std::to_string(best->major) : "A compatible Java runtime must be downloaded");
}

JavaSelection validate_manual_java(const JavaRequirement& requirement, const JavaCandidate& candidate) {
    auto reason = invalid_candidate(requirement, candidate);
    if (!reason.empty()) throw std::invalid_argument(reason);
    return selection(requirement, &candidate, "manual", "Validated manually selected Java runtime");
}

unsigned parse_java_major(std::string_view version) noexcept {
    if (version.starts_with("1.")) version.remove_prefix(2);
    while (!version.empty() && !std::isdigit(static_cast<unsigned char>(version.front()))) version.remove_prefix(1);
    unsigned major = 0;
    const auto end = version.find_first_not_of("0123456789");
    const auto number = version.substr(0, end);
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), major);
    return parsed.ec == std::errc{} ? major : 0;
}

// Parses `java -XshowSettings:properties -version` output: properties first, banner
// as fallback. `executable` stays false when no major could be parsed.
JavaCandidate parse_java_probe(const std::filesystem::path& executable, std::string_view output) {
    JavaCandidate result;
    result.path = executable;
    const std::string text(output);
    std::smatch match;
    if (std::regex_search(text, match, std::regex(R"(java\.version\s*=\s*([^\r\n]+))"))) result.version = match[1];
    else if (std::regex_search(text, match, std::regex(R"re((?:openjdk|java) version\s+"([^"]+)")re"))) result.version = match[1];
    result.major = parse_java_major(result.version);
    if (std::regex_search(text, match, std::regex(R"(java\.vendor\s*=\s*([^\r\n]+))"))) result.vendor = match[1];
    if (std::regex_search(text, match, std::regex(R"(os\.arch\s*=\s*([^\r\n]+))"))) result.architecture = match[1];
    if (std::regex_search(text, match, std::regex(R"(sun\.arch\.data\.model\s*=\s*(32|64))"))) result.bits = parse_java_major(match[1].str());
    else if (lower(text).find("64-bit") != std::string::npos || lower(text).find("aarch64") != std::string::npos || lower(text).find("amd64") != std::string::npos) result.bits = 64;
    else if (lower(text).find("32-bit") != std::string::npos || lower(result.architecture) == "x86") result.bits = 32;
    result.executable = result.major != 0;
    return result;
}

std::string mojang_runtime_platform(std::string_view os, std::string_view architecture) {
    const auto arch = lower(architecture);
    if (os == "windows") {
        if (arch == "aarch64" || arch == "arm64") return "windows-arm64";
        return arch == "x86" || arch == "i386" ? "windows-x86" : "windows-x64";
    }
    if (os == "osx" || os == "macos") return arch == "aarch64" || arch == "arm64" ? "mac-os-arm64" : "mac-os";
    if (os == "linux") {
        if (arch == "x86" || arch == "i386") return "linux-i386";
        if (arch == "aarch64" || arch == "arm64") return "linux-arm64";
        if (arch == "arm" || arch == "arm32") return "linux-arm32";
        return "linux";
    }
    throw std::invalid_argument("Unsupported Java runtime platform");
}

std::string adoptium_architecture(std::string_view architecture) {
    const auto arch = lower(architecture);
    if (arch == "x86_64" || arch == "amd64" || arch == "x64") return "x64";
    if (arch == "arm64" || arch == "aarch64") return "aarch64";
    if (arch == "x86" || arch == "i386") return "x86";
    if (arch == "arm" || arch == "arm32") return "arm";
    throw std::invalid_argument("Unsupported Java runtime architecture");
}

// Adoptium publishes only LTS majors; prefer the requested one, then the nearest
// higher LTS, then any accepted LTS, rather than naming an unavailable build.
unsigned compatible_adoptium_major(const JavaRequirement& requirement) {
    constexpr std::array<unsigned, 5> lts{8, 11, 17, 21, 25};
    if (std::find(lts.begin(), lts.end(), requirement.preferredMajor) != lts.end() && requirement.accepts(requirement.preferredMajor)) return requirement.preferredMajor;
    for (auto major : lts) if (major >= requirement.preferredMajor && requirement.accepts(major)) return major;
    for (auto major : lts) if (requirement.accepts(major)) return major;
    throw std::invalid_argument("No Adoptium LTS runtime satisfies the required Java range");
}

}
