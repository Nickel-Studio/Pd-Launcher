#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "fixtures.hpp"
#include "../../core/accounts/transport_adapter.hpp"

#include <charconv>
#include <future>
#include <thread>

namespace identity_test {
namespace {
using namespace nickel::accounts;
using namespace nickel::media;
using Json = nickel::accounts::detail::Json;
using namespace std::chrono_literals;
#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
void close_socket(NativeSocket value) { closesocket(value); }
struct SocketRuntime {
    SocketRuntime() { WSADATA data{}; require(WSAStartup(MAKEWORD(2,2), &data) == 0, "fixture Winsock startup"); }
    ~SocketRuntime() { WSACleanup(); }
};
#else
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;
void close_socket(NativeSocket value) { close(value); }
struct SocketRuntime {};
#endif
struct Socket {
    NativeSocket value = invalid_socket;
    explicit Socket(NativeSocket handle = invalid_socket) : value(handle) {}
    ~Socket() { if (value != invalid_socket) close_socket(value); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
bool readable(NativeSocket socket) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket, &read_set);
    timeval timeout{0,50000};
#ifdef _WIN32
    return select(0, &read_set, nullptr, nullptr, &timeout) > 0;
#else
    return select(socket + 1, &read_set, nullptr, nullptr, &timeout) > 0;
#endif
}
void send_all(NativeSocket socket, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto size = static_cast<int>(std::min<std::size_t>(bytes.size(),4096));
#ifdef _WIN32
        const auto sent = send(socket,bytes.data(),size,0);
#else
        const auto sent = send(socket,bytes.data(),static_cast<std::size_t>(size),MSG_NOSIGNAL);
#endif
        if (sent <= 0) return;
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
}
struct WireRequest {
    std::string method;
    std::string path;
    std::vector<std::pair<std::string,std::string>> headers;
    std::string body;
    std::string header(std::string_view name) const {
        for (const auto& [key,value] : headers) if (key == name) return value;
        return {};
    }
};
class LoopbackFixture {
    SocketRuntime runtime_;
    Socket listener_{socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)};
    std::uint16_t port_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::vector<WireRequest> requests_;
    std::string failure_;
    bool released_ = false;
    std::jthread worker_;

