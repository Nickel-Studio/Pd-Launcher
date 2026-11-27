#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace nickel::minecraft {

struct MavenCoordinate {
    std::string group;
    std::string artifact;
    std::string version;
    std::string classifier;
    std::string extension = "jar";
    static MavenCoordinate parse(std::string_view coordinate);
    std::filesystem::path relative_path() const;
    std::string key() const;
    std::string str() const;
};

int compare_versions(std::string_view left, std::string_view right);
std::filesystem::path validate_relative_path(std::string_view relative);
void validate_version_id(std::string_view versionId);
std::filesystem::path path_inside(const std::filesystem::path& root, std::string_view relative);
bool path_is_inside(const std::filesystem::path& root, const std::filesystem::path& candidate);
void reject_links(const std::filesystem::path& path);
std::string url_encode_segment(std::string_view segment);

}
