#include "live_environment.hpp"
#include "filesystem.hpp"
#include "maven.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/path_guard.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace test::live {
using namespace nickel::minecraft;
using nickel::Error;
using nickel::ErrorException;
using nickel::Json;
namespace fs = std::filesystem;
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

[[noreturn]] void invalid(std::string message) {
    throw ErrorException(Error{"minecraft.live.environment", std::move(message)});
}

bool inside(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() &&
           std::none_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; });
}

std::size_t jvm_end(const std::vector<std::string>& arguments) {
    constexpr std::array<std::string_view, 12> takesValue{
        "-cp", "-classpath", "--class-path", "-p", "--module-path", "--upgrade-module-path",
        "--add-modules", "--limit-modules", "--add-exports", "--add-opens", "--add-reads", "--patch-module"};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "-jar" || argument == "-m" || argument == "--module") return index;
        if (argument.empty() || argument.front() != '-') return index;
        if (std::find(takesValue.begin(), takesValue.end(), argument) != takesValue.end()) {
            if (++index == arguments.size()) invalid("Java option is missing its value");
        }
    }
    return arguments.size();
}

#ifdef _WIN32
[[noreturn]] void win_error(const char* message) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle) : value(handle) { if (value == INVALID_HANDLE_VALUE) win_error("Cannot open live evidence handle"); }
    ~Handle() { CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::wstring final_name(HANDLE handle) {
    const auto needed = GetFinalPathNameByHandleW(handle, nullptr, 0, VOLUME_NAME_GUID);
    if (!needed) win_error("Cannot resolve actual live volume");
    std::wstring result(needed + 1, L'\0');
    const auto count = GetFinalPathNameByHandleW(handle, result.data(), static_cast<DWORD>(result.size()), VOLUME_NAME_GUID);
    if (!count || count >= result.size()) win_error("Cannot read actual live volume");
    result.resize(count);
    return result;
}

std::wstring volume_root(std::wstring_view finalPath) {
    if (!finalPath.starts_with(L"\\\\?\\Volume{")) invalid("Live path did not resolve to a local volume GUID");
    const auto end = finalPath.find(L"}\\");
    if (end == std::wstring_view::npos) invalid("Live volume GUID is incomplete");
    return std::wstring(finalPath.substr(0, end + 2));
}

Json volume_json(const fs::path& logical, HANDLE handle) {
    const auto final = final_name(handle);
    return {{"logicalPath", path_utf8(logical)}, {"finalPath", path_utf8(fs::path(final))},
            {"volumeGuid", path_utf8(fs::path(volume_root(final)))}};
}

Json free_space(const std::wstring& volume) {
    ULARGE_INTEGER available{}, total{}, free{};
    if (!GetDiskFreeSpaceExW(volume.c_str(), &available, &total, &free)) win_error("Cannot sample actual live volume capacity");
    return {{"volumeGuid", path_utf8(fs::path(volume))}, {"availableBytes", available.QuadPart},
            {"totalBytes", total.QuadPart}, {"totalFreeBytes", free.QuadPart}};
}
#endif

}

bool capacity_sufficient(std::uint64_t cAvailable, std::uint64_t dAvailable, std::uint64_t requiredD) noexcept {
    return cAvailable >= cMinimumBytes && dAvailable >= requiredD;
}

struct Environment::Impl {
    fs::path cache, temp, home, evidence;
    mutable std::mutex mutex;
    mutable std::map<fs::path, nickel::foundation_detail::DirectoryGuard> directories;
#ifdef _WIN32
    nickel::foundation_detail::DirectoryGuard cRoot{fs::path(L"C:\\")};
    nickel::foundation_detail::DirectoryGuard dRoot{fs::path(L"D:\\")};
    std::wstring cVolume, dVolume;
#endif

    explicit Impl(fs::path selected) : cache(nickel::foundation_detail::checked_absolute_path(selected)),
        temp(cache / "temp"), home(cache / "home"), evidence(cache / "evidence") {
#ifdef _WIN32
        if (!selected.is_absolute()) invalid("Live cache must be an absolute logical path");
        cVolume = volume_root(final_name(reinterpret_cast<HANDLE>(cRoot.native_handle())));
        dVolume = volume_root(final_name(reinterpret_cast<HANDLE>(dRoot.native_handle())));
        if (cVolume == dVolume) invalid("C and D resolve to the same actual volume");
        hold(cache, false);
        const auto marker = cache / ".nickel-minecraft-live-cache";
        reject_links(marker);
        nickel::foundation_detail::reject_file_hardlinks(marker);
        if (nickel::read_text_bounded(marker, 128) != markerBytes) invalid("Live cache ownership marker does not match");
        for (const auto& entry : fs::recursive_directory_iterator(filesystem_path(cache))) reject_links(entry.path());
        for (const auto& name : {"managed", "games", "evidence", "temp", "home"}) hold(cache / name, false);
#else
        invalid("Physical D-volume live acceptance requires Windows");
#endif
    }

