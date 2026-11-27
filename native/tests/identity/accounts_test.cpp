#include "fixtures.hpp"
#include "offline_uuid_golden.hpp"
#include "../../core/accounts/detail/internal.hpp"
#include "../../core/accounts/detail/protocol.hpp"

#include <sstream>
#include <thread>

namespace identity_test {
using namespace nickel::accounts;
using nickel::accounts::detail::Json;

void run_accounts_tests() {
    for (const auto& golden : offline_uuid_golden) {
        require(offline_uuid(golden.name) == golden.uuid, "offline UUID matches immutable old Rust source golden");
        require(detail::valid_profile_id(golden.uuid), "old Rust golden is a valid dashed UUID");
    }
    require(offline_uuid("Notch") != "b50ad385829d3141a2167e7d7539ba7f", "old Rust identity is not Java MD5 identity");
    require(offline_uuid("Notch") != offline_uuid("notch"), "offline name case sensitivity");
    for (const auto& invalid : std::array<std::string, 9>{
            "\xff", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe7\x8e", "\x80",
            std::string("A\0B", 3), std::string(257, 'a'), repeat_offline_uuid_name("\xe7\x8e\xa9", 85) + "aa"}) {
        expect_error(IdentityErrorCode::invalid_input, [&] { offline_uuid(invalid); });
    }
    require(detail::valid_profile_id("069A79F444E94726A5BEFCA90E38AAF5"), "uppercase compact profile remains valid");
    require(detail::valid_profile_id("069A79F4-44E9-4726-A5BE-FCA90E38AAF5"), "uppercase dashed profile remains valid");
    require(!detail::valid_profile_id(std::string(4, '-') + std::string(32, 'a')), "misplaced UUID dashes are not repairable formatting");
    require(!detail::valid_profile_id(std::string(31, 'a')) && !detail::valid_profile_id(std::string(33, 'a')) &&
            !detail::valid_profile_id(std::string(32, 'g')), "profile UUID length and hexadecimal bounds");
    require(dash_uuid("069a79f444e94726a5befca90e38aaf5") == "069a79f4-44e9-4726-a5be-fca90e38aaf5", "dash UUID");
    require(normalize_uuid("069A79F4-44E9-4726-A5BE-FCA90E38AAF5") == "069a79f444e94726a5befca90e38aaf5", "normalize UUID");
    require(validate_server(std::nullopt) == "https://littleskin.cn", "default site");
    require(validate_server("example.com:8443/mc/") == "https://example.com:8443/mc", "site subpath and port");
    require(validate_server("https://example.com/mc/api/yggdrasil/") == "https://example.com/mc", "strip long API suffix");
    require(validate_server("https://example.com/mc/yggdrasil") == "https://example.com/mc/yggdrasil", "custom API root survives storage");
    require(yggdrasil_base("https://example.com/mc") == "https://example.com/mc/api/yggdrasil", "skin site API path");
    require(yggdrasil_base("https://example.com/mc/yggdrasil") == "https://example.com/mc/yggdrasil", "custom API root");
    require(site_base("https://example.com/mc/api/yggdrasil") == "https://example.com/mc", "avatar site path");
    require(yggdrasil_base("https://yggdrasil") == "https://yggdrasil/api/yggdrasil", "host name not API suffix");
    for (const auto* server : {"https:","https://","http:///","https:/host","http:host","ftp://host",
                                "https://user:secret@host","https://host?token=fixture","https://host#fragment",
                                "https://host/../escape","https://host/%2e%2e/escape","https://host\\escape"}) {
        expect_error(IdentityErrorCode::invalid_server, [&] { validate_server(std::string(server)); });
    }
    expect_error(IdentityErrorCode::invalid_server, [&] { validate_server(std::string(" "), true); });

    MemoryStorage storage;
    FixtureVault vault;
    std::string logs;
    AccountService accounts(storage, vault, [&](const SafeIdentityEvent& event) {
        logs += std::string(event.stage) + std::string(event.outcome);
    });
    auto offline = accounts.add_offline("FixturePlayer", "fixture-offline");
    const auto before_invalid = storage.accounts;
    const auto before_writes = storage.writes;
    expect_error(IdentityErrorCode::invalid_input, [&] { accounts.add_offline(""); });
    expect_error(IdentityErrorCode::invalid_input, [&] { accounts.add_offline(" \t\n"); });
    require(storage.accounts == before_invalid && storage.writes == before_writes, "empty raw golden does not allow blank account creation");
    accounts.set_active(offline.id);
    require(Json::parse(storage.accounts).is_array(), "storage must be direct array");
    require(Json::parse(storage.active) == Json{{"id", "fixture-offline"}}, "active must be id object");
    require(accounts.active()->id == offline.id, "active lookup");

    storage.accounts = R"([{"id":"fixture-online","type":"littleskin","name":"FixtureProfile","uuid":"069a79f4-44e9-4726-a5be-fca90e38aaf5","email":"fixture@example.invalid","server":"https://fixture.invalid/mc","avatarUrl":null,"accessToken":"FIXTURE_ACCESS_SECRET","clientToken":"FIXTURE_CLIENT_SECRET","refreshToken":"FIXTURE_REFRESH_SECRET","expiresAt":1234,"password":"v1:RklYVFVSRS1QQVNTV09SRA=="}])";
    auto list = accounts.list();
    require(list.size() == 1, "fixture account load");
    const auto public_text = public_accounts_json(list);
    const auto public_value = Json::parse(public_text)[0];
    require(public_value.size() == 7, "public keys exact count");
    for (const auto* name : {"id","type","name","uuid","email","server","avatarUrl"}) require(public_value.contains(name), "public camelCase key");
    for (const auto* name : {"accessToken","clientToken","refreshToken","expiresAt","password"}) require(!public_value.contains(name), "public secrets stripped");
    require(public_text.find("FIXTURE_") == std::string::npos, "public token strip");
    auto updated = list[0];
    updated.name = "RenamedFixture";
    updated.email.reset();
    updated.server.reset();
    accounts.upsert_public(updated);
    const auto persisted = Json::parse(storage.accounts)[0];
    require(persisted["accessToken"] == "FIXTURE_ACCESS_SECRET", "public upsert keeps hidden access");
    require(persisted["clientToken"] == "FIXTURE_CLIENT_SECRET", "public upsert keeps client");
    require(persisted["refreshToken"] == "FIXTURE_REFRESH_SECRET", "public upsert keeps refresh");
    require(persisted["password"] == "v1:RklYVFVSRS1QQVNTV09SRA==", "public upsert keeps Vault password");
    require(persisted["email"] == "fixture@example.invalid", "public upsert retains email");
    require(logs.find("FIXTURE_") == std::string::npos, "safe logs strip");
    storage.fail_write = true;
    expect_error(IdentityErrorCode::storage, [&] { accounts.upsert_public(updated); });
    storage.fail_write = false;
    storage.active = "{\"id\":\"fixture-online\"}";
    accounts.remove("fixture-online");
    require(accounts.list().empty() && accounts.active_id().empty(), "remove clears active");
    expect_error(IdentityErrorCode::missing_account, [&] { accounts.set_active("missing-fixture"); });

    for (const auto* damaged : {"", " ", "{not json", "{\"accounts\":[]}", "[false]", "[{\"type\":false}]"}) {
        storage.accounts = damaged;
        require(accounts.list().empty(), "legacy corruption safe empty");
    }
    storage.accounts = "[{\"id\":\"partial\"}]";
    require(accounts.list().size() == 1 && accounts.list()[0].type == AccountType::offline, "legacy defaults");

    detail::AccountState state(storage, vault, {});
    const auto snapshot = state.snapshot("partial");
    auto commit = snapshot.account;
    commit.public_account.name = "commit";
    storage.accounts = "[]";
    expect_error(IdentityErrorCode::stale_commit, [&] { state.commit(commit, snapshot, false); });
    storage.accounts = "[{\"id\":\"partial\",\"name\":\"new-fixture\"}]";
    expect_error(IdentityErrorCode::stale_commit, [&] { state.commit(commit, snapshot, false); });

    Secret secret("FIXTURE_SECRET_STREAM");
    std::ostringstream stream;
    stream << secret;
    require(stream.str() == "<redacted>", "Secret stream redacts");
    secret.clear();
    require(secret.empty(), "Secret clears");
    const auto config = parse_msa_config(R"({"microsoft":{"clientId":"fixture-client","authority":" ","objectId":"NO_PUBLIC_OBJECT","tenantId":"NO_PUBLIC_TENANT"}})");
    require(config && config->client_id == "fixture-client" && config->authority == "consumers", "MSA config whitelist/default");
    require(!parse_msa_config("{}"), "MSA no config");
    expect_error(IdentityErrorCode::invalid_response, [&] { parse_msa_config("{malformed FIXTURE_TOKEN"); });
    std::cout << "accounts: schema, legacy UUID golden, public strip, Vault fixture, safe errors, revision tests passed\n";
}

} // namespace identity_test
