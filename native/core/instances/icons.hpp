#pragma once

#include "instances.hpp"

namespace nickel::media { class ImageService; }

namespace nickel::instances {

inline constexpr std::size_t selected_icon_max_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t managed_icon_max_bytes = 8 * 1024;

struct InstanceIcon {
    std::string reference;
    std::vector<std::uint8_t> png;
};

// Pins the complete selected file, normalizes once with the existing media service,
// and publishes a content-addressed PNG before atomically saving its relative reference.
// The caller must not hold the root DirectoryLock. No Task is created here.
std::string set_instance_icon(const fs::path& root, std::string_view id,
                              const fs::path& selected_file, media::ImageService& images,
                              std::stop_token cancellation = {});

// Returns only bounded, verified managed PNG bytes. Built-in or legacy external
// references return nullopt; a malformed or changed managed reference fails closed.
std::optional<InstanceIcon> read_instance_icon(const fs::path& root, std::string_view id,
                                              std::stop_token cancellation = {});

} // namespace nickel::instances
