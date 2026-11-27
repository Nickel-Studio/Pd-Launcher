#pragma once

#include "platform/process.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nickel {

struct JavaRuntime {
    std::filesystem::path executable;
    std::string version;
    unsigned major = 0;
    std::string vendor;
    std::string architecture;
    bool is_64_bit = false;
};

struct JavaDiscoveryOptions {
    std::vector<std::filesystem::path> executables;
    std::vector<std::filesystem::path> roots;
    bool include_environment = true;
    bool include_system_locations = true;
    std::size_t max_candidates = 128;
    std::size_t max_depth = 4;
    std::chrono::milliseconds probe_timeout = std::chrono::seconds(15);
};

std::optional<JavaRuntime> parse_java_output(const std::filesystem::path& executable,
                                             std::string_view text);
JavaRuntime probe_java(const std::filesystem::path& executable,
                        const Cancellation* cancellation = nullptr,
                        std::chrono::milliseconds timeout = std::chrono::seconds(15));
std::vector<JavaRuntime> discover_java(const JavaDiscoveryOptions& options = {},
                                       const Cancellation* cancellation = nullptr);

} // namespace nickel
