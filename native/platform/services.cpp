#include "platform/services.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace nickel {
namespace {
using namespace foundation_detail;
std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}
void replace_all(std::string& value, std::string_view key, std::string_view replacement) {
    std::size_t start = 0;
    while ((start = value.find(key, start)) != std::string::npos) {
        value.replace(start, key.size(), replacement);
        start += replacement.size();
    }
}
void require_game(const OwnedProcess& game) {
    if (game.purpose() != ProcessPurpose::Game || !game.pid() || !game.running())
        fail("PermissionDenied", "Window operations require a live owned game process");
}
#ifdef _WIN32
std::wstring wide(std::string_view text) { return path_from_utf8(text).native(); }
std::string utf8(std::wstring_view text) { return path_to_utf8(std::filesystem::path(text)); }
struct WindowSearch {
    const OwnedProcess* game = nullptr;
    std::uint32_t pid = 0;
    HWND window = nullptr;
    GameWindowInfo info;
};
BOOL CALLBACK visit(HWND window, LPARAM data) {
    auto& search = *reinterpret_cast<WindowSearch*>(data);
    try {
        if (!search.game->running()) return FALSE;
        if (!IsWindowVisible(window)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != search.pid) return TRUE;
        std::array<wchar_t, 128> class_buffer{};
        std::array<wchar_t, 1024> title_buffer{};
        const auto class_length = GetClassNameW(window, class_buffer.data(), static_cast<int>(class_buffer.size()));
        const auto title_length = GetWindowTextW(window, title_buffer.data(), static_cast<int>(title_buffer.size()));
        const auto class_name = utf8(std::wstring_view(class_buffer.data(), static_cast<std::size_t>(std::max(0, class_length))));
        const auto title = utf8(std::wstring_view(title_buffer.data(), static_cast<std::size_t>(std::max(0, title_length))));
        if (is_game_window(search.pid, pid, class_name, title) && search.game->running()) {
            search.window = window;
            search.info = {title, class_name};
            return FALSE;
        }
    } catch (...) {}
    return TRUE;
}
WindowSearch search_window(const OwnedProcess& game) {
    require_game(game);
    WindowSearch search;
    search.game = &game;
    search.pid = game.pid();
    EnumWindows(visit, reinterpret_cast<LPARAM>(&search));
    require_game(game);
    return search;
}
bool window_still_owned(const OwnedProcess& game, HWND window) {
    require_game(game);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return IsWindowVisible(window) && pid == game.pid() && game.running();
}
std::pair<std::string, std::string> local_date_time() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    auto two = [](unsigned value) { return (value < 10 ? "0" : "") + std::to_string(value); };
    return {std::to_string(now.wYear) + "/" + std::to_string(now.wMonth) + "/" + std::to_string(now.wDay),
        two(now.wHour) + ":" + two(now.wMinute) + ":" + two(now.wSecond)};
}
class ComApartment {
public:
    bool release = false;
    ComApartment() {
        const auto status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (status == S_OK || status == S_FALSE) release = true;
        else if (status == RPC_E_CHANGED_MODE) fail("InvalidArgument", "Native dialogs must run on the GUI apartment thread");
        else fail("IoError", "Cannot initialize the native dialog service");
    }
    ~ComApartment() { if (release) CoUninitialize(); }
};
template<class T> struct ComPointer {
    T* value = nullptr;
    ~ComPointer() { if (value) value->Release(); }
    T** out() { return &value; }
};
#else
std::filesystem::path find_opener(std::string_view filename) {
    const auto environment = current_environment();
    const auto item = environment.find("PATH");
    if (item != environment.end()) {
        std::string_view remaining(item->second);
        while (!remaining.empty()) {
            const auto end = remaining.find(':');
            const auto directory = remaining.substr(0, end);
            if (!directory.empty()) {
                const auto candidate = path_from_utf8(directory) / path_from_utf8(filename);
                std::error_code error;
                if (std::filesystem::is_regular_file(candidate, error) && !error) return candidate;
            }
            if (end == std::string_view::npos) break;
            remaining.remove_prefix(end + 1);
        }
    }
    fail("Unsupported", "No native open service is installed");
}
void execute_open(std::vector<std::string> arguments) {
    ProcessOptions options;
#ifdef __APPLE__
    options.executable = "/usr/bin/open";
#else
    options.executable = find_opener("xdg-open");
#endif
    options.arguments = std::move(arguments);
    options.purpose = ProcessPurpose::Service;
    options.capture_limit = 64 * 1024;
    auto process = OwnedProcess::spawn(options);
    if (const auto result = process.wait(std::chrono::seconds(5))) {
        if (result->exit_code != 0) fail("IoError", "The native open service rejected the request");
    } else process.detach();
}
#endif
} // namespace

