#include "protocol.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace nickel::accounts::detail {
namespace {
constexpr std::string_view xbl_url = "https://user.auth.xboxlive.com/user/authenticate";
constexpr std::string_view xsts_url = "https://xsts.auth.xboxlive.com/xsts/authorize";
constexpr std::string_view minecraft_login_url = "https://api.minecraftservices.com/authentication/login_with_xbox";
constexpr std::string_view entitlement_url = "https://api.minecraftservices.com/entitlements/mcstore";
constexpr std::string_view profile_url = "https://api.minecraftservices.com/minecraft/profile";

void erase_request(IdentityHttpRequest& request) noexcept {
    if (!request.body.empty()) erase_bytes(request.body.data(), request.body.size());
    for (auto& field : request.headers) erase_string(field.second);
}
struct EraseRequest {
    IdentityHttpRequest& request;
    ~EraseRequest() { erase_request(request); }
};
struct EraseResponse {
    IdentityHttpResponse& response;
    ~EraseResponse() { if (!response.body.empty()) erase_bytes(response.body.data(), response.body.size()); }
};

void require_ok(const JsonResponse& response) {
    if (response.status == 401 || response.status == 403) throw IdentityError(IdentityErrorCode::unauthorized, response.status);
    if (response.status != 200) throw IdentityError(IdentityErrorCode::network, response.status);
}

const Json& claim(const Json& response) {
    const auto found = response.find("DisplayClaims");
    if (found == response.end() || !found->is_object()) throw IdentityError(IdentityErrorCode::invalid_response);
    const auto xui = found->find("xui");
    if (xui == found->end() || !xui->is_array() || xui->empty() || !(*xui)[0].is_object()) throw IdentityError(IdentityErrorCode::invalid_response);
    return (*xui)[0];
}

IdentityErrorCode xsts_code(std::uint64_t code) {
    switch (code) {
    case 2148916233: return IdentityErrorCode::xbox_account_missing;
    case 2148916238: return IdentityErrorCode::xbox_child_account;
    case 2148916235: return IdentityErrorCode::xbox_region;
    case 2148916227: return IdentityErrorCode::xbox_banned;
    case 2148916236: case 2148916237: return IdentityErrorCode::xbox_verification;
    default: return IdentityErrorCode::unauthorized;
    }
}

YggResponse ygg_response(const JsonResponse& response, std::string requested_client) {
    ygg_require_success(response);
    YggResponse result;
    auto access = string_field(response.body, "accessToken", true);
    EraseString erase{access};
    result.access = Secret(access);
    result.client = string_field(response.body, "clientToken");
    if (result.client.empty()) result.client = std::move(requested_client);
    else if (result.client != requested_client) throw IdentityError(IdentityErrorCode::invalid_response);
    const auto selected = response.body.find("selectedProfile");
    if (selected != response.body.end() && !selected->is_null()) result.selected = parse_profile(*selected);
    const auto available = response.body.find("availableProfiles");
    if (available != response.body.end()) {
        if (!available->is_array() || available->size() > 256) throw IdentityError(IdentityErrorCode::invalid_response);
        std::set<std::string> ids;
        for (const auto& profile : *available) {
            auto parsed = parse_profile(profile);
            if (!ids.insert(normalize_uuid(parsed.id)).second) throw IdentityError(IdentityErrorCode::invalid_response);
            result.available.push_back(std::move(parsed));
        }
    }
    return result;
}
}

JsonResponse request_json(IdentityHttp& http, std::string_view method,
                          std::string_view url, std::string body,
                          std::string_view content_type, std::string_view bearer,
                          std::stop_token stop, const IdentityLog& log,
                          std::string_view stage, std::size_t limit) {
    EraseString erase_body{body};
    check_stop(stop);
    const auto parsed_url = parse_url(url, true);
    if (parsed_url.scheme != "https" && parsed_url.host != "127.0.0.1" && parsed_url.host != "[::1]" && parsed_url.host != "localhost")
        throw IdentityError(IdentityErrorCode::invalid_server);
    IdentityHttpRequest request;
    EraseRequest erase{request};
    request.method = std::string(method);
    request.url = parsed_url.str();
    request.headers = {{"Accept", "application/json"}};
    if (!content_type.empty()) request.headers.emplace_back("Content-Type", content_type);
    if (!bearer.empty()) request.headers.emplace_back("Authorization", "Bearer " + std::string(bearer));
    request.body.assign(body.begin(), body.end());
    request.sensitive = true;
    request.max_response_bytes = limit;
    IdentityHttpResponse response;
    EraseResponse erase_response{response};
    try { response = http.perform(request, stop); }
    catch (const IdentityError&) { throw; }
    catch (...) { safe_log(log, stage, "transport_failure"); check_stop(stop); throw IdentityError(IdentityErrorCode::network); }
    check_stop(stop);
    if (response.body.size() > limit) throw IdentityError(IdentityErrorCode::invalid_response);
    if (response.status < 100 || response.status > 599) throw IdentityError(IdentityErrorCode::invalid_response);
    // No authentication redirect is ever followed, including same-origin POST replay.
    if (response.status >= 300 && response.status < 400) throw IdentityError(IdentityErrorCode::network, response.status);
    safe_log(log, stage, response.status >= 200 && response.status < 300 ? "success" : "http_failure", response.status);
    JsonResponse result;
    result.status = response.status;
    if (response.body.empty()) result.body = Json::object();
    else result.body = parse_json({reinterpret_cast<const char*>(response.body.data()), response.body.size()}, limit);
    const auto retry = response.header("Retry-After");
    unsigned delay = 0;
    const auto parsed = std::from_chars(retry.data(), retry.data() + retry.size(), delay);
    if (parsed.ec == std::errc{} && parsed.ptr == retry.data() + retry.size()) result.retry_after = std::chrono::seconds(std::min(delay, 3600U));
    return result;
}

