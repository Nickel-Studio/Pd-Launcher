#pragma once

#include "platform/process.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nickel {

struct GameWindowInfo {
    std::string title;
    std::string class_name;
};

bool is_game_window_class(std::string_view class_name) noexcept;
bool is_splash_title(std::string_view title);
bool is_game_window(std::uint32_t target_pid, std::uint32_t window_pid,
                     std::string_view class_name, std::string_view title);
std::string replace_title_placeholders(std::string_view text, std::string_view date,
                                        std::string_view time);
std::optional<GameWindowInfo> find_game_window(const OwnedProcess& game);
bool set_game_window_title(const OwnedProcess& game, std::string_view text);
bool maximize_game_window(const OwnedProcess& game);

void open_url(std::string_view url);
void open_path(const std::filesystem::path& path, bool reveal = false);

enum class DialogKind { OpenFile, OpenDirectory, SaveFile };
struct DialogFilter {
    std::string label;
    std::vector<std::string> patterns;
};
struct DialogOptions {
    DialogKind kind = DialogKind::OpenFile;
    std::string title;
    std::filesystem::path initial_directory;
    std::string suggested_name;
    std::vector<DialogFilter> filters;
    std::uintptr_t owner_window = 0;
};

// Cancellation returns nullopt; unavailable native service throws Unsupported.
std::optional<std::filesystem::path> show_native_dialog(const DialogOptions& options);

} // namespace nickel
