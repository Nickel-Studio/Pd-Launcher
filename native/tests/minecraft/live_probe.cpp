#include "installer.hpp"
#include "launcher.hpp"
#include "runtime.hpp"
#include "maven.hpp"
#include "services_native.hpp"
#include "live_environment.hpp"
#include "filesystem.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/path_guard.hpp"
#include "core/network/hash.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#define NOMINMAX
#include <windows.h>
#undef CopyFile

using namespace nickel::minecraft;
using nickel::Cancellation;
using nickel::Error;
using nickel::ErrorException;
using nickel::Json;
namespace fs = std::filesystem;
namespace net = nickel::transport;
namespace live = test::live;
namespace {
struct Options {
    fs::path cache, java, cancelFile;
    std::string minecraft = "1.20.1", instance = "native-live";
    std::vector<LoaderRequest> loaders;
    unsigned runtimeMajor = 0, observeSeconds = 60, timeoutSeconds = 1800;
    bool launch = false, runtimeOnly = false, mirrors = true, javaProofOnly = false, javaVersionOnly = false;
};
unsigned number(std::string_view value, unsigned maximum) {
    std::size_t used = 0;
    const auto parsed = std::stoul(std::string(value), &used);
    if (used != value.size() || !parsed || parsed > maximum) throw std::invalid_argument("Invalid positive probe limit");
    return static_cast<unsigned>(parsed);
}
Options options(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string key = argv[index];
        const auto value = [&]() -> std::string {
            if (++index >= argc) throw std::invalid_argument("Probe option has no value");
            return argv[index];
        };
        if (key == "--cache") result.cache = fs::u8path(value());
        else if (key == "--java") result.java = fs::u8path(value());
        else if (key == "--cancel-file") result.cancelFile = fs::u8path(value());
        else if (key == "--minecraft") result.minecraft = value();
        else if (key == "--instance") result.instance = value();
        else if (key == "--loader") {
            const auto selected = value(); const auto at = selected.find('@');
            if (at == std::string::npos) throw std::invalid_argument("Loader must be kind@exact-version");
            result.loaders.push_back({parse_loader_kind(selected.substr(0, at)), selected.substr(at + 1), {}});
        } else if (key == "--runtime-major") result.runtimeMajor = number(value(), 25);
        else if (key == "--observe-seconds") result.observeSeconds = number(value(), 300);
        else if (key == "--timeout-seconds") result.timeoutSeconds = number(value(), 7200);
        else if (key == "--launch") result.launch = true;
        else if (key == "--runtime-only") result.runtimeOnly = true;
        else if (key == "--java-proof-only") result.javaProofOnly = true;
        else if (key == "--java-version-only") result.javaVersionOnly = true;
        else if (key == "--official-only") result.mirrors = false;
        else throw std::invalid_argument("Unknown live probe option");
    }
    if (result.cache.empty() || !result.cache.is_absolute() || !fs::is_directory(filesystem_path(result.cache)))
        throw std::invalid_argument("Probe requires an existing explicitly provisioned absolute cache directory");
    if (!result.java.is_absolute() || !fs::is_regular_file(filesystem_path(result.java)))
        throw std::invalid_argument("D-volume probe requires explicit existing genuine Java");
    validate_version_id(result.minecraft); validate_launch_id(result.instance);
    if (result.javaVersionOnly && (result.javaProofOnly || result.launch || result.runtimeOnly || result.runtimeMajor || !result.loaders.empty()))
        throw std::invalid_argument("Version-only proof cannot install, run source mode or launch a Game");
    if (result.runtimeOnly && !result.runtimeMajor) throw std::invalid_argument("Runtime-only probe requires --runtime-major");
    if (result.runtimeMajor && result.runtimeMajor != 8 && result.runtimeMajor != 11 && result.runtimeMajor != 17 && result.runtimeMajor != 21 && result.runtimeMajor != 25)
        throw std::invalid_argument("Probe installs supported LTS runtimes only");
    if (!result.cancelFile.empty() && (!result.cancelFile.is_absolute() || result.cancelFile.parent_path() != result.cache / "evidence"))
        throw std::invalid_argument("Cancellation signal must be in the owned evidence directory");
    return result;
}

