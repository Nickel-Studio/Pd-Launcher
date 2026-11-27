#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nickel::minecraft {

struct Platform {
    std::string os;
    std::string architecture;
    std::string osVersion;
    std::map<std::string, bool, std::less<>> features;
    static Platform current();
    std::string normalized_architecture() const;
    char classpath_separator() const noexcept;
};

struct Rule {
    bool allow = false;
    std::optional<std::string> osName;
    std::optional<std::string> osVersion;
    std::optional<std::string> architecture;
    std::map<std::string, bool, std::less<>> features;
};

bool rule_matches(const Rule& rule, const Platform& platform);
bool rules_allow(std::span<const Rule> rules, const Platform& platform);
bool native_classifier_matches(std::string_view classifier, const Platform& platform);
std::string normalize_architecture(std::string_view architecture);
std::string java_path_argument(std::string_view value, bool windows);

}