bool is_game_window_class(std::string_view class_name) noexcept {
    return class_name == "GLFW30" || class_name == "SDL_app" || class_name == "LWJGL" || class_name == "SunAwtFrame";
}
bool is_splash_title(std::string_view title) {
    std::string normalized(trim(title));
    for (auto& c : normalized) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return normalized.starts_with("fml") || normalized.starts_with("quilt") || normalized.starts_with("glfw") || normalized == "popupmessagewindow";
}
bool is_game_window(std::uint32_t target_pid, std::uint32_t window_pid, std::string_view class_name, std::string_view title) {
    return target_pid == window_pid && is_game_window_class(class_name) && !is_splash_title(title);
}
std::string replace_title_placeholders(std::string_view text, std::string_view date, std::string_view time) {
    std::string result(text);
    replace_all(result, "{date}", date);
    replace_all(result, "{time}", time);
    return result;
}
std::optional<GameWindowInfo> find_game_window(const OwnedProcess& game) {
#ifdef _WIN32
    const auto search = search_window(game);
    if (search.window && window_still_owned(game, search.window)) return search.info;
    return std::nullopt;
#else
    require_game(game);
    return std::nullopt;
#endif
}
bool set_game_window_title(const OwnedProcess& game, std::string_view text) {
#ifdef _WIN32
    const auto search = search_window(game);
    if (!search.window || !window_still_owned(game, search.window)) return false;
    if (text.empty()) return true;
    const auto [date, time] = local_date_time();
    const auto title = wide(replace_title_placeholders(text, date, time));
    if (!window_still_owned(game, search.window)) return false;
    DWORD_PTR result = 0;
    const auto sent = SendMessageTimeoutW(search.window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(title.c_str()),
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
    return sent != 0 && result != 0 && window_still_owned(game, search.window);
#else
    require_game(game); (void)text;
    return false;
#endif
}
bool maximize_game_window(const OwnedProcess& game) {
#ifdef _WIN32
    const auto search = search_window(game);
    if (!search.window || !window_still_owned(game, search.window)) return false;
    const auto shown = ShowWindowAsync(search.window, SW_MAXIMIZE);
    return shown != 0 && window_still_owned(game, search.window);
#else
    require_game(game);
    return false;
#endif
}
void open_url(std::string_view url) {
    if (url.size() > 8192 || !valid_utf8(url) || url.find('\0') != std::string_view::npos ||
        url.find_first_of("\r\n\t ") != std::string_view::npos || !(url.starts_with("https://") || url.starts_with("http://"))) {
        fail("InvalidArgument", "Only valid HTTP or HTTPS links may be opened");
    }
    const auto authority_start = url.find("://") + 3;
    const auto authority = url.substr(authority_start, url.find_first_of("/?#", authority_start) - authority_start);
    if (authority.empty() || authority.find('@') != std::string_view::npos || authority.find('\\') != std::string_view::npos) {
        fail("InvalidArgument", "External link authority is invalid");
    }
#ifdef _WIN32
    const auto target = wide(url);
    const auto result = reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) fail("IoError", "Cannot open the external link");
#else
    execute_open({std::string(url)});
#endif
}
void open_path(const std::filesystem::path& path, bool reveal) {
    if (path.empty()) fail("InvalidArgument", "Open path must not be empty");
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error || !std::filesystem::exists(absolute, error) || error) fail("NotFound", "Open path does not exist");
#ifdef _WIN32
    if (reveal) {
        PIDLIST_ABSOLUTE item = ILCreateFromPathW(absolute.c_str());
        if (!item) fail("IoError", "Cannot resolve the shell item");
        const auto status = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ILFree(item);
        if (FAILED(status)) fail("IoError", "Cannot reveal the file");
    } else {
        const auto result = reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", absolute.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) fail("IoError", "Cannot open the path");
    }
