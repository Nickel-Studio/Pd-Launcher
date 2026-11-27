#pragma once

#ifdef NICKEL_TRANSPORT_TEST_HOOKS
#include <filesystem>
#include <functional>
#include <string_view>

namespace nickel::tasks::detail::testing {
inline thread_local std::function<void(std::string_view, const std::filesystem::path&)> checkpoint;
inline void reach(std::string_view point, const std::filesystem::path& path) {
    if (checkpoint) checkpoint(point, path);
}
}
#define NICKEL_TRANSPORT_CHECKPOINT(point, path) ::nickel::tasks::detail::testing::reach(point, path)
#else
#define NICKEL_TRANSPORT_CHECKPOINT(point, path) ((void)0)
#endif
