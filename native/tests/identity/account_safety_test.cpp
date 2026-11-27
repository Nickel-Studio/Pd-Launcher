#include "fixtures.hpp"
#include "../../core/accounts/detail/internal.hpp"

namespace identity_test {
namespace {
void test_document_bounds() {
    using namespace nickel::accounts;
    std::vector<detail::StoredAccount> list(80);
    for (std::size_t i = 0; i < list.size(); ++i) {
        list[i].public_account = {"fixture-large-"+std::to_string(i),AccountType::offline,"FixturePlayer",
                                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",std::nullopt,std::nullopt,std::nullopt};
        list[i].password = "v1:" + std::string(64*1024-3,'p');
    }
    const auto text = detail::stored_accounts_json(list);
    require(text.size() > 4*1024*1024 && text.size() < detail::account_document_max_bytes && detail::parse_accounts(text).size() == list.size(),
            "stored document between old 4MiB and owner 8MiB quota remains readable");
    list.resize(136,list.front());
    expect_error(IdentityErrorCode::quota,[&] { detail::stored_accounts_json(list); });
    list.clear();
    list.resize(detail::account_document_max_entries);
    for (std::size_t i = 0; i < list.size(); ++i) {
        list[i].public_account.id = "fixture-count-"+std::to_string(i);
        list[i].public_account.name = "FixturePlayer";
    }
    MemoryStorage storage;
    FixtureVault vault;
    storage.accounts = detail::stored_accounts_json(list);
    const auto before = storage.accounts;
    AccountService accounts(storage,vault);
    expect_error(IdentityErrorCode::quota,[&] { accounts.add_offline("FixtureExtra","fixture-count-extra"); });
    require(storage.accounts == before && storage.writes == 0 && accounts.list().size() == detail::account_document_max_entries,
            "4097th account rejects before storage and preserves readable existing array");
}
void test_revision_metadata_reclamation() {
    using namespace nickel::accounts;
    MemoryStorage storage;
    FixtureVault vault;
    detail::AccountState state(storage,vault,{});
    for (unsigned i = 0; i < 256; ++i) {
        std::lock_guard lock(state.mutex);
        storage.accounts = nickel::Json::array({{{"id","fixture-churn-"+std::to_string(i)},{"name","FixturePlayer"}}}).dump();
        state.load_locked();
        require(state.revisions.size() == 1 && state.fingerprints.size() == 1, "only present account metadata retained");
        storage.accounts = "[]";
        state.load_locked();
        require(state.revisions.empty() && state.fingerprints.empty(), "removal leaves no revision/fingerprint tombstone");
    }
    storage.accounts = R"([{"id":"fixture-reused","name":"FixturePlayer"}])";
    const auto original = storage.accounts;
    const auto snapshot = state.snapshot("fixture-reused");
    {
        std::lock_guard lock(state.mutex);
        storage.accounts = "[]";
        state.load_locked();
        storage.accounts = original;
        state.load_locked();
    }
    expect_error(IdentityErrorCode::stale_commit,[&] { state.commit(snapshot.account,snapshot,false); });
    require(state.revisions.size() == 1 && state.revisions.at("fixture-reused") > snapshot.revision,
            "monotonic revision rejects identical removed/recreated account after metadata reclamation");
}
} // namespace

void run_account_safety_tests() {
    using namespace nickel::accounts;
    MemoryStorage storage;
    FixtureVault vault;
    detail::StoredAccount stored;
    stored.public_account = {"fixture-online", AccountType::littleskin, "FixturePlayer",
        "069a79f4-44e9-4726-a5be-fca90e38aaf5", "fixture@example.invalid", "https://fixture.invalid/mc", std::nullopt};
    stored.access_token = "FIXTURE_ACCESS";
    stored.client_token = "FIXTURE_CLIENT";
    stored.password = vault.seal("FIXTURE_PASSWORD");
    storage.accounts = detail::stored_accounts_json({stored});
    AccountService accounts(storage, vault);
    auto public_account = *accounts.find("fixture-online");
    const auto before = storage.accounts;
    public_account.server = "https://evil.invalid/credential-replay";
    expect_error(IdentityErrorCode::invalid_server, [&] { accounts.upsert_public(public_account); });
    require(storage.accounts == before && vault.opens == 0, "public server edit cannot retarget hidden credentials");
    public_account = stored.public_account;
    public_account.uuid = "11111111111111111111111111111111";
    expect_error(IdentityErrorCode::invalid_input, [&] { accounts.upsert_public(public_account); });
    public_account = stored.public_account;
    public_account.email = "other@example.invalid";
    expect_error(IdentityErrorCode::invalid_input, [&] { accounts.upsert_public(public_account); });
    public_account = stored.public_account;
    public_account.name = "RenamedFixture";
    public_account.server = "https://fixture.invalid/mc/api/yggdrasil/";
    public_account.uuid = "069A79F444E94726A5BEFCA90E38AAF5";
    accounts.upsert_public(public_account);
    require(accounts.find("fixture-online")->name == "RenamedFixture", "public rename and equivalent canonical server/UUID remain compatible");
    const auto persisted = nickel::Json::parse(storage.accounts)[0];
    require(persisted["accessToken"] == "FIXTURE_ACCESS" && persisted["password"] == stored.password,
            "safe public update keeps hidden credentials");
    public_account.id = "manufactured-online";
    expect_error(IdentityErrorCode::missing_credentials, [&] { accounts.upsert_public(public_account); });
    test_document_bounds();
    test_revision_metadata_reclamation();
    std::cout << "account safety: public credential integrity, symmetric 8MiB/4096-entry storage quotas, revision reclamation and recreated-account staleness passed\n";
}
} // namespace identity_test
