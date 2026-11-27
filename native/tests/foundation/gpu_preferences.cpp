#include "core/common/cancellation.hpp"
#include "core/common/error.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/path_guard.hpp"
#include "platform/gpu_preferences.hpp"
#include "platform/gpu_preferences_detail.hpp"
#include "platform/process.hpp"
#include "platform/services.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace nickel;
using namespace nickel::foundation_detail;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string failure(const auto& callback) {
    try { callback(); } catch (const ErrorException& error) { return error.error().code; }
    throw std::runtime_error("Restricted GPU operation unexpectedly succeeded");
}
bool eventually(const auto& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
GpuRegistryValue string_value(std::u16string_view text) {
    GpuRegistryValue value{1, {}};
    for (const auto unit : text) {
        value.bytes.push_back(static_cast<std::uint8_t>(unit & 0xff));
        value.bytes.push_back(static_cast<std::uint8_t>(unit >> 8));
    }
    value.bytes.push_back(0);
    value.bytes.push_back(0);
    return value;
}
void test_merge() {
    const std::array<std::pair<std::u16string_view, std::u16string_view>, 7> cases{{
        {u"", u"GpuPreference=2;"},
        {u"AutoHDREnable=1;GpuPreference=1;SwapEffectUpgradeEnable=1;", u"AutoHDREnable=1;GpuPreference=2;SwapEffectUpgradeEnable=1;"},
        {u"AutoHDREnable=1;SwapEffectUpgradeEnable=0", u"AutoHDREnable=1;SwapEffectUpgradeEnable=0;GpuPreference=2;"},
        {u"Other=\u503c;;gpupreference=0;Unknown=1;GpuPreference=1", u"Other=\u503c;;GpuPreference=2;Unknown=1;GpuPreference=2"},
        {u"; GpUpReFeReNcE =0;;AutoHDREnable=0;", u";GpuPreference=2;;AutoHDREnable=0;"},
        {u"\u2003GpuPreference\u2003=1;Other=\U0001f600;", u"GpuPreference=2;Other=\U0001f600;"},
        {u"GpuPreference;Other=1;", u"GpuPreference;Other=1;GpuPreference=2;"}
    }};
    for (const auto& [input, expected] : cases)
        require(high_performance_gpu_value(string_value(input)) == string_value(expected), "GPU merge lost an unrelated fragment or duplicate");
    require(high_performance_gpu_value(std::nullopt) == string_value(u"GpuPreference=2;"), "Absent GPU value was not initialized");
    std::vector<GpuRegistryValue> invalid{
        {3, {1, 2}}, {1, {}}, {1, {1}}, {1, {0, 0xd8, 0, 0}}, {1, {0, 0xdc, 0, 0}}, {1, {u'X', 0}},
        string_value(std::u16string(u"A\0B", 3)), {1, {0, 0, 0, 0}}
    };
    auto unterminated = string_value(u"GpuPreference=1;");
    unterminated.bytes.resize(unterminated.bytes.size() - 2);
    invalid.push_back(unterminated);
    for (const auto& value : invalid)
        require(failure([&] { (void)high_performance_gpu_value(value); }) == "InvalidArgument", "Malformed REG_SZ was accepted");
    GpuRegistryValue oversized{1, std::vector<std::uint8_t>(gpu_preference_max_bytes + 2, 0)};
    require(failure([&] { (void)high_performance_gpu_value(oversized); }) == "LimitExceeded", "Oversized GPU value was accepted");
    std::u16string boundary = u"GpuPreference=1;";
    boundary.resize(gpu_preference_max_bytes / 2 - 1, u'x');
    auto expected = boundary;
    expected[14] = u'2';
    require(high_performance_gpu_value(string_value(boundary)) == string_value(expected), "Exact 64 KiB GPU replacement failed");
    require(failure([&] { (void)high_performance_gpu_value(string_value(std::u16string(gpu_preference_max_bytes / 2 - 1, u'x'))); }) == "LimitExceeded",
            "GPU append expanded beyond 64 KiB");
    std::cout << "PASS GPU merge: absent/empty, duplicate keys, exact HDR/unrelated fragments, Unicode, strict type/UTF16/NUL, 64KiB bounds\n";
}

struct Fixture {
    fs::path root = fs::current_path() / path_from_utf8("foundation-gpu-" + unique_token());
    fs::path java = root / "JavaGpu.Exe";
    Fixture() { fs::create_directories(root); atomic_write_text(java, "synthetic selected Java path, not a launch image"); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};
struct Callbacks {
    std::array<std::atomic<unsigned>, 2> eof{};
    std::atomic<unsigned> warnings{0};
    std::atomic<bool> ready{false};
    std::atomic<bool> warning_off_main{false};
    std::atomic<bool> warning_code_ok{true};
};
ProcessOptions game_options(const fs::path& mock, const fs::path& java, const std::shared_ptr<Callbacks>& state) {
    ProcessOptions options;
    options.executable = mock;
    options.purpose = ProcessPurpose::Game;
    options.high_performance_gpu = true;
    options.gpu_preference_executable = java;
    const auto caller = std::this_thread::get_id();
    options.gpu_preference_warning = [state, caller](const Error& error) {
        require(!error.code.empty(), "GPU warning omitted its error code");
        state->warnings.fetch_add(1);
        state->warning_off_main = std::this_thread::get_id() != caller;
    };
    options.output = [state](ProcessStream stream, std::string_view bytes, bool eof) {
        if (bytes.find("descendant-ready") != std::string_view::npos) state->ready = true;
        if (eof) state->eof[stream == ProcessStream::Stdout ? 0 : 1].fetch_add(1);
    };
    return options;
}

#ifdef _WIN32
class IsolatedRegistry {
    HKEY real_user_ = nullptr;
    HKEY hive_ = nullptr;
    HKEY values_ = nullptr;
    std::wstring name_;
    bool redirected_ = false;
public:
    IsolatedRegistry() {
        const auto token = path_from_utf8(unique_token()).native();
        name_ = L"Software\\NickelFoundationGpuTests-" + token;
        require(RegOpenCurrentUser(KEY_READ | KEY_WRITE, &real_user_) == ERROR_SUCCESS, "Cannot open actual HKCU for isolated test hive");
        try {
            require(RegCreateKeyExW(real_user_, name_.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                KEY_ALL_ACCESS, nullptr, &hive_, nullptr) == ERROR_SUCCESS, "Cannot create isolated volatile GPU hive");
            require(RegCreateKeyExW(hive_, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr,
                REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &values_, nullptr) == ERROR_SUCCESS, "Cannot create isolated preference key");
            require(RegOverridePredefKey(HKEY_CURRENT_USER, hive_) == ERROR_SUCCESS, "Cannot redirect GPU test process HKCU");
            redirected_ = true;
        } catch (...) { close(); throw; }
    }
    ~IsolatedRegistry() { close(); }
    IsolatedRegistry(const IsolatedRegistry&) = delete;
    IsolatedRegistry& operator=(const IsolatedRegistry&) = delete;
    void close() noexcept {
        if (redirected_) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        redirected_ = false;
        if (values_) RegCloseKey(std::exchange(values_, nullptr));
        if (hive_) RegCloseKey(std::exchange(hive_, nullptr));
        if (real_user_) {
            if (!name_.empty()) RegDeleteTreeW(real_user_, name_.c_str());
            RegCloseKey(std::exchange(real_user_, nullptr));
        }
    }
    std::optional<GpuRegistryValue> read(const fs::path& java) const {
        const auto name = java.native();
        DWORD type = 0, size = 0;
        auto status = RegQueryValueExW(values_, name.c_str(), nullptr, &type, nullptr, &size);
        if (status == ERROR_FILE_NOT_FOUND) return std::nullopt;
        require(status == ERROR_SUCCESS && size <= gpu_preference_max_bytes + 2, "Cannot read isolated preference metadata");
        std::vector<std::uint8_t> bytes(size);
        status = RegQueryValueExW(values_, name.c_str(), nullptr, &type, bytes.data(), &size);
        require(status == ERROR_SUCCESS && size == bytes.size(), "Cannot read isolated preference bytes");
        return GpuRegistryValue{type, std::move(bytes)};
    }
    void write(const fs::path& java, const std::optional<GpuRegistryValue>& data) const {
        const auto name = java.native();
        const auto status = data ? RegSetValueExW(values_, name.c_str(), 0, data->type, data->bytes.data(), static_cast<DWORD>(data->bytes.size())) :
            RegDeleteValueW(values_, name.c_str());
        require(status == ERROR_SUCCESS || (!data && status == ERROR_FILE_NOT_FOUND), "Cannot write isolated preference fixture");
    }
};
struct RootObservation {
    HANDLE handle = nullptr;
    explicit RootObservation(const OwnedProcess& game) : handle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, game.pid())) {
        require(handle && game.running(), "Cannot independently retain the live test root");
    }
    ~RootObservation() { if (handle) CloseHandle(handle); }
    RootObservation(const RootObservation&) = delete;
    RootObservation& operator=(const RootObservation&) = delete;
};

