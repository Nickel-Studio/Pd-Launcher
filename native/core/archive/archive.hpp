#pragma once

#include "core/network/hash.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nickel { class Cancellation; }
namespace nickel::archive {
struct Limits {
    std::uint64_t max_entries{100000};
    std::uint64_t max_entry_bytes{512ULL * 1024 * 1024};
    std::uint64_t max_total_bytes{8ULL * 1024 * 1024 * 1024};
    std::uint64_t max_archive_bytes{8ULL * 1024 * 1024 * 1024};
    std::uint32_t max_depth{64};
    std::uint32_t max_path_bytes{4096};
    std::uint64_t max_expansion_ratio{1000};
    std::chrono::milliseconds max_duration{std::chrono::minutes(10)};
};
struct Entry {
    std::string name;
    std::uint64_t size{};
    bool directory{};
};
struct ExtractOptions {
    Limits limits;
    std::vector<std::string> subtrees;
    std::vector<std::string> protected_paths;
    std::vector<std::string> existing_owned_files;
    bool allow_owned_overlay{};
};
struct Progress {
    std::string entry;
    std::uint64_t files_done{};
    std::uint64_t files_total{};
    std::uint64_t expanded_bytes{};
};
using ProgressCallback = std::function<void(const Progress&)>;
struct ExtractionReceipt {
    std::vector<std::filesystem::path> files;
    std::uint64_t expanded_bytes{};
};
struct ZipSource {
    std::string name;
    std::filesystem::path source;
};

class Archive {
public:
    static std::string validate_relative_path(std::string_view name);
    static std::vector<Entry> list(const std::filesystem::path& source,
                                   const Limits& limits, const Cancellation& cancel);
    // Read-only member names are never resolved as filesystem destinations.
    static std::vector<Entry> inspect_members(const std::filesystem::path& source,
                                             const Limits& limits, const Cancellation& cancel);
    static std::vector<std::byte> read_member_exact(const std::filesystem::path& source,
                                                  std::string_view name, std::uint64_t max_bytes,
                                                  const Cancellation& cancel, const Limits& limits = {});
    static std::vector<Entry> inspect(const std::filesystem::path& source,
                                      const Limits& limits, const Cancellation& cancel);
    static std::vector<std::byte> read_entry(const std::filesystem::path& source,
                                             std::string_view name,
                                             std::uint64_t max_bytes,
                                             const Cancellation& cancel,
                                             const Limits& limits = {});
    static ExtractionReceipt extract(const std::filesystem::path& source,
                                      const std::filesystem::path& destination,
                                      const ExtractOptions& options,
                                      const Cancellation& cancel,
                                      ProgressCallback progress = {});
    static void write_zip(const std::filesystem::path& destination,
                          const std::vector<ZipSource>& sources, const Limits& limits,
                          const Cancellation& cancel, ProgressCallback progress = {});
    static void validate(const std::filesystem::path& source, const Limits& limits,
                         const Cancellation& cancel);
    static void validate_executable_jar(const std::filesystem::path& source,
                                         const std::vector<std::string>& required_entries,
                                         const Cancellation& cancel,
                                         const Limits& limits = {});
};
inline std::vector<Entry> inspect(const std::filesystem::path& source,
                                  const Limits& limits, const Cancellation& cancel) {
    return Archive::inspect(source, limits, cancel);
}
inline std::vector<std::byte> read_entry(const std::filesystem::path& source,
                                         std::string_view name, std::uint64_t max_bytes,
                                         const Cancellation& cancel) {
    return Archive::read_entry(source, name, max_bytes, cancel);
}
inline ExtractionReceipt extract(const std::filesystem::path& source,
                                  const std::filesystem::path& destination,
                                  const ExtractOptions& options, const Cancellation& cancel,
                                  ProgressCallback progress = {}) {
    return Archive::extract(source, destination, options, cancel, std::move(progress));
}
} // namespace nickel::archive
