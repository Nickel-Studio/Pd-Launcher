#pragma once

#include "../auth.hpp"
#include "internal.hpp"

#include <functional>

namespace nickel::accounts::detail {

struct JsonResponse {
    int status = 0;
    Json body;
    std::chrono::seconds retry_after{0};
};
JsonResponse request_json(IdentityHttp& http, std::string_view method,
                          std::string_view url, std::string body,
                          std::string_view content_type,
                          std::string_view bearer, std::stop_token stop,
                          const IdentityLog& log, std::string_view stage,
                          std::size_t limit = 1024 * 1024);
JsonResponse post_json(IdentityHttp& http, std::string_view url,
                       const Json& body, std::stop_token stop,
                       const IdentityLog& log, std::string_view stage);
JsonResponse post_form(IdentityHttp& http, std::string_view url,
                       const std::vector<std::pair<std::string,std::string>>& fields,
                       std::stop_token stop, const IdentityLog& log,
                       std::string_view stage);

struct OAuthTokens {
    Secret access;
    std::optional<Secret> refresh;
};
OAuthTokens oauth_tokens(const Json& body);
struct MinecraftSession {
    SkinProfile profile;
    Secret token;
    std::uint64_t expires_at = 0;
    std::string xuid;
};
MinecraftSession microsoft_chain(IdentityHttp& http, const Secret& msa,
                                  std::uint64_t now_secs,
                                  std::stop_token stop, const IdentityLog& log,
                                  const std::function<void()>& checkpoint);

struct YggResponse {
    Secret access;
    std::string client;
    std::optional<SkinProfile> selected;
    std::vector<SkinProfile> available;
};
std::string authentication_base(AccountType type, std::string_view server);
YggResponse ygg_authenticate(IdentityHttp& http, AccountType type,
                             std::string_view server, std::string_view username,
                             const Secret& password, std::stop_token stop,
                             const IdentityLog& log);
YggResponse ygg_refresh(IdentityHttp& http, AccountType type,
                        std::string_view server, const Secret& access,
                        std::string_view client, const std::optional<SkinProfile>& profile,
                        std::stop_token stop, const IdentityLog& log);
void ygg_require_success(const JsonResponse& response);
void merge_ygg(StoredAccount& account, const YggResponse& response,
               const std::optional<SkinProfile>& fallback);
SkinProfile parse_profile(const Json& value);
bool valid_profile_id(std::string_view value) noexcept;

} // namespace nickel::accounts::detail
