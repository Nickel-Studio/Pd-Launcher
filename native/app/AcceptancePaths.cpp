#include "AcceptancePaths.hpp"

#include "core/config/atomic_file.hpp"
#include "core/network/hash.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nickel::app {
namespace {
constexpr std::uint64_t maximum_context_bytes = 64 * 1024;

[[noreturn]] void invalid_context() {
    throw std::runtime_error("The explicit acceptance context is invalid or unsafe");
}

bool acceptance_flag(const QString& value) {
    return value.startsWith(QStringLiteral("--acceptance-"))
        || value == QStringLiteral("--performance")
        || value.startsWith(QStringLiteral("--performance="))
        || value.startsWith(QStringLiteral("--performance-"));
}

std::filesystem::path explicit_path(const std::filesystem::path& value) {
    if (value.empty() || !value.is_absolute()) invalid_context();
    const auto checked = foundation_detail::checked_absolute_path(value);
    if (checked == checked.root_path()) invalid_context();
    return checked;
}

void require_child(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative == "." || relative.is_absolute()) invalid_context();
    for (const auto& component : relative)
        if (component == "..") invalid_context();
}

bool same_token(const tasks::detail::RegularFileToken& left, const tasks::detail::RegularFileToken& right) {
    return left.file_identity == right.file_identity && left.parent_identity == right.parent_identity
        && left.root_identity == right.root_identity && left.revision == right.revision && left.size == right.size
        && transport::detail::digest_equal(left.sha256, right.sha256);
}

std::string text_field(const Json& value, const char* name, std::size_t limit = 32768) {
    const auto found = value.find(name);
    if (found == value.end() || !found->is_string()) invalid_context();
    const auto result = found->get<std::string>();
    if (result.empty() || result.size() > limit || result.find('\0') != std::string::npos || !valid_utf8(result))
        invalid_context();
    return result;
}

void require_hash(const std::string& value, std::size_t length = 64) {
    if (value.size() != length || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) invalid_context();
}

void require_identifier(const Json& value, const char* name) {
    const auto text = text_field(value, name, 80);
    if (!std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.';
    })) invalid_context();
}

std::uint64_t positive_integer(const Json& value, const char* name) {
    const auto found = value.find(name);
    if (found == value.end() || !found->is_number_integer()
        || (!found->is_number_unsigned() && found->get<std::int64_t>() <= 0)) invalid_context();
    const auto result = found->get<std::uint64_t>();
    if (!result) invalid_context();
    return result;
}

std::filesystem::path evidence_path(const Json& value, const char* name, const std::filesystem::path& context) {
    const auto path = explicit_path(path_from_utf8(text_field(value, name)));
    if (path.parent_path() != context.parent_path() || path == context) invalid_context();
    reject_path_links(path);
    return path;
}

void require_native_request(const Json& value) {
    if (!value.contains("schemaVersion") || !value.at("schemaVersion").is_number_integer()
        || value.at("schemaVersion") != 1 || text_field(value, "protocol", 64) != "nickel-evidence-driver-v1"
        || text_field(value, "role", 16) != "native"
        || text_field(value, "observationAdapter", 64) != "native-production-markers-v1") invalid_context();
    for (const auto* name : {"runId", "scenarioId", "requestNonce", "phaseRequestNonce"}) require_identifier(value, name);
    for (const auto* name : {"executableSha256", "sourceSnapshotSha256", "buildSourceSha256", "driverSourceSha256", "inputCorpusSha256"})
        require_hash(text_field(value, name, 64));
    const auto commit = text_field(value, "sourceCommit", 64);
    require_hash(commit, commit.size() == 40 ? 40 : 64);
    const auto phase = text_field(value, "phase", 16);
    if (phase != "ready" && phase != "state" && phase != "shutdown") invalid_context();
}