// Launching a file that is not a PE image makes Windows raise a blocking
// critical-error dialog unless the caller has SEM_FAILCRITICALERRORS. CTest
// starts the suite with error mode 0, so that modal prompt is never dismissed
// and CreateProcessW blocks until the harness timeout. Suppress hard-error UI
// only for this intentional bad-image fixture; the assertion that the real
// CreateProcess fails with IoError is unchanged.
class SuppressCriticalErrorDialogs {
    UINT previous_ = 0;
public:
    SuppressCriticalErrorDialogs() : previous_(SetErrorMode(SEM_FAILCRITICALERRORS)) {}
    ~SuppressCriticalErrorDialogs() { SetErrorMode(previous_); }
    SuppressCriticalErrorDialogs(const SuppressCriticalErrorDialogs&) = delete;
    SuppressCriticalErrorDialogs& operator=(const SuppressCriticalErrorDialogs&) = delete;
};

void test_guard(const Fixture& fixture, const IsolatedRegistry& registry) {
    const auto& java = fixture.java;
    registry.write(java, std::nullopt);
    auto guard = GpuPreferenceGuard::acquire(native_filesystem_path(java));
    require(guard.java_executable() == java && registry.read(java) == string_value(u"GpuPreference=2;"), "GPU Java canonical logical path or initial value differs");
    auto upper = java.native();
    for (auto& unit : upper) if (unit >= L'a' && unit <= L'z') unit = static_cast<wchar_t>(unit - (L'a' - L'A'));
    require(failure([&] { (void)GpuPreferenceGuard::acquire(fs::path(upper)); }) == "Conflict", "Same Java was not case-insensitively excluded");
    std::atomic<unsigned> conflicts{0};
    std::atomic<bool> unexpected{false};
    std::array<std::jthread, 4> contenders;
    for (auto& thread : contenders) thread = std::jthread([&] {
        try {
            if (failure([&] { (void)GpuPreferenceGuard::acquire(java); }) == "Conflict") conflicts.fetch_add(1);
            else unexpected = true;
        } catch (...) { unexpected = true; }
    });
    for (auto& thread : contenders) thread.join();
    require(!unexpected && conflicts == contenders.size(), "Concurrent same-Java preference leases were admitted");
    auto moved = std::move(guard);
    require(guard.restore() == GpuPreferenceRestore::Inactive && guard.java_executable().empty(), "Moved GPU guard retained ownership");
    require(moved.restore() == GpuPreferenceRestore::Restored && !registry.read(java), "Absent original GPU value was not deleted");
    require(moved.restore() == GpuPreferenceRestore::Inactive, "GPU restoration was not idempotent");
    const auto original = string_value(u"AutoHDREnable=1;GpuPreference=1;SwapEffectUpgradeEnable=1;");
    const auto applied = high_performance_gpu_value(original);
    registry.write(java, original);
    { auto value = GpuPreferenceGuard::acquire(java); require(registry.read(java) == applied, "Original GPU value was not merged"); }
    require(registry.read(java) == original, "GPU destructor did not restore original type and bytes");
    for (const auto& external : {string_value(u"AutoHDREnable=0;GpuPreference=0;"), GpuRegistryValue{3, applied.bytes}}) {
        auto value = GpuPreferenceGuard::acquire(java);
        registry.write(java, external);
        require(value.restore() == GpuPreferenceRestore::ExternalChange && registry.read(java) == external, "External GPU type/bytes were overwritten");
        registry.write(java, original);
    }
    { auto value = GpuPreferenceGuard::acquire(java); registry.write(java, std::nullopt);
      require(value.restore() == GpuPreferenceRestore::ExternalChange && !registry.read(java), "External GPU deletion was undone"); }
    // Win32 normalizes an unterminated REG_SZ to a terminated string while storing it,
    // so a raw 2-byte "X\0" fixture cannot round-trip. The pure merge test still rejects
    // that exact byte sequence; here the registry can only hold genuinely malformed data.
    for (const auto& invalid : {GpuRegistryValue{3, {1, 2}}, GpuRegistryValue{1, {1}}, GpuRegistryValue{1, {0, 0xd8, 0, 0}},
                                GpuRegistryValue{1, {}}, string_value(std::u16string(u"A\0B", 3))}) {
        registry.write(java, invalid);
        require(failure([&] { (void)GpuPreferenceGuard::acquire(java); }) == "InvalidArgument" && registry.read(java) == invalid,
                "Invalid registry data was changed or leased");
    }
    const GpuRegistryValue oversized{1, std::vector<std::uint8_t>(gpu_preference_max_bytes + 2, 0)};
    registry.write(java, oversized);
    require(failure([&] { (void)GpuPreferenceGuard::acquire(java); }) == "LimitExceeded" && registry.read(java) == oversized, "Registry 64KiB read bound was bypassed");
    registry.write(java, std::nullopt);
    { auto value = GpuPreferenceGuard::acquire(java); }
    require(!registry.read(java), "Failed GPU acquisition leaked a same-Java lease");
    std::cout << "PASS isolated GPU guard: canonical/case-insensitive exclusion, concurrent refusal, move, restore/delete, external change/type/deletion, invalid bounded reads\n";
}