    nickel::foundation_detail::DirectoryGuard& hold(const fs::path& selected, bool create) const {
        const auto path = nickel::foundation_detail::checked_absolute_path(selected);
        if (!inside(cache, path)) invalid("Live child directory is outside the marked owned cache");
        const auto existing = directories.find(path);
        if (existing != directories.end()) { existing->second.verify(); return existing->second; }
        reject_links(path);
        nickel::foundation_detail::DirectoryGuard guard(path, create);
#ifdef _WIN32
        if (volume_root(final_name(reinterpret_cast<HANDLE>(guard.native_handle()))) != dVolume)
            invalid("Live child directory is not on the verified physical D volume");
#endif
        guard.verify();
        return directories.emplace(path, std::move(guard)).first->second;
    }

    void verify() const {
#ifdef _WIN32
        cRoot.verify(); dRoot.verify();
#endif
        for (const auto& [path, directory] : directories) { (void)path; directory.verify(); }
    }
};

Environment::Environment(fs::path cache) : impl_(std::make_unique<Impl>(std::move(cache))) {}
Environment::~Environment() = default;
const fs::path& Environment::cache() const noexcept { return impl_->cache; }
const fs::path& Environment::temp() const noexcept { return impl_->temp; }
const fs::path& Environment::home() const noexcept { return impl_->home; }
const fs::path& Environment::evidence() const noexcept { return impl_->evidence; }

Json Environment::layout() const {
    std::lock_guard lock(impl_->mutex);
    impl_->verify();
    Json result = Json::object();
#ifdef _WIN32
    for (const auto& name : {"cache", "managed", "games", "evidence", "temp", "home"}) {
        const auto path = name == std::string_view("cache") ? impl_->cache : impl_->cache / name;
        const auto& directory = impl_->hold(path, false);
        result[name] = volume_json(path, reinterpret_cast<HANDLE>(directory.native_handle()));
    }
#endif
    return result;
}

Json Environment::inspect_file(const fs::path& selected) const {
    std::lock_guard lock(impl_->mutex);
    const auto path = nickel::foundation_detail::checked_absolute_path(selected);
    if (!inside(impl_->cache, path)) invalid("Live tempfile is outside the marked cache");
    impl_->hold(path.parent_path(), false);
    reject_links(path);
    nickel::foundation_detail::reject_file_hardlinks(path);
#ifdef _WIN32
    const auto native = filesystem_path(path);
    Handle handle(CreateFileW(native.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.value, &info)) win_error("Cannot inspect live tempfile");
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks != 1)
        invalid("Live tempfile is a link or directory");
    if (volume_root(final_name(handle.value)) != impl_->dVolume) invalid("Live tempfile is not on physical D");
    return volume_json(path, handle.value);
#else
    return Json::object();
#endif
}

Json Environment::sample(std::string_view phase) const {
    std::lock_guard lock(impl_->mutex);
    impl_->verify();
#ifdef _WIN32
    auto c = free_space(impl_->cVolume), d = free_space(impl_->dVolume);
    const auto cBytes = c["availableBytes"].get<std::uint64_t>(), dBytes = d["availableBytes"].get<std::uint64_t>();
    return {{"phase", phase}, {"c", std::move(c)}, {"d", std::move(d)},
            {"globalGatePassed", capacity_sufficient(cBytes, dBytes)},
            {"cMinimumBytes", cMinimumBytes}, {"dMinimumBytes", dMinimumBytes},
            {"gameMinimumBytes", gameMinimumBytes}, {"runtimeMinimumBytes", runtimeMinimumBytes},
            {"progressMinimumBytes", progressMinimumBytes}};
#else
    (void)phase;
    return Json::object();
#endif
}

void Environment::require_capacity(std::string_view phase, std::uint64_t requiredD) const {
    const auto sampled = sample(phase);
    if (!capacity_sufficient(sampled.at("c").at("availableBytes").get<std::uint64_t>(),
                             sampled.at("d").at("availableBytes").get<std::uint64_t>(), requiredD))
        throw ErrorException(Error{"minecraft.live.capacity", "Actual volume capacity gate failed", sampled});
}

