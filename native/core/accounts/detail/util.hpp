#pragma once

#include "../accounts.hpp"

#include "core/common/json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::accounts::detail {

using Json = nickel::Json;

void erase_bytes(void* data, std::size_t size) noexcept;
void erase_string(std::string& value) noexcept;
struct EraseString final {
    std::string& value;
    ~EraseString() { erase_string(value); }
};
struct EraseJson final {
    Json& value;
    ~EraseJson();
};

std::string trim(std::string_view value);
std::string ascii_lower(std::string_view value);
bool valid_utf8(std::string_view value) noexcept;
std::string random_hex(std::size_t bytes = 16);
std::array<std::uint8_t, 16> md5(std::string_view input);
Json parse_json(std::string_view text, std::size_t byte_limit,
                std::size_t depth_limit = 32);
std::string string_field(const Json& value, std::string_view key,
                         bool required = false, std::size_t limit = 16384);
std::uint64_t uint_field(const Json& value, std::string_view key,
                         std::uint64_t fallback = 0,
                         std::uint64_t maximum = UINT64_MAX);
void check_stop(std::stop_token stop);
void safe_log(const IdentityLog& log, std::string_view stage,
              std::string_view outcome, int http_status = 0) noexcept;

struct Url {
    std::string scheme;
    std::string host;
    std::uint16_t port = 0;
    std::string path;
    std::string query;
    std::string origin() const;
    std::string str() const;
};
Url parse_url(std::string_view url, bool allow_query = false);
std::string join_url(std::string_view base, std::string_view path);
std::string urlencode(std::string_view input);
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields);
std::vector<std::uint8_t> base64_decode(std::string_view encoded,
                                       std::size_t limit);
std::string base64_encode(std::span<const std::uint8_t> bytes);

} // namespace nickel::accounts::detail
