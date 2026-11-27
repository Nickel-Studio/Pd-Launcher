#include "platform/gpu_preferences.hpp"
#include "platform/gpu_preferences_detail.hpp"
#include "core/config/detail.hpp"
#include "core/config/path_guard.hpp"
#include "core/common/error.hpp"

#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nickel::foundation_detail {
namespace {
bool unicode_space(char16_t unit) {
    return (unit >= u'\t' && unit <= u'\r') || unit == u' ' || unit == 0x0085 || unit == 0x00a0 ||
        unit == 0x1680 || (unit >= 0x2000 && unit <= 0x200a) || unit == 0x2028 || unit == 0x2029 ||
        unit == 0x202f || unit == 0x205f || unit == 0x3000;
}

bool gpu_key(std::u16string_view key) {
    while (!key.empty() && unicode_space(key.front())) key.remove_prefix(1);
    while (!key.empty() && unicode_space(key.back())) key.remove_suffix(1);
    constexpr std::u16string_view expected = u"gpupreference";
    if (key.size() != expected.size()) return false;
    for (std::size_t index = 0; index < key.size(); ++index) {
        const auto unit = key[index];
        const auto lower = unit >= u'A' && unit <= u'Z' ? static_cast<char16_t>(unit + (u'a' - u'A')) : unit;
        if (lower != expected[index]) return false;
    }
    return true;
}

std::u16string registry_text(const std::optional<GpuRegistryValue>& original) {
    if (!original) return {};
    const auto& value = *original;
    if (value.bytes.size() > gpu_preference_max_bytes) fail("LimitExceeded", "GPU preference exceeds its bounded registry limit");
    if (value.type != 1 || value.bytes.size() < 2 || value.bytes.size() % 2 != 0 ||
        value.bytes[value.bytes.size() - 2] != 0 || value.bytes.back() != 0)
        fail("InvalidArgument", "Existing GPU preference is not a terminated REG_SZ value");
    std::u16string text;
    text.reserve(value.bytes.size() / 2 - 1);
    for (std::size_t index = 0; index + 2 < value.bytes.size(); index += 2) {
        text.push_back(static_cast<char16_t>(value.bytes[index] | (static_cast<unsigned>(value.bytes[index + 1]) << 8)));
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto unit = text[index];
        if (unit == 0) fail("InvalidArgument", "Existing GPU preference contains embedded NUL");
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (++index >= text.size() || text[index] < 0xdc00 || text[index] > 0xdfff)
                fail("InvalidArgument", "Existing GPU preference contains invalid UTF-16");
        } else if (unit >= 0xdc00 && unit <= 0xdfff) {
            fail("InvalidArgument", "Existing GPU preference contains invalid UTF-16");
        }
    }
    return text;
}
} // namespace

GpuRegistryValue high_performance_gpu_value(const std::optional<GpuRegistryValue>& original) {
    const auto text = registry_text(original);
    std::u16string applied;
    applied.reserve(text.size() + 17);
    bool replaced = false;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto delimiter = text.find(u';', start);
        const bool terminated = delimiter != std::u16string::npos;
        const auto end = terminated ? delimiter : text.size();
        const auto body = std::u16string_view(text).substr(start, end - start);
        const auto equal = body.find(u'=');
        if (equal != std::u16string_view::npos && gpu_key(body.substr(0, equal))) {
            applied += u"GpuPreference=2";
            replaced = true;
        } else {
            // Auto HDR and other options share this value; retain their exact fragments.
            applied.append(body);
        }
        if (terminated) applied.push_back(u';');
        start = terminated ? end + 1 : end;
    }
    if (!replaced) {
        if (!applied.empty() && applied.back() != u';') applied.push_back(u';');
        applied += u"GpuPreference=2;";
    }
    if (applied.size() + 1 > gpu_preference_max_bytes / 2)
        fail("LimitExceeded", "Applied GPU preference exceeds its bounded registry limit");
    GpuRegistryValue result{1, {}};
    result.bytes.reserve((applied.size() + 1) * 2);
    for (const auto unit : applied) {
        result.bytes.push_back(static_cast<std::uint8_t>(unit & 0xff));
        result.bytes.push_back(static_cast<std::uint8_t>(unit >> 8));
    }
    result.bytes.push_back(0);
    result.bytes.push_back(0);
    return result;
}
} // namespace nickel::foundation_detail

