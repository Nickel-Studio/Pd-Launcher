#include "app/service/trusted_launcher.hpp"

#include "BuildInfo.hpp"
#include "core/common/error.hpp"
#include "core/network/hash.hpp"
#include "core/tasks/secure_fs.hpp"
#include "platform/process.hpp"

#if __has_include("TrustedLauncherBuild.hpp")
#include "TrustedLauncherBuild.hpp"
#define NICKEL_HAS_TRUSTED_LAUNCHER_BUILD 1
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <winver.h>
#ifdef _MSC_VER
#pragma comment(lib, "version.lib")
#endif
#endif

namespace nickel::app {
namespace {
using File = tasks::detail::SecureFile;
using Token = tasks::detail::RegularFileToken;
#ifdef _WIN32
std::span<const std::byte> module_resource(HMODULE module, LPCWSTR name, LPCWSTR type);
#endif
constexpr std::size_t notice_count_limit = 512;
constexpr std::size_t notice_bytes_limit = 16 * 1024 * 1024;
constexpr std::array<std::string_view, 4> required_notices{"LICENSE", "LICENSE-ASSETS", "NOTICE.md", "SOURCE.txt"};

[[noreturn]] void reject(std::string_view code = "integrity") {
    throw ErrorException(Error{std::string(code), code == "cancelled" ? "Operation cancelled"
        : "Trusted launcher material could not be verified"});
}

void check_stop(std::stop_token stop) {
    if (stop.stop_requested()) reject("cancelled");
}

bool digest_text(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

std::string digest_bytes(std::string_view bytes) {
    transport::detail::IncrementalHash hash(transport::HashAlgorithm::Sha256);
    hash.update(std::as_bytes(std::span(bytes.data(), bytes.size())));
    return hash.finish();
}

#ifdef NICKEL_HAS_TRUSTED_LAUNCHER_BUILD
// Payloads are either inline build literals or compiled RCDATA resources.
// A resource read is fixed to the generated metadata: no caller name or path
// selects a resource, and the result must match the recorded byte count.
bool load_notice_payload(const EmbeddedLauncherNotice& notice, std::span<const std::byte>& payload) {
    if (notice.resource.empty()) {
        payload = std::as_bytes(std::span(notice.bytes.data(), notice.bytes.size()));
        return payload.size() == static_cast<std::size_t>(notice.byte_count);
    }
#ifdef _WIN32
    if (notice.resource.size() > 128
        || !std::all_of(notice.resource.begin(), notice.resource.end(), [](unsigned char character) {
            return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
                || character == '_' || character == '-';
        })) return false;
    const std::wstring resource(notice.resource.begin(), notice.resource.end());
    payload = module_resource(GetModuleHandleW(nullptr), resource.c_str(), MAKEINTRESOURCEW(10));
    return payload.size() == static_cast<std::size_t>(notice.byte_count);
#else
    return false;
#endif
}
#endif

[[maybe_unused]] std::string notice_set_digest(std::span<const packs::ArchiveInput> notices) {
    Json rows = Json::array();
    for (const auto& notice : notices) {
        rows.push_back({{"bytes", static_cast<std::uint64_t>(notice.bytes.size())},
            {"name", notice.name}, {"sha256", digest_bytes(notice.bytes)}});
    }
    // Brand hashes sorted name/byte-length/SHA rows as compact, ASCII-escaped JSON.
    return digest_bytes(rows.dump(-1, ' ', true));
}

struct Materials {
    std::string product;
    std::string version;
    std::string internal_name;
    std::string executable_name;
    std::string source_set;
    std::string notice_set;
    std::wstring stamp_resource;
    std::string stamp;
    std::vector<packs::ArchiveInput> notices;
};

bool executable_stem(std::string_view value) {
    return !value.empty() && value.size() <= 128
        && std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9') || character == '_' || character == '-';
        });
}

std::optional<Materials> compiled_materials(std::stop_token stop) {
    check_stop(stop);
#ifdef NICKEL_HAS_TRUSTED_LAUNCHER_BUILD
    // Candidate notices alone cannot grant legal or production release readiness.
    if (!build::trusted_launcher_ready) return {};
    if (build::display_name != "Palladium" || build::application_id != "studio.nickel.launcher"
        || build::version != "0.1.0" || !executable_stem(build::executable_name)
        || !digest_text(build::trusted_launcher_source_set_sha256)
        || !digest_text(build::trusted_launcher_notice_set_sha256)
        || !build::trusted_launcher_repository.starts_with("https://")
        || build::trusted_launcher_repository.size() > 2048
        || build::trusted_launcher_notices.size() > notice_count_limit
        || build::trusted_launcher_stamp_resource.empty() || build::trusted_launcher_stamp_resource.size() > 128
        || !std::all_of(build::trusted_launcher_stamp_resource.begin(), build::trusted_launcher_stamp_resource.end(),
            [](unsigned char character) {
                return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
                    || character == '_' || character == '-';
            })) reject();
    Materials result;
    result.product = build::display_name;
    result.version = build::version;
    // BuildInfo and windows.rc share the actual metadata stem, independent of the disk/package name.
    result.internal_name = build::executable_name;
    result.executable_name = result.internal_name + ".exe";
    result.source_set = build::trusted_launcher_source_set_sha256;
    result.notice_set = build::trusted_launcher_notice_set_sha256;
    result.stamp_resource.assign(build::trusted_launcher_stamp_resource.begin(), build::trusted_launcher_stamp_resource.end());
    std::set<std::string> names;
    std::size_t total = 0;
    for (const auto& notice : build::trusted_launcher_notices) {
        check_stop(stop);
        const bool required = std::find(required_notices.begin(), required_notices.end(), notice.name) != required_notices.end();
        if ((!required && !notice.name.starts_with("LICENSES/")) || notice.byte_count == 0
            || notice.byte_count > std::uint64_t{4} * 1024 * 1024 || !digest_text(notice.sha256)
            // Exactly one payload source: inline literals or a compiled resource.
            || notice.bytes.empty() == notice.resource.empty()) reject();
        std::span<const std::byte> payload;
        if (!load_notice_payload(notice, payload)) reject();
        const std::string_view bytes(reinterpret_cast<const char*>(payload.data()), payload.size());
        if (bytes.find('\0') != std::string_view::npos
            || total > notice_bytes_limit - bytes.size()) reject();
        const auto name = tasks::detail::validate_relative(notice.name, 64, 4096,
            tasks::detail::RelativePathPolicy::ArchiveMember);
        if (name != notice.name || !names.insert(tasks::detail::case_key(name)).second
            || !transport::detail::digest_equal(digest_bytes(bytes), notice.sha256)) reject();
        if (notice.name == "SOURCE.txt"
            && bytes.find(build::trusted_launcher_repository) == std::string_view::npos) reject();
        total += bytes.size();
        result.notices.push_back({name, {}, std::string(bytes)});
    }
    for (const auto name : required_notices)
        if (!names.contains(tasks::detail::case_key(name))) reject("missing_service");
    std::sort(result.notices.begin(), result.notices.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
    check_stop(stop);
    if (!transport::detail::digest_equal(notice_set_digest(result.notices), result.notice_set)) reject();
    // Keep original SOURCE bytes intact; the builder binds its source set separately in this stamp.
    result.stamp = result.product + '\n' + result.version + '\n' + result.source_set + '\n' + result.notice_set + '\n';
    check_stop(stop);
    return result;
#else
    return {};
#endif
}

bool same_token(const Token& left, const Token& right) {
    return left.root == right.root && left.path == right.path
        && left.root_identity == right.root_identity && left.parent_identity == right.parent_identity
        && left.file_identity == right.file_identity && left.size == right.size && left.revision == right.revision
        && transport::detail::digest_equal(left.sha256, right.sha256);
}

void read_exact(File& file, std::span<std::byte> bytes) {
    while (!bytes.empty()) {
        const auto count = file.read(bytes);
        if (!count || count > bytes.size()) reject();
        bytes = bytes.subspan(count);
    }
}

std::uint16_t little16(std::span<const std::byte> bytes, std::size_t offset) {
    return std::to_integer<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t little32(std::span<const std::byte> bytes, std::size_t offset) {
    return little16(bytes, offset) | (static_cast<std::uint32_t>(little16(bytes, offset + 2)) << 16);
}

void validate_pe_x64(File& file) {
    if (file.size() < 320 || file.size() > packs::max_pack_file_bytes) reject();
    std::array<std::byte, 64> dos{};
    file.seek(0);
    read_exact(file, dos);
    if (little16(dos, 0) != 0x5a4d) reject();
    const auto offset = little32(dos, 0x3c);
    if (offset < dos.size() || offset > file.size() - 264) reject();
    std::array<std::byte, 264> pe{};
    file.seek(offset);
    read_exact(file, pe);
    const auto characteristics = little16(pe, 22);
    if (little32(pe, 0) != 0x00004550 || little16(pe, 4) != 0x8664 || little16(pe, 6) == 0
        || little16(pe, 20) < 240 || little16(pe, 24) != 0x20b
        || !(characteristics & 0x0002) || (characteristics & 0x2000)) reject();
}

#ifdef _WIN32
struct DataModule {
    HMODULE value = nullptr;
    ~DataModule() { if (value) FreeLibrary(value); }
};

std::span<const std::byte> module_resource(HMODULE module, LPCWSTR name, LPCWSTR type) {
    const auto resource = FindResourceW(module, name, type);
    if (!resource) return {};
    const auto size = SizeofResource(module, resource);
    const auto loaded = LoadResource(module, resource);
    const auto bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || size == 0 || size > 1024 * 1024) return {};
    return {static_cast<const std::byte*>(bytes), size};
}

bool stamped_module(HMODULE module, const Materials& material) {
    if (material.stamp_resource.empty()) return false;
    const auto stamp = module_resource(module, material.stamp_resource.c_str(), MAKEINTRESOURCEW(10));
    return stamp.size() == material.stamp.size()
        && std::memcmp(stamp.data(), material.stamp.data(), stamp.size()) == 0;
}

void validate_version(HMODULE module, const Materials& material) {
    const auto resource = module_resource(module, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16));
    if (resource.empty()) reject();
    const auto contained = [&](const void* value, std::size_t bytes) {
        const auto first = reinterpret_cast<std::uintptr_t>(resource.data());
        const auto address = reinterpret_cast<std::uintptr_t>(value);
        return address >= first && address - first <= resource.size()
            && bytes <= resource.size() - (address - first);
    };
    void* fixed_value = nullptr;
    UINT fixed_bytes = 0;
    if (!VerQueryValueW(resource.data(), L"\\", &fixed_value, &fixed_bytes)
        || fixed_bytes != sizeof(VS_FIXEDFILEINFO) || !contained(fixed_value, fixed_bytes)) reject();
    VS_FIXEDFILEINFO fixed{};
    std::memcpy(&fixed, fixed_value, sizeof(fixed));
    constexpr DWORD version_ms = (1UL << 16);
    constexpr DWORD version_ls = (2UL << 16);
    if (fixed.dwSignature != VS_FFI_SIGNATURE || fixed.dwFileVersionMS != version_ms
        || fixed.dwFileVersionLS != version_ls || fixed.dwProductVersionMS != version_ms
        || fixed.dwProductVersionLS != version_ls || fixed.dwFileType != VFT_APP
        || fixed.dwFileOS != VOS_NT_WINDOWS32 || fixed.dwFileFlags != 0) reject();
    const auto field = [&](const wchar_t* key, std::string_view expected) {
        void* value = nullptr;
        UINT count = 0;
        if (!VerQueryValueW(resource.data(), key, &value, &count) || !value || count == 0 || count > 256
            || !contained(value, static_cast<std::size_t>(count) * sizeof(wchar_t))) reject();
        const auto text = static_cast<const wchar_t*>(value);
        if (text[count - 1] != L'\0' || count != expected.size() + 1) reject();
        for (std::size_t index = 0; index != expected.size(); ++index)
            if (text[index] != static_cast<unsigned char>(expected[index])) reject();
    };
    field(L"\\StringFileInfo\\040904B0\\ProductName", material.product);
    field(L"\\StringFileInfo\\040904B0\\ProductVersion", material.version);
    field(L"\\StringFileInfo\\040904B0\\FileVersion", material.version);
    field(L"\\StringFileInfo\\040904B0\\InternalName", material.internal_name);
    field(L"\\StringFileInfo\\040904B0\\OriginalFilename", material.executable_name);
}

void validate_disk_module(const std::filesystem::path& path, const Materials& material) {
    DataModule module;
    module.value = LoadLibraryExW(tasks::detail::io_path(path).c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module.value || !stamped_module(module.value, material)) reject();
    validate_version(module.value, material);
}
#endif

struct PinnedArtifact {
    File file;
    Token token;
};

PinnedArtifact pin_artifact(const std::filesystem::path& path, std::stop_token stop) {
    check_stop(stop);
    // Only the leaf denies writes/deletion; ancestor namespace writers must remain usable.
    auto file = File::open_read(path, tasks::detail::GuardMode::NamespaceMutation);
    if (file.size() == 0 || file.size() > packs::max_pack_file_bytes) reject();
    const auto logical = tasks::detail::logical_path(file.path());
    tasks::detail::ParentGuard parent(logical.parent_path(), false, tasks::detail::GuardMode::NamespaceMutation);
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(reinterpret_cast<HANDLE>(parent.native_handle()), &information)) reject();
    const tasks::detail::FileIdentity parent_identity{information.dwVolumeSerialNumber,
        (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow};
#else
    const auto parent_identity = tasks::detail::directory_identity(logical.parent_path());
#endif
    Token token{logical.parent_path(), logical, parent_identity, parent_identity,
        file.identity(), file.size(), file.revision(), {}};
    // The generic capture helper retains ReadOnly directory guards; reuse the bounded hash instead.
    token.sha256 = packs::hash_file(logical, "sha256", stop);
    parent.verify();
    if (file.identity() != token.file_identity || file.revision() != token.revision || file.size() != token.size) reject();
    check_stop(stop);
    return {std::move(file), std::move(token)};
}

void validate_pin(PinnedArtifact& pin, const Materials& material, std::stop_token stop) {
    check_stop(stop);
    validate_pe_x64(pin.file);
#ifdef _WIN32
    validate_disk_module(pin.token.path, material);
#else
    (void)material;
    reject("unsupported");
#endif
    if (pin.file.identity() != pin.token.file_identity || pin.file.revision() != pin.token.revision
        || pin.file.size() != pin.token.size) reject();
    check_stop(stop);
}

void verify_source(const Token& source, std::stop_token stop) {
    const auto current = pin_artifact(source.path, stop);
    if (!same_token(current.token, source)) reject();
    check_stop(stop);
}

void validate_candidate(PinnedArtifact& candidate, const Token& source,
                        const Materials& material, std::stop_token stop) {
    if (candidate.token.size != source.size
        || !transport::detail::digest_equal(candidate.token.sha256, source.sha256)) reject();
    if ((candidate.token.path == source.path || candidate.token.file_identity == source.file_identity)
        && !same_token(candidate.token, source)) reject();
    validate_pin(candidate, material, stop);
    const auto current = pin_artifact(candidate.token.path, stop);
    if (!same_token(current.token, candidate.token)) reject();
}

#ifdef _WIN32
std::filesystem::path os_image_path() {
    std::vector<wchar_t> path(512);
    for (;;) {
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(GetCurrentProcess(), 0, path.data(), &size))
            return tasks::detail::checked_absolute(std::filesystem::path(std::wstring(path.data(), size)));
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || path.size() >= 32768) reject();
        path.resize(path.size() * 2);
    }
}