ProcessSpec Environment::constrain(const ProcessSpec& original) const {
    std::lock_guard lock(impl_->mutex);
    impl_->verify();
    auto spec = original;
    const bool java = [&] {
        const auto name = lower(path_utf8(spec.executable.filename()));
        return name == "java.exe" || name == "javaw.exe" || name == "java" || name == "javaw";
    }();
    auto home = impl_->home;
    const auto end = java ? jvm_end(spec.arguments) : 0;
    if (java) {
        for (std::size_t index = 0; index < end; ++index) {
            if (spec.arguments[index].starts_with("-Duser.home=")) {
                home = fs::u8path(spec.arguments[index].substr(12));
                if (!home.is_absolute()) invalid("Java user.home must be absolute");
                impl_->hold(home, true);
            }
        }
    }
    // Core Java probes normally use the read-only executable directory as cwd.
    const bool propertyProbe = java && spec.purpose == ProcessKind::Preparation &&
        original.arguments == std::vector<std::string>{"-XshowSettings:properties", "-version"} &&
        original.workingDirectory == original.executable.parent_path();
    if (spec.workingDirectory.empty() || propertyProbe) spec.workingDirectory = home;
    impl_->hold(spec.workingDirectory, true);
    const std::map<std::string, fs::path, std::less<>> imposed{
        {"TEMP", impl_->temp}, {"TMP", impl_->temp}, {"TMPDIR", impl_->temp},
        {"HOME", home}, {"USERPROFILE", home}, {"APPDATA", home}, {"LOCALAPPDATA", home}};
    for (const auto& [name, path] : imposed) {
        const auto folded = lower(name);
        for (auto it = spec.environment.begin(); it != spec.environment.end();) {
            if (lower(it->first) != folded) { ++it; continue; }
            const auto existing = fs::u8path(it->second);
            if (!existing.is_absolute()) invalid("Live child environment path must be absolute");
            impl_->hold(existing, true);
            it = spec.environment.erase(it);
        }
        spec.environment[name] = path_utf8(path);
        std::erase_if(spec.unsetEnvironment, [&](const auto& value) { return lower(value) == folded; });
    }
    for (const auto& name : {"JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS"}) {
        std::erase_if(spec.environment, [&](const auto& item) { return lower(item.first) == lower(name); });
        if (std::none_of(spec.unsetEnvironment.begin(), spec.unsetEnvironment.end(), [&](const auto& item) { return lower(item) == lower(name); }))
            spec.unsetEnvironment.emplace_back(name);
    }
    if (java) {
        const std::array<std::pair<std::string_view, fs::path>, 7> properties{{
            {"-Djava.io.tmpdir=", impl_->temp}, {"-Duser.home=", home},
            {"-Djna.tmpdir=", impl_->temp / "jna"},
            {"-Dorg.lwjgl.system.SharedLibraryExtractPath=", impl_->temp / "lwjgl"},
            {"-Dio.netty.native.workdir=", impl_->temp / "netty"},
            {"-XX:ErrorFile=", impl_->evidence / "hs_err_pid%p.log"},
            {"-XX:HeapDumpPath=", impl_->evidence / "java_pid%p.hprof"}}};
        std::vector<std::string> prefix;
        for (const auto& [key, fallback] : properties) {
            bool found = false;
            for (std::size_t index = 0; index < end; ++index) {
                if (!spec.arguments[index].starts_with(key)) continue;
                if (found) invalid("Live Java has a duplicate write-directory option");
                found = true;
                const auto existing = fs::u8path(spec.arguments[index].substr(key.size()));
                if (!existing.is_absolute()) invalid("Live Java write-directory option must be absolute");
                if (key.starts_with("-XX:")) {
                    if (!inside(impl_->cache, existing)) invalid("Live Java error output escapes its cache");
                    impl_->hold(existing.parent_path(), true); reject_links(existing);
                } else impl_->hold(existing, true);
            }
            if (!found) {
                impl_->hold(key.starts_with("-XX:") ? fallback.parent_path() : fallback, true);
                prefix.emplace_back(std::string(key) + path_utf8(fallback));
            }
        }
        spec.arguments.insert(spec.arguments.begin(), prefix.begin(), prefix.end());
    }
    return spec;
}

Services constrain_services(Services services, std::shared_ptr<Environment> environment, SpawnRecorder beforeSpawn) {
    if (!environment || !services.spawn) invalid("Live owned-process services are unavailable");
    const auto spawn = services.spawn;
    services.spawn = [spawn, environment, beforeSpawn](const ProcessSpec& original, const OutputCallback& output) {
        auto spec = environment->constrain(original);
        if (beforeSpawn) beforeSpawn(spec);
        return spawn(spec, output);
    };
    // Foundation discovery has its own spawn path, so an explicit, probed Java is mandatory in this fixture.
    services.discoverJava = [](const CancelCheck& check) -> std::vector<JavaCandidate> {
        if (check) check();
        invalid("Live fixture requires explicit Java; uncontrolled discovery probes are disabled");
    };
    if (services.forOperation) {
        const auto bind = services.forOperation;
        services.forOperation = [bind, environment, beforeSpawn](const nickel::Cancellation& cancellation) {
            return constrain_services(bind(cancellation), environment, beforeSpawn);
        };
    }
    return services;
}

}