Json read_context(const std::filesystem::path& path) {
    const auto text = read_text_bounded(path, maximum_context_bytes);
    if (!valid_utf8(text)) invalid_context();
    std::vector<std::set<std::string>> keys;
    auto callback = [&keys](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 32) invalid_context();
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            if (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second) invalid_context();
        } else if (event == Json::parse_event_t::object_end) {
            if (keys.empty()) invalid_context();
            keys.pop_back();
        }
        return true;
    };
    auto value = Json::parse(text, callback);
    if (!value.is_object()) invalid_context();
    return value;
}

void require_private_directory(const foundation_detail::DirectoryGuard& directory) {
    directory.verify();
#ifdef _WIN32
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) invalid_context();
    struct Token { HANDLE value; ~Token() { CloseHandle(value); } } token{raw_token};
    DWORD bytes = 0;
    (void)GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    if (!bytes || bytes > 65536) invalid_context();
    std::vector<std::byte> storage(bytes);
    if (!GetTokenInformation(token.value, TokenUser, storage.data(), bytes, &bytes)) invalid_context();
    const auto user = reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid;
    // DirectoryGuard deliberately lacks READ_CONTROL; use a read-only security handle while its namespace stays pinned.
    const auto native = foundation_detail::native_filesystem_path(directory.path());
    const auto raw = CreateFileW(native.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) invalid_context();
    struct Handle { HANDLE value; ~Handle() { CloseHandle(value); } } handle{raw};
    BY_HANDLE_FILE_INFORMATION current{}, pinned{};
    if (!GetFileInformationByHandle(handle.value, &current)
        || !GetFileInformationByHandle(reinterpret_cast<HANDLE>(directory.native_handle()), &pinned)
        || (current.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        || current.dwVolumeSerialNumber != pinned.dwVolumeSerialNumber || current.nFileIndexHigh != pinned.nFileIndexHigh
        || current.nFileIndexLow != pinned.nFileIndexLow) invalid_context();
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(handle.value, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) invalid_context();
    struct Security { PSECURITY_DESCRIPTOR value; ~Security() { LocalFree(value); } } security{descriptor};
    if (!owner || !EqualSid(owner, user) || !acl || !IsValidAcl(acl)) invalid_context();
    std::array<std::byte, SECURITY_MAX_SID_SIZE> system_storage{}, admin_storage{};
    DWORD system_bytes = static_cast<DWORD>(system_storage.size()), admin_bytes = static_cast<DWORD>(admin_storage.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_storage.data(), &system_bytes)
        || !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_storage.data(), &admin_bytes)) invalid_context();
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* ace_pointer = nullptr;
        if (!GetAce(acl, index, &ace_pointer)) invalid_context();
        const auto header = static_cast<ACE_HEADER*>(ace_pointer);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceSize < sizeof(ACCESS_ALLOWED_ACE)) invalid_context();
        const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(ace_pointer);
        const auto sid = reinterpret_cast<PSID>(&ace->SidStart);
        if (!IsValidSid(sid) || (!EqualSid(sid, user) && !EqualSid(sid, system_storage.data())
            && !EqualSid(sid, admin_storage.data()))) invalid_context();
    }
#else
    struct stat status{};
    if (::fstat(static_cast<int>(directory.native_handle()), &status) != 0 || !S_ISDIR(status.st_mode)
        || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) invalid_context();
#endif
    directory.verify();
}

void verify_fixture_tree(const std::filesystem::path& root) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(foundation_detail::native_filesystem_path(root))) {
        if (++count > 16384) invalid_context();
        const auto path = tasks::detail::logical_path(entry.path());
        reject_path_links(path);
        const auto status = entry.symlink_status();
        if (std::filesystem::is_regular_file(status)) foundation_detail::reject_file_hardlinks(path);
        else if (!std::filesystem::is_directory(status)) invalid_context();
    }
}

