#pragma once

#ifdef _WIN32
#include "core/config/detail.hpp"
#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>

namespace nickel::foundation_detail {
struct PrivateSecurity {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    explicit PrivateSecurity(bool enabled) {
        if (!enabled) return;
        HANDLE raw_token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
            fail("PermissionDenied", "Cannot determine the current credential owner");
        }
        struct Token { HANDLE value; ~Token() { CloseHandle(value); } } token{raw_token};
        DWORD bytes = 0;
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
        if (!bytes) fail("PermissionDenied", "Cannot determine the current credential owner");
        std::vector<std::uint8_t> storage(bytes);
        if (!GetTokenInformation(token.value, TokenUser, storage.data(), bytes, &bytes)) {
            fail("PermissionDenied", "Cannot determine the current credential owner");
        }
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid)) {
            fail("PermissionDenied", "Cannot determine the current credential owner");
        }
        const std::wstring sddl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")(A;;FA;;;SY)";
        LocalFree(sid);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                  &descriptor, nullptr)) {
            fail("PermissionDenied", "Cannot protect the credential file");
        }
        attributes.lpSecurityDescriptor = descriptor;
    }
    ~PrivateSecurity() { if (descriptor) LocalFree(descriptor); }
    PrivateSecurity(const PrivateSecurity&) = delete;
    PrivateSecurity& operator=(const PrivateSecurity&) = delete;
    SECURITY_ATTRIBUTES* get() { return descriptor ? &attributes : nullptr; }
};
} // namespace nickel::foundation_detail
#endif
