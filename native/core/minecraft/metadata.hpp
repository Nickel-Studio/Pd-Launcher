#pragma once

#include "json.hpp"
#include "java.hpp"
#include "loader_policy.hpp"
#include "rules.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::minecraft {

struct FileHashes {
    std::optional<std::string> sha1;
    std::optional<std::string> sha256;
    std::optional<std::string> md5;
    bool empty() const noexcept;
};

enum class FileKind { Metadata, Client, Library, Native, Asset, Logging, Java, Loader, Processor, Agent };

struct DownloadFile {
    std::filesystem::path destination;
    std::string url;
    std::vector<std::string> fallbackUrls;
    FileHashes hashes;
    std::optional<std::uint64_t> size;
    std::uint64_t maximumBytes = 1024ULL * 1024 * 1024;
    FileKind kind = FileKind::Library;
    std::string coordinate;
    bool executable = false;
    bool classpath = false;
};

struct WriteFile {
    std::filesystem::path destination;
    std::string content;
};

struct CopyFile {
    std::filesystem::path source;
    std::filesystem::path destination;
    FileHashes hashes;
    bool preserveExisting = false;
};

struct NativeArchive {
    std::filesystem::path archive;
    std::filesystem::path destination;
    std::vector<std::string> excludePrefixes{"META-INF/"};
};

struct LibraryPlan {
    std::vector<DownloadFile> downloads;
    std::vector<std::filesystem::path> classpath;
    std::vector<NativeArchive> natives;
};

struct AssetPlan {
    std::vector<DownloadFile> downloads;
    std::vector<CopyFile> copies;
    std::filesystem::path assetsRoot;
    std::filesystem::path gameAssets;
    bool virtualAssets = false;
    bool mapToResources = false;
};

struct RuntimeLink {
    std::filesystem::path destination;
    std::filesystem::path target;
};

struct RuntimePlan {
    std::filesystem::path root;
    std::filesystem::path executable;
    std::vector<DownloadFile> downloads;
    std::vector<std::filesystem::path> directories;
    std::vector<RuntimeLink> links;
    std::optional<DownloadFile> archive;
    unsigned stripComponents = 1;
};

struct ResolvedVersion {
    Json metadata;
    std::vector<Json> inheritance;
    std::string versionId;
    std::string baseVersionId;
    std::string clientVersionId;
    std::filesystem::path clientJar;
};

using ProfileResolver = std::function<Json(std::string_view)>;

Json parse_metadata(std::string_view text, std::size_t maximumBytes = 16 * 1024 * 1024);
std::vector<Rule> parse_rules(const Json& rules);
Json merge_version_profiles(const Json& parent, const Json& child);
ResolvedVersion resolve_version(std::string_view versionId, const std::filesystem::path& root,
                                const ProfileResolver& resolver, std::size_t maximumDepth = 64);
std::string resolve_version_alias(std::string_view versionId, const Json& manifest);
Json manifest_version_entry(std::string_view versionId, const Json& manifest);
LibraryPlan plan_libraries(const Json& metadata, const std::filesystem::path& root,
                           const Platform& platform, const std::filesystem::path& nativesDirectory);
AssetPlan plan_assets(const Json& metadata, const Json& index, const std::filesystem::path& root,
                      const std::filesystem::path& gameDirectory,
                      std::string_view resourcesBase = "https://resources.download.minecraft.net");
RuntimePlan plan_mojang_runtime(const Json& manifest, const std::filesystem::path& runtimeRoot,
                                const Platform& platform);
std::filesystem::path native_library_directory(const Json& metadata, const std::filesystem::path& base,
                                               const Platform& platform);
DownloadFile artifact_download(const Json& artifact, const std::filesystem::path& destination,
                                FileKind kind, std::string_view coordinate = {});
Json download_plan_json(std::span<const DownloadFile> files);
std::string path_utf8(const std::filesystem::path& path);

}
