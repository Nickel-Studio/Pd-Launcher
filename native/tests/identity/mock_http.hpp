#pragma once

#include "fixtures.hpp"
#include "../../core/accounts/http_port.hpp"

#include <deque>
#include <future>
#include <map>
#include <thread>

namespace identity_test {
using nickel::accounts::IdentityHttpRequest;
using nickel::accounts::IdentityHttpResponse;
using nickel::accounts::detail::Json;

inline std::string body_text(const IdentityHttpRequest& request) {
    return {request.body.begin(), request.body.end()};
}
inline IdentityHttpResponse json_response(int status, const Json& value) {
    const auto text = value.dump();
    return {status, {{"Content-Type", "application/json"}}, {text.begin(), text.end()}};
}

struct ExpectedHttp {
    std::string method;
    std::string url;
    std::function<IdentityHttpResponse(const IdentityHttpRequest&,std::stop_token)> respond;
};

class MockHttp final : public nickel::accounts::IdentityHttp {
public:
    std::mutex mutex;
    std::deque<ExpectedHttp> expected;
    std::atomic<unsigned> calls{0};
    std::atomic<unsigned> active{0};
    std::atomic<unsigned> maximum_active{0};
    std::string failure;

    void add(std::string method, std::string url,
             std::function<IdentityHttpResponse(const IdentityHttpRequest&,std::stop_token)> handler) {
        std::lock_guard lock(mutex);
        expected.push_back({std::move(method), std::move(url), std::move(handler)});
    }
    void add_json(std::string method, std::string url, int status, Json response) {
        add(std::move(method),std::move(url),[status,response = std::move(response)](const auto&,auto) { return json_response(status,response); });
    }
    IdentityHttpResponse perform(const IdentityHttpRequest& request, std::stop_token stop) override {
        ++calls;
        ExpectedHttp next;
        {
            std::lock_guard lock(mutex);
            if (expected.empty()) { failure = "unexpected controlled HTTP call"; throw std::runtime_error(failure); }
            next = std::move(expected.front());
            expected.pop_front();
            if (request.method != next.method || request.url != next.url) { failure = "controlled HTTP method/URL mismatch"; throw std::runtime_error(failure); }
            if (!request.max_response_bytes || request.max_response_bytes > 4 * 1024 * 1024) { failure = "missing HTTP response bound"; throw std::runtime_error(failure); }
        }
        const auto current = ++active;
        auto maximum = maximum_active.load();
        while (current > maximum && !maximum_active.compare_exchange_weak(maximum,current)) {}
        struct Finish { std::atomic<unsigned>& value; ~Finish() { --value; } } finish{active};
        return next.respond(request,stop);
    }
    void verify() {
        std::lock_guard lock(mutex);
        require(failure.empty(), failure.c_str());
        require(expected.empty(), "unconsumed controlled HTTP expectations");
    }
};

struct FakeClock {
    std::atomic<std::int64_t> elapsed{0};
    nickel::accounts::AuthOptions options() {
        nickel::accounts::AuthOptions result;
        result.clock.monotonic = [this] { return std::chrono::steady_clock::time_point(std::chrono::seconds(elapsed.load())); };
        result.clock.wall = [this] { return std::chrono::system_clock::time_point(std::chrono::seconds(1700000000 + elapsed.load())); };
        return result;
    }
    void advance(std::int64_t seconds) { elapsed += seconds; }
};

class PausePoint {
    std::mutex mutex;
    std::condition_variable condition;
    bool reached = false;
    bool released = false;
public:
    void pause() {
        std::unique_lock lock(mutex);
        reached = true;
        condition.notify_all();
        condition.wait(lock,[&] { return released; });
    }
    void wait() {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock,std::chrono::seconds(10),[&] { return reached; }), "pause point timeout");
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
};

