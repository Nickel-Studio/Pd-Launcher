#include "memory.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace nickel::minecraft {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

}

VersionKind classify_version(std::string_view versionId, std::string_view loader, std::string_view hint) {
    const auto all = lower(std::string(versionId) + ' ' + std::string(loader) + ' ' + std::string(hint));
    for (auto name : {"forge", "neoforge", "fabric", "quilt", "liteloader"}) {
        if (all.find(name) != std::string::npos) return VersionKind::Moddable;
    }
    return all.find("optifine") != std::string::npos ? VersionKind::OptiFine : VersionKind::Regular;
}

std::uint64_t count_mods(const std::filesystem::path& directory) {
    std::uint64_t result = 0;
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    for (; !error && iterator != std::filesystem::directory_iterator{}; iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (error || !std::filesystem::is_regular_file(status)) continue;
        const auto extension = lower(iterator->path().extension().string());
        if (extension == ".jar" || extension == ".zip" || extension == ".litemod") ++result;
    }
    return result;
}

std::uint64_t automatic_memory_mb(std::optional<std::uint64_t> availableMb,
                                  std::optional<std::uint64_t> totalMb,
                                  std::uint64_t mods, VersionKind kind) {
    constexpr std::uint64_t minimumMb = 512;
    const auto ceiling = std::max(minimumMb, totalMb.value_or(availableMb ? std::max(*availableMb, std::uint64_t{4096}) : 4096));
    if (!availableMb) return std::min(std::uint64_t{4096}, ceiling);
    double available = std::round(static_cast<double>(std::min(*availableMb, ceiling)) / 1024.0 * 10.0) / 10.0;
    std::array<double, 4> targets{0.5, 1.5, 2.5, 4.0};
    if (kind == VersionKind::OptiFine) targets = {0.5, 1.5, 3.0, 5.0};
    if (kind == VersionKind::Moddable) {
        const auto count = static_cast<double>(mods);
        targets = {0.5 + count / 150.0, 1.5 + count / 90.0, 2.7 + count / 50.0, 4.5 + count / 25.0};
    }
    double give = std::min(available, targets[1]);
    available -= targets[1];
    if (available >= 0.1) {
        const auto delta = targets[2] - targets[1];
        give += std::min(available * 0.7, delta);
        available -= delta / 0.7;
    }
    if (available >= 0.1) {
        const auto delta = targets[3] - targets[2];
        give += std::min(available * 0.4, delta);
        available -= delta / 0.4;
    }
    if (available >= 0.1) give += std::min(available * 0.15, targets[3]);
    const auto rounded = static_cast<std::uint64_t>(std::round(std::round(std::max(give, targets[0]) * 10.0) / 10.0 * 1024.0));
    return std::clamp(rounded, minimumMb, ceiling);
}

// Reject an unusable manual heap up front: below 512 MiB or above physical RAM cannot
// start the JVM. totalMb == 0 means the host value was unavailable, not a real limit.
std::uint64_t manual_memory_mb(std::uint64_t savedMb, std::optional<std::uint64_t> totalMb) {
    if (savedMb < 512) throw std::invalid_argument("Manual heap must be at least 512 MiB");
    if (totalMb && *totalMb && savedMb > *totalMb) throw std::invalid_argument("Manual heap exceeds physical memory");
    return savedMb;
}

// ZGC requires Windows 10 1809+; generational mode needs Java 21+, and the
// -XX:+ZGenerational flag itself only exists in Java 21-22. Unknown modes are errors.
std::optional<std::vector<std::string>> gc_arguments(std::string_view requested, unsigned major,
                                                    std::optional<unsigned> windowsBuild, bool windows) {
    const auto mode = requested.empty() ? std::string_view{"g1-tuned"} : requested;
    if (mode == "custom") return {};
    if (mode != "g1" && mode != "g1-tuned" && mode != "auto-zgc" && mode != "generational-zgc") throw std::invalid_argument("Unknown GC mode");
    const bool supportedWindows = windowsBuild ? *windowsBuild >= 17763 : !windows;
    const bool zgc = supportedWindows && ((mode == "auto-zgc" && major >= 15) || (mode == "generational-zgc" && major >= 21));
    std::vector<std::string> result{"-XX:+UnlockExperimentalVMOptions"};
    if (major >= 24) result.emplace_back("-XX:+UseCompactObjectHeaders");
    if (zgc) {
        result.emplace_back("-XX:+UseZGC");
        if (major >= 21 && major <= 22) result.emplace_back("-XX:+ZGenerational");
    } else {
        result.insert(result.end(), {"-XX:+UseG1GC", "-XX:G1NewSizePercent=20", "-XX:G1ReservePercent=20", "-XX:G1HeapRegionSize=32M", "-XX:MaxGCPauseMillis=50"});
        if (major <= 7) result.emplace_back("-XX:MaxPermSize=512m");
        if (mode == "g1-tuned") {
            result.emplace_back("-XX:+PerfDisableSharedMem");
            if (major == 8) result.emplace_back("-XX:+ParallelRefProcEnabled");
            if (major >= 12) result.insert(result.end(), {"-XX:MinHeapFreeRatio=25", "-XX:MaxHeapFreeRatio=40"});
        }
    }
    return result;
}

bool is_gc_option(std::string_view argument) {
    if (!argument.starts_with("-XX:")) return false;
    argument.remove_prefix(4);
    if (argument.starts_with('+') || argument.starts_with('-')) argument.remove_prefix(1);
    const auto name = lower(argument.substr(0, argument.find('=')));
    if (name == "unlockexperimentalvmoptions" || name == "zgenerational" || name == "usecompactobjectheaders" || name == "perfdisablesharedmem") return true;
    if (name.starts_with("use") && name.ends_with("gc")) return true;
    for (auto prefix : {"g1", "maxgcpausemillis", "mingcpausemillis", "maxheapfreeratio", "minheapfreeratio"}) if (name.starts_with(prefix)) return true;
    return false;
}

// A missing plan means "leave user-supplied GC flags untouched"; otherwise recognized
// GC flags are replaced wholesale so repeated launches cannot accumulate duplicates.
void apply_gc_plan(std::vector<std::string>& arguments, const std::optional<std::vector<std::string>>& plan) {
    if (!plan) return;
    std::erase_if(arguments, [](const auto& argument) { return is_gc_option(argument); });
    arguments.insert(arguments.end(), plan->begin(), plan->end());
}

}
