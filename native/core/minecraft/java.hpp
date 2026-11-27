#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nickel::minecraft {

struct JavaRequirement {
    unsigned minMajor = 8;
    std::optional<unsigned> maxMajor;
    unsigned preferredMajor = 8;
    std::string reason;
    bool accepts(unsigned major) const noexcept;
};

struct JavaCandidate {
    std::filesystem::path path;
    std::string version;
    std::string vendor;
    std::string architecture;
    unsigned major = 0;
    unsigned bits = 0;
    bool executable = false;
};

struct JavaSelection {
    std::optional<std::filesystem::path> path;
    std::string source = "unavailable";
    std::string version;
    std::string vendor;
    unsigned major = 0;
    unsigned bits = 0;
    bool compatible = false;
    bool needsDownload = true;
    std::string reason;
    JavaRequirement requirement;
};

JavaRequirement java_requirement(std::string_view minecraftVersion,
                                 std::string_view loader = {},
                                 std::string_view loaderVersion = {},
                                 std::string_view releaseTime = {},
                                 std::optional<unsigned> metadataMajor = {});
JavaRequirement intersect_java_requirements(std::span<const JavaRequirement> requirements);
JavaSelection select_java(const JavaRequirement& requirement, std::span<const JavaCandidate> candidates);
JavaSelection validate_manual_java(const JavaRequirement& requirement, const JavaCandidate& candidate);
unsigned parse_java_major(std::string_view version) noexcept;
JavaCandidate parse_java_probe(const std::filesystem::path& executable, std::string_view output);
std::string mojang_runtime_platform(std::string_view os, std::string_view architecture);
std::string adoptium_architecture(std::string_view architecture);
unsigned compatible_adoptium_major(const JavaRequirement& requirement);

}