Json artifact(const fs::path& path) {
    reject_links(path);
    return {{"path", path_utf8(path)}, {"bytes", fs::file_size(filesystem_path(path))},
            {"sha256", net::detail::hash_file(path, {net::HashAlgorithm::Sha256}).front().hex}};
}

struct Evidence {
    std::mutex mutex;
    std::ofstream events, standardOutput, standardError;
    std::string recent;
    std::uint64_t eventBytes = 0, omittedEvents = 0, rawBytes = 0, omittedRawBytes = 0;
    bool resourcesStarted = false, textureAtlas = false, stdoutEof = false, stderrEof = false, exited = false;
    explicit Evidence(const fs::path& directory, const std::string& id) :
        events(filesystem_path(directory / (id + ".events.jsonl")), std::ios::binary),
        standardOutput(filesystem_path(directory / (id + ".stdout.log")), std::ios::binary),
        standardError(filesystem_path(directory / (id + ".stderr.log")), std::ios::binary) {
        if (!events || !standardOutput || !standardError) throw std::runtime_error("Cannot open owned acceptance evidence files");
    }
    void raw(bool error, std::string_view bytes) {
        std::lock_guard lock(mutex);
        constexpr std::uint64_t budget = 16ULL * 1024 * 1024;
        const auto written = std::min<std::uint64_t>(bytes.size(), budget - rawBytes);
        auto& stream = error ? standardError : standardOutput;
        stream.write(bytes.data(), static_cast<std::streamsize>(written)); stream.flush();
        if (!stream) throw std::runtime_error("Cannot write owned raw Game output");
        rawBytes += written; omittedRawBytes += bytes.size() - written;
        if (bytes.size() >= 256 * 1024) recent.assign(bytes.substr(bytes.size() - 256 * 1024));
        else { recent.append(bytes); if (recent.size() > 256 * 1024) recent.erase(0, recent.size() - 256 * 1024); }
        if (recent.find("Reloading ResourceManager") != std::string::npos || recent.find("OpenAL initialized") != std::string::npos) resourcesStarted = true;
        if (recent.find("Created:") != std::string::npos && recent.find("textures/atlas") != std::string::npos) textureAtlas = true;
    }
    void event(const LaunchEvent& event) {
        std::lock_guard lock(mutex);
        const auto encoded = Json{{"launchId", event.launchId}, {"kind", event.kind}, {"message", event.message}, {"pid", event.pid},
            {"exitCode", event.exitCode ? Json(*event.exitCode) : Json(nullptr)}}.dump();
        constexpr std::uint64_t budget = 16ULL * 1024 * 1024;
        if (encoded.size() + 1 <= budget - eventBytes) { events << encoded << '\n'; events.flush(); eventBytes += encoded.size() + 1; }
        else ++omittedEvents;
        if (event.kind == "stdout-eof") { stdoutEof = true; standardOutput.close(); }
        if (event.kind == "stderr-eof") { stderrEof = true; standardError.close(); }
        if (event.kind == "exited") exited = true;
        if (event.kind != "stdout" && event.kind != "stderr") std::cout << event.kind << " pid=" << event.pid << '\n' << std::flush;
    }
    void spawn(const ProcessSpec& spec) {
        std::lock_guard lock(mutex);
        events << Json{{"kind", "owned-spawn-spec"}, {"executable", path_utf8(spec.executable)},
            {"cwd", path_utf8(spec.workingDirectory)}, {"arguments", spec.arguments}, {"environment", spec.environment},
            {"unsetEnvironment", spec.unsetEnvironment}, {"purpose", static_cast<int>(spec.purpose)}}.dump() << '\n';
        events.flush();
        if (!events) throw std::runtime_error("Cannot record owned child specification before spawn");
    }
};