#else
#ifdef __APPLE__
    if (reveal) execute_open({"-R", path_to_utf8(absolute)});
    else execute_open({path_to_utf8(absolute)});
#else
    execute_open({path_to_utf8(reveal ? absolute.parent_path() : absolute)});
#endif
#endif
}
std::optional<std::filesystem::path> show_native_dialog(const DialogOptions& options) {
#ifdef _WIN32
    ComApartment apartment;
    ComPointer<IFileDialog> dialog;
    const auto status = options.kind == DialogKind::SaveFile ?
        CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.out())) :
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.out()));
    if (FAILED(status)) fail("IoError", "Cannot create the native file dialog");
    DWORD flags = 0;
    if (FAILED(dialog.value->GetOptions(&flags))) fail("IoError", "Cannot configure the native file dialog");
    flags |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (options.kind == DialogKind::OpenDirectory) flags |= FOS_PICKFOLDERS;
    if (options.kind == DialogKind::SaveFile) flags |= FOS_OVERWRITEPROMPT;
    if (FAILED(dialog.value->SetOptions(flags))) fail("IoError", "Cannot configure the native file dialog");
    const auto title = wide(options.title);
    if (!title.empty() && FAILED(dialog.value->SetTitle(title.c_str()))) fail("IoError", "Cannot configure the native file dialog title");
    if (!options.suggested_name.empty()) {
        if (options.suggested_name.find_first_of("/\\") != std::string::npos) fail("InvalidArgument", "Suggested file name must not contain a directory");
        const auto name = wide(options.suggested_name);
        if (FAILED(dialog.value->SetFileName(name.c_str()))) fail("IoError", "Cannot configure the native save file name");
    }
    if (!options.initial_directory.empty()) {
        ComPointer<IShellItem> folder;
        if (FAILED(SHCreateItemFromParsingName(options.initial_directory.c_str(), nullptr, IID_PPV_ARGS(folder.out()))) ||
            FAILED(dialog.value->SetDefaultFolder(folder.value))) fail("InvalidArgument", "Dialog initial directory is invalid");
    }
    std::vector<std::wstring> labels, patterns;
    std::vector<COMDLG_FILTERSPEC> filters;
    labels.reserve(options.filters.size()); patterns.reserve(options.filters.size());
    for (const auto& filter : options.filters) {
        labels.push_back(wide(filter.label));
        std::string combined;
        for (const auto& pattern : filter.patterns) {
            if (pattern.empty() || pattern.find_first_of(";\r\n\\/") != std::string::npos) fail("InvalidArgument", "Dialog file filter is invalid");
            if (!combined.empty()) combined.push_back(';');
            combined += pattern;
        }
        patterns.push_back(wide(combined.empty() ? "*.*" : combined));
    }
    for (std::size_t i = 0; i < labels.size(); ++i) filters.push_back({labels[i].c_str(), patterns[i].c_str()});
    if (!filters.empty() && FAILED(dialog.value->SetFileTypes(static_cast<UINT>(filters.size()), filters.data()))) {
        fail("IoError", "Cannot configure native dialog filters");
    }
    const auto shown = dialog.value->Show(reinterpret_cast<HWND>(options.owner_window));
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    if (FAILED(shown)) fail("IoError", "Native file dialog failed");
    ComPointer<IShellItem> item;
    if (FAILED(dialog.value->GetResult(item.out()))) fail("IoError", "Cannot read native dialog result");
    PWSTR raw = nullptr;
    if (FAILED(item.value->GetDisplayName(SIGDN_FILESYSPATH, &raw))) fail("IoError", "Native dialog result is not a filesystem path");
    struct Cleanup { PWSTR raw; ~Cleanup() { CoTaskMemFree(raw); } } cleanup{raw};
    return std::filesystem::path(raw);
#else
    (void)options;
    fail("Unsupported", "This platform requires the Qt native file dialog adapter");
#endif
}
} // namespace nickel