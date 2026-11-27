#include "core/network/url_policy.hpp"
#include "core/tasks/secure_fs.hpp"
#include "core/network/control.hpp"
#include "core/network/errors.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <curl/curl.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace nickel::transport::detail {
namespace {
struct CurlUrlDeleter { void operator()(CURLU* value) const { curl_url_cleanup(value); } };
using CurlUrl = std::unique_ptr<CURLU, CurlUrlDeleter>;
std::string part(CURLU* url, CURLUPart key, unsigned flags = 0) {
    char* value{};
    const auto code = curl_url_get(url, key, &value, flags);
    if (code != CURLUE_OK) return {};
    std::string out(value); curl_free(value); return out;
}
std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}
void check(const std::function<bool()>& cancelled) {
    detail::check_cancel(cancelled);
}

bool parse_ip(std::string_view input, std::array<unsigned char, 16>& bytes, bool& ipv6) noexcept {
    std::string text(input);
    if (text.starts_with('[') && text.ends_with(']')) text = text.substr(1, text.size() - 2);
    if (text.find('%') != std::string::npos) return false;
    if (inet_pton(AF_INET, text.c_str(), bytes.data()) == 1) { ipv6 = false; return true; }
    if (inet_pton(AF_INET6, text.c_str(), bytes.data()) == 1) { ipv6 = true; return true; }
    return false;
}
bool allowed_v4(const unsigned char* ip) noexcept {
    const auto a = ip[0], b = ip[1], c = ip[2];
    return !(a == 0 || a == 10 || a == 127 || a >= 224 ||
             (a == 100 && b >= 64 && b <= 127) || (a == 169 && b == 254) ||
             (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
             (a == 192 && b == 0) || (a == 192 && b == 2) ||
             (a == 192 && b == 88 && c == 99) ||
             (a == 198 && (b == 18 || b == 19)) ||
             (a == 198 && b == 51 && c == 100) ||
             (a == 203 && b == 0 && c == 113));
}
#ifdef _WIN32
struct Winsock {
    Winsock() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Winsock initialization failed"); }
    ~Winsock() { WSACleanup(); }
};
void ensure_winsock() { static Winsock state; }
std::vector<std::string> dns(std::string host, const std::function<bool()>& cancelled,
                             std::chrono::milliseconds timeout) {
    ensure_winsock();
    const auto wide = tasks::detail::utf8_path(host).native();
    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    PADDRINFOEXW addresses{};
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) throw std::runtime_error("DNS event creation failed");
    HANDLE cancellation{};
    const auto code = GetAddrInfoExW(wide.c_str(), nullptr, NS_DNS, nullptr, &hints,
                                     &addresses, nullptr, &overlapped, nullptr,
                                     &cancellation);
    int result = code;
    if (code == WSA_IO_PENDING) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        bool aborted{};
        while (WaitForSingleObject(overlapped.hEvent, 20) == WAIT_TIMEOUT) {
            if ((cancelled && cancelled()) || std::chrono::steady_clock::now() >= deadline) {
                GetAddrInfoExCancel(&cancellation);
                aborted = true;
                break;
            }
        }
        if (aborted) WaitForSingleObject(overlapped.hEvent, INFINITE);
        result = GetAddrInfoExOverlappedResult(&overlapped);
        CloseHandle(overlapped.hEvent);
        if (aborted) {
            if (addresses) FreeAddrInfoExW(addresses);
            check(cancelled);
            throw std::runtime_error("DNS timeout");
        }
    } else { CloseHandle(overlapped.hEvent); }
    if (result) { if (addresses) FreeAddrInfoExW(addresses); throw std::runtime_error("DNS resolution failed"); }
    std::vector<std::string> out;
    for (auto* address = addresses; address; address = address->ai_next) {
        std::array<char, INET6_ADDRSTRLEN> text{};
        const void* bytes{};
        if (address->ai_family == AF_INET)
            bytes = &reinterpret_cast<const sockaddr_in*>(address->ai_addr)->sin_addr;
        else if (address->ai_family == AF_INET6)
            bytes = &reinterpret_cast<const sockaddr_in6*>(address->ai_addr)->sin6_addr;
        if (bytes && inet_ntop(address->ai_family, bytes, text.data(), text.size())) out.emplace_back(text.data());
        if (out.size() > 64) { FreeAddrInfoExW(addresses); throw std::runtime_error("DNS answer limit exceeded"); }
    }
    FreeAddrInfoExW(addresses);
    return out;
}
#else
std::vector<std::string> dns(std::string host, const std::function<bool()>& cancelled,
                             std::chrono::milliseconds timeout) {
    (void)timeout;
    check(cancelled);
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses{};
    if (getaddrinfo(host.c_str(), nullptr, &hints, &addresses)) throw std::runtime_error("DNS resolution failed");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(addresses, &freeaddrinfo);
    std::vector<std::string> out;
    for (auto* address = addresses; address; address = address->ai_next) {
        check(cancelled);
        std::array<char, INET6_ADDRSTRLEN> text{};
        const void* bytes{};
        if (address->ai_family == AF_INET) bytes = &reinterpret_cast<const sockaddr_in*>(address->ai_addr)->sin_addr;
        else if (address->ai_family == AF_INET6) bytes = &reinterpret_cast<const sockaddr_in6*>(address->ai_addr)->sin6_addr;
        if (bytes && inet_ntop(address->ai_family, bytes, text.data(), text.size())) out.emplace_back(text.data());
        if (out.size() > 64) throw std::runtime_error("DNS answer limit exceeded");
    }
    return out;
}
#endif
} // namespace