void validate_os_image(const PinnedArtifact& image, HMODULE module) {
    MEMORY_BASIC_INFORMATION memory{};
    if (sizeof(void*) != 8 || VirtualQuery(module, &memory, sizeof(memory)) != sizeof(memory)
        || memory.Type != MEM_IMAGE || memory.AllocationBase != module) reject();
    const auto mapped_name = [&] {
        std::vector<wchar_t> path(512);
        for (;;) {
            const auto count = K32GetMappedFileNameW(GetCurrentProcess(), module, path.data(), static_cast<DWORD>(path.size()));
            if (!count) reject();
            if (count < path.size() - 1) return std::wstring(path.data(), count);
            if (path.size() >= 32768) reject();
            path.resize(path.size() * 2);
        }
    }();
    tasks::detail::ParentGuard parent(image.token.path.parent_path(), false, tasks::detail::GuardMode::NamespaceMutation);
    struct Handle {
        HANDLE value;
        ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } file{reinterpret_cast<HANDLE>(foundation_detail::windows_open_relative(parent, image.token.path.filename(),
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ, 1, 0x40))};
    if (file.value == INVALID_HANDLE_VALUE) reject();
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.value, &information)
        || tasks::detail::FileIdentity{information.dwVolumeSerialNumber,
            (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow}
            != image.token.file_identity) reject();
    std::vector<wchar_t> final_name(32768);
    const auto count = GetFinalPathNameByHandleW(file.value, final_name.data(), static_cast<DWORD>(final_name.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    if (!count || count >= final_name.size() || mapped_name.size() != count
        || CompareStringOrdinal(mapped_name.data(), static_cast<int>(mapped_name.size()), final_name.data(),
            static_cast<int>(count), TRUE) != CSTR_EQUAL) reject();
    parent.verify();
}
#endif

} // namespace