JsonResponse post_json(IdentityHttp& http, std::string_view url, const Json& body,
                       std::stop_token stop, const IdentityLog& log, std::string_view stage) {
    return request_json(http, "POST", url, body.dump(), "application/json", {}, stop, log, stage);
}

JsonResponse post_form(IdentityHttp& http, std::string_view url,
                       const std::vector<std::pair<std::string,std::string>>& fields,
                       std::stop_token stop, const IdentityLog& log, std::string_view stage) {
    return request_json(http, "POST", url, form_encode(fields), "application/x-www-form-urlencoded", {}, stop, log, stage);
}

OAuthTokens oauth_tokens(const Json& body) {
    OAuthTokens result;
    auto access = string_field(body, "access_token", true);
    EraseString erase{access};
    result.access = Secret(access);
    auto refresh = string_field(body, "refresh_token");
    EraseString erase_refresh{refresh};
    if (!refresh.empty()) result.refresh.emplace(refresh);
    const auto type = ascii_lower(string_field(body, "token_type"));
    if (!type.empty() && type != "bearer") throw IdentityError(IdentityErrorCode::invalid_response);
    return result;
}

MinecraftSession microsoft_chain(IdentityHttp& http, const Secret& msa, std::uint64_t now,
                                  std::stop_token stop, const IdentityLog& log,
                                  const std::function<void()>& checkpoint) {
    checkpoint();
    Json xbl_body{{"Properties", {{"AuthMethod", "RPS"}, {"SiteName", "user.auth.xboxlive.com"}, {"RpsTicket", "d=" + std::string(msa.view())}}},
                  {"RelyingParty", "http://auth.xboxlive.com"}, {"TokenType", "JWT"}};
    EraseJson erase_xbl_body{xbl_body};
    auto xbl = post_json(http, xbl_url, xbl_body, stop, log, "msa.xbl");
    EraseJson erase_xbl{xbl.body};
    require_ok(xbl);
    auto xbl_token = string_field(xbl.body, "Token", true);
    EraseString erase_xbl_token{xbl_token};
    const auto xbl_uhs = string_field(claim(xbl.body), "uhs", true, 256);
    checkpoint();
    Json xsts_body{{"Properties", {{"SandboxId", "RETAIL"}, {"UserTokens", Json::array({xbl_token})}}},
                   {"RelyingParty", "rp://api.minecraftservices.com/"}, {"TokenType", "JWT"}};
    EraseJson erase_xsts_body{xsts_body};
    auto xsts = post_json(http, xsts_url, xsts_body, stop, log, "msa.xsts");
    EraseJson erase_xsts{xsts.body};
    if (xsts.status != 200) throw IdentityError(xsts_code(uint_field(xsts.body, "XErr")), xsts.status);
    auto xsts_token = string_field(xsts.body, "Token", true);
    EraseString erase_xsts_token{xsts_token};
    const auto uhs = string_field(claim(xsts.body), "uhs", true, 256);
    if (uhs != xbl_uhs) throw IdentityError(IdentityErrorCode::invalid_response);
    auto xuid = string_field(claim(xsts.body), "xid", false, 64);
    if (!xuid.empty() && !std::all_of(xuid.begin(), xuid.end(), [](char c) { return c >= '0' && c <= '9'; })) throw IdentityError(IdentityErrorCode::invalid_response);
    checkpoint();
    Json minecraft_body{{"identityToken", "XBL3.0 x=" + uhs + ";" + xsts_token}};
    EraseJson erase_minecraft_body{minecraft_body};
    auto login = post_json(http, minecraft_login_url, minecraft_body, stop, log, "msa.minecraft");
    EraseJson erase_login{login.body};
    require_ok(login);
    auto token_value = string_field(login.body, "access_token", true);
    EraseString erase_token{token_value};
    Secret minecraft_token(token_value);
    const auto expires = uint_field(login.body, "expires_in", 0, 31 * 24 * 3600);
    if (!expires || expires > std::numeric_limits<std::uint64_t>::max() - now) throw IdentityError(IdentityErrorCode::invalid_response);
    checkpoint();
    auto entitlements = request_json(http, "GET", entitlement_url, {}, {}, minecraft_token.view(), stop, log, "msa.entitlement");
    EraseJson erase_entitlements{entitlements.body};
    require_ok(entitlements);
    const auto items = entitlements.body.find("items");
    if (items == entitlements.body.end() || !items->is_array() || items->size() > 256) throw IdentityError(IdentityErrorCode::invalid_response);
    bool owned = false;
    for (const auto& item : *items) {
        const auto name = string_field(item, "name", true, 128);
        if (name == "game_minecraft" || name == "product_minecraft") owned = true;
    }
    if (!owned) throw IdentityError(IdentityErrorCode::no_entitlement);
    checkpoint();
    auto profile = request_json(http, "GET", profile_url, {}, {}, minecraft_token.view(), stop, log, "msa.profile");
    EraseJson erase_profile{profile.body};
    if (profile.status == 404) throw IdentityError(IdentityErrorCode::no_profile, profile.status);
    require_ok(profile);
    auto selected = parse_profile(profile.body);
    checkpoint();
    return {std::move(selected), std::move(minecraft_token), now + expires, std::move(xuid)};
}

