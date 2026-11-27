#include "platform/java.hpp"
#include "core/config/atomic_file.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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
std::string lower(std::string_view text) {
    std::string value(text);
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
std::string identity(const std::filesystem::path& path) {
#ifdef _WIN32
    auto text = path.native();
    if (!text.empty()) {
        const auto length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(),
            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr, 0);
        if (length > 0) {
            std::wstring mapped(static_cast<std::size_t>(length), L'\0');
            LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), static_cast<int>(text.size()),
                          mapped.data(), length, nullptr, nullptr, 0);
            return path_to_utf8(std::filesystem::path(mapped));
        }
    }
#endif
    return path_to_utf8(path);
}

} // namespace

std::optional<JavaRuntime> parse_java_output(const std::filesystem::path& executable, std::string_view text) {
    std::map<std::string, std::string> properties;
    auto lines = text;
    while (!lines.empty()) {
        const auto end = lines.find('\n');
        const auto line = trim(lines.substr(0, end));
        if (const auto equal = line.find('='); equal != std::string_view::npos) {
            const auto name = trim(line.substr(0, equal));
            const auto value = trim(line.substr(equal + 1));
            if (name == "java.version" || name == "java.vendor" || name == "os.arch" || name == "sun.arch.data.model") {
                properties[std::string(name)] = value;
            }
        }
        if (end == std::string_view::npos) break;
        lines.remove_prefix(end + 1);
    }
    std::string version;
    if (const auto property = properties.find("java.version"); property != properties.end()) version = property->second;
    if (version.empty()) {
        const auto marker = text.find("version \"");
        if (marker != std::string_view::npos) {
            const auto start = marker + 9;
            const auto end = text.find('"', start);
            if (end != std::string_view::npos) version = text.substr(start, end - start);
        } else {
            const auto value = trim(text);
            for (const auto prefix : {std::string_view("openjdk "), std::string_view("java ")}) {
                if (value.starts_with(prefix)) {
                    const auto tail = value.substr(prefix.size());
                    version = tail.substr(0, tail.find_first_of(" \r\n"));
                    break;
                }
            }
        }
    }
    if (version.empty() || version.size() > 256) return std::nullopt;
    auto normalized = std::string_view(version);
    if (normalized.starts_with("1.")) normalized.remove_prefix(2);
    const auto major_text = normalized.substr(0, normalized.find_first_of("._-+"));
    unsigned major = 0;
    const auto number = std::from_chars(major_text.data(), major_text.data() + major_text.size(), major);
    if (number.ec != std::errc{} || number.ptr != major_text.data() + major_text.size() || major == 0 || major > 1000) return std::nullopt;
    JavaRuntime result;
    result.executable = executable;
    result.version = std::move(version);
    result.major = major;
    result.vendor = properties["java.vendor"];
    result.architecture = properties["os.arch"];
    const auto arch = lower(result.architecture);
    result.is_64_bit = properties["sun.arch.data.model"] == "64" || arch == "amd64" || arch == "x86_64" ||
        arch == "aarch64" || arch == "arm64" || arch == "ppc64" || arch == "ppc64le" || arch == "s390x";
    if (properties["sun.arch.data.model"] == "32") result.is_64_bit = false;
    return result;
}

JavaRuntime probe_java(const std::filesystem::path& executable, const Cancellation* cancellation,
                        std::chrono::milliseconds timeout) {
    check_cancel(cancellation);
    if (timeout.count() <= 0) fail("InvalidArgument", "Java probe timeout must be positive");
    ProcessOptions options;
    options.executable = executable;
    options.arguments = {"-XshowSettings:properties", "-version"};
    options.capture_limit = 256 * 1024;
    options.purpose = ProcessPurpose::Preparation;
    auto process = OwnedProcess::spawn(options);
    auto result = process.wait(timeout, cancellation);
    if (!result) {
        process.terminate();
        (void)process.drain(std::chrono::seconds(5));
        fail("Timeout", "Java discovery probe timed out");
    }
    if (result->exit_code != 0 || result->stdout_truncated || result->stderr_truncated) fail("InvalidArgument", "Java discovery probe failed");
    auto text = result->stdout_text;
    text.push_back('\n');
    text.append(result->stderr_text);
    const auto runtime = parse_java_output(executable, text);
    if (!runtime) fail("CorruptData", "Java discovery returned an unrecognized version");
    return *runtime;
}