Json java_json(const JavaSelection& java) {
    return Json{{"path", java.path ? Json(path_utf8(*java.path)) : Json(nullptr)}, {"major", java.major},
        {"bits", java.bits}, {"source", java.source}, {"compatible", java.compatible}};
}
void save(Services& services, const fs::path& path, const Json& value) {
    Cancellation uncancelled;
    services.atomicWrite(path, value.dump(2), cancellation_check(services, uncancelled));
}
Services observed_services(Services services, const std::shared_ptr<Evidence>& evidence) {
    const auto download = services.download;
    services.download = [download, evidence](const DownloadFile& file, const CancelCheck& check, const ProgressCallback& progress) {
        try { download(file, check, progress); }
        catch (...) { evidence->event({{}, "artifact-failed", "Managed artifact: " + path_utf8(file.destination.filename())}); throw; }
    };
    const auto fetch = services.fetch;
    services.fetch = [fetch, evidence](std::string_view url, std::uint64_t maximum, const CancelCheck& check) {
        try { return fetch(url, maximum, check); }
        catch (...) { evidence->event({{}, "metadata-failed", "Public metadata request failed"}); throw; }
    };
    const auto spawn = services.spawn;
    services.spawn = [spawn, evidence](const ProcessSpec& spec, const OutputCallback& output) {
        if (spec.purpose == ProcessKind::Game)
            return spawn(spec, [evidence](bool error, std::string_view bytes) { evidence->raw(error, bytes); });
        return spawn(spec, output);
    };
    if (services.forOperation) {
        const auto bind = services.forOperation;
        services.forOperation = [bind, evidence](const Cancellation& cancellation) {
            return observed_services(bind(cancellation), evidence);
        };
    }
    return services;
}

std::string java_output_text(std::string_view bytes) {
    if (nickel::valid_utf8(bytes)) return std::string(bytes);
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(INT_MAX)) throw std::runtime_error("Java output decoding quota exceeded");
    const auto units = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (!units) throw std::runtime_error("Java output cannot be decoded with the system code page");
    std::wstring wide(static_cast<std::size_t>(units), L'\0');
    if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), wide.data(), units) != units)
        throw std::runtime_error("Java output changed while decoding");
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), units, nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Java output cannot be converted to UTF-8");
    std::string text(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), units, text.data(), size, nullptr, nullptr) != size)
        throw std::runtime_error("Java output UTF-8 conversion failed");
    return text;
}


