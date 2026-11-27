#pragma once

#include "offline_skin.hpp"

namespace nickel::media {

// Stateless adapters to canonical foundation/tasks/archive services. Supplied
// instance/launch resolution remains authoritative; no private path guessing.
class NativeSkinFileStore final : public SkinFileStore {
public:
    bool directory(const std::filesystem::path& path) override;
    std::filesystem::path canonical_directory(const std::filesystem::path& path) override;
    std::string directory_identity(const std::filesystem::path& path) override;
    std::unique_ptr<SkinFileLock> pin_directory(const std::filesystem::path& path) override;
    std::optional<SkinBytes> read(const std::filesystem::path& path, std::size_t max_bytes) override;
    std::unique_ptr<SkinOptionsSnapshot> observe_options(
        const std::filesystem::path& path, std::stop_token stop) override;
    void check_write_capacity(const std::filesystem::path& path, std::uint64_t bytes) override;
    void ensure_directory(const std::filesystem::path& path) override;
    void create_directory(const std::filesystem::path& path) override;
    void create_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) override;
    void replace_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) override;
    std::unique_ptr<SkinFileLock> lock(const std::filesystem::path& runtime,
                                      std::stop_token stop) override;
};

class NativeClientSkinArchive final : public ClientSkinArchive {
public:
    std::vector<SkinArchiveEntry> entries(const std::filesystem::path& jar,
                                         std::stop_token stop) override;
    SkinBytes read(const std::filesystem::path& jar, std::string_view entry,
                    std::size_t max_bytes, std::stop_token stop) override;
};

} // namespace nickel::media
