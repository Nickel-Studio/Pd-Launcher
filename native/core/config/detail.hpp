#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <memory>
#include <string_view>
#include <string>

namespace nickel {
class Cancellation;
namespace foundation_detail {
[[noreturn]] void fail(std::string_view code, std::string_view message);
[[noreturn]] void io_fail(std::string_view operation, const std::filesystem::path& path,
                          int native_error);
void check_cancel(const Cancellation* cancellation);
bool cancelled(const Cancellation* cancellation) noexcept;
void random_bytes(std::span<std::uint8_t> output);
void erase_bytes(std::span<std::uint8_t> bytes) noexcept;
std::string sha256_text(std::string_view text);
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(std::span<const std::uint8_t> bytes);
    std::string finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool path_missing(const std::filesystem::path& path);
bool atomic_create_file(const std::filesystem::path& path,
                        std::span<const std::uint8_t> bytes, bool private_file);
bool remove_file_if_matches(const std::filesystem::path& path,
                            std::string_view expected_sha256);
bool move_file_if_matches(const std::filesystem::path& source,
                          const std::filesystem::path& destination,
                          std::string_view expected_sha256);
} // namespace foundation_detail
} // namespace nickel