std::string owned_java(Services& services, const fs::path& executable, std::vector<std::string> arguments,
                       const CancelCheck& check, const std::shared_ptr<live::Environment>& environment,
                       const fs::path& receipt) {
    struct Capture { std::mutex mutex; std::string text, standardOutput, standardError; };
    auto capture = std::make_shared<Capture>();
    ProcessSpec spec; spec.executable = executable; spec.workingDirectory = environment->home();
    spec.arguments = std::move(arguments); spec.purpose = ProcessKind::Preparation; spec.terminateOnDrop = true;
    Json result{{"phase", path_utf8(receipt.filename())}, {"executable", artifact(executable)}, {"pid", 0},
                {"ownedPidVerified", false}, {"rootExitedVerified", false}, {"authoritativeDrainVerified", false},
                {"eofVerificationSource", "Native OwnedProcess wait/drain; byte callbacks do not carry EOF"}};
    std::shared_ptr<OwnedProcess> child;
    std::exception_ptr failure;
    try {
        child = services.spawn(spec, [capture](bool error, std::string_view bytes) {
            std::lock_guard lock(capture->mutex);
            if (bytes.size() > 256 * 1024 - capture->text.size()) throw std::runtime_error("Java proof output exceeded its quota");
            capture->text.append(bytes);
            (error ? capture->standardError : capture->standardOutput).append(bytes);
        });
        if (child) result["pid"] = child->pid();
        if (!child || !child->pid()) throw std::runtime_error("Java proof did not create an owned child");
        result["ownedPidVerified"] = true;
        const auto code = child->wait(check, true, std::chrono::seconds(30));
        result["exitCode"] = code;
        result["rootExitedVerified"] = !child->running();
        result["authoritativeDrainVerified"] = child->drain_owned(std::chrono::seconds(5));
        if (!result["rootExitedVerified"].get<bool>() || !result["authoritativeDrainVerified"].get<bool>())
            throw std::runtime_error("Java proof did not exit with both output EOFs");
        if (code != 0) throw ErrorException(Error{"minecraft.live.java", "Genuine Java proof exited unsuccessfully",
            {{"phase", path_utf8(receipt.filename())}, {"exitCode", code}, {"pid", child->pid()}}});
    } catch (...) {
        failure = std::current_exception();
        if (child) {
            try { child->terminate_owned(); result["explicitOwnedCleanup"] = true; result["cleanupDrained"] = child->drain_owned(std::chrono::seconds(5)); }
            catch (const std::exception& error) { result["cleanupError"] = error.what(); }
        }
        try { std::rethrow_exception(failure); }
        catch (const ErrorException& error) { result["error"] = {{"code", error.error().code}, {"message", error.what()}, {"details", error.error().details}}; }
        catch (const std::exception& error) { result["error"] = {{"code", "minecraft.live.java"}, {"message", error.what()}}; }
    }
    std::string standardOutput, standardError, text;
    {
        std::lock_guard lock(capture->mutex);
        standardOutput = capture->standardOutput; standardError = capture->standardError;
    }
    const auto preserve = [&](std::string_view stream, std::string_view bytes) {
        const auto path = environment->evidence() / (path_utf8(receipt.filename()) + "." + std::string(stream) + ".bin");
        nickel::create_file_exclusive(path, {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
        return artifact(path);
    };
    result["rawStdout"] = preserve("stdout", standardOutput); result["rawStderr"] = preserve("stderr", standardError);
    result["stdoutRawUtf8"] = nickel::valid_utf8(standardOutput); result["stderrRawUtf8"] = nickel::valid_utf8(standardError);
    result["systemCodePage"] = GetACP();
    try {
        result["stdout"] = java_output_text(standardOutput); result["stderr"] = java_output_text(standardError);
        text = result["stdout"].get<std::string>() + result["stderr"].get<std::string>();
    } catch (const std::exception& error) {
        result["outputDecodeError"] = error.what();
        if (!failure) failure = std::current_exception();
    }
    if (child) {
        const auto crash = environment->evidence() / ("hs_err_pid" + std::to_string(child->pid()) + ".log");
        if (fs::is_regular_file(filesystem_path(crash))) result["hotspotCrash"] = artifact(crash);
    }
    save(services, receipt, result);
    if (failure) std::rethrow_exception(failure);
    return text;
}

Json java_properties(Services& services, const Options& options, const CancelCheck& check,
                     const std::shared_ptr<live::Environment>& environment, std::uint64_t requiredD) {
    environment->require_capacity("before-java-properties", requiredD);
    Json result{{"javaExecutable", artifact(options.java)}, {"before", environment->sample("before-java-properties")}};
    const auto receipt = environment->evidence() / (options.instance + ".java-properties-process.json");
    const auto properties = owned_java(services, options.java, {"-XshowSettings:properties", "-version"}, check, environment, receipt);
    const auto candidate = parse_java_probe(options.java, properties);
    if (candidate.major != 17 || candidate.bits != 64) throw std::runtime_error("Vanilla 1.20.1 proof requires genuine 64-bit Java 17");
    if (properties.find("java.io.tmpdir = " + path_utf8(environment->temp())) == std::string::npos ||
        properties.find("user.home = " + path_utf8(environment->home())) == std::string::npos ||
        properties.find("user.dir = " + path_utf8(environment->home())) == std::string::npos)
        throw std::runtime_error("Java properties do not match the verified D directories");
    const auto output = environment->evidence() / (options.instance + ".java-properties.txt");
    write_managed(services, {output, properties}, check);
    result["properties"] = artifact(output); result["propertiesProcess"] = artifact(receipt);
    result["java"] = {{"major", candidate.major}, {"bits", candidate.bits}};
    result["propertiesVerified"] = true; result["sourceModeStarted"] = false;
    environment->require_capacity("after-java-properties", requiredD);
    return result;
}

Json java_proof(Services& services, const Options& options, const CancelCheck& check,
                const std::shared_ptr<live::Environment>& environment) {
    auto result = java_properties(services, options, check, environment, live::dMinimumBytes);
    const auto source = environment->evidence() / (options.instance + "-TempProof.java");
    constexpr std::string_view program =
        "import java.nio.file.*;\n"
        "class TempProof { public static void main(String[] args) throws Exception {\n"
        " Path file = Files.createTempFile(\"pd-minecraft-\", \".txt\");\n"
        " Files.writeString(file, \"Palladium genuine Java tempfile proof\\n\");\n"
        " System.out.println(\"TEMPFILE=\" + file.toAbsolutePath());\n"
        " System.out.println(\"USER_HOME=\" + System.getProperty(\"user.home\"));\n"
        " System.out.println(\"USER_DIR=\" + System.getProperty(\"user.dir\"));\n"
        " for (String key : new String[]{\"TEMP\",\"TMP\",\"TMPDIR\",\"HOME\",\"USERPROFILE\",\"APPDATA\",\"LOCALAPPDATA\"})\n"
        "  System.out.println(\"ENV_\" + key + \"=\" + System.getenv(key));\n"
        "} }\n";
    environment->require_capacity("before-java-tempfile");
    write_managed(services, {source, std::string(program)}, check);
    const auto tempfileReceipt = environment->evidence() / (options.instance + ".java-tempfile-process.json");
    const auto output = owned_java(services, options.java, {path_utf8(source)}, check, environment, tempfileReceipt);
    write_managed(services, {environment->evidence() / (options.instance + ".java-tempfile.txt"), output}, check);
    const auto start = output.find("TEMPFILE=");
    if (start == std::string::npos) throw std::runtime_error("Genuine Java did not report its created tempfile");
    const auto at = start + 9, end = output.find_first_of("\r\n", at);
    const auto path = fs::u8path(output.substr(at, end - at));
    result["createdFile"] = environment->inspect_file(path);
    result["createdFileArtifact"] = artifact(path);
    result["source"] = artifact(source); result["sourceModeStarted"] = true;
    result["tempfileProcess"] = artifact(tempfileReceipt);
    result["environmentOutput"] = artifact(environment->evidence() / (options.instance + ".java-tempfile.txt"));
    result["after"] = environment->sample("after-java-tempfile");
    environment->require_capacity("after-java-tempfile");
    return result;
}

struct WindowSearch { DWORD pid; HWND window = nullptr; };
BOOL CALLBACK find_window(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid == search.pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) { search.window = window; return FALSE; }
    return TRUE;
}
Json capture_window(std::uint64_t pid, const fs::path& destination) {
    WindowSearch search{static_cast<DWORD>(pid)}; EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
    if (!search.window) return {{"captured", false}, {"reason", "No visible window belongs to the owned Game PID"}};
    RECT rect{}; if (!GetClientRect(search.window, &rect)) throw std::runtime_error("Cannot measure owned Game window");
    const auto width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) throw std::runtime_error("Owned Game window capture exceeds its dimension quota");
    const auto dc = GetDC(search.window); if (!dc) throw std::runtime_error("Cannot capture owned Game window");
    const auto memory = CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr; const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memory || !bitmap) { if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); ReleaseDC(search.window, dc); throw std::runtime_error("Cannot allocate owned Game capture surface"); }
    const auto old = SelectObject(memory, bitmap);
    const bool copied = BitBlt(memory, 0, 0, width, height, dc, 0, 0, SRCCOPY | CAPTUREBLT);
    try {
        if (!copied) throw std::runtime_error("Owned Game window capture failed");
        const auto bytes = static_cast<DWORD>(width * height * 4);
        BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader); header.bfSize = header.bfOffBits + bytes;
        std::ofstream stream(filesystem_path(destination), std::ios::binary);
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        stream.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        stream.write(static_cast<const char*>(pixels), bytes); stream.flush();
        if (!stream) throw std::runtime_error("Cannot save owned Game capture on D");
    } catch (...) { SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(search.window, dc); throw; }
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(search.window, dc);
    return {{"captured", true}, {"gamePid", pid}, {"width", width}, {"height", height}, {"artifact", artifact(destination)}, {"visualMainMenuVerified", false}};
}
}

