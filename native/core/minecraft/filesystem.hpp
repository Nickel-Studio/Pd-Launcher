#pragma once

#include <filesystem>
#ifdef NICKEL_MINECRAFT_NATIVE_SERVICES
#include "core/config/path_guard.hpp"
#endif

namespace nickel::minecraft {

// Native I/O representation only; saved profiles and process arguments keep logical paths.
inline std::filesystem::path filesystem_path(const std::filesystem::path& path) {
#ifdef NICKEL_MINECRAFT_NATIVE_SERVICES
    return foundation_detail::native_filesystem_path(path);
#else
    return path;
#endif
}

}