void test_spawn_failure(const Fixture& fixture, const IsolatedRegistry& registry) {
    const auto image = fixture.root / "invalid-image.exe";
    atomic_write_text(image, "synthetic invalid PE image");
    // Intentionally invalid PE image: keep the failure a hard error, not a dialog.
    const SuppressCriticalErrorDialogs suppress_dialogs;
    const auto original = string_value(u"AutoHDREnable=1;GpuPreference=1;");
    registry.write(fixture.java, original);
    auto state = std::make_shared<Callbacks>();
    auto options = game_options(image, fixture.java, state);
    require(failure([&] { (void)OwnedProcess::spawn(options); }) == "IoError", "Invalid PE image did not fail actual CreateProcess");
    require(registry.read(fixture.java) == original && state->warnings == 0, "Spawn failure leaked an applied GPU preference");
    { auto value = GpuPreferenceGuard::acquire(fixture.java); }
    require(registry.read(fixture.java) == original, "Spawn failure leaked the selected-Java exclusion");
    std::cout << "PASS GPU spawn failure: actual bad-image CreateProcess restores original and releases lease\n";
}
void test_root_exit(const fs::path& mock, const Fixture& fixture, const IsolatedRegistry& registry) {
    const auto marker = fixture.root / "gpu-descendant.txt";
    const auto original = string_value(u"AutoHDREnable=1;GpuPreference=1;");
    registry.write(fixture.java, original);
    auto state = std::make_shared<Callbacks>();
    auto options = game_options(mock, fixture.java, state);
    options.arguments = {"--spawn-pipe-holder", path_to_utf8(marker), "8000"};
    auto game = OwnedProcess::spawn(options);
    require(eventually([&] { return state->ready && !game.running() && registry.read(fixture.java) == original; }), "GPU preference waited for descendant EOF rather than real root exit");
    require(!game.wait(30ms) && state->eof[0] == 0 && state->eof[1] == 0, "GPU root exit fabricated pipe completion");
    require(failure([&] { (void)find_game_window(game); }) == "PermissionDenied", "Exited game root remained authorized for window search");
    game.stop_owned_game(); game.stop_owned_game();
    const auto result = game.wait(5s);
    require(result && result->exit_code == 29 && result->terminated && result->stdout_eof && result->stderr_eof &&
            state->eof[0] == 1 && state->eof[1] == 1 && state->warnings == 0 && !fs::exists(marker),
            "GPU release discarded the retained descendant Job or real EOF contract");
    std::cout << "PASS GPU real root exit: restore before held descendant EOF, exited window denial, retained Job explicit stop and dualEOF\n";
}
void test_session_retention(const fs::path& mock, const Fixture& fixture, const IsolatedRegistry& registry) {
    const auto original = string_value(u"AutoHDREnable=1;GpuPreference=1;");
    const auto applied = high_performance_gpu_value(original);
    for (const bool detach : {false, true}) {
        registry.write(fixture.java, original);
        auto state = std::make_shared<Callbacks>();
        auto options = game_options(mock, fixture.java, state);
        const auto marker = fixture.root / (detach ? "gpu-detach.txt" : "gpu-destructor.txt");
        options.arguments = {"--game-marker", path_to_utf8(marker), "400"};
        std::unique_ptr<RootObservation> observation;
        {
            auto game = OwnedProcess::spawn(options);
            observation = std::make_unique<RootObservation>(game);
            require(registry.read(fixture.java) == applied && !registry.read(mock), "Selected Java was replaced with wrapper/root registry key");
            Cancellation cancellation; cancellation.cancel();
            require(failure([&] { (void)game.wait(10ms, &cancellation); }) == "Cancelled" && game.running() && registry.read(fixture.java) == applied,
                    "Game cancellation restored GPU state early or killed root");
            if (detach) {
                game.detach();
                require(!game.running() && game.pid() == 0 && registry.read(fixture.java) == applied, "Detach restored GPU state before root exit");
                require(failure([&] { (void)find_game_window(game); }) == "PermissionDenied", "Detached game retained window authority");
            }
        }
        require(WaitForSingleObject(observation->handle, 0) == WAIT_TIMEOUT && registry.read(fixture.java) == applied, "Game destructor restored GPU state early or killed root");
        require(WaitForSingleObject(observation->handle, 5000) == WAIT_OBJECT_0 && eventually([&] { return registry.read(fixture.java) == original; }) &&
                fs::exists(marker) && state->warnings == 0, "Detached/destructed session did not restore at actual surviving root exit");
    }
    std::cout << "PASS GPU lifetime: cancellation, detach and destructor leave root/preference live; retained reaper restores at root exit\n";
}
void test_restore_warning(const fs::path& mock, const Fixture& fixture, const IsolatedRegistry& registry) {
    const auto original = string_value(u"AutoHDREnable=1;GpuPreference=1;");
    registry.write(fixture.java, original);
    auto state = std::make_shared<Callbacks>();
    auto options = game_options(mock, fixture.java, state);
    options.arguments = {"--sleep", "300"};
    const auto caller = std::this_thread::get_id();
    options.gpu_preference_warning = [state, caller](const Error& error) {
        state->warnings.fetch_add(1);
        state->warning_off_main = std::this_thread::get_id() != caller;
        state->warning_code_ok = error.code == "LimitExceeded";
        throw std::runtime_error("Synthetic warning callback exception");
    };
    auto game = OwnedProcess::spawn(options);
    const GpuRegistryValue changed{3, std::vector<std::uint8_t>(gpu_preference_max_bytes + 2, 0x5a)};
    registry.write(fixture.java, changed);
    const auto result = game.wait(5s);
    require(result && result->exit_code == 0 && result->stdout_eof && result->stderr_eof && state->warnings == 1 &&
            state->warning_off_main && state->warning_code_ok && registry.read(fixture.java) == changed,
            "Restore read failure changed user data, lost reaper warning or failed game drain");
    registry.write(fixture.java, original);
    { auto value = GpuPreferenceGuard::acquire(fixture.java); }
    require(registry.read(fixture.java) == original, "Failed restore retained the process-local GPU lease");
    std::cout << "PASS GPU restore warning: external oversized data preserved, root-reaper callback, callback exception ignored, lease released\n";
}
#endif

