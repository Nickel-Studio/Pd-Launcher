#include "maven.hpp"
#ifdef NICKEL_MINECRAFT_NATIVE_SERVICES
#include "core/config/atomic_file.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace nickel::minecraft {
namespace {

bool safe_part(std::string_view part) {
    return !part.empty() && part != "." && part != ".." && part.find("..") == std::string_view::npos &&
           std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == '+'; });
}

std::vector<std::string_view> split(std::string_view value, char delimiter) {
    std::vector<std::string_view> result;
    while (true) {
        const auto at = value.find(delimiter);
        result.emplace_back(value.substr(0, at));
        if (at == std::string_view::npos) return result;
        value.remove_prefix(at + 1);
    }
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

int compare_tokens(std::string_view left, std::string_view right) {
    std::size_t a = 0, b = 0;
    while (a < left.size() || b < right.size()) {
        while (a < left.size() && !std::isalnum(static_cast<unsigned char>(left[a]))) ++a;
        while (b < right.size() && !std::isalnum(static_cast<unsigned char>(right[b]))) ++b;
        const auto numberA = a < left.size() && std::isdigit(static_cast<unsigned char>(left[a]));
        const auto numberB = b < right.size() && std::isdigit(static_cast<unsigned char>(right[b]));
        auto endA = a, endB = b;
        while (endA < left.size() && std::isalnum(static_cast<unsigned char>(left[endA])) && static_cast<bool>(std::isdigit(static_cast<unsigned char>(left[endA]))) == numberA) ++endA;
        while (endB < right.size() && std::isalnum(static_cast<unsigned char>(right[endB])) && static_cast<bool>(std::isdigit(static_cast<unsigned char>(right[endB]))) == numberB) ++endB;
        auto tokenA = left.substr(a, endA - a), tokenB = right.substr(b, endB - b);
        if (numberA && numberB) {
            while (tokenA.size() > 1 && tokenA.front() == '0') tokenA.remove_prefix(1);
            while (tokenB.size() > 1 && tokenB.front() == '0') tokenB.remove_prefix(1);
            if (tokenA.size() != tokenB.size()) return tokenA.size() < tokenB.size() ? -1 : 1;
        }
        const auto comparison = tokenA.compare(tokenB);
        if (comparison) return comparison < 0 ? -1 : 1;
        if (a == endA && b == endB) break;
        a = endA; b = endB;
    }
    return 0;
}

}

MavenCoordinate MavenCoordinate::parse(std::string_view coordinate) {
    if (coordinate.starts_with('[') && coordinate.ends_with(']')) coordinate = coordinate.substr(1, coordinate.size() - 2);
    MavenCoordinate result;
    const auto extension = coordinate.find('@');
    if (extension != std::string_view::npos) {
        result.extension = coordinate.substr(extension + 1);
        coordinate = coordinate.substr(0, extension);
    }
    const auto parts = split(coordinate, ':');
    if (parts.size() < 3 || parts.size() > 4 || !safe_part(result.extension)) throw std::invalid_argument("Invalid Maven coordinate");
    for (auto part : parts) if (!safe_part(part)) throw std::invalid_argument("Unsafe Maven coordinate");
    result.group = parts[0]; result.artifact = parts[1]; result.version = parts[2];
    if (parts.size() == 4) result.classifier = parts[3];
    for (auto part : split(result.group, '.')) if (!safe_part(part)) throw std::invalid_argument("Invalid Maven group");
    return result;
}

std::filesystem::path MavenCoordinate::relative_path() const {
    std::string groupPath = group;
    std::replace(groupPath.begin(), groupPath.end(), '.', '/');
    auto filename = artifact + '-' + version;
    if (!classifier.empty()) filename += '-' + classifier;
    filename += '.' + extension;
    return validate_relative_path(groupPath + '/' + artifact + '/' + version + '/' + filename);
}

std::string MavenCoordinate::key() const {
    return group + ':' + artifact + ':' + classifier + '@' + extension;
}

std::string MavenCoordinate::str() const {
    auto result = group + ':' + artifact + ':' + version;
    if (!classifier.empty()) result += ':' + classifier;
    if (extension != "jar") result += '@' + extension;
    return result;
}

int compare_versions(std::string_view left, std::string_view right) {
    const auto a = left.find('-'), b = right.find('-');
    const auto mainComparison = compare_tokens(left.substr(0, a), right.substr(0, b));
    if (mainComparison) return mainComparison;
    if (a == std::string_view::npos && b != std::string_view::npos) return 1;
    if (b == std::string_view::npos && a != std::string_view::npos) return -1;
    return compare_tokens(a == std::string_view::npos ? std::string_view{} : left.substr(a + 1),
                          b == std::string_view::npos ? std::string_view{} : right.substr(b + 1));
}

std::filesystem::path validate_relative_path(std::string_view relative) {
    if (relative.empty() || relative.size() > 4096 || relative.front() == '/' || relative.front() == '\\') throw std::invalid_argument("Expected a nonempty relative Minecraft path");
    std::string normalized(relative);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.find(':') != std::string::npos || normalized.find('%') != std::string::npos || normalized.find('\0') != std::string::npos) throw std::invalid_argument("Unsafe relative Minecraft path");
    for (auto part : split(normalized, '/')) {
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') throw std::invalid_argument("Unsafe Minecraft path component");
        if (std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32 || c == 127 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*'; })) throw std::invalid_argument("Unsafe Minecraft path characters");
        const auto name = lower(std::string(part.substr(0, part.find('.'))));
        if (name == "con" || name == "prn" || name == "aux" || name == "nul" || (name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) && name[3] >= '1' && name[3] <= '9')) throw std::invalid_argument("Reserved Windows path component");
    }
    return std::filesystem::u8path(normalized);
}

void validate_version_id(std::string_view versionId) {
    const auto path = validate_relative_path(versionId);
    if (path.has_parent_path() || versionId.size() > 240) throw std::invalid_argument("Invalid Minecraft version ID");
}

bool path_is_inside(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto base = std::filesystem::absolute(root).lexically_normal();
    const auto target = std::filesystem::absolute(candidate).lexically_normal();
    auto a = base.begin(), b = target.begin();
    for (; a != base.end(); ++a, ++b) {
        if (b == target.end()) return false;
#ifdef _WIN32
        if (lower(a->string()) != lower(b->string())) return false;
#else
        if (*a != *b) return false;
#endif
    }
    return true;
}

void reject_links(const std::filesystem::path& path) {
#ifdef NICKEL_MINECRAFT_NATIVE_SERVICES
    reject_path_links(path);
#else
    auto current = std::filesystem::absolute(path).lexically_normal();
    while (!current.empty()) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (!error && std::filesystem::is_symlink(status)) throw std::invalid_argument("Minecraft paths cannot traverse symbolic links");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::invalid_argument("Minecraft paths cannot traverse reparse points");
#endif
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
#endif
}

std::filesystem::path path_inside(const std::filesystem::path& root, std::string_view relative) {
    auto result = std::filesystem::absolute(root).lexically_normal() / validate_relative_path(relative);
    if (!path_is_inside(root, result)) throw std::invalid_argument("Minecraft path escapes its trusted root");
    reject_links(result);
    return result;
}

std::string url_encode_segment(std::string_view segment) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : segment) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result.push_back(static_cast<char>(c));
        else { result.push_back('%'); result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    }
    return result;
}

}
