#pragma once

#include "core/common/cancellation.hpp"
#include "core/packs/packs.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::app {

struct EmbeddedLauncherNotice {
    std::string_view name;
    // Inline payload bytes, or empty when the payload is compiled as a resource.
    std::string_view bytes;
    std::string_view sha256;
    // RCDATA resource name carrying the payload when `bytes` is empty.
    std::string_view resource;
    // Exact payload length, used to bound the resource read before hashing.
    std::uint64_t byte_count = 0;
};

// Bounded, read-only view of notice material compiled into this executable.
// Listing never loads payload bytes; reading verifies size and SHA256 before
// returning anything. Neither call executes notice content, resolves a caller
// path or grants the trusted-launcher receipt capability.
struct LauncherNoticeInfo {
    std::string_view name;
    std::string_view sha256;
    std::uint64_t bytes = 0;
};

[[nodiscard]] std::vector<LauncherNoticeInfo> compiled_launcher_notices();
[[nodiscard]] std::optional<std::string>
read_compiled_launcher_notice(std::string_view name, std::size_t limit);

class TrustedLauncherReceipt final : public std::enable_shared_from_this<TrustedLauncherReceipt> {
public:
    ~TrustedLauncherReceipt();
    TrustedLauncherReceipt(const TrustedLauncherReceipt&) = delete;
    TrustedLauncherReceipt& operator=(const TrustedLauncherReceipt&) = delete;

    [[nodiscard]] bool matches_application(const std::filesystem::path& declared_executable) const;
    [[nodiscard]] packs::ExportServices export_fields(const Cancellation& cancellation) const;
    [[nodiscard]] std::shared_ptr<packs::ArchiveBackend>
    protect_archive(std::shared_ptr<packs::ArchiveBackend> canonical_archive) const;

private:
    struct Impl;
    explicit TrustedLauncherReceipt(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
    friend std::shared_ptr<const TrustedLauncherReceipt> capture_trusted_launcher(const Cancellation&);
    friend class TrustedLauncherArchive;
};

// No caller-selected path or runtime notice file can create this capability.
// Missing compiled material or a non-production process image returns an empty receipt.
[[nodiscard]] std::shared_ptr<const TrustedLauncherReceipt>
capture_trusted_launcher(const Cancellation& cancellation = {});

} // namespace nickel::app