void test_warning_continuation(const fs::path& mock, const Fixture& fixture) {
    auto state = std::make_shared<Callbacks>();
    auto options = game_options(mock, fixture.root / "missing-java.exe", state);
    options.arguments = {"--sleep", "20"};
    options.gpu_preference_warning = [state](const Error&) { state->warnings.fetch_add(1); throw std::runtime_error("Synthetic warning callback exception"); };
    auto game = OwnedProcess::spawn(options);
    const auto result = game.wait(5s);
    require(result && result->exit_code == 0 && result->stdout_eof && result->stderr_eof && state->warnings == 1, "GPU acquisition failure or warning exception prevented Game spawn/drain");
    state->warnings = 0;
    options.high_performance_gpu = false;
    { auto disabled = OwnedProcess::spawn(options); require(disabled.wait(5s).has_value() && state->warnings == 0, "Disabled GPU setting still acquired or warned"); }
    for (const auto purpose : {ProcessPurpose::Preparation, ProcessPurpose::Service}) {
        state->warnings = 0;
        options.high_performance_gpu = true;
        options.purpose = purpose;
        { auto other = OwnedProcess::spawn(options); require(other.wait(5s).has_value() && state->warnings == 1, "GPU purpose misuse prevented non-game spawn or lacked warning"); }
    }
    std::cout << "PASS GPU warning-only acquisition: selected path failure, throwing callback, default-off behavior and non-Game misuse continuation\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Mock child path is required");
        const auto mock = fs::absolute(path_from_utf8(argv[1]));
        test_merge();
        Fixture fixture;
#ifdef _WIN32
        require(high_performance_gpu_supported(), "Windows GPU adapter availability is false");
        IsolatedRegistry registry;
        test_guard(fixture, registry);
        test_spawn_failure(fixture, registry);
        test_root_exit(mock, fixture, registry);
        test_session_retention(mock, fixture, registry);
        test_restore_warning(mock, fixture, registry);
#else
        require(!high_performance_gpu_supported(), "Unsupported platform advertised GPU preferences");
        require(failure([&] { (void)GpuPreferenceGuard::acquire(fixture.java); }) == "Unsupported", "POSIX GPU guard did not fail explicitly");
#endif
        test_warning_continuation(mock, fixture);
        return 0;
    } catch (const ErrorException& error) {
        std::cerr << "FAIL " << error.error().code << ": " << error.what() << " details=" << error.error().details.dump() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
    }
    return 1;
}
