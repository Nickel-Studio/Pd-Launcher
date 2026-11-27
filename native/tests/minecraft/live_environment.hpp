#pragma once

#include "services.hpp"

#include <memory>
#include <string_view>

namespace test::live {

inline constexpr std::uint64_t cMinimumBytes = 1073741824ULL;
inline constexpr std::uint64_t dMinimumBytes = 4294967296ULL;
inline constexpr std::uint64_t gameMinimumBytes = 2147483648ULL;
inline constexpr std::uint64_t runtimeMinimumBytes = 536870912ULL;
inline constexpr std::uint64_t progressMinimumBytes = 268435456ULL;
inline constexpr std::string_view markerBytes = "Nickel isolated Minecraft acceptance cache v1\n";

bool capacity_sufficient(std::uint64_t cAvailable, std::uint64_t dAvailable,
                         std::uint64_t requiredD = dMinimumBytes) noexcept;

class Environment {
public:
    explicit Environment(std::filesystem::path cache);
    ~Environment();
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;

    const std::filesystem::path& cache() const noexcept;
    const std::filesystem::path& temp() const noexcept;
    const std::filesystem::path& home() const noexcept;
    const std::filesystem::path& evidence() const noexcept;
    nickel::Json layout() const;
    nickel::Json inspect_file(const std::filesystem::path& path) const;
    nickel::Json sample(std::string_view phase) const;
    void require_capacity(std::string_view phase, std::uint64_t requiredD = dMinimumBytes) const;
    nickel::minecraft::ProcessSpec constrain(const nickel::minecraft::ProcessSpec& spec) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using SpawnRecorder = std::function<void(const nickel::minecraft::ProcessSpec&)>;

// All decoration and recording finish before the irreversible owned spawn.
nickel::minecraft::Services constrain_services(nickel::minecraft::Services services,
                                               std::shared_ptr<Environment> environment,
                                               SpawnRecorder beforeSpawn = {});

}