int main(int argc, char** argv) {
    Json result{{"tier", "real-public-installation"}, {"status", "failed"}, {"installationVerified", false},
        {"gameLaunchVerified", false}, {"visualMainMenuVerified", false}, {"javaTempfileVolumeVerified", false},
        {"javaVersionPropertiesVerified", false}};
    std::optional<fs::path> evidencePath;
    std::optional<Services> storage;
    std::shared_ptr<Evidence> evidence;
    std::shared_ptr<live::Environment> environment;
    std::unique_ptr<Launcher> launcher;
    std::unique_ptr<Installer> installer;
    std::shared_ptr<GameSession> game;
    std::string launchId;
    try {
        const auto selected = options(argc, argv);
        const auto requiredD = selected.javaVersionOnly ? live::runtimeMinimumBytes : live::dMinimumBytes;
        launchId = selected.instance;
        environment = std::make_shared<live::Environment>(selected.cache);
        environment->require_capacity("before-native-evidence", requiredD);
        const auto& cache = environment->cache();
        evidencePath = environment->evidence() / (selected.instance + ".json");
        if (fs::exists(filesystem_path(*evidencePath))) throw std::invalid_argument("Acceptance evidence identity already exists");
        evidence = std::make_shared<Evidence>(environment->evidence(), selected.instance);
        net::NetworkSnapshot snapshot; snapshot.version_list_source = net::SourcePolicy::OfficialFirst; snapshot.file_download_source = net::SourcePolicy::OfficialFirst;
        snapshot.connect_timeout = std::chrono::seconds(10); snapshot.total_timeout = std::chrono::seconds(45);
        snapshot.user_agent = "Palladium/native-minecraft-acceptance";
        NativeServiceOptions native; native.useMirrors = selected.mirrors;
        auto services = live::constrain_services(observed_services(make_native_services(std::make_shared<net::HttpClient>(snapshot), native), evidence), environment,
            [evidence, environment, requiredD, versionOnly = selected.javaVersionOnly, java = selected.java](const ProcessSpec& spec) {
                if (versionOnly && (spec.purpose != ProcessKind::Preparation || spec.executable != java))
                    throw std::runtime_error("Version-only proof cannot spawn installers or Games");
                environment->require_capacity("before-owned-child", spec.purpose == ProcessKind::Game ? live::dMinimumBytes : requiredD);
                evidence->spawn(spec);
            });
        storage = services;
        Cancellation cancellation;
        std::mutex capacityMutex;
        std::exception_ptr capacityError;
        const auto begin = std::chrono::steady_clock::now();
        std::jthread monitor([&, cancellation](std::stop_token stop) mutable {
            while (!stop.stop_requested()) {
                try {
                    environment->require_capacity("continuous-native-monitor", requiredD);
                    if (!selected.cancelFile.empty() && fs::exists(filesystem_path(selected.cancelFile))) {
                        reject_links(selected.cancelFile); cancellation.cancel();
                    }
                    if (std::chrono::steady_clock::now() - begin >= std::chrono::seconds(selected.timeoutSeconds)) cancellation.cancel();
                } catch (...) { std::lock_guard lock(capacityMutex); if (!capacityError) capacityError = std::current_exception(); cancellation.cancel(); }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
        const auto checked = [&] {
            { std::lock_guard lock(capacityMutex); if (capacityError) std::rethrow_exception(capacityError); }
            environment->require_capacity("native-phase-check", requiredD); cancellation.check();
        };
        result["cache"] = path_utf8(cache); result["minecraft"] = selected.minecraft; result["instance"] = selected.instance;
        result["network"] = selected.mirrors ? "PublicHttps / OfficialFirst / native HttpClient / mirror fallback enabled" : "PublicHttps / OfficialFirst / native HttpClient / official-only";
        result["mockServices"] = false; result["physicalLayout"] = environment->layout(); result["capacityBefore"] = environment->sample("native-start");
        result["requiredDBytes"] = requiredD;
        result["capacityScope"] = selected.javaVersionOnly ? "Java version/properties only; no source mode, installation or Game" : "Full Java tempfile/Game acceptance; strict 4 GiB D floor";
        checked();
        if (selected.javaVersionOnly) {
            result["tier"] = "real-java-version-properties-only";
            result["javaVersionProof"] = java_properties(services, selected, checked, environment, requiredD);
            result["javaVersionPropertiesVerified"] = true;
            result["status"] = "passed-genuine-java-version-properties-only-not-tempfile-or-game";
            checked(); result["capacityAfter"] = environment->sample("native-java-version-end");
            save(*storage, *evidencePath, result); return 0;
        }
        if (selected.javaProofOnly) {
            result["javaProof"] = java_proof(services, selected, checked, environment);
            result["javaVersionPropertiesVerified"] = true;
            result["javaTempfileVolumeVerified"] = true; result["status"] = "passed-genuine-java-properties-and-tempfile-only";
            checked(); result["capacityAfter"] = environment->sample("native-java-proof-end");
            save(*storage, *evidencePath, result); return 0;
        }
        environment->require_capacity("internal-runtime-or-game", selected.runtimeOnly ? live::runtimeMinimumBytes : live::gameMinimumBytes);
        InstallRequest request; request.root = cache / "managed"; request.gameDirectory = cache / "games" / selected.instance;
        request.versionId = selected.minecraft; request.instanceName = selected.instance; request.reservationId = selected.instance;
        request.loaders = selected.loaders; request.platform = Platform::current(); request.manualJava = selected.java;
        nickel::foundation_detail::DirectoryGuard gameDirectory(request.gameDirectory, true); gameDirectory.verify();
        auto lastProgress = std::chrono::steady_clock::now() - std::chrono::seconds(3);
        const auto progress = [&](const Progress& update) {
            environment->require_capacity("internal-progress", live::progressMinimumBytes); checked();
            const auto now = std::chrono::steady_clock::now();
            if (now - lastProgress >= std::chrono::seconds(2) || update.stage == "processor" || update.stage == "prepared") {
                std::cout << update.stage << ' ' << update.completed << '/' << update.total << " bytes=" << update.downloadedBytes << ' ' << update.message << '\n' << std::flush;
                lastProgress = now;
            }
        };
        if (selected.runtimeMajor) {
            auto operation = services.forOperation(cancellation); auto check = cancellation_check(operation, cancellation);
            const auto component = selected.runtimeMajor == 8 ? "jre-legacy" : selected.runtimeMajor == 17 ? "java-runtime-gamma" : selected.runtimeMajor == 21 ? "java-runtime-delta" : "";
            const Json metadata{{"javaVersion", {{"majorVersion", selected.runtimeMajor}, {"component", component}}}};
            const JavaRequirement requirement{selected.runtimeMajor, selected.runtimeMajor, selected.runtimeMajor, "Explicit live runtime acceptance"};
            const auto runtime = plan_java_runtime(operation, request, requirement, metadata, check);
            execute_java_runtime(operation, runtime, check, progress, false);
            auto java = validate_manual_java(requirement, probe_java(operation, runtime.executable, check)); java.source = "downloaded";
            result["runtime"] = java_json(java); result["runtimeInstallationVerified"] = true; request.manualJava = runtime.executable;
        }
        if (selected.runtimeOnly) {
            checked(); result["status"] = "passed-runtime-only"; result["capacityAfter"] = environment->sample("runtime-end");
            save(*storage, *evidencePath, result); return 0;
        }
        installer = std::make_unique<Installer>(services);
        const auto installed = installer->install(request, cancellation, progress);
        if (!installer->owned_installer_pids().empty() || !installer->drain_owned_installer(std::chrono::seconds(5)))
            throw std::runtime_error("Real installer left undrained owned processes");
        checked(); result["installationVerified"] = true; result["java"] = java_json(installed.java);
        result["files"] = installed.files.size(); result["profile"] = artifact(installed.profilePath);
        result["loaders"] = Json::array(); result["installedArtifacts"] = Json::array();
        for (const auto& loader : installed.loaders) result["loaders"].push_back({{"kind", loader_name(loader.kind)}, {"version", loader.version}});
        for (const auto& file : installed.files) { checked(); result["installedArtifacts"].push_back(artifact(file.destination)); }
        result["javaExecutable"] = artifact(*installed.java.path);
        // This cache is not an instances reservation; production publication remains instances-owned.
        if (selected.launch) {
            LaunchRequest launch; launch.launchId = selected.instance; launch.version = installed; launch.platform = request.platform;
            launch.identity = {"offline", "PdAcceptance", "1234567890abcdef1234567890abcdef", "0"};
            launch.settings.memoryMb = 2048; launch.settings.initialMemoryMb = 512; launch.settings.memoryManagement = "g1";
            launch.settings.width = 854; launch.settings.height = 480;
            launcher = std::make_unique<Launcher>(services);
            game = launcher->launch(launch, cancellation, [evidence](const LaunchEvent& event) { evidence->event(event); });
            result["gamePid"] = game->pid(); result["sessionDiscoverable"] = launcher->session(selected.instance) == game;
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(selected.observeSeconds);
            while (game->running() && std::chrono::steady_clock::now() < end) { checked(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
            const bool wasRunning = game->running();
            if (wasRunning) result["ownedWindowCapture"] = capture_window(game->pid(), environment->evidence() / (selected.instance + ".game.bmp"));
            // This is the test's explicit end-of-observation stop, never a Task-success rollback.
            game->stop_owned(); result["explicitOwnedStop"] = true;
            result["gameExitCode"] = game->wait(std::chrono::seconds(10));
            { std::lock_guard lock(evidence->mutex); result["resourcesStarted"] = evidence->resourcesStarted; result["textureAtlasCreated"] = evidence->textureAtlas;
              result["rawLogBytes"] = evidence->rawBytes; result["omittedRawBytes"] = evidence->omittedRawBytes;
              result["stdoutEof"] = evidence->stdoutEof; result["stderrEof"] = evidence->stderrEof;
              result["authoritativeDrainVerified"] = evidence->stdoutEof && evidence->stderrEof && evidence->exited;
              result["gameLaunchVerified"] = wasRunning && evidence->resourcesStarted && evidence->textureAtlas; }
            if (!result["gameLaunchVerified"].get<bool>()) throw std::runtime_error("Real game initialization markers were not observed before exit or deadline");
        }
        checked(); result["capacityAfter"] = environment->sample("native-end");
        result["status"] = selected.launch ? "passed-install-and-game-initialization-not-visual-acceptance" : "passed-install-only";
        save(*storage, *evidencePath, result); return 0;
    } catch (const ErrorException& error) {
        result["error"] = {{"code", error.error().code}, {"message", error.what()}, {"details", error.error().details}};
    } catch (const std::exception& error) {
        result["error"] = {{"code", "minecraft.live-test"}, {"message", error.what()}};
    }
    if (!game && launcher && !launchId.empty()) game = launcher->session(launchId);
    if (game) {
        try { game->stop_owned(); result["explicitOwnedStopAfterFailure"] = true; result["gameExitCode"] = game->wait(std::chrono::seconds(10)); }
        catch (const std::exception& error) { result["ownedStopError"] = error.what(); }
    }
    if (installer) {
        try { result["installerDrainAfterFailure"] = installer->drain_owned_installer(std::chrono::seconds(10)); }
        catch (const std::exception& error) { result["installerDrainError"] = error.what(); }
    }
    if (environment) { try { result["capacityAfterFailure"] = environment->sample("native-failure"); } catch (...) {} }
    if (evidencePath && storage) { try { save(*storage, *evidencePath, result); } catch (...) {} }
    std::cerr << result.dump(2) << '\n'; return 1;
}