    WireRequest read_request(NativeSocket socket, std::stop_token stop) {
        std::string bytes;
        std::size_t header_end = std::string::npos;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        auto receive = [&] {
            while (!readable(socket)) {
                require(!stop.stop_requested() && std::chrono::steady_clock::now() < deadline, "fixture request receive deadline");
            }
            std::array<char,4096> chunk{};
            const auto count = recv(socket,chunk.data(),static_cast<int>(chunk.size()),0);
            require(count > 0 && bytes.size() + static_cast<std::size_t>(count) <= 32768, "fixture request byte bound");
            bytes.append(chunk.data(),static_cast<std::size_t>(count));
        };
        while ((header_end = bytes.find("\r\n\r\n")) == std::string::npos) receive();
        WireRequest request;
        const auto first_end = bytes.find("\r\n");
        const auto first_space = bytes.find(' '), second_space = bytes.find(' ',first_space + 1);
        require(first_end < header_end && first_space < second_space && second_space < first_end, "fixture request line");
        request.method = bytes.substr(0,first_space);
        request.path = bytes.substr(first_space + 1,second_space - first_space - 1);
        for (auto start = first_end + 2; start < header_end;) {
            const auto end = bytes.find("\r\n",start), colon = bytes.find(':',start);
            require(colon < end, "fixture request header");
            request.headers.emplace_back(detail::ascii_lower(bytes.substr(start,colon-start)), detail::trim(std::string_view(bytes).substr(colon+1,end-colon-1)));
            start = end + 2;
        }
        std::size_t length = 0;
        const auto content_length = request.header("content-length");
        if (!content_length.empty()) {
            const auto [end,error] = std::from_chars(content_length.data(),content_length.data()+content_length.size(),length);
            require(error == std::errc{} && end == content_length.data()+content_length.size() && length <= 16384, "fixture request body bound");
        }
        while (bytes.size() - header_end - 4 < length) receive();
        request.body = bytes.substr(header_end + 4,length);
        return request;
    }
    void reply(NativeSocket socket, int status, std::string_view body, std::string_view headers = {}) {
        send_all(socket,"HTTP/1.1 " + std::to_string(status) + " Fixture\r\nConnection: close\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\n" + std::string(headers) + "\r\n" + std::string(body));
    }
    void respond(NativeSocket socket, const WireRequest& request, std::stop_token stop) {
        if (request.path == "/echo") reply(socket,200,request.body,"X-Fixture: MixedCase\r\n");
        else if (request.path == "/repeat") reply(socket,200,"ok","Cache-Control: max-age=900\r\nSet-Cookie: fixture_cookie=1\r\n");
        else if (request.path == "/no-retry") reply(socket,503,"FIXTURE_RESPONSE_SECRET");
        else if (request.path == "/redirect") reply(socket,302,"redirect","Location: /should-not-hit\r\n");
        else if (request.path == "/over-header") reply(socket,200,std::string(129,'a'));
        else if (request.path == "/over-chunked") send_all(socket,"HTTP/1.1 200 Fixture\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n40\r\n" + std::string(64,'a') + "\r\n10\r\n" + std::string(16,'b') + "\r\n0\r\n\r\n");
        else if (request.path == "/malformed") send_all(socket,"HTTP/1.1 200 Fixture\r\nContent-Length: 2\r\nContent-Length: 3\r\nConnection: close\r\n\r\nok");
        else if (request.path == "/slow") {
            send_all(socket,"HTTP/1.1 200 Fixture\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n");
            std::unique_lock lock(mutex_);
            condition_.wait(lock,stop,[&] { return released_; });
            lock.unlock();
            send_all(socket,"0\r\n\r\n");
        } else if (request.path == "/api/yggdrasil/authserver/authenticate") {
            const auto body = Json::parse(request.body);
            const auto value = Json{{"accessToken","FIXTURE_RUNTIME_ACCESS"},{"clientToken",body.at("clientToken")},
                {"availableProfiles",Json::array({{{"id","069a79f444e94726a5befca90e38aaf5"},{"name","First"}},
                                                 {{"id","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},{"name","Second"}}})}};
            reply(socket,200,value.dump(),"Content-Type: application/json\r\nSet-Cookie: fixture_auth=1\r\n");
        } else if (request.path == "/api/yggdrasil/authserver/refresh") {
            const auto body = Json::parse(request.body);
            reply(socket,200,Json{{"accessToken","FIXTURE_RUNTIME_ROTATED"},{"clientToken",body.at("clientToken")},
                                 {"selectedProfile",body.at("selectedProfile")}}.dump(),"Content-Type: application/json\r\n");
        } else if (request.path == "/api/yggdrasil/authserver/validate") reply(socket,204,{});
        else if (request.path.starts_with("/api/yggdrasil/sessionserver/session/minecraft/profile/")) {
            const auto text = Json{{"textures",{{"SKIN",{{"url",origin()+"/texture/start"}}}}}}.dump();
            const auto property = detail::base64_encode({reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});
            reply(socket,200,Json{{"properties",Json::array({{{"name","textures"},{"value",property}}})}}.dump(),"Content-Type: application/json\r\n");
        } else if (request.path == "/texture/start") reply(socket,302,{},"Location: /texture/pixel\r\nSet-Cookie: fixture_image=1\r\n");
        else if (request.path == "/texture/pixel") reply(socket,200,std::string("\x89PNG\r\n\x1a\n",8),"Content-Type: image/png\r\n");
        else reply(socket,404,{});
    }
    void run(std::stop_token stop) noexcept {
        while (!stop.stop_requested()) {
            if (!readable(listener_.value)) continue;
            Socket client{accept(listener_.value,nullptr,nullptr)};
            if (client.value == invalid_socket) continue;
            try {
                auto request = read_request(client.value,stop);
                {
                    std::lock_guard lock(mutex_);
                    requests_.push_back(request);
                    condition_.notify_all();
                }
                respond(client.value,request,stop);
            } catch (...) {
                std::lock_guard lock(mutex_);
                if (!stop.stop_requested()) failure_ = "native loopback fixture failed";
            }
        }
    }
public:
    LoopbackFixture() {
        require(listener_.value != invalid_socket, "fixture socket creation");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener_.value,reinterpret_cast<sockaddr*>(&address),sizeof(address)) == 0 && listen(listener_.value,8) == 0,
                "fixture bind loopback ephemeral port");
#ifdef _WIN32
        int size = sizeof(address);
#else
        socklen_t size = sizeof(address);
#endif
        require(getsockname(listener_.value,reinterpret_cast<sockaddr*>(&address),&size) == 0, "fixture bound address");
        port_ = ntohs(address.sin_port);
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ~LoopbackFixture() { worker_.request_stop(); condition_.notify_all(); worker_.join(); }
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::vector<WireRequest> requests() const { std::lock_guard lock(mutex_); return requests_; }
    void wait_for(std::string_view path) {
        std::unique_lock lock(mutex_);
        require(condition_.wait_for(lock,3s,[&] { return std::any_of(requests_.begin(),requests_.end(),[&](const auto& value) { return value.path == path; }); }),
                "fixture wait for actual request");
    }
    void release() { std::lock_guard lock(mutex_); released_ = true; condition_.notify_all(); }
    unsigned count(std::string_view path) const {
        const auto values = requests();
        return static_cast<unsigned>(std::count_if(values.begin(),values.end(),[&](const auto& value) { return value.path == path; }));
    }
    void verify() const { std::lock_guard lock(mutex_); require(failure_.empty(), "native loopback fixture has no internal failures"); }
};
nickel::transport::NetworkSnapshot network_snapshot() {
    nickel::transport::NetworkSnapshot snapshot;
    snapshot.connect_timeout = 1s;
    snapshot.idle_timeout = 2s;
    snapshot.total_timeout = 3s;
    return snapshot;
}
IdentityHttpRequest http_request(std::string method, std::string url) {
    IdentityHttpRequest request;
    request.method = std::move(method);
    request.url = std::move(url);
    request.max_response_bytes = 256;
    return request;
}
template<class Action> void expect_safe_network_error(Action action) {
    try { action(); }
    catch (const IdentityError& error) {
        require(error.code() == IdentityErrorCode::network && std::string(error.what()).find("FIXTURE_") == std::string::npos &&
                error.error().details.dump().find("FIXTURE_") == std::string::npos, "owner failure maps to static safe identity error");
        return;
    }
    require(false,"expected actual transport network error");
}
void test_actual_wire_policy_and_bounds() {
    LoopbackFixture fixture;
    nickel::transport::HttpClient client(network_snapshot());
    auto adapter = transport_identity_http(client);
    auto request = http_request("POST",fixture.origin()+"/echo");
    request.headers = {{"X-Fixture","request"},{"Authorization","Bearer FIXTURE_RUNTIME_SECRET"}};
    request.body = {0,255,7,10};
    request.sensitive = true;
    const auto echo = adapter->perform(request,{});
    require(echo.status == 200 && echo.header("x-FIXTURE") == "MixedCase" && echo.body == request.body,
            "actual owner preserves method, raw bytes and case-insensitive response headers");
    require(fixture.requests().front().header("authorization") == "Bearer FIXTURE_RUNTIME_SECRET", "explicit auth header reaches only requested origin");
    auto redirect = http_request("GET",fixture.origin()+"/redirect");
    require(adapter->perform(redirect,{}).status == 302 && fixture.count("/should-not-hit") == 0, "adapter disables actual automatic redirects");
    require(adapter->perform(http_request("GET",fixture.origin()+"/no-retry"),{}).status == 503 && fixture.count("/no-retry") == 1,
            "adapter disables actual transient GET retry");
    for (unsigned i = 0; i < 2; ++i) adapter->perform(http_request("GET",fixture.origin()+"/repeat"),{});
    require(fixture.count("/repeat") == 2 && client.cache_stats().entries == 0, "identity calls actual uncached perform");
    for (const auto path : {"/over-header","/over-chunked","/malformed"}) {
        auto bounded = http_request("GET",fixture.origin()+path);
        bounded.max_response_bytes = 64;
        expect_safe_network_error([&] { adapter->perform(bounded,{}); });
    }
    expect_safe_network_error([&] { adapter->perform(http_request("GET","https://169.254.169.254/latest/meta-data"),{}); });
    auto injection = http_request("GET",fixture.origin()+"/must-not-hit");
    injection.headers = {{"Authorization","FIXTURE_SECRET\r\nCookie: injected"}};
    expect_safe_network_error([&] { adapter->perform(injection,{}); });
    require(fixture.count("/must-not-hit") == 0, "unsafe headers reject before actual socket request");
    for (const auto& value : fixture.requests()) require(value.header("cookie").empty(), "actual transport never replays Set-Cookie");
    const auto stats = client.budget()->stats();
    require(stats.active_connections == 0 && stats.queued_connections == 0 && stats.reserved_buffer_bytes == 0, "all actual error/response budget leases release");
    fixture.verify();
}
void test_actual_transfer_and_queue_cancellation() {
    LoopbackFixture fixture;
    auto budget = std::make_shared<nickel::transport::SharedBudget>(nickel::transport::BudgetLimits{1,0,8*1024*1024});
    nickel::transport::HttpClient client(network_snapshot(),budget);
    auto adapter = transport_identity_http(client);
    nickel::Cancellation first_cancel, queued_cancel;
    auto call = [&](std::string path, const nickel::Cancellation& cancel) {
        try { adapter->perform(http_request("GET",fixture.origin()+path),cancel.stop_token()); return false; }
        catch (const IdentityError& error) { return error.code() == IdentityErrorCode::cancelled; }
    };
    auto first = std::async(std::launch::async,[&] { return call("/slow",first_cancel); });
    fixture.wait_for("/slow");
    auto queued = std::async(std::launch::async,[&] { return call("/repeat",queued_cancel); });
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (budget->stats().queued_connections == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    const bool was_queued = budget->stats().queued_connections == 1;
    queued_cancel.cancel();
    const bool queued_prompt = queued.wait_for(1s) == std::future_status::ready;
    first_cancel.cancel();
    const bool first_prompt = first.wait_for(1s) == std::future_status::ready;
    fixture.release();
    const auto queued_cancelled = queued.get(), transfer_cancelled = first.get();
    require(was_queued && queued_prompt && first_prompt && queued_cancelled && transfer_cancelled && fixture.count("/repeat") == 0,
            "canonical cancellation aborts actual queued permit and in-flight curl transfer promptly");
    const auto stats = budget->stats();
    require(stats.active_connections == 0 && stats.queued_connections == 0 && stats.reserved_buffer_bytes == 0, "cancelled actual transport returns every budget lease");
    fixture.verify();
}
class FixtureDecoder final : public ImageDecoder {
public:
    ImageProbe probe(std::span<const std::uint8_t>) override { return {64,64,"image/png",1}; }
    RgbaImage decode(std::span<const std::uint8_t>, const ImageProbe&, const ImageLimits&, std::stop_token stop) override {
        detail::check_stop(stop);
        return image(64,64,{20,40,60,255});
    }
};
void test_auth_and_media_production_http_constructors() {
    LoopbackFixture fixture;
    nickel::transport::HttpClient client(network_snapshot());
    MemoryStorage storage;
    FixtureVault vault;
    AccountService accounts(storage,vault);
    AuthService auth(accounts,client);
    const std::optional<std::string> server = fixture.origin()+"/api/yggdrasil";
    const auto pending = auth.skin_login(server,"fixture@example.invalid",Secret("FIXTURE_RUNTIME_PASSWORD"),AccountType::yggdrasil);
    require(pending.status == SkinLoginOutcome::Status::select && pending.selection && storage.writes == 0, "actual HTTP pending profiles never persist credentials");
    const auto account = auth.skin_select(pending.selection->session_id,pending.selection->generation,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    require(account.name == "Second" && !account.uuid.empty() && normalize_uuid(account.uuid) == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" && storage.writes == 1,
            "actual HTTP selection refresh binds selectedProfile before commit");
    require(public_account_json(account).find("FIXTURE_") == std::string::npos && Json::parse(storage.accounts).at(0).at("accessToken") == "FIXTURE_RUNTIME_ROTATED",
            "actual transport auth preserves private rotated token but strips public DTO");
    require(auth.skin_validate(account.id), "actual 204 Yggdrasil validation");
    FixtureDecoder decoder;
    ImageService images(client,decoder);
    SkinRequest skin;
    skin.account_id = account.id;
    skin.source = AccountType::yggdrasil;
    skin.name = account.name;
    skin.uuid = account.uuid;
    skin.server = server;
    skin.output_size = 64;
    const auto head = images.head(skin);
    require(!head.fallback && images.head(skin).cached && fixture.count("/texture/start") == 1 && fixture.count("/texture/pixel") == 1,
            "actual owner HTTP is manually redirected only after media validates approved origin and then cached by identity");
    const auto wire = fixture.requests();
    const auto login = Json::parse(wire.at(0).body), refresh = Json::parse(wire.at(1).body);
    require(login.at("password") == "FIXTURE_RUNTIME_PASSWORD" && refresh.at("clientToken") == login.at("clientToken") &&
            refresh.at("selectedProfile").at("id") == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "actual auth wire casing and selected-profile client binding");
    for (const auto& request : wire) {
        require(request.header("cookie").empty(), "auth/image responses never introduce cookie state");
        if (request.path.find("sessionserver") != std::string::npos || request.path.starts_with("/texture/"))
            require(request.header("authorization").empty(), "actual image requests carry no authorization credentials");
    }
    fixture.verify();
}
} // namespace
} // namespace identity_test

int main() {
    try {
        identity_test::test_actual_wire_policy_and_bounds();
        identity_test::test_actual_transfer_and_queue_cancellation();
        identity_test::test_auth_and_media_production_http_constructors();
        std::cout << "transport runtime: actual HttpClient wire DTO, no retry/redirect/cookies/cache, streaming byte quotas, safe URL/header rejection, queued/in-flight cancellation, auth selected-profile binding and media redirects passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "identity native transport fixture failure: " << error.what() << '\n';
        return 1;
    }
}
