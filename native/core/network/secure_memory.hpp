#pragma once

#include "core/network/http.hpp"
#include "core/network/url_policy.hpp"
#include <algorithm>
#include <atomic>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace nickel::transport::detail {
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
namespace testing {
inline thread_local void (*after_erase)(std::span<const std::byte>) noexcept = nullptr;
}
#endif
inline void erase_bytes(void* data, std::size_t size) noexcept {
    auto* output = static_cast<volatile unsigned char*>(data);
    for (std::size_t at = 0; at < size; ++at) output[at] = 0;
    std::atomic_signal_fence(std::memory_order_seq_cst);
#ifdef NICKEL_TRANSPORT_TEST_HOOKS
    if (testing::after_erase) testing::after_erase({static_cast<const std::byte*>(data), size});
#endif
}
inline void erase(std::string& value) noexcept {
    const auto size = value.size();
    // Include reserved short-string storage without writing outside the live character range.
    value.resize(value.capacity(), '\0');
    erase_bytes(value.data(), value.size());
    value.resize(size);
}
inline void erase(std::vector<std::byte>& value) noexcept { erase_bytes(value.data(), value.size()); }
inline void erase(std::vector<std::string>& value) noexcept { for (auto& item : value) erase(item); }
inline void erase(Header& value) noexcept { erase(value.name); erase(value.value); }
inline void erase(Headers& value) noexcept { for (auto& header : value) erase(header); }
inline void erase(Url& value) noexcept {
    erase(value.url); erase(value.scheme); erase(value.host); erase(value.origin);
    for (auto& address : value.addresses) erase(address);
}
template<class Predicate> void erase_headers_if(Headers& headers, Predicate predicate) {
    std::size_t kept{};
    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (predicate(headers[index])) {
            erase(headers[index]);
        } else {
            if (kept != index) {
                erase(headers[kept]);
                headers[kept] = std::move(headers[index]);
                erase(headers[index]);
            }
            ++kept;
        }
    }
    headers.resize(kept);
}
inline void erase(HttpRequest& value) noexcept {
    erase(value.url); erase(value.headers); erase(value.body);
    for (auto& origin : value.allowed_origins) erase(origin);
}
inline void erase(ResponseHead& value) noexcept { erase(value.headers); erase(value.effective_url); }
inline void erase(HttpResponse& value) noexcept { erase(static_cast<ResponseHead&>(value)); erase(value.body); }
inline void erase(NetworkSnapshot& value) noexcept {
    erase(value.proxy.endpoint); erase(value.proxy.username); erase(value.proxy.password); erase(value.user_agent);
}
inline void erase(RequestCandidate& value) noexcept { erase(value.url); erase(value.headers); }
inline void erase(std::vector<RequestCandidate>& value) noexcept { for (auto& item : value) erase(item); }
template<class T> void erase(std::optional<T>& value) noexcept { if (value) erase(*value); }

template<class T> class EraseGuard {
public:
    explicit EraseGuard(T& value) noexcept : value_(&value) {}
    ~EraseGuard() { if (value_) erase(*value_); }
    EraseGuard(const EraseGuard&) = delete;
    EraseGuard& operator=(const EraseGuard&) = delete;
    void release() noexcept { value_ = nullptr; }
private:
    T* value_;
};

template<class T> T take(T& value) noexcept(std::is_nothrow_move_constructible_v<T>) {
    return T(std::move(value));
}
inline void copy_sensitive(Headers& output, const Headers& input) {
    erase(output); output.clear(); output.reserve(std::max<std::size_t>(128, input.size()));
    for (const auto& header : input) {
        output.emplace_back();
        output.back().name = header.name; output.back().value = header.value;
    }
}
inline void copy_sensitive(ResponseHead& output, const ResponseHead& input) {
    output.status = input.status; copy_sensitive(output.headers, input.headers);
    output.effective_url = input.effective_url; output.content_length = input.content_length;
    output.transferred_bytes = input.transferred_bytes;
}
inline void copy_sensitive(HttpResponse& output, const HttpResponse& input) {
    copy_sensitive(static_cast<ResponseHead&>(output), static_cast<const ResponseHead&>(input));
    output.body = input.body;
}
inline void copy_sensitive(HttpRequest& output, const HttpRequest& input) {
    output.method = input.method; output.url = input.url; copy_sensitive(output.headers, input.headers);
    output.body = input.body; output.max_response_bytes = input.max_response_bytes;
    output.redirect_policy = input.redirect_policy; output.trust = input.trust;
    output.retry = input.retry; output.max_redirects = input.max_redirects; output.timeout = input.timeout;
    output.allowed_origins.reserve(input.allowed_origins.size());
    for (const auto& origin : input.allowed_origins) {
        output.allowed_origins.emplace_back(); output.allowed_origins.back() = origin;
    }
}
inline void copy_sensitive(NetworkSnapshot& output, const NetworkSnapshot& input) {
    output.version_list_source = input.version_list_source; output.file_download_source = input.file_download_source;
    output.community_api_source = input.community_api_source; output.proxy.mode = input.proxy.mode;
    output.proxy.endpoint = input.proxy.endpoint; output.proxy.username = input.proxy.username;
    output.proxy.password = input.proxy.password; output.proxy.allow_loopback_proxy = input.proxy.allow_loopback_proxy;
    output.ca_bundle = input.ca_bundle; output.user_agent = input.user_agent;
    output.connect_timeout = input.connect_timeout; output.idle_timeout = input.idle_timeout;
    output.total_timeout = input.total_timeout;
}
template<class T> void copy_sensitive(std::optional<T>& output, const std::optional<T>& input) {
    if (input) { output.emplace(); copy_sensitive(*output, *input); }
}

// Capture owning DTOs without unguarded aggregate/vector copies, including allocation failures.
template<class T> class Erased {
public:
    Erased() = default;
    explicit Erased(const T& input) {
        try { copy_sensitive(value_, input); }
        catch (...) { erase(value_); throw; }
    }
    explicit Erased(T&& input) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(input)) { erase(input); }
    Erased(const Erased& input) : Erased(input.value_) {}
    Erased(Erased&& input) noexcept(std::is_nothrow_move_constructible_v<T>)
        : Erased(std::move(input.value_)) {}
    Erased& operator=(const Erased&) = delete;
    Erased& operator=(Erased&&) = delete;
    ~Erased() { erase(value_); }
    T& get() noexcept { return value_; }
    const T& get() const noexcept { return value_; }
    T take() noexcept(std::is_nothrow_move_constructible_v<T>) { return detail::take(value_); }
private:
    T value_{};
};
} // namespace nickel::transport::detail
