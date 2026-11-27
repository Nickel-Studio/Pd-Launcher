#include "util.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace nickel::accounts::detail {

void erase_bytes(void* data, std::size_t size) noexcept {
    volatile auto* bytes = static_cast<volatile unsigned char*>(data);
    while (size-- != 0) *bytes++ = 0;
}

void erase_string(std::string& value) noexcept {
    if (!value.empty()) erase_bytes(value.data(), value.size());
    value.clear();
}

namespace {
void erase_json(Json& value) noexcept {
    if (value.is_string()) {
        erase_string(value.get_ref<std::string&>());
    } else if (value.is_array() || value.is_object()) {
        for (auto& child : value) erase_json(child);
    }
}
bool ascii_space(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
bool hex_digit(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
}

EraseJson::~EraseJson() { erase_json(value); }

std::string trim(std::string_view value) {
    while (!value.empty() && ascii_space(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && ascii_space(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}

std::string ascii_lower(std::string_view value) {
    std::string result(value);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

bool valid_utf8(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size();) {
        const auto c = static_cast<unsigned char>(value[i++]);
        if (c < 0x80) { if (c == 0) return false; continue; }
        std::uint32_t code = 0;
        unsigned count = 0;
        std::uint32_t minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; code = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; code = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; code = c & 0x07; minimum = 0x10000; }
        else return false;
        if (count > value.size() - i) return false;
        for (unsigned j = 0; j < count; ++j) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

std::string random_hex(std::size_t bytes) {
    if (bytes == 0 || bytes > 64) throw IdentityError(IdentityErrorCode::invalid_input);
    std::vector<unsigned char> buffer(bytes);
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, buffer.data(), static_cast<ULONG>(buffer.size()),
                       BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw IdentityError(IdentityErrorCode::vault);
    }
#else
    std::random_device source;
    for (auto& byte : buffer) byte = static_cast<unsigned char>(source());
#endif
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes * 2);
    for (const auto byte : buffer) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    erase_bytes(buffer.data(), buffer.size());
    return result;
}

std::array<std::uint8_t, 16> md5(std::string_view input) {
    constexpr std::array<std::uint32_t, 64> constants{
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    constexpr std::array<unsigned, 64> shifts{
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    if (input.size() > 1024 * 1024) throw IdentityError(IdentityErrorCode::invalid_input);
    std::vector<std::uint8_t> message(input.begin(), input.end());
    const std::uint64_t bit_count = static_cast<std::uint64_t>(message.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (unsigned i = 0; i < 8; ++i) message.push_back(static_cast<std::uint8_t>(bit_count >> (i * 8)));
    std::array<std::uint32_t, 4> state{0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::array<std::uint32_t, 16> words{};
        for (unsigned i = 0; i < 16; ++i) {
            for (unsigned j = 0; j < 4; ++j) words[i] |= static_cast<std::uint32_t>(message[offset + i * 4 + j]) << (j * 8);
        }
        auto a = state[0]; auto b = state[1]; auto c = state[2]; auto d = state[3];
        for (unsigned i = 0; i < 64; ++i) {
            std::uint32_t f = 0; unsigned g = 0;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const auto next = b + std::rotl(a + f + constants[i] + words[g], static_cast<int>(shifts[i]));
            a = d; d = c; c = b; b = next;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    }
    std::array<std::uint8_t, 16> result{};
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = 0; j < 4; ++j) result[i * 4 + j] = static_cast<std::uint8_t>(state[i] >> (j * 8));
    erase_bytes(message.data(), message.size());
    return result;
}

Json parse_json(std::string_view text, std::size_t byte_limit, std::size_t depth_limit) {
    if (text.size() > byte_limit || depth_limit == 0) throw IdentityError(IdentityErrorCode::invalid_response);
    std::size_t depth = 0;
    bool quoted = false, escaped = false;
    for (const char c : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > depth_limit) throw IdentityError(IdentityErrorCode::invalid_response); }
        else if (c == '}' || c == ']') { if (depth == 0) throw IdentityError(IdentityErrorCode::invalid_response); --depth; }
    }
    Json parsed = Json::parse(text.begin(), text.end(), nullptr, false);
    if (parsed.is_discarded()) throw IdentityError(IdentityErrorCode::invalid_response);
    return parsed;
}

std::string string_field(const Json& value, std::string_view key, bool required, std::size_t limit) {
    const auto it = value.is_object() ? value.find(std::string(key)) : value.end();
    if (it == value.end() || it->is_null()) {
        if (required) throw IdentityError(IdentityErrorCode::invalid_response);
        return {};
    }
    if (!it->is_string()) throw IdentityError(IdentityErrorCode::invalid_response);
    const auto& text = it->get_ref<const std::string&>();
    if (text.size() > limit || !valid_utf8(text) || (required && text.empty())) throw IdentityError(IdentityErrorCode::invalid_response);
    return text;
}

std::uint64_t uint_field(const Json& value, std::string_view key, std::uint64_t fallback, std::uint64_t maximum) {
    const auto it = value.is_object() ? value.find(std::string(key)) : value.end();
    if (it == value.end() || it->is_null()) return fallback;
    std::uint64_t result = 0;
    if (it->is_number_unsigned()) result = it->get<std::uint64_t>();
    else if (it->is_number_integer() && it->get<std::int64_t>() >= 0) result = static_cast<std::uint64_t>(it->get<std::int64_t>());
    else throw IdentityError(IdentityErrorCode::invalid_response);
    if (result > maximum) throw IdentityError(IdentityErrorCode::invalid_response);
    return result;
}

void check_stop(std::stop_token stop) {
    if (stop.stop_requested()) throw IdentityError(IdentityErrorCode::cancelled);
}

void safe_log(const IdentityLog& log, std::string_view stage, std::string_view outcome, int http_status) noexcept {
    if (!log) return;
    try { log(SafeIdentityEvent{stage, outcome, http_status}); } catch (...) { }
}

std::string Url::origin() const {
    const bool standard = (scheme == "https" && port == 443) || (scheme == "http" && port == 80);
    return scheme + "://" + host + (standard ? "" : ":" + std::to_string(port));
}

std::string Url::str() const { return origin() + path + (query.empty() ? "" : "?" + query); }

Url parse_url(std::string_view raw, bool allow_query) {
    auto fail = [] { throw IdentityError(IdentityErrorCode::invalid_server); };
    if (raw.empty() || raw.size() > 4096 || !valid_utf8(raw)) fail();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const auto c = static_cast<unsigned char>(raw[i]);
        if (c <= 0x20 || c == 0x7f || c == '\\' || c == '#') fail();
        if (c == '%' && (i + 2 >= raw.size() || !hex_digit(raw[i + 1]) || !hex_digit(raw[i + 2]))) fail();
    }
    const auto separator = raw.find("://");
    if (separator == std::string_view::npos) fail();
    Url result;
    result.scheme = ascii_lower(raw.substr(0, separator));
    if (result.scheme != "http" && result.scheme != "https") fail();
    auto rest = raw.substr(separator + 3);
    const auto authority_end = rest.find_first_of("/?");
    const auto authority = rest.substr(0, authority_end);
    if (authority.empty() || authority.find('@') != std::string_view::npos) fail();
    std::string_view host, port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close <= 1) fail();
        host = authority.substr(0, close + 1);
        for (std::size_t i = 1; i < close; ++i) if (!(hex_digit(authority[i]) || authority[i] == ':' || authority[i] == '.')) fail();
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') fail();
            port = authority.substr(close + 2);
            if (port.empty()) fail();
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string_view::npos) { port = authority.substr(colon + 1); if (port.empty()) fail(); }
        if (host.empty() || host.size() > 253 || host.front() == '.' || host.front() == '-' || host.back() == '-') fail();
        for (const unsigned char c : host) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) fail();
        if (host.find("..") != std::string_view::npos) fail();
    }
    result.host = ascii_lower(host);
    result.port = result.scheme == "https" ? 443 : 80;
    if (!port.empty()) {
        unsigned parsed = 0;
        const auto converted = std::from_chars(port.data(), port.data() + port.size(), parsed);
        if (converted.ec != std::errc{} || converted.ptr != port.data() + port.size() || parsed == 0 || parsed > 65535) fail();
        result.port = static_cast<std::uint16_t>(parsed);
    }
    rest = authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);
    const auto question = rest.find('?');
    if (question != std::string_view::npos && !allow_query) fail();
    result.path = std::string(rest.substr(0, question));
    if (question != std::string_view::npos) result.query = std::string(rest.substr(question + 1));
    if (!result.path.empty() && result.path.front() != '/') fail();
    auto lowered_path = ascii_lower(result.path);
    if (lowered_path.find("%2f") != std::string::npos || lowered_path.find("%5c") != std::string::npos || lowered_path.find("%00") != std::string::npos) fail();
    for (std::size_t start = 0; start < lowered_path.size();) {
        const auto end = lowered_path.find('/', start + 1);
        const auto segment = lowered_path.substr(start + 1, end == std::string::npos ? end : end - start - 1);
        if (segment == "." || segment == ".." || segment == "%2e" || segment == "%2e%2e" || segment == ".%2e" || segment == "%2e.") fail();
        if (end == std::string::npos) break;
        start = end;
    }
    return result;
}