std::vector<JavaRuntime> discover_java(const JavaDiscoveryOptions& options, const Cancellation* cancellation) {
    if (options.max_candidates == 0 || options.max_candidates > 4096 || options.max_depth > 16) {
        fail("InvalidArgument", "Java discovery limits are invalid");
    }
    check_cancel(cancellation);
    std::vector<std::filesystem::path> candidates;
    std::set<std::string> seen;
    auto add = [&](const std::filesystem::path& path) {
        if (candidates.size() >= options.max_candidates || path.empty()) return;
        std::error_code error;
        auto native = path;
#ifdef _WIN32
        native = native_filesystem_path(path);
#endif
        if (!std::filesystem::is_regular_file(native, error) || error) return;
        auto canonical = std::filesystem::canonical(native, error);
        if (error) return;
#ifdef _WIN32
        canonical = checked_absolute_path(canonical);
#endif
        if (!seen.insert(identity(canonical)).second) return;
        candidates.push_back(canonical);
    };
    for (const auto& path : options.executables) add(path);
    std::vector<std::filesystem::path> roots = options.roots;
#ifdef _WIN32
    constexpr std::string_view binary = "java.exe";
#else
    constexpr std::string_view binary = "java";
#endif
    if (options.include_environment) {
        const auto environment = current_environment();
        for (const auto key : {"JAVA_HOME", "JDK_HOME"}) {
            if (const auto value = environment.find(key); value != environment.end() && !value->second.empty()) {
                const auto root = path_from_utf8(value->second);
                add(root / "bin" / path_from_utf8(binary));
                add(root / "jre/bin" / path_from_utf8(binary));
            }
        }
        if (const auto path = environment.find("PATH"); path != environment.end()) {
            std::string_view remaining(path->second);
#ifdef _WIN32
            constexpr char delimiter = ';';
#else
            constexpr char delimiter = ':';
#endif
            while (!remaining.empty()) {
                const auto end = remaining.find(delimiter);
                auto item = trim(remaining.substr(0, end));
                if (item.size() >= 2 && item.front() == '"' && item.back() == '"') item = item.substr(1, item.size() - 2);
                if (!item.empty()) add(path_from_utf8(item) / path_from_utf8(binary));
                if (end == std::string_view::npos) break;
                remaining.remove_prefix(end + 1);
            }
        }
    }
    if (options.include_system_locations) {
#ifdef _WIN32
        const auto environment = current_environment();
        for (const auto key : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
            if (const auto base = environment.find(key); base != environment.end() && !base->second.empty()) {
                for (const auto relative : {"Java", "Eclipse Adoptium", "Microsoft", "Zulu", "BellSoft", "Amazon Corretto"}) {
                    roots.push_back(path_from_utf8(base->second) / relative);
                }
            }
        }
#elif defined(__APPLE__)
        roots.emplace_back("/Library/Java/JavaVirtualMachines");
        roots.emplace_back("/System/Library/Java/JavaVirtualMachines");
        roots.emplace_back("/opt/homebrew/opt/openjdk/libexec/openjdk.jdk");
#else
        roots.emplace_back("/usr/lib/jvm");
        roots.emplace_back("/usr/java");
        add("/usr/bin/java");
#endif
    }
    for (const auto& root : roots) {
        check_cancel(cancellation);
        if (candidates.size() >= options.max_candidates) break;
        add(root / "bin" / path_from_utf8(binary));
        add(root / "jre/bin" / path_from_utf8(binary));
        add(root / "Contents/Home/bin" / path_from_utf8(binary));
        std::error_code error;
        auto native = root;
#ifdef _WIN32
        native = native_filesystem_path(root);
#endif
        if (!std::filesystem::is_directory(native, error) || error) continue;
        std::filesystem::recursive_directory_iterator iterator(native, std::filesystem::directory_options::skip_permission_denied, error), end;
        std::size_t visited = 0;
        while (!error && iterator != end && candidates.size() < options.max_candidates && visited++ < 65536) {
            check_cancel(cancellation);
            if (static_cast<std::size_t>(iterator.depth()) >= options.max_depth) iterator.disable_recursion_pending();
            const auto path = iterator->path();
            if (path.filename() == path_from_utf8(binary)) add(path);
            iterator.increment(error);
        }
    }
    std::vector<JavaRuntime> result;
    for (const auto& path : candidates) {
        check_cancel(cancellation);
        try { result.push_back(probe_java(path, cancellation, options.probe_timeout)); }
        catch (...) { check_cancel(cancellation); }
    }
    std::stable_sort(result.begin(), result.end(), [](const JavaRuntime& a, const JavaRuntime& b) {
        if (a.major != b.major) return a.major > b.major;
        return a.is_64_bit && !b.is_64_bit;
    });
    return result;
}

} // namespace nickel
