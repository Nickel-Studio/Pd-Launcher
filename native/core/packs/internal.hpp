#pragma once
#include "packs.hpp"
#include "core/instances/domain_io.hpp"
#include <array>

namespace nickel::packs::detail {
namespace io = instances::detail;
using instances::Error;
using instances::path_from_utf8;
using instances::path_to_utf8;
using instances::reject_links;
inline constexpr std::size_t copy_buffer_bytes = 64 * 1024;
inline constexpr std::size_t policy_max_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t portable_json_max_bytes = 64 * 1024;
inline constexpr std::uint64_t background_max_bytes = 32 * 1024 * 1024;

class NativeFile {
public:
    enum class Mode { read, exclusive };
    NativeFile(const fs::path& path, Mode mode);
    ~NativeFile();
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    std::size_t read_at(std::uint64_t offset, void* buffer, std::size_t size);
    void write_at(std::uint64_t offset, const void* buffer, std::size_t size);
    std::uint64_t size() const;
    void unchanged() const;
    void flush();
    void close() noexcept;
private:
    fs::path path_;
    std::intptr_t handle_ = -1;
    std::vector<std::intptr_t> parents_;
    Mode mode_;
    std::uint64_t original_size_ = 0;
    std::array<std::uint64_t, 4> original_identity_{};
    std::array<std::uint64_t, 4> identity() const;
};
class Workspace {
public:
    explicit Workspace(const fs::path& parent);
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    const fs::path& directory() const noexcept;
    void cleanup();
private:
    fs::path directory_;
    std::string owner_;
};
std::uint64_t copy_file(const fs::path& source, const fs::path& destination,
                        std::uint64_t max_bytes, std::stop_token cancel);
bool valid_utf8(std::string_view text) noexcept;
bool credential_key(std::string_view key);
bool credential_value(std::string_view text);
bool embedded_credentials(std::string_view text);
void validate_content_file(const fs::path& file, std::string_view relative, bool portable = false);
void validate_background(const fs::path& file);
bool within(std::string_view path, std::string_view directory);
std::string required_text(const Json& value, std::string_view key, std::size_t max_bytes = 512);
std::uint64_t required_unsigned(const Json& value, std::string_view key, std::uint64_t maximum);
std::optional<std::string> optional_hash(const Json& hashes, std::string_view key, std::size_t length);
Json file_to_json(const PackFile& file);
void verify_pack_file(const PackFile& file, const fs::path& path, std::stop_token cancel);
std::optional<Json> settings_for_payload(const fs::path& payload);
void write_portable(const ExportRequest& request, const fs::path& payload,
                    const instances::SettingsProjection& settings, const fs::path& target,
                    const fs::path& workspace, const ExportServices& services, std::stop_token cancel);
} // namespace nickel::packs::detail