void verify_registered_roots(const PathContext& context, const AcceptancePathOptions& options) {
    const auto file = options.config_root / "instances.json";
    std::error_code error;
    const auto status = std::filesystem::symlink_status(foundation_detail::native_filesystem_path(file), error);
    if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found) return;
    if (error || !std::filesystem::is_regular_file(status)) invalid_context();
    const auto value = read_context(file);
    const auto check = [&options](const Json& path) {
        if (!path.is_string()) invalid_context();
        const auto root = explicit_path(path_from_utf8(path.get<std::string>()));
        require_child(options.fixture_root, root);
        foundation_detail::DirectoryGuard guard(root);
        require_private_directory(guard);
    };
    if (value.contains("roots")) {
        if (!value["roots"].is_array() || value["roots"].size() > 256) invalid_context();
        for (const auto& root : value["roots"]) check(root);
    }
    if (value.contains("active") && !value["active"].is_null()) check(value["active"]);
    if (PathResolver(context).config_root(false) != options.config_root) invalid_context();
}
} // namespace

bool has_acceptance_arguments(const QStringList& arguments) {
    for (qsizetype index = 1; index < arguments.size(); ++index)
        if (acceptance_flag(arguments.at(index))) return true;
    return false;
}

ProductionArguments parse_production_arguments(const QStringList& arguments) {
    ProductionArguments result;
    result.business_arguments.reserve(static_cast<std::size_t>(arguments.size()));
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const auto& argument = arguments.at(index);
        if (argument == QStringLiteral("--acceptance-context")
            || argument.startsWith(QStringLiteral("--acceptance-context="))) {
            if (result.acceptance_context) invalid_context();
            QString file;
            if (argument == QStringLiteral("--acceptance-context")) {
                if (++index >= arguments.size() || acceptance_flag(arguments.at(index))) invalid_context();
                file = arguments.at(index);
            } else file = argument.mid(QStringLiteral("--acceptance-context=").size());
            result.acceptance_context = explicit_path(path_from_utf8(file.toUtf8().toStdString()));
        } else if (argument == QStringLiteral("--performance")) {
            if (result.performance) invalid_context();
            result.performance = true;
        } else if (acceptance_flag(argument)) invalid_context();
        else {
            const auto utf8 = argument.toUtf8();
            result.business_arguments.emplace_back(utf8.constData(), static_cast<std::size_t>(utf8.size()));
        }
    }
    if (result.performance && !result.acceptance_context) invalid_context();
    return result;
}

AcceptancePaths::AcceptancePaths(AcceptancePathOptions options) : options_(std::move(options)) {
    options_.fixture_root = explicit_path(options_.fixture_root);
    directories_.emplace_back(options_.fixture_root);
    require_private_directory(directories_.back());
    const std::array paths{&options_.executable, &options_.config_root, &options_.game_root,
                           &options_.cache_root, &options_.temporary_root};
    for (const auto path : paths) {
        *path = explicit_path(*path);
        require_child(options_.fixture_root, *path);
        reject_path_links(*path);
    }
    require_hash(options_.executable_sha256);
    const std::array data_roots{options_.config_root, options_.game_root, options_.cache_root, options_.temporary_root};
    for (std::size_t left = 0; left < data_roots.size(); ++left)
        for (std::size_t right = left + 1; right < data_roots.size(); ++right)
            if (tasks::detail::paths_overlap(data_roots[left], data_roots[right])) invalid_context();
    for (const auto& path : data_roots) {
        directories_.emplace_back(path);
        require_private_directory(directories_.back());
    }
    directories_.emplace_back(options_.executable.parent_path());
    executable_ = tasks::detail::capture_regular_file(options_.fixture_root, options_.executable, 512 * 1024 * 1024ULL);
    if (!transport::detail::digest_equal(options_.executable_sha256, executable_.sha256)) invalid_context();

    context_.executable = options_.executable;
    context_.system_config_base = options_.config_root.parent_path();
    context_.app_data_base = options_.cache_root;
#ifdef __APPLE__
    context_.macos = true;
#endif
    const PathResolver resolver(context_);
    const auto executable_directory = explicit_path(resolver.executable_directory());
    if (executable_directory != options_.fixture_root) require_child(options_.fixture_root, executable_directory);
    directories_.emplace_back(executable_directory);
    directories_.emplace_back(context_.system_config_base);
    require_private_directory(directories_.back());
    // Preserve the resolver's system/adjacent/portable precedence, but never accept its fallback.
    if (options_.config_root.filename() == path_from_utf8(application_identifier)) {
        if (!has_configuration(options_.config_root)) invalid_context();
    } else if (options_.config_root != executable_directory / path_from_utf8(data_directory_name)) invalid_context();
    verify_fixture_tree(options_.fixture_root);
    if (resolver.config_root(false) != options_.config_root
        || explicit_path(resolver.default_game_directory()) != options_.game_root) invalid_context();
    verify_registered_roots(context_, options_);
    verify();
}

