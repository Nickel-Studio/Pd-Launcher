#pragma once
#include "packs.hpp"

namespace nickel::packs {
// Opt-in real ZIP test infrastructure; production uses the transport adapter.
std::shared_ptr<ArchiveBackend> make_zip_backend();
class ZipArchiveBackend final : public ArchiveBackend {
public:
    ZipArchiveBackend();
    ~ZipArchiveBackend() override;
    std::vector<ArchiveEntry> inspect(const fs::path&, const ArchiveLimits&, std::stop_token) override;
    std::string read(const fs::path&, std::string_view, std::uint64_t, std::stop_token) override;
    void extract_file(const fs::path&, std::string_view, const fs::path&,
                      std::uint64_t, std::stop_token) override;
    void write(const fs::path&, std::span<const ArchiveInput>, const ArchiveLimits&,
               std::stop_token) override;
    void release(const fs::path&) noexcept override;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace nickel::packs