std::string join_url(std::string_view base, std::string_view path) {
    while (!base.empty() && base.back() == '/') base.remove_suffix(1);
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    return std::string(base) + "/" + std::string(path);
}

std::string urlencode(std::string_view input) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size());
    for (const unsigned char c : input) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') output.push_back(static_cast<char>(c));
        else { output.push_back('%'); output.push_back(hex[c >> 4]); output.push_back(hex[c & 15]); }
    }
    return output;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string result;
    for (const auto& [key, value] : fields) {
        if (!result.empty()) result += '&';
        result += urlencode(key); result += '='; result += urlencode(value);
    }
    return result;
}

std::vector<std::uint8_t> base64_decode(std::string_view encoded, std::size_t limit) {
    if (encoded.empty() || encoded.size() % 4 != 0 || encoded.size() / 4 > (limit + 2) / 3) throw IdentityError(IdentityErrorCode::invalid_response);
    auto digit = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> result;
    result.reserve(std::min(limit, encoded.size() / 4 * 3));
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        const bool last = i + 4 == encoded.size();
        const int a = digit(encoded[i]), b = digit(encoded[i + 1]);
        const bool pad2 = encoded[i + 2] == '=', pad3 = encoded[i + 3] == '=';
        const int c = pad2 ? 0 : digit(encoded[i + 2]);
        const int d = pad3 ? 0 : digit(encoded[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad3) || ((pad2 || pad3) && !last) || (pad2 && (b & 15)) || (pad3 && !pad2 && (c & 3))) throw IdentityError(IdentityErrorCode::invalid_response);
        result.push_back(static_cast<std::uint8_t>((a << 2) | (b >> 4)));
        if (!pad2) result.push_back(static_cast<std::uint8_t>((b << 4) | (c >> 2)));
        if (!pad3) result.push_back(static_cast<std::uint8_t>((c << 6) | d));
        if (result.size() > limit) throw IdentityError(IdentityErrorCode::invalid_response);
    }
    return result;
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = bytes[i];
        const auto b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const auto c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        result.push_back(digits[a >> 2]);
        result.push_back(digits[((a & 3) << 4) | (b >> 4)]);
        result.push_back(i + 1 < bytes.size() ? digits[((b & 15) << 2) | (c >> 6)] : '=');
        result.push_back(i + 2 < bytes.size() ? digits[c & 63] : '=');
    }
    return result;
}

} // namespace nickel::accounts::detail