struct TrustedLauncherReceipt::Impl {
    Impl(Token image, Materials inputs) : source(std::move(image)), material(std::move(inputs)) {}
    // Receipts freeze values, never OS handles that would lock adjacent Nickel transactions.
    const Token source;
    const Materials material;
};

TrustedLauncherReceipt::TrustedLauncherReceipt(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
TrustedLauncherReceipt::~TrustedLauncherReceipt() = default;

std::shared_ptr<const TrustedLauncherReceipt> capture_trusted_launcher(const Cancellation& cancellation) {
    cancellation.check();
    try {
        auto material = compiled_materials(cancellation.stop_token());
        if (!material) return {};
#ifdef _WIN32
        const auto module = GetModuleHandleW(nullptr);
        if (!module || !stamped_module(module, *material)) return {};
        validate_version(module, *material);
        auto image = pin_artifact(os_image_path(), cancellation.stop_token());
        validate_os_image(image, module);
        validate_pin(image, *material, cancellation.stop_token());
        verify_source(image.token, cancellation.stop_token());
        auto implementation = std::make_unique<TrustedLauncherReceipt::Impl>(std::move(image.token), std::move(*material));
        image.file.close();
        return std::shared_ptr<const TrustedLauncherReceipt>(new TrustedLauncherReceipt(std::move(implementation)));
#else
        return {};
#endif
    } catch (...) {
        cancellation.check();
        // Failure to mint an optional capability must not stop the normal application.
        return {};
    }
}

bool TrustedLauncherReceipt::matches_application(const std::filesystem::path& declared_executable) const {
    try {
        const auto image = pin_artifact(declared_executable, {});
        return same_token(image.token, implementation_->source);
    } catch (...) { return false; }
}

packs::ExportServices TrustedLauncherReceipt::export_fields(const Cancellation& cancellation) const {
    cancellation.check();
    try {
        verify_source(implementation_->source, cancellation.stop_token());
    } catch (...) {
        cancellation.check();
        reject();
    }
    packs::ExportServices result;
    const auto receipt = shared_from_this();
    const auto& source = implementation_->source;
    result.launcher = source.path;
    result.launcher_os = "windows";
    result.launcher_arch = "x64";
    result.launcher_notices = implementation_->material.notices;
    result.validate_launcher_artifact = [receipt](const std::filesystem::path& path, std::stop_token stop) {
        check_stop(stop);
        try {
            const auto& implementation = *receipt->implementation_;
            verify_source(implementation.source, stop);
            auto candidate = pin_artifact(path, stop);
            validate_candidate(candidate, implementation.source, implementation.material, stop);
            // Reopening the original after copy validation also rejects replacement of its path.
            verify_source(implementation.source, stop);
        } catch (...) {
            check_stop(stop);
            reject();
        }
    };
    return result;
}

class TrustedLauncherArchive final : public packs::ArchiveBackend {
public:
    TrustedLauncherArchive(std::shared_ptr<packs::ArchiveBackend> canonical,
                           std::shared_ptr<const TrustedLauncherReceipt> receipt)
        : canonical_(std::move(canonical)), receipt_(std::move(receipt)) {}

    std::vector<packs::ArchiveEntry> inspect(const std::filesystem::path& path, const packs::ArchiveLimits& limits,
                                           std::stop_token stop) override {
        return canonical_->inspect(path, limits, stop);
    }
    std::string read(const std::filesystem::path& path, std::string_view name, std::uint64_t limit,
                     std::stop_token stop) override {
        return canonical_->read(path, name, limit, stop);
    }
    void extract_file(const std::filesystem::path& path, std::string_view name, const std::filesystem::path& destination,
                      std::uint64_t size, std::stop_token stop) override {
        canonical_->extract_file(path, name, destination, size, stop);
    }
    void release(const std::filesystem::path& path) noexcept override { canonical_->release(path); }

    void write(const std::filesystem::path& path, std::span<const packs::ArchiveInput> inputs,
               const packs::ArchiveLimits& limits, std::stop_token stop) override {
        check_stop(stop);
        std::optional<PinnedArtifact> pin;
        try {
            const auto launcher = std::find_if(inputs.begin(), inputs.end(), [](const auto& input) {
                return input.name == "Nickel.exe" || input.name == "Nickel-launcher";
            });
            const auto manifest = std::find_if(inputs.begin(), inputs.end(), [](const auto& input) {
                return input.name == packs::portable_manifest_file;
            });
            if (manifest != inputs.end()) {
                if (manifest->file || manifest->bytes.empty() || manifest->bytes.size() > 1024 * 1024
                    || std::count_if(inputs.begin(), inputs.end(), [](const auto& input) {
                        return input.name == packs::portable_manifest_file;
                    }) != 1) reject();
                const auto envelope = packs::validate_portable_manifest(Json::parse(manifest->bytes));
                if (envelope.contains("launcher")) {
                    const auto& declared = envelope.at("launcher");
                    if (declared.at("path") != "Nickel.exe" || declared.at("os") != "windows"
                        || declared.at("arch") != "x64" || launcher == inputs.end()) reject();
                } else if (launcher != inputs.end()) {
                    reject();
                }
            } else if (launcher != inputs.end()) {
                reject();
            }
            if (launcher != inputs.end()) {
                if (launcher->name != "Nickel.exe" || !launcher->file || !launcher->bytes.empty()
                    || std::count_if(inputs.begin(), inputs.end(), [](const auto& input) {
                        return input.name == "Nickel.exe" || input.name == "Nickel-launcher";
                    }) != 1) reject();
                const auto& implementation = *receipt_->implementation_;
                verify_source(implementation.source, stop);
                pin.emplace(pin_artifact(*launcher->file, stop));
                validate_candidate(*pin, implementation.source, implementation.material, stop);
                for (const auto& notice : implementation.material.notices) {
                    const auto count = std::count_if(inputs.begin(), inputs.end(), [&](const auto& input) {
                        return input.name == notice.name;
                    });
                    if (count != 1) reject();
                }
                for (const auto& input : inputs) {
                    if (std::find(required_notices.begin(), required_notices.end(), input.name) == required_notices.end()
                        && !input.name.starts_with("LICENSES/")) continue;
                    const auto expected = std::find_if(implementation.material.notices.begin(),
                        implementation.material.notices.end(), [&](const auto& notice) { return notice.name == input.name; });
                    if (expected == implementation.material.notices.end() || input.file || input.bytes != expected->bytes) reject();
                }
                verify_source(implementation.source, stop);
            }
        } catch (...) {
            check_stop(stop);
            reject();
        }
        check_stop(stop);
        // Canonical write owns the final commit. Only noexcept leaf/ancestor release may follow it.
        canonical_->write(path, inputs, limits, stop);
        pin.reset();
    }

private:
    std::shared_ptr<packs::ArchiveBackend> canonical_;
    std::shared_ptr<const TrustedLauncherReceipt> receipt_;
};

std::shared_ptr<packs::ArchiveBackend>
TrustedLauncherReceipt::protect_archive(std::shared_ptr<packs::ArchiveBackend> canonical_archive) const {
    if (!canonical_archive) reject("missing_service");
    return std::make_shared<TrustedLauncherArchive>(std::move(canonical_archive), shared_from_this());
}

std::vector<LauncherNoticeInfo> compiled_launcher_notices() {
    std::vector<LauncherNoticeInfo> result;
#ifdef NICKEL_HAS_TRUSTED_LAUNCHER_BUILD
    if (build::trusted_launcher_notices.size() > notice_count_limit) return {};
    std::uint64_t total = 0;
    result.reserve(build::trusted_launcher_notices.size());
    for (const auto& notice : build::trusted_launcher_notices) {
        // Same bounds as the receipt path: fail closed on any malformed metadata.
        if (notice.name.empty() || notice.name.size() > 4096 || !digest_text(notice.sha256)
            || notice.byte_count == 0 || notice.byte_count > std::uint64_t{4} * 1024 * 1024
            || notice.bytes.empty() == notice.resource.empty()
            || total > notice_bytes_limit - notice.byte_count) return {};
        total += notice.byte_count;
        result.push_back({notice.name, notice.sha256, notice.byte_count});
    }
#endif
    return result;
}

std::optional<std::string> read_compiled_launcher_notice(std::string_view name, std::size_t limit) {
#ifdef NICKEL_HAS_TRUSTED_LAUNCHER_BUILD
    // Exact match against compiled metadata only: no caller path, glob or case folding.
    if (name.empty() || name.size() > 4096 || limit == 0 || limit > notice_bytes_limit) return {};
    try {
        for (const auto& notice : build::trusted_launcher_notices) {
            if (notice.name != name) continue;
            if (notice.byte_count == 0 || notice.byte_count > limit || !digest_text(notice.sha256)
                || notice.bytes.empty() == notice.resource.empty()) return {};
            std::span<const std::byte> payload;
            if (!load_notice_payload(notice, payload)) return {};
            const std::string_view bytes(reinterpret_cast<const char*>(payload.data()), payload.size());
            if (bytes.find('\0') != std::string_view::npos
                || !transport::detail::digest_equal(digest_bytes(bytes), notice.sha256)) return {};
            return std::string(bytes);
        }
    } catch (...) {
        return {};
    }
#else
    (void)name;
    (void)limit;
#endif
    return {};
}

} // namespace nickel::app