inline Json profile(std::string name = "FixturePlayer", std::string id = "069a79f444e94726a5befca90e38aaf5") {
    return Json{{"id",std::move(id)},{"name",std::move(name)}};
}
inline void add_device(MockHttp& http, std::string code = "FIXTURE_DEVICE_SECRET", int expires = 900) {
    http.add("POST","https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode",
        [code = std::move(code),expires](const auto& request,auto) {
            const auto body = body_text(request);
            require(body == "client_id=fixture-client&scope=XboxLive.signin%20offline_access", "OAuth device form exact scope/client");
            require(request.sensitive, "OAuth request sensitive");
            return json_response(200,Json{{"device_code",code},{"user_code","FIXTURE-CODE"},
                {"verification_uri","https://www.microsoft.com/link"},{"interval",5},{"expires_in",expires}});
        });
}
inline void add_token(MockHttp& http, bool refresh = false, bool rotate = true) {
    http.add("POST","https://login.microsoftonline.com/consumers/oauth2/v2.0/token",
        [refresh,rotate](const auto& request,auto) {
            const auto body = body_text(request);
            require(body.find("client_id=fixture-client") != std::string::npos, "token client bound");
            require(body.find(refresh ? "grant_type=refresh_token" : "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code") != std::string::npos, "OAuth grant type exact");
            Json value{{"access_token","FIXTURE_MSA_SECRET"},{"token_type","Bearer"}};
            if (rotate) value["refresh_token"] = "FIXTURE_REFRESH_SECRET";
            return json_response(200,value);
        });
}
inline void add_minecraft_chain(MockHttp& http, bool entitlement = true,
                                std::function<void()> before_profile = {}) {
    http.add("POST","https://user.auth.xboxlive.com/user/authenticate",[](const auto& request,auto) {
        const auto value = Json::parse(body_text(request));
        require(value["Properties"]["AuthMethod"] == "RPS" && value["Properties"]["RpsTicket"] == "d=FIXTURE_MSA_SECRET", "XBL RPS protocol");
        require(value["Properties"]["SiteName"] == "user.auth.xboxlive.com" && value["RelyingParty"] == "http://auth.xboxlive.com" && value["TokenType"] == "JWT", "XBL exact JSON casing");
        return json_response(200,Json{{"Token","FIXTURE_XBL_SECRET"},{"DisplayClaims",{{"xui",Json::array({{{"uhs","fixture-uhs"}}})}}}});
    });
    http.add("POST","https://xsts.auth.xboxlive.com/xsts/authorize",[](const auto& request,auto) {
        const auto value = Json::parse(body_text(request));
        require(value["Properties"]["SandboxId"] == "RETAIL" && value["Properties"]["UserTokens"] == Json::array({"FIXTURE_XBL_SECRET"}), "XSTS retail token array");
        require(value["RelyingParty"] == "rp://api.minecraftservices.com/", "XSTS relying party");
        return json_response(200,Json{{"Token","FIXTURE_XSTS_SECRET"},{"DisplayClaims",{{"xui",Json::array({{{"uhs","fixture-uhs"},{"xid","1234567890"}}})}}}});
    });
    http.add("POST","https://api.minecraftservices.com/authentication/login_with_xbox",[](const auto& request,auto) {
        require(Json::parse(body_text(request))["identityToken"] == "XBL3.0 x=fixture-uhs;FIXTURE_XSTS_SECRET", "Minecraft identity token exact");
        return json_response(200,Json{{"access_token","FIXTURE_MC_SECRET"},{"expires_in",7200}});
    });
    http.add("GET","https://api.minecraftservices.com/entitlements/mcstore",[entitlement](const auto& request,auto) {
        require(request.headers.back() == std::pair<std::string,std::string>{"Authorization","Bearer FIXTURE_MC_SECRET"}, "entitlement bearer");
        return json_response(200,Json{{"items",entitlement ? Json::array({{{"name","game_minecraft"},{"signature","fixture-signature"}}}) : Json::array()}});
    });
    if (entitlement) http.add("GET","https://api.minecraftservices.com/minecraft/profile",[before_profile = std::move(before_profile)](const auto&,auto) {
        if (before_profile) before_profile();
        return json_response(200,profile());
    });
}

inline void add_ygg_auth(MockHttp& http, const std::string& base, bool multi = true,
                         bool selected = false) {
    http.add("POST",base+"/authserver/authenticate",[multi,selected](const auto& request,auto) {
        const auto body = Json::parse(body_text(request));
        require(body["agent"] == Json{{"name","Minecraft"},{"version",1}}, "Ygg agent exact case");
        require(body["requestUser"] == true && body["username"] == "fixture@example.invalid" && body["password"] == "FIXTURE_PASSWORD", "Ygg credentials only body");
        require(body["clientToken"].is_string() && body["clientToken"].template get<std::string>().size() == 32, "native client token");
        Json response{{"accessToken","FIXTURE_YGG_SECRET"},{"clientToken",body["clientToken"]}};
        auto values = Json::array({profile("First")});
        if (multi) values.push_back(profile("Second","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        response["availableProfiles"] = std::move(values);
        if (selected) response["selectedProfile"] = profile("First");
        return json_response(200,response);
    });
}
inline void add_ygg_refresh(MockHttp& http, const std::string& base,
                            std::string selected = "069a79f444e94726a5befca90e38aaf5",
                            std::function<void()> pause = {}) {
    http.add("POST",base+"/authserver/refresh",[selected = std::move(selected),pause = std::move(pause)](const auto& request,auto) {
        const auto body = Json::parse(body_text(request));
        require(body.contains("accessToken") && body.contains("clientToken") && body["requestUser"] == true, "Ygg refresh camelCase");
        require(body["selectedProfile"]["id"] == selected, "Ygg refresh binds selected profile");
        if (pause) pause();
        return json_response(200,Json{{"accessToken","FIXTURE_YGG_ROTATED"},{"clientToken",body["clientToken"]},{"selectedProfile",body["selectedProfile"]}});
    });
}

} // namespace identity_test