bool loopback_address(std::string_view text) noexcept {
    std::array<unsigned char, 16> bytes{}; bool ipv6{};
    if (!parse_ip(text, bytes, ipv6)) return false;
    if (!ipv6) return bytes[0] == 127;
    return std::all_of(bytes.begin(), bytes.end() - 1, [](auto byte) { return byte == 0; }) && bytes[15] == 1;
}
bool public_address(std::string_view text) noexcept {
    std::array<unsigned char, 16> bytes{}; bool ipv6{};
    if (!parse_ip(text, bytes, ipv6)) return false;
    if (!ipv6) return allowed_v4(bytes.data());
    // Only native global-unicast IPv6; deny mapped, NAT64, transition, ULA and documentation ranges.
    if ((bytes[0] & 0xe0) != 0x20) return false;
    if (bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] < 0x02) return false;
    if (bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0x0d && bytes[3] == 0xb8) return false;
    if (bytes[0] == 0x20 && bytes[1] == 0x02) return false;
    if (bytes[0] == 0x3f && bytes[1] == 0xff && (bytes[2] & 0xf0) == 0) return false;
    return true;
}
Url parse_url(std::string_view raw, TrustPolicy trust) {
    if (raw.empty() || raw.size() > 8192 || raw.find('\\') != std::string::npos ||
        std::any_of(raw.begin(), raw.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
        throw std::runtime_error("invalid request URL");
    CurlUrl handle(curl_url());
    if (!handle || curl_url_set(handle.get(), CURLUPART_URL, std::string(raw).c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("invalid request URL");
    Url url;
    url.url = part(handle.get(), CURLUPART_URL);
    url.scheme = lower(part(handle.get(), CURLUPART_SCHEME));
    url.host = lower(part(handle.get(), CURLUPART_HOST));
    if (url.host.starts_with('[') && url.host.ends_with(']')) url.host = url.host.substr(1, url.host.size() - 2);
    if (url.host.empty() || !part(handle.get(), CURLUPART_USER).empty() ||
        !part(handle.get(), CURLUPART_PASSWORD).empty() || !part(handle.get(), CURLUPART_OPTIONS).empty() ||
        !part(handle.get(), CURLUPART_FRAGMENT).empty() || !part(handle.get(), CURLUPART_ZONEID).empty() ||
        url.host.find('%') != std::string::npos || url.host.ends_with('.') ||
        std::any_of(url.host.begin(), url.host.end(), [](unsigned char c) { return c >= 0x80; }))
        throw std::runtime_error("request URL contains credentials or unsafe host");
    const auto port = part(handle.get(), CURLUPART_PORT, CURLU_DEFAULT_PORT);
    unsigned number{};
    const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
    if (error != std::errc{} || end != port.data() + port.size() || !number || number > 65535)
        throw std::runtime_error("invalid request port");
    url.port = static_cast<std::uint16_t>(number);
    std::array<unsigned char, 16> bytes{}; bool ipv6{};
    const auto literal = parse_ip(url.host, bytes, ipv6);
    if (trust == TrustPolicy::ControlledLoopback) {
        if ((url.scheme != "http" && url.scheme != "https") ||
            !(loopback_address(url.host) || url.host == "localhost"))
            throw std::runtime_error("controlled trust permits only loopback endpoints");
        url.loopback = true;
    } else {
        if (url.scheme != "https" || url.host == "localhost" || url.host.ends_with(".localhost") ||
            url.host.ends_with(".local") || (literal && !public_address(url.host)))
            throw std::runtime_error("request endpoint is not public HTTPS");
        if (!literal && url.host.find('.') == std::string::npos)
            throw std::runtime_error("public host must be fully qualified");
    }
    url.origin = url.scheme + "://" + (ipv6 ? "[" + url.host + "]" : url.host);
    if ((url.scheme == "https" && url.port != 443) || (url.scheme == "http" && url.port != 80))
        url.origin += ":" + std::to_string(url.port);
    return url;
}
Url resolve_url(std::string_view raw, TrustPolicy trust,
                const std::function<bool()>& cancelled, std::chrono::milliseconds timeout) {
    check(cancelled);
    auto url = parse_url(raw, trust);
    std::array<unsigned char, 16> bytes{}; bool ipv6{};
    if (parse_ip(url.host, bytes, ipv6)) url.addresses.push_back(url.host);
    else url.addresses = dns(url.host, cancelled, timeout);
    check(cancelled);
    if (url.addresses.empty()) throw std::runtime_error("DNS returned no usable addresses");
    for (const auto& address : url.addresses) {
        if (trust == TrustPolicy::ControlledLoopback ? !loopback_address(address) : !public_address(address))
            throw std::runtime_error("DNS resolved to a forbidden address");
    }
    std::sort(url.addresses.begin(), url.addresses.end());
    url.addresses.erase(std::unique(url.addresses.begin(), url.addresses.end()), url.addresses.end());
    return url;
}
std::string redirect_url(std::string_view base, std::string_view location) {
    if (location.empty() || location.size() > 8192 || location.find('\\') != std::string::npos ||
        std::any_of(location.begin(), location.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
        throw std::runtime_error("invalid redirect location");
    CurlUrl handle(curl_url());
    if (!handle || curl_url_set(handle.get(), CURLUPART_URL, std::string(base).c_str(), 0) != CURLUE_OK ||
        curl_url_set(handle.get(), CURLUPART_URL, std::string(location).c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("invalid redirect URL");
    return part(handle.get(), CURLUPART_URL);
}
std::string resolve_rule(const Url& url) {
    std::string out = url.host + ":" + std::to_string(url.port) + ":";
    for (std::size_t index = 0; index < url.addresses.size(); ++index) {
        if (index) out += ',';
        const auto& address = url.addresses[index];
        out += address.find(':') == std::string::npos ? address : "[" + address + "]";
    }
    return out;
}
std::string proxy_connection(const Url& target, const ProxySnapshot& proxy,
                             const std::function<bool()>& cancelled,
                             std::chrono::milliseconds timeout) {
    if (proxy.mode == ProxySnapshot::Mode::Direct) return {};
    const auto proxy_url = resolve_url(proxy.endpoint,
        proxy.allow_loopback_proxy ? TrustPolicy::ControlledLoopback : TrustPolicy::PublicHttps,
        cancelled, timeout);
    const auto& ip = proxy_url.addresses.front();
    (void)target;
    return (ip.find(':') == std::string::npos ? ip : "[" + ip + "]") + ":" + std::to_string(proxy_url.port);
}
} // namespace nickel::transport::detail

namespace nickel::transport {
std::string safe_origin(std::string_view url) {
    try { return detail::parse_url(url, TrustPolicy::PublicHttps).origin; }
    catch (...) {
        try { return detail::parse_url(url, TrustPolicy::ControlledLoopback).origin; }
        catch (...) { return "invalid-origin"; }
    }
}
void validate_url(std::string_view url, TrustPolicy trust) {
    detail::boundary("unsafe_url", "Request URL is not permitted", [&] { (void)detail::parse_url(url, trust); });
}
} // namespace nickel::transport