namespace nickel {
#ifdef _WIN32
namespace {
using namespace foundation_detail;
constexpr wchar_t gpu_key_path[] = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";

[[noreturn]] void registry_fail(std::string_view operation, LSTATUS status) {
    throw ErrorException(Error{"IoError", "Cannot " + std::string(operation) + " temporary GPU preference",
                               Json{{"nativeError", status}, {"operation", operation}}});
}

struct RegistryKey {
    HKEY value = nullptr;
    RegistryKey() {
        const auto status = RegCreateKeyExW(HKEY_CURRENT_USER, gpu_key_path, 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &value, nullptr);
        if (status != ERROR_SUCCESS) registry_fail("open", status);
    }
    ~RegistryKey() { if (value) RegCloseKey(value); }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    std::optional<GpuRegistryValue> read(const std::wstring& name) const {
        DWORD type = 0, size = 0;
        auto status = RegQueryValueExW(value, name.c_str(), nullptr, &type, nullptr, &size);
        if (status == ERROR_FILE_NOT_FOUND) return std::nullopt;
        if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) registry_fail("read", status);
        if (size > gpu_preference_max_bytes) fail("LimitExceeded", "GPU registry value exceeds its bounded read limit");
        const auto expected_type = type;
        std::vector<std::uint8_t> bytes(size);
        status = RegQueryValueExW(value, name.c_str(), nullptr, &type, bytes.data(), &size);
        if (status == ERROR_MORE_DATA || status == ERROR_FILE_NOT_FOUND || type != expected_type || size != bytes.size())
            fail("Conflict", "GPU registry value changed during its bounded read");
        if (status != ERROR_SUCCESS) registry_fail("read", status);
        return GpuRegistryValue{type, std::move(bytes)};
    }

    void write(const std::wstring& name, const std::optional<GpuRegistryValue>& data) const {
        if (data && data->bytes.size() > gpu_preference_max_bytes)
            fail("LimitExceeded", "GPU registry write exceeds its bounded limit");
        const auto status = data ? RegSetValueExW(value, name.c_str(), 0, data->type, data->bytes.data(),
            static_cast<DWORD>(data->bytes.size())) : RegDeleteValueW(value, name.c_str());
        if (status != ERROR_SUCCESS && !(status == ERROR_FILE_NOT_FOUND && !data)) registry_fail("write", status);
    }
};

struct ImageHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit ImageHandle(HANDLE file) : value(file) {}
    ~ImageHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    ImageHandle(const ImageHandle&) = delete;
    ImageHandle& operator=(const ImageHandle&) = delete;
};

struct JavaImage {
    DirectoryGuard parent;
    ImageHandle image;
    std::filesystem::path logical;
    explicit JavaImage(const std::filesystem::path& selected)
        : parent(checked_absolute_path(selected).parent_path()),
          image(reinterpret_cast<HANDLE>(windows_open_relative(parent, checked_absolute_path(selected).filename(),
              FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, 1, 0x40))) {
        if (image.value == INVALID_HANDLE_VALUE) io_fail("retain selected GPU Java image", selected, static_cast<int>(GetLastError()));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(image.value, &info)) io_fail("inspect selected GPU Java image", selected, static_cast<int>(GetLastError()));
        if (GetFileType(image.value) != FILE_TYPE_DISK || info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            fail("UnsafePath", "GPU preference requires an ordinary nofollow Java image");
        const auto required = GetFinalPathNameByHandleW(image.value, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!required || required >= 32767) fail("IoError", "Cannot resolve the retained GPU Java image name");
        std::vector<wchar_t> name(required);
        const auto written = GetFinalPathNameByHandleW(image.value, name.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!written || written >= required) fail("IoError", "Cannot resolve the retained GPU Java image name");
        logical = checked_absolute_path(std::filesystem::path(std::wstring_view(name.data(), written)));
        logical.make_preferred();
        parent.verify();
    }
};