AcceptancePaths AcceptancePaths::from_context(const std::filesystem::path& file,
                                               const std::filesystem::path& executable) {
    const auto input = explicit_path(file);
    foundation_detail::DirectoryGuard parent(input.parent_path());
    require_private_directory(parent);
    const auto token = tasks::detail::capture_regular_file(parent.path(), input, maximum_context_bytes);
    const auto value = read_context(input);
    require_native_request(value);
    if (value.contains("productionEvents")) (void)evidence_path(value, "productionEvents", input);
    if (value.contains("launchContext")) {
        const auto channel = evidence_path(value, "launchContext", input);
        if (value.contains("productionEvents") && channel == evidence_path(value, "productionEvents", input)) invalid_context();
    }
    const auto accepted = value.find("explicitAcceptedPathContext");
    if (accepted == value.end() || !accepted->is_object()
        || accepted->value("protocol", std::string{}) != "explicit-accepted-path-context-v1"
        || accepted->value("systemKnownFolderAccess", std::string{}) != "disabled"
        || text_field(*accepted, "executableSha256", 64) != text_field(value, "executableSha256", 64)) invalid_context();
    const auto roots = accepted->find("dataRoots");
    if (roots == accepted->end() || !roots->is_object() || roots->size() != 4) invalid_context();
    AcceptancePathOptions options;
    options.fixture_root = explicit_path(path_from_utf8(text_field(value, "fixtureRoot")));
    if (options.fixture_root != explicit_path(path_from_utf8(text_field(*accepted, "runFixture")))) invalid_context();
    options.executable = explicit_path(executable);
    options.executable_sha256 = text_field(value, "executableSha256", 64);
    options.config_root = explicit_path(path_from_utf8(text_field(*roots, "config")));
    options.game_root = explicit_path(path_from_utf8(text_field(*roots, "game")));
    options.cache_root = explicit_path(path_from_utf8(text_field(*roots, "cache")));
    options.temporary_root = explicit_path(path_from_utf8(text_field(*roots, "temp")));
    AcceptancePaths result(std::move(options));
    if (!same_token(token, tasks::detail::capture_regular_file(parent.path(), input, maximum_context_bytes))) invalid_context();
    result.context_file_ = input;
    result.context_token_ = token;
    result.request_ = value;
    result.directories_.push_back(std::move(parent));
    return result;
}

const PathContext& AcceptancePaths::context() const { return context_; }
const AcceptancePathOptions& AcceptancePaths::options() const { return options_; }
const Json& AcceptancePaths::request() const { return request_; }
const std::filesystem::path& AcceptancePaths::context_file() const { return context_file_; }