bool valid_profile_id(std::string_view raw) noexcept {
    if (raw.size() != 32 && raw.size() != 36) return false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (raw.size() == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

SkinProfile parse_profile(const Json& value) {
    SkinProfile profile{string_field(value, "id", true, 64), string_field(value, "name", true, 256)};
    if (!valid_profile_id(profile.id)) throw IdentityError(IdentityErrorCode::invalid_response);
    profile.id = normalize_uuid(profile.id);
    return profile;
}

std::string authentication_base(AccountType type, std::string_view server) {
    if (type == AccountType::littleskin) return yggdrasil_base(server);
    if (type == AccountType::yggdrasil) return parse_url(server).str();
    throw IdentityError(IdentityErrorCode::invalid_input);
}

void ygg_require_success(const JsonResponse& response) {
    if (response.status == 200 || response.status == 204) return;
    if (response.status == 401 || response.status == 403) throw IdentityError(IdentityErrorCode::unauthorized, response.status);
    const auto error = ascii_lower(string_field(response.body, "error"));
    const auto message = ascii_lower(string_field(response.body, "errorMessage"));
    if (error == "forbiddenoperationexception" || message.find("invalid credentials") != std::string::npos || message.find("invalid token") != std::string::npos)
        throw IdentityError(IdentityErrorCode::unauthorized, response.status);
    if (message.find("no profile assigned") != std::string::npos) throw IdentityError(IdentityErrorCode::no_profile, response.status);
    // Untrusted server messages and exception payloads are never echoed to UI/logs.
    throw IdentityError(IdentityErrorCode::network, response.status);
}

YggResponse ygg_authenticate(IdentityHttp& http, AccountType type, std::string_view server,
                             std::string_view username, const Secret& password,
                             std::stop_token stop, const IdentityLog& log) {
    auto client = random_hex();
    Json body{{"username", username}, {"password", password.view()}, {"clientToken", client},
               {"agent", {{"name", "Minecraft"}, {"version", 1}}}, {"requestUser", true}};
    EraseJson erase{body};
    auto response = post_json(http, join_url(authentication_base(type, server), "/authserver/authenticate"), body, stop, log, "ygg.authenticate");
    EraseJson erase_response{response.body};
    return ygg_response(response, std::move(client));
}

YggResponse ygg_refresh(IdentityHttp& http, AccountType type, std::string_view server,
                        const Secret& access, std::string_view client,
                        const std::optional<SkinProfile>& profile,
                        std::stop_token stop, const IdentityLog& log) {
    if (access.empty() || client.empty()) throw IdentityError(IdentityErrorCode::missing_credentials);
    Json body{{"accessToken", access.view()}, {"clientToken", client}, {"requestUser", true}};
    EraseJson erase{body};
    if (profile) body["selectedProfile"] = Json{{"id", normalize_uuid(profile->id)}, {"name", profile->name}};
    auto response = post_json(http, join_url(authentication_base(type, server), "/authserver/refresh"), body, stop, log, "ygg.refresh");
    EraseJson erase_response{response.body};
    auto result = ygg_response(response, std::string(client));
    if (profile && result.selected && normalize_uuid(profile->id) != normalize_uuid(result.selected->id)) throw IdentityError(IdentityErrorCode::invalid_response);
    return result;
}

void merge_ygg(StoredAccount& account, const YggResponse& response,
               const std::optional<SkinProfile>& fallback) {
    account.access_token = std::string(response.access.view());
    account.client_token = response.client;
    if (response.selected || fallback) {
        const auto& selected = response.selected ? *response.selected : *fallback;
        account.public_account.name = selected.name;
        account.public_account.uuid = dash_uuid(selected.id);
    }
}

} // namespace nickel::accounts::detail
