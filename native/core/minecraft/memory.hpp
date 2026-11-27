#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::minecraft {

enum class VersionKind { Regular, OptiFine, Moddable };

VersionKind classify_version(std::string_view versionId, std::string_view loader = {},
                             std::string_view metadataHint = {});
std::uint64_t count_mods(const std::filesystem::path& modsDirectory);
std::uint64_t automatic_memory_mb(std::optional<std::uint64_t> availableMb,
                                  std::optional<std::uint64_t> totalMb,
                                  std::uint64_t modCount, VersionKind kind);
std::uint64_t manual_memory_mb(std::uint64_t savedMb, std::optional<std::uint64_t> totalMb);
std::optional<std::vector<std::string>> gc_arguments(std::string_view mode, unsigned javaMajor,
                                                    std::optional<unsigned> windowsBuild,
                                                    bool windows = false);
bool is_gc_option(std::string_view argument);
void apply_gc_plan(std::vector<std::string>& arguments,
                   const std::optional<std::vector<std::string>>& plan);

}