struct CaseLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};
struct ActivePreferences {
    std::mutex mutex;
    std::set<std::wstring, CaseLess> java;
};
ActivePreferences& active_preferences() {
    // Reaper threads may outlive the UI; this small lease table has process lifetime.
    static auto* value = new ActivePreferences;
    return *value;
}
} // namespace

struct GpuPreferenceGuard::Impl {
    std::filesystem::path java;
    RegistryKey key;
    std::wstring name;
    std::optional<foundation_detail::GpuRegistryValue> original;
    foundation_detail::GpuRegistryValue applied;
    bool active = false;

    explicit Impl(const std::filesystem::path& selected) : java(selected), name(java.native()) {}
    ~Impl() { if (active) { try { (void)restore(); } catch (...) {} } }

    GpuPreferenceRestore restore() {
        auto& leases = active_preferences();
        std::scoped_lock lock(leases.mutex);
        if (!active) return GpuPreferenceRestore::Inactive;
        // One comparison/restore attempt. A failed read never licenses an overwrite.
        active = false;
        try {
            const auto current = key.read(name);
            const auto result = current == std::optional(applied) ? GpuPreferenceRestore::Restored : GpuPreferenceRestore::ExternalChange;
            if (result == GpuPreferenceRestore::Restored) key.write(name, original);
            leases.java.erase(name);
            return result;
        } catch (...) {
            leases.java.erase(name);
            throw;
        }
    }
};
#else
struct GpuPreferenceGuard::Impl {
    std::filesystem::path java;
};
#endif

bool high_performance_gpu_supported() noexcept {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

GpuPreferenceGuard::GpuPreferenceGuard(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuPreferenceGuard::~GpuPreferenceGuard() = default;
GpuPreferenceGuard::GpuPreferenceGuard(GpuPreferenceGuard&&) noexcept = default;
GpuPreferenceGuard& GpuPreferenceGuard::operator=(GpuPreferenceGuard&&) noexcept = default;

GpuPreferenceGuard GpuPreferenceGuard::acquire(const std::filesystem::path& selected_java) {
#ifdef _WIN32
    // Retain nofollow ancestors/image through application, not for the whole game:
    // a preference lease must not lock the Java runtime against unrelated work.
    JavaImage image(selected_java);
    auto impl = std::make_unique<Impl>(image.logical);
    auto& leases = active_preferences();
    std::scoped_lock lock(leases.mutex);
    if (leases.java.contains(impl->name)) foundation_detail::fail("Conflict", "Selected Java GPU preference is already owned by another game");
    impl->original = impl->key.read(impl->name);
    impl->applied = foundation_detail::high_performance_gpu_value(impl->original);
    leases.java.insert(impl->name);
    try {
        impl->key.write(impl->name, impl->applied);
        impl->active = true;
    } catch (...) {
        leases.java.erase(impl->name);
        throw;
    }
    return GpuPreferenceGuard(std::move(impl));
#else
    (void)selected_java;
    foundation_detail::fail("Unsupported", "Temporary high-performance GPU preferences are only supported on Windows");
#endif
}

const std::filesystem::path& GpuPreferenceGuard::java_executable() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->java : empty;
}
GpuPreferenceRestore GpuPreferenceGuard::restore() {
#ifdef _WIN32
    return impl_ ? impl_->restore() : GpuPreferenceRestore::Inactive;
#else
    return GpuPreferenceRestore::Inactive;
#endif
}
} // namespace nickel
