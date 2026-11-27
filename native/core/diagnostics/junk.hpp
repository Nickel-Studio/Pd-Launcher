#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::diagnostics {

struct JunkInstanceLocation {
    std::string versionId;
    std::filesystem::path effectiveDirectory;
};

struct JunkGameRoot {
    std::filesystem::path root;
    std::vector<JunkInstanceLocation> instances;
};

// Only the backend's registry/paths/settings composition supplies these locations.
struct JunkLocations {
    std::vector<JunkGameRoot> gameRoots;
    std::optional<std::filesystem::path> configRoot;
};

struct JunkLimits {
    std::size_t maxRoots{256};
    std::size_t maxInstances{4096};
    std::size_t maxScannedEntries{100000};
    std::size_t maxFiles{10000};
    std::size_t maxRetainedPreviews{4};
    std::uint64_t maxFileBytes{1024ULL * 1024ULL * 1024ULL};
    std::uint64_t maxTotalBytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
    std::chrono::seconds previewLifetime{std::chrono::minutes{10}};
};

enum class JunkKind {
    GameLog,
    CrashReport,
    JvmLog,
    AuthlibLog,
    ConfigLog,
    ConfigTemporary
};

struct JunkRegistrationIssue {
    std::filesystem::path path;
    std::string code;
};

struct JunkRegistration {
    std::uint64_t revision{};
    std::size_t acceptedRootCount{};
    std::size_t skippedRootCount{};
    std::size_t effectiveDirectoryCount{};
    std::vector<JunkRegistrationIssue> issues;
};

struct JunkItem {
    std::filesystem::path path;
    JunkKind kind{};
    std::uint64_t size{};
    std::string revision;
};

struct JunkPreview {
    std::string previewId;
    std::uint64_t revision{};
    std::uint64_t totalBytes{};
    std::size_t fileCount{};
    std::size_t skippedCount{};
    std::vector<JunkItem> items;
};

struct JunkCleanResult {
    std::uint64_t freedBytes{};
    std::size_t removedCount{};
    // Includes post-commit durability/publication errors and may overlap removedCount.
    std::size_t failedCount{};
    std::size_t missingCount{};
    std::size_t conflictCount{};
    std::size_t cancelledCount{};
    bool cancelled{};
    bool locationConflict{};
};

// The callback must be bounded and must not call back into this service. Backend
// mutation leases coordinate registry/settings changes without nested directory locks.
using JunkLocationValidator = std::function<bool(const JunkLocations&)>;

class JunkService final {
public:
    explicit JunkService(JunkLimits limits = {}, JunkLocationValidator validator = {});
    ~JunkService();
    JunkService(const JunkService&) = delete;
    JunkService& operator=(const JunkService&) = delete;
    JunkService(JunkService&&) = delete;
    JunkService& operator=(JunkService&&) = delete;

    // Re-registration invalidates every outstanding preview, including an empty one.
    JunkRegistration registerLocations(const JunkLocations& locations);
    JunkPreview preview(std::stop_token cancellation = {});

    // A preview is consumed once. No caller-supplied paths, items, or rescanning.
    JunkCleanResult clean(std::string_view previewId, std::stop_token cancellation = {});
    void discardPreview(std::string_view previewId);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nickel::diagnostics
