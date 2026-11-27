#pragma once

#include "../../../native/core/accounts/accounts.hpp"
#include "../../../native/core/accounts/auth.hpp"
#include "../../../native/core/media/images.hpp"
#include "../../../native/core/accounts/detail/util.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace identity_test {

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
void expect_error(nickel::accounts::IdentityErrorCode code, Operation operation) {
    try { operation(); }
    catch (const nickel::accounts::IdentityError& error) {
        require(error.code() == code, "unexpected identity error code");
        return;
    }
    throw std::runtime_error("expected identity error was not thrown");
}

class MemoryStorage final : public nickel::accounts::AccountStorage {
public:
    std::string accounts = "[]";
    std::string active = "{\"id\":\"\"}";
    std::function<void()> before_write;
    bool fail_write = false;
    unsigned writes = 0;
    std::string read_accounts() override { return accounts; }
    void write_accounts(std::string_view text) override {
        if (before_write) before_write();
        if (fail_write) throw std::runtime_error("FIXTURE_STORAGE_SECRET_MUST_NOT_ESCAPE");
        accounts = std::string(text);
        ++writes;
    }
    std::string read_active() override { return active; }
    void write_active(std::string_view text) override { active = text; }
};

// Only a controlled vault fixture; production adapter delegates to foundation Vault.
class FixtureVault final : public nickel::accounts::SecretVault {
public:
    unsigned seals = 0;
    unsigned opens = 0;
    std::string seal(std::string_view value) override {
        ++seals;
        return "v1:" + nickel::accounts::detail::base64_encode({
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    }
    std::string open(std::string_view value) override {
        ++opens;
        if (!value.starts_with("v1:")) throw std::runtime_error("FIXTURE_VAULT_SECRET_MUST_NOT_ESCAPE");
        auto bytes = nickel::accounts::detail::base64_decode(value.substr(3), 64 * 1024);
        return {bytes.begin(), bytes.end()};
    }
};

inline nickel::media::RgbaImage image(unsigned width, unsigned height,
                                     std::array<std::uint8_t,4> color = {0,0,0,0}) {
    nickel::media::RgbaImage result{width, height, std::vector<std::uint8_t>(width * height * 4)};
    for (std::size_t i = 0; i < result.pixels.size(); i += 4)
        std::copy(color.begin(), color.end(), result.pixels.begin() + i);
    return result;
}

inline void fill(nickel::media::RgbaImage& image, unsigned x, unsigned y,
                 unsigned width, unsigned height, std::array<std::uint8_t,4> color) {
    for (unsigned row = y; row < y + height; ++row)
        for (unsigned col = x; col < x + width; ++col)
            std::copy(color.begin(), color.end(), image.pixels.begin() + (row * image.width + col) * 4);
}

inline std::array<std::uint8_t,4> pixel(const nickel::media::RgbaImage& image, unsigned x, unsigned y) {
    std::array<std::uint8_t,4> result{};
    std::copy_n(image.pixels.begin() + (y * image.width + x) * 4, 4, result.begin());
    return result;
}

void run_accounts_tests();
void run_account_safety_tests();
void run_auth_tests();
void run_media_tests();
void run_media_lifecycle_tests();
void run_media_publication_tests();
void run_common_tests();
void run_lifecycle_tests();
void run_builtin_asset_tests();

} // namespace identity_test