std::optional<AcceptancePhaseRequest> AcceptancePaths::phase_request(std::uint64_t root_pid,
    std::uint64_t root_creation_time_100ns, std::int64_t now_ns) const {
    verify_context_namespace();
    if (!root_pid || !root_creation_time_100ns || now_ns <= 0) invalid_context();
    const auto channel = evidence_path(request_, "launchContext", context_file_);
    std::error_code error;
    const auto status = std::filesystem::symlink_status(foundation_detail::native_filesystem_path(channel), error);
    if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found) return std::nullopt;
    if (error || !std::filesystem::is_regular_file(status)) invalid_context();
    const auto token = tasks::detail::capture_regular_file(channel.parent_path(), channel, maximum_context_bytes);
    const auto value = read_context(channel);
    if (!same_token(token, tasks::detail::capture_regular_file(channel.parent_path(), channel, maximum_context_bytes))) invalid_context();
    require_native_request(value);
    if (positive_integer(value, "rootPid") != root_pid
        || positive_integer(value, "rootCreationTime100ns") != root_creation_time_100ns) invalid_context();
    const auto launch = positive_integer(value, "launchBeginNs");
    const auto begin = positive_integer(value, "phaseBeginNs");
    const auto returned = positive_integer(value, "launchCreateReturnedNs");
    if (launch > begin || begin > static_cast<std::uint64_t>(now_ns) || returned < launch
        || returned > static_cast<std::uint64_t>(now_ns)) invalid_context();
    const auto arguments = value.find("launchArgv");
    if (arguments == value.end() || !arguments->is_array() || arguments->empty() || arguments->size() > 256) invalid_context();
    for (const auto& argument : *arguments)
        if (!argument.is_string() || argument.get_ref<const std::string&>().size() > 32768
            || argument.get_ref<const std::string&>().find('\0') != std::string::npos) invalid_context();
    if (explicit_path(path_from_utf8(arguments->at(0).get<std::string>())) != options_.executable) invalid_context();
    if (!ready_phase_) {
        if (value.at("phase") != "ready" || value.at("phaseRequestNonce") != request_.at("phaseRequestNonce")) invalid_context();
        auto initial = value;
        for (const auto* key : {"rootPid", "rootCreationTime100ns", "phaseBeginNs", "launchBeginNs", "launchCreateReturnedNs", "launchArgv"}) {
            if (request_.contains(key)) initial[key] = request_.at(key);
            else initial.erase(key);
        }
        if (initial != request_) invalid_context();
        ready_phase_ = AcceptancePhaseRequest{value, token.sha256};
    } else {
        auto frozen = value;
        for (const auto* key : {"phase", "phaseRequestNonce", "phaseBeginNs"}) frozen[key] = ready_phase_->request.at(key);
        // Compare the whole object: unknown, removed and future protocol fields are frozen too.
        if (frozen != ready_phase_->request) invalid_context();
    }
    verify_context_namespace();
    return AcceptancePhaseRequest{value, token.sha256};
}


void AcceptancePaths::configure_process_environment() const {
    verify();
    const std::array<std::pair<const char*, std::filesystem::path>, 9> environment{{
        {"TEMP", options_.temporary_root}, {"TMP", options_.temporary_root}, {"TMPDIR", options_.temporary_root},
        {"HOME", options_.fixture_root}, {"USERPROFILE", options_.fixture_root},
        {"APPDATA", context_.system_config_base}, {"LOCALAPPDATA", options_.cache_root},
        {"XDG_CONFIG_HOME", context_.system_config_base}, {"XDG_DATA_HOME", options_.cache_root}}};
    for (const auto& [name, path] : environment) {
#ifdef _WIN32
        const std::wstring key(name, name + std::char_traits<char>::length(name));
        if (!SetEnvironmentVariableW(key.c_str(), path.c_str()) || _wputenv_s(key.c_str(), path.c_str()) != 0)
            invalid_context();
#else
        const auto value = path_to_utf8(path);
        if (::setenv(name, value.c_str(), 1) != 0) invalid_context();
#endif
    }
    // Environment controls temp/auxiliary consumers; the configuration source is still explicit PathContext.
    verify();
}


void AcceptancePaths::verify_context_namespace() const {
    for (const auto& directory : directories_) directory.verify();
    if (context_token_ && !same_token(*context_token_, tasks::detail::capture_regular_file(
        context_file_.parent_path(), context_file_, maximum_context_bytes))) invalid_context();
}

void AcceptancePaths::verify() const {
    verify_context_namespace();
    for (const auto& root : {options_.fixture_root, options_.config_root, options_.game_root,
                            options_.cache_root, options_.temporary_root}) {
        foundation_detail::DirectoryGuard directory(root);
        require_private_directory(directory);
    }
    if (!same_token(executable_, tasks::detail::capture_regular_file(options_.fixture_root, options_.executable,
                                                                   512 * 1024 * 1024ULL))) invalid_context();
    if (PathResolver(context_).config_root(false) != options_.config_root) invalid_context();
    verify_registered_roots(context_, options_);
}

} // namespace nickel::app
