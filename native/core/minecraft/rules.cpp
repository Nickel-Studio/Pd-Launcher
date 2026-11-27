#include "rules.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

namespace nickel::minecraft {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Rule expressions come from remote version metadata; cap their length and turn
// regex_error into an input error instead of leaking the regex exception.
bool regex_matches(const std::string& pattern, const std::string& value) {
    if (pattern.size() > 1024) throw std::invalid_argument("Platform rule expression is too long");
    try {
        return std::regex_search(value, std::regex(pattern));
    } catch (const std::regex_error&) {
        throw std::invalid_argument("Invalid platform rule expression");
    }
}

}

Platform Platform::current() {
    Platform result;
    // GetVersionEx reports a compatibility-lied version without an app manifest;
    // RtlGetVersion supplies the real build number Mojang's OS rules expect.
#ifdef _WIN32
    result.os = "windows";
    using RtlGetVersionType = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto module = GetModuleHandleW(L"ntdll.dll");
    auto function = module ? reinterpret_cast<RtlGetVersionType>(GetProcAddress(module, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (function && function(&version) == 0) {
        result.osVersion = std::to_string(version.dwMajorVersion) + '.' + std::to_string(version.dwMinorVersion) + '.' + std::to_string(version.dwBuildNumber);
    }
#elif defined(__APPLE__)
    result.os = "osx";
    struct utsname name{};
    if (uname(&name) == 0) result.osVersion = name.release;
#else
    result.os = "linux";
    struct utsname name{};
    if (uname(&name) == 0) result.osVersion = name.release;
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
    result.architecture = "aarch64";
#elif defined(_M_IX86) || defined(__i386__)
    result.architecture = "x86";
#elif defined(_M_ARM) || defined(__arm__)
    result.architecture = "arm";
#else
    result.architecture = "x86_64";
#endif
    return result;
}

std::string normalize_architecture(std::string_view architecture) {
    const auto value = lower(architecture);
    if (value == "amd64" || value == "x64" || value == "64" || value == "x86_64") return "x86_64";
    if (value == "x86" || value == "32" || value == "x86_32" || value == "i386" || value == "i686") return "x86";
    if (value == "arm64" || value == "aarch64") return "aarch64";
    if (value == "arm" || value == "arm32" || value == "aarch" || value == "armv7l") return "arm";
    return value;
}

std::string Platform::normalized_architecture() const {
    return normalize_architecture(architecture);
}

char Platform::classpath_separator() const noexcept {
    return os == "windows" ? ';' : ':';
}

// Rules are ordered and the last match wins; a non-empty rule set defaults to deny,
// so an unmatched platform never falls through to an implicit allow.
bool rule_matches(const Rule& rule, const Platform& platform) {
    if (rule.osName && *rule.osName != platform.os && !(*rule.osName == "macos" && platform.os == "osx")) return false;
    if (rule.osVersion && !regex_matches(*rule.osVersion, platform.osVersion)) return false;
    if (rule.architecture) {
        if (!regex_matches(*rule.architecture, platform.architecture) &&
            !regex_matches(*rule.architecture, platform.normalized_architecture()) &&
            !(platform.normalized_architecture() == "x86_64" && regex_matches(*rule.architecture, "amd64"))) return false;
    }
    for (const auto& [feature, expected] : rule.features) {
        const auto found = platform.features.find(feature);
        const auto actual = found != platform.features.end() && found->second;
        if (actual != expected) return false;
    }
    return true;
}

bool rules_allow(std::span<const Rule> rules, const Platform& platform) {
    if (rules.empty()) return true;
    bool allowed = false;
    for (const auto& rule : rules) if (rule_matches(rule, platform)) allowed = rule.allow;
    return allowed;
}

// Anything that is not natives-<os>[-<arch>] is not a native selector and must not
// filter an artifact; unknown OS/arch tags are tolerated for forward compatibility.
bool native_classifier_matches(std::string_view classifier, const Platform& platform) {
    if (!classifier.starts_with("natives-")) return true;
    classifier.remove_prefix(8);
    const auto separator = classifier.find('-');
    auto os = classifier.substr(0, separator);
    if (os == "macos") os = "osx";
    if (os != "linux" && os != "windows" && os != "osx") return true;
    if (os != platform.os) return false;
    const auto suffix = separator == std::string_view::npos ? std::string_view{} : classifier.substr(separator + 1);
    const auto expected = suffix.empty() ? std::string{"x86_64"} : normalize_architecture(suffix);
    if (expected != "x86_64" && expected != "x86" && expected != "aarch64" && expected != "arm") return true;
    return expected == platform.normalized_architecture();
}

// The JVM rejects extended-length Win32 namespace paths, so strip the prefix only at
// argument boundaries; a prefix appearing mid-token must survive unchanged.
std::string java_path_argument(std::string_view value, bool windows) {
    if (!windows) return std::string(value);
    constexpr std::string_view prefix = "\\\\?\\";
    std::string output;
    while (true) {
        const auto at = value.find(prefix);
        if (at == std::string_view::npos) break;
        output.append(value.substr(0, at));
        const auto tail = value.substr(at + prefix.size());
        const auto boundary = at == 0 || std::string_view{"=;:\"'"}.find(value[at - 1]) != std::string_view::npos;
        if (boundary && tail.size() >= 4 && lower(tail.substr(0, 4)) == "unc\\") {
            output.append("\\\\");
            value = tail.substr(4);
        } else if (boundary && tail.size() >= 3 && std::isalpha(static_cast<unsigned char>(tail[0])) && tail[1] == ':' && (tail[2] == '\\' || tail[2] == '/')) {
            value = tail;
        } else {
            output.append(prefix);
            value = tail;
        }
    }
    output.append(value);
    return output;
}

}
