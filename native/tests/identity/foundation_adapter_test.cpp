#include "mock_http.hpp"
#include "offline_uuid_golden.hpp"
#include "../../core/accounts/foundation_adapter.hpp"
#include "../../core/accounts/detail/internal.hpp"
#include "core/config/repository.hpp"
#include "core/config/vault.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace {
using namespace identity_test;
using namespace nickel::accounts;

class FixtureDirectory final {
    std::filesystem::path path_;
public:
    explicit FixtureDirectory(const std::filesystem::path& parent) {
        require(parent.is_absolute(), "fixture parent is absolute");
        auto existing = parent;
        while (!std::filesystem::exists(existing)) existing = existing.parent_path();
        std::error_code error;
        const auto capacity = std::filesystem::space(existing, error);
        require(!error && capacity.available >= 64ULL * 1024 * 1024,
                "owned foundation fixtures require 64 MiB free before any write");
        std::filesystem::create_directories(parent);
        path_ = parent / detail::random_hex(4);
        require(std::filesystem::create_directory(path_), "fixture directory is newly owned");
    }
    ~FixtureDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }
};

void test_real_repository_and_vault(const std::filesystem::path& root) {
    nickel::JsonRepository repository(root);
    nickel::Vault vault(root);
    RepositoryAccountStorage storage(repository);
    FoundationSecretVault secret_vault(vault);
    require(storage.read_accounts() == "[]" && storage.read_active() == "{\"id\":\"\"}", "missing legacy documents use domain defaults");
    auto ciphertext = secret_vault.seal("FIXTURE_PASSWORD");
    require(ciphertext.starts_with("v1:") && ciphertext.find("FIXTURE_PASSWORD") == std::string::npos,
            "real foundation Vault seals legacy v1 AESGCM ciphertext");
    auto plaintext = secret_vault.open(ciphertext);
    require(plaintext == "FIXTURE_PASSWORD", "real foundation Vault opens password");
    detail::erase_string(plaintext);
    detail::StoredAccount stored;
    stored.public_account = {"fixture-online", AccountType::littleskin, "FixturePlayer",
        "069a79f4-44e9-4726-a5be-fca90e38aaf5", "fixture@example.invalid", "https://fixture.invalid/mc", std::nullopt};
    stored.access_token = "FIXTURE_ACCESS";
    stored.client_token = "FIXTURE_CLIENT";
    stored.password = ciphertext;
    storage.write_accounts(detail::stored_accounts_json({stored}));
    AccountService accounts(repository, vault);
    auto public_account = *accounts.find("fixture-online");
    public_account.name = "RenamedFixture";
    accounts.upsert_public(public_account);
    const auto text = storage.read_accounts();
    const auto persisted = Json::parse(text);
    require(persisted.is_array() && persisted[0]["accessToken"] == "FIXTURE_ACCESS" && persisted[0]["password"] == ciphertext,
            "production public update preserves legacy secret fields in direct array");
    require(Json::parse(public_account_json(*accounts.find("fixture-online"))).size() == 7 &&
            public_account_json(*accounts.find("fixture-online")).find("FIXTURE_ACCESS") == std::string::npos,
            "production DTO exposes only seven public fields");
    accounts.set_active("fixture-online");
    const auto before_accounts = storage.read_accounts();
    const auto before_active = storage.read_active();
    expect_error(IdentityErrorCode::invalid_input, [&] {
        storage.transaction([&] {
            storage.write_accounts("[]");
            storage.write_active("{\"id\":\"\"}");
            throw IdentityError(IdentityErrorCode::invalid_input);
        });
    });
    require(storage.read_accounts() == before_accounts && storage.read_active() == before_active,
            "foundation transaction exception publishes neither document");
    require(accounts.remove("fixture-online").empty() && Json::parse(storage.read_active()) == Json{{"id", ""}},
            "production removal clears active id in one foundation transaction");
}

void test_concurrent_services(const std::filesystem::path& root) {
    nickel::JsonRepository first_repository(root), second_repository(root);
    nickel::Vault first_vault(root), second_vault(root);
    AccountService first(first_repository, first_vault), second(second_repository, second_vault);
    auto left = std::async(std::launch::async, [&] {
        for (unsigned i = 0; i < 6; ++i) first.add_offline("LeftFixture", "left-" + std::to_string(i));
    });
    auto right = std::async(std::launch::async, [&] {
        for (unsigned i = 0; i < 6; ++i) second.add_offline("RightFixture", "right-" + std::to_string(i));
    });
    left.get();
    right.get();
    require(first.list().size() == 12 && second.list().size() == 12, "shared owner transaction prevents cross-service lost updates");
    first.set_active("left-0");
    second.remove("left-0");
    require(first.active_id().empty(), "other service active removal is atomic and visible");
}

void test_real_repository_stale_refresh(const std::filesystem::path& root) {
    nickel::JsonRepository repository(root);
    nickel::Vault vault(root);
    RepositoryAccountStorage storage(repository);
    detail::StoredAccount stored;
    stored.public_account = {"fixture-stale", AccountType::littleskin, "First", "069a79f444e94726a5befca90e38aaf5",
                             "fixture@example.invalid", "https://fixture.invalid/mc", std::nullopt};
    stored.access_token = "FIXTURE_YGG_SECRET";
    stored.client_token = "FIXTURE_CLIENT";
    storage.write_accounts(detail::stored_accounts_json({stored}));
    AccountService accounts(repository, vault);
    MockHttp http;
    AuthService auth(accounts, http);
    PausePoint pause;
    add_ygg_refresh(http, "https://fixture.invalid/mc/api/yggdrasil", "069a79f444e94726a5befca90e38aaf5", [&] { pause.pause(); });
    auto refresh = std::async(std::launch::async, [&] {
        try { auth.skin_refresh("fixture-stale"); return false; }
        catch (const IdentityError& error) { return error.code() == IdentityErrorCode::stale_commit; }
    });
    pause.wait();
    nickel::JsonRepository other_repository(root);
    nickel::Vault other_vault(root);
    AccountService other(other_repository, other_vault);
    other.remove("fixture-stale");
    pause.release();
    require(refresh.get() && accounts.list().empty(), "real repository refresh cannot resurrect account removed by another service");
    http.verify();
}

void test_real_repository_document_budget(const std::filesystem::path& root) {
    nickel::JsonRepository repository(root);
    nickel::Vault vault(root);
    RepositoryAccountStorage storage(repository);
    std::vector<detail::StoredAccount> list(72);
    for (std::size_t i = 0; i < list.size(); ++i) {
        list[i].public_account = {"fixture-budget-"+std::to_string(i),AccountType::offline,"FixturePlayer",
                                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",std::nullopt,std::nullopt,std::nullopt};
        list[i].password = "v1:" + std::string(64*1024-3,'p');
    }
    auto text = detail::stored_accounts_json(list);
    require(text.size() > 4*1024*1024 && text.size() < nickel::accounts_max_bytes, "owned actual-repository fixture crosses old reader quota");
    storage.write_accounts(text);
    detail::erase_string(text);
    AccountService accounts(repository,vault);
    auto account = accounts.find("fixture-budget-0");
    require(account.has_value(), "actual Repository 4-8MiB document is visible through production codec");
    account->name = "RenamedFixture";
    accounts.upsert_public(*account);
    text = storage.read_accounts();
    require(text.size() > 4*1024*1024 && accounts.list().size() == list.size() && accounts.find(account->id)->name == "RenamedFixture",
            "actual native rewrite stays readable without silently replacing old large document");
    detail::erase_string(text);
}

void test_active_and_sibling_commit(const std::filesystem::path& root) {
    nickel::JsonRepository repository(root);
    nickel::Vault vault(root);
    AccountService accounts(repository, vault);
    accounts.add_offline("FirstFixture", "fixture-left");
    accounts.add_offline("SecondFixture", "fixture-right");
    accounts.set_active("fixture-left");
    repository.write("settings.json", Json{{"unknown", 17}, {"game", {{"account", "fixture-left"}}}});
    const nickel::Cancellation success;
    accounts.set_active("fixture-right", [&] {
        require(repository.read("active_account.json")["id"] == "fixture-right", "sibling observes staged active selection");
        repository.update("settings.json", Json::object(), [](Json& settings) { settings["game"]["account"] = "fixture-right"; });
    }, success);
    require(accounts.active_id() == "fixture-right" && repository.read("settings.json")["game"]["account"] == "fixture-right" &&
            repository.read("settings.json")["unknown"] == 17, "active and sibling settings publish together without losing unknown fields");
    const auto before_active = repository.read_text("active_account.json");
    const auto before_settings = repository.read_text("settings.json");
    expect_error(IdentityErrorCode::invalid_input, [&] {
        accounts.set_active("fixture-left", [&] {
            repository.write("settings.json", Json{{"bad", true}});
            throw IdentityError(IdentityErrorCode::invalid_input);
        }, success);
    });
    require(repository.read_text("active_account.json") == before_active && repository.read_text("settings.json") == before_settings &&
            accounts.active_id() == "fixture-right", "sibling exception publishes neither active nor settings and changes no cached active");
    nickel::Cancellation before;
    before.cancel();
    bool called{};
    expect_error(IdentityErrorCode::cancelled, [&] { accounts.set_active("fixture-left", [&] { called = true; }, before); });
    require(!called, "pre-cancelled sibling selection does not invoke callback");
    nickel::Cancellation during;
    expect_error(IdentityErrorCode::cancelled, [&] {
        accounts.set_active("fixture-left", [&] {
            repository.write("settings.json", Json{{"cancelled", true}});
            during.cancel();
        }, during);
    });
    require(repository.read_text("active_account.json") == before_active && repository.read_text("settings.json") == before_settings,
            "cancellation inside sibling callback commits neither document");
    expect_error(IdentityErrorCode::missing_account, [&] { accounts.set_active("missing", [&] { called = true; }, success); });
    require(!called, "missing account is rejected before sibling callback");
    PausePoint pause;
    auto selection = std::async(std::launch::async, [&] {
        accounts.set_active("fixture-left", [&] {
            repository.update("settings.json", Json::object(), [](Json& settings) { settings["game"]["account"] = "fixture-left"; });
            pause.pause();
        }, success);
    });
    pause.wait();
    auto background = std::async(std::launch::async, [&] { accounts.add_offline("BackgroundFixture", "fixture-background"); });
    const auto waiting = background.wait_for(std::chrono::milliseconds(30));
    pause.release();
    selection.get();
    background.get();
    require(waiting == std::future_status::timeout && accounts.find("fixture-background").has_value(),
            "background account commit waits for account-mutex then repository without ABBA");
}
void test_foundation_cancellation_mapping(const std::filesystem::path& root) {
    nickel::JsonRepository repository(root);
    RepositoryAccountStorage storage(repository);
    storage.write_accounts("[]");
    const auto before = storage.read_accounts();
    for (const auto code : {"Cancelled", "cancelled"}) {
        expect_error(IdentityErrorCode::cancelled, [&] {
            storage.transaction([&] {
                storage.write_accounts("[{\"fixture\":true}]");
                throw nickel::ErrorException({code, "Controlled cancellation", Json::object()});
            });
        });
        require(storage.read_accounts() == before, "both canonical cancellation spellings preserve the repository transaction");
    }
    nickel::Cancellation cancellation;
    cancellation.cancel();
    expect_error(IdentityErrorCode::cancelled, [&] { storage.transaction([] {}, &cancellation); });
    require(storage.read_accounts() == before, "direct canonical pre-cancellation remains identity cancellation");
}


void test_offline_uuid_repository(const std::filesystem::path& root) {
    nickel::JsonRepository repository(root);
    nickel::Vault vault(root);
    RepositoryAccountStorage storage(repository);
    FoundationSecretVault secret_vault(vault);
    const auto fixture_token = secret_vault.seal("G03_FIXTURE_MSA_TOKEN");
    struct SavedIdentity {
        const char* id;
        const char* name;
        const char* uuid;
    };
    const std::array<SavedIdentity, 4> saved{{
        {"g03-old-fnv", "Notch", "f14dc048-28d2-3bc4-803c-36d82b6053b9"},
        {"g03-saved-md5", "Notch", "b50ad385829d3141a2167e7d7539ba7f"},
        {"g03-saved-spelling", "Notch", " \tF14DC04828D23BC4803C36D82B6053B9 \n"},
        {"g03-chinese", "\xe7\x8e\xa9\xe5\xae\xb6", "e9325e85-3f71-3886-9df7-82da62c7f606"},
    }};
    Json legacy = Json::array();
    for (const auto& row : saved)
        legacy.push_back(Json{{"id", row.id}, {"type", "offline"}, {"name", row.name}, {"uuid", row.uuid}});
    legacy.push_back(Json{{"id", "g03-msa-valid"}, {"type", "microsoft"}, {"name", "FixtureMSA"},
        {"uuid", "069A79F4-44E9-4726-A5BE-FCA90E38AAF5"}, {"accessToken", fixture_token},
        {"expiresAt", 1700010000}});
    storage.write_accounts(legacy.dump(2) + "\n");
    storage.write_active("{\"id\":\"g03-old-fnv\"}\n");
    AccountService accounts(repository, vault);
    MockHttp http;
    FakeClock clock;
    AuthService auth(accounts, http, clock.options());
    // Reads, launch preparation and rejected changes must not rewrite either legacy document.
    const auto protect_documents = [&](auto operation) {
        const auto account_bytes = storage.read_accounts();
        const auto active_bytes = storage.read_active();
        const auto account_time = std::filesystem::last_write_time(root / "accounts.json");
        const auto active_time = std::filesystem::last_write_time(root / "active_account.json");
        operation();
        require(storage.read_accounts() == account_bytes && storage.read_active() == active_bytes &&
                std::filesystem::last_write_time(root / "accounts.json") == account_time &&
                std::filesystem::last_write_time(root / "active_account.json") == active_time,
                "G03 read/launch/rejection preserves exact Repository bytes and last-write times");
    };
    protect_documents([&] {
        require(accounts.list().size() == saved.size() + 1, "G03 parses the real legacy direct-array document");
        for (const auto& row : saved) {
            const auto loaded = accounts.find(row.id);
            require(loaded && loaded->id == row.id && loaded->uuid == row.uuid, "G03 parse preserves saved UUID spelling and id");
        }
        const auto msa = auth.prepare_launch("g03-msa-valid");
        require(msa.type == AccountType::microsoft && msa.user_type == "msa" &&
                msa.uuid == "069a79f444e94726a5befca90e38aaf5" && msa.access_token.view() == "G03_FIXTURE_MSA_TOKEN",
                "strict UUID validation preserves real-Vault Microsoft launch type and token");
    });
    for (const auto& row : saved) {
        auto incoming = *accounts.find(row.id);
        incoming.name = std::string(row.name) + "-renamed";
        incoming.uuid.clear();
        const auto updated = accounts.upsert_public(incoming);
        require(updated.id == row.id && updated.uuid == row.uuid, "ordinary save cannot regenerate saved FNV or MD5 UUID");
        incoming.uuid = normalize_uuid(row.uuid);
        for (char& c : incoming.uuid) if (c >= 'a' && c <= 'f') c = static_cast<char>(c - ('a' - 'A'));
        require(accounts.upsert_public(incoming).uuid == row.uuid, "equivalent explicit UUID retains original stored bytes");
        require(accounts.add_offline("ChangedFixtureName", row.id).uuid == row.uuid,
                "add_offline with an existing id observes saved UUID before generating a default");
        nickel::JsonRepository refreshed_repository(root);
        nickel::Vault refreshed_vault(root);
        AccountService refreshed(refreshed_repository, refreshed_vault);
        AuthService refreshed_auth(refreshed, http, clock.options());
        protect_documents([&] {
            const auto loaded = refreshed.find(row.id);
            require(loaded && loaded->uuid == row.uuid && loaded->id == row.id, "new Repository/service refresh retains durable identity");
            const auto launch = refreshed_auth.prepare_launch(row.id);
            require(launch.account_id == row.id && launch.uuid == normalize_uuid(row.uuid) && launch.type == AccountType::offline &&
                    launch.access_token.view() == "0", "Repository save-refresh-launch retains player identity and offline token");
            auto replacement = *loaded;
            replacement.uuid = "47cc85be-501b-307c-b33d-c56ddffd8ea0";
            expect_error(IdentityErrorCode::invalid_input, [&] { refreshed.upsert_public(replacement); });
            replacement.uuid = std::string(4, '-') + std::string(32, 'a');
            expect_error(IdentityErrorCode::invalid_input, [&] { refreshed.upsert_public(replacement); });
        });
    }
    Json missing = Json::parse(storage.read_accounts());
    for (const auto* id : {"g03-missing", "g03-whitespace", "g03-absent"}) {
        Json record{{"id", id}, {"type", "offline"}, {"name", " \tNotch\r\n"}};
        if (std::string_view(id) != "g03-absent") record["uuid"] = std::string_view(id) == "g03-missing" ? "" : " \t\r\n";
        missing.push_back(std::move(record));
    }
    storage.write_accounts(missing.dump(2) + "\n");
    for (const auto* id : {"g03-missing", "g03-whitespace", "g03-absent"}) {
        protect_documents([&] {
            const auto launch = auth.prepare_launch(id);
            require(launch.account_id == id && launch.uuid == "f14dc04828d23bc4803c36d82b6053b9",
                    "missing UUID launch uses the single old Rust helper after explicit name trimming");
        });
        const auto updated = accounts.upsert_public(*accounts.find(id));
        require(updated.id == id && updated.uuid == "f14dc048-28d2-3bc4-803c-36d82b6053b9",
                "ordinary save of missing UUID uses old Rust default, not MD5");
        nickel::JsonRepository refreshed_repository(root);
        nickel::Vault refreshed_vault(root);
        AccountService refreshed(refreshed_repository, refreshed_vault);
        AuthService refreshed_auth(refreshed, http, clock.options());
        protect_documents([&] {
            require(refreshed.find(id)->uuid == updated.uuid &&
                    refreshed_auth.prepare_launch(id).uuid == "f14dc04828d23bc4803c36d82b6053b9",
                    "generated UUID survives real Repository reopen and launch");
        });
    }
    const auto created = accounts.add_offline(" \tNotch\r\n", "g03-new");
    require(created.id == "g03-new" && created.name == "Notch" && created.uuid == "f14dc048-28d2-3bc4-803c-36d82b6053b9",
            "new account creation uses trimmed-name legacy default");
    protect_documents([&] {
        require(auth.prepare_launch(created.id).uuid == "f14dc04828d23bc4803c36d82b6053b9", "new account reaches launch with same UUID");
        expect_error(IdentityErrorCode::invalid_input, [&] { accounts.add_offline(" \t\n", "g03-blank"); });
        expect_error(IdentityErrorCode::invalid_input, [&] { accounts.add_offline(std::string(257, 'a'), "g03-long"); });
        PublicAccount invalid;
        invalid.id = "g03-explicit-invalid";
        invalid.name = "FixtureInvalid";
        invalid.uuid = "broken-uuid";
        expect_error(IdentityErrorCode::invalid_input, [&] { accounts.upsert_public(invalid); });
    });
    const std::array<std::string, 5> malformed{{
        "broken-uuid", std::string(31, 'a'), std::string(33, 'a'), std::string(32, 'g'), std::string(4, '-') + std::string(32, 'a'),
    }};
    for (const auto type : {AccountType::offline, AccountType::microsoft, AccountType::littleskin, AccountType::yggdrasil}) {
        for (const auto& uuid : malformed) {
            Json record{{"id", "g03-invalid-stored"}, {"type", account_type_name(type)}, {"name", "FixtureInvalid"}, {"uuid", uuid},
                {"accessToken", fixture_token}, {"expiresAt", 0}, {"server", "https://fixture.invalid/mc"}};
            storage.write_accounts(Json::array({record}).dump(2) + "\n");
            protect_documents([&] {
                require(accounts.list().size() == 1 && accounts.find("g03-invalid-stored")->uuid == uuid,
                        "malformed nonempty UUID remains readable without automatic repair");
                const auto stored = *accounts.find("g03-invalid-stored");
                expect_error(IdentityErrorCode::invalid_profile, [&] { accounts.upsert_public(stored); });
                auto repair = stored;
                repair.uuid.clear();
                expect_error(IdentityErrorCode::invalid_profile, [&] { accounts.upsert_public(repair); });
                repair.uuid = "069a79f444e94726a5befca90e38aaf5";
                expect_error(IdentityErrorCode::invalid_profile, [&] { accounts.upsert_public(repair); });
                if (type == AccountType::offline)
                    expect_error(IdentityErrorCode::invalid_profile, [&] { accounts.add_offline("RepairFixture", stored.id); });
                expect_error(IdentityErrorCode::invalid_profile, [&] {
                    auth.prepare_launch(stored.id, MsaConfig{"fixture-client", "consumers", "nickel://auth"});
                });
            });
        }
    }
    protect_documents([&] {
        nickel::Cancellation cancelled;
        cancelled.cancel();
        expect_error(IdentityErrorCode::cancelled, [&] { auth.prepare_launch("g03-invalid-stored", cancelled); });
    });
    require(http.calls.load() == 0, "G03 offline and invalid-profile preparation performs no HTTP requests");
    http.verify();
    std::cout << "G03 offline UUID: real Repository/Vault parse-save-refresh-launch, preserved FNV/MD5/id, missing default and no-write rejection passed\n";
}


void test_real_repository_deep_journal(const std::filesystem::path& base) {
    constexpr std::size_t legacy_win32_path_chars = 260;
    const std::string token(32, '0');
    const std::string journal_name = ".nickel-transaction-" + token + ".json";
    const std::string temporary_name = "." + journal_name + ".tmp-" + token;
    const std::string prefix = "original-deep-journal-";
    const auto root_chars = legacy_win32_path_chars - journal_name.size() - 2;
    require(base.native().size() + 1 + prefix.size() <= root_chars,
            "absolute owned fixture parent leaves room for the deep-journal boundary");
    const auto component_chars = root_chars - base.native().size() - 1;
    const auto root = base / (prefix + std::string(component_chars - prefix.size(), 'd'));
    require(root.native().size() == root_chars &&
            (root / "accounts.json").native().size() < legacy_win32_path_chars &&
            (root / journal_name).native().size() < legacy_win32_path_chars &&
            (root / temporary_name).native().size() > legacy_win32_path_chars,
            "deep fixture keeps normal paths below 260 while the owner journal temporary crosses it");
    require(std::filesystem::create_directory(root), "deep fixture directory is newly owned");
    std::cout << "foundation deep-journal fixture: root=" << root.native().size()
              << " journal=" << (root / journal_name).native().size()
              << " owner-temporary=" << (root / temporary_name).native().size() << " native characters\n";
    // Reuse the original real-Vault/public-update/active-removal flow without a shorter-path fallback.
    test_real_repository_and_vault(root);
}


} // namespace

int main(int argc, char** argv) {
    const char* stage = "fixture-directory";
    try {
        const std::string_view mode = argc == 3 ? argv[2] : "";
        require(argc == 2 || (argc == 3 && (mode == "--deep-journal" || mode == "--offline-uuid-only")),
                "owned fixture parent and optional --deep-journal or --offline-uuid-only supplied");
        const std::filesystem::path parent(argv[1]);
        FixtureDirectory fixture(parent);
        if (mode == "--deep-journal") {
            stage = "deep-journal";
            test_real_repository_deep_journal(fixture.path());
            std::cout << "foundation deep-journal: actual Repository/Vault public update and atomic active removal passed\n";
            return 0;
        }
        if (mode == "--offline-uuid-only") {
            stage = "offline-uuid-repository";
            test_offline_uuid_repository(fixture.path());
            return 0;
        }
        const auto first = fixture.path() / "v";
        const auto second = fixture.path() / "c";
        const auto third = fixture.path() / "s";
        const auto fourth = fixture.path() / "q";
        const auto fifth = fixture.path() / "u";
        std::filesystem::create_directory(first);
        std::filesystem::create_directory(second);
        std::filesystem::create_directory(third);
        std::filesystem::create_directory(fourth);
        std::filesystem::create_directory(fifth);
        stage = "real-repository-and-vault";
        test_real_repository_and_vault(first);
        stage = "concurrent-services";
        test_concurrent_services(second);
        stage = "stale-refresh";
        test_real_repository_stale_refresh(third);
        stage = "document-budget";
        test_real_repository_document_budget(fourth);
        stage = "active-and-sibling-settings";
        test_active_and_sibling_commit(first);
        stage = "canonical-cancellation-mapping";
        test_foundation_cancellation_mapping(third);
        stage = "offline-uuid-repository";
        test_offline_uuid_repository(fifth);
        std::cout << "foundation adapter: actual JsonRepository/Vault v1, direct legacy schema, atomic active removal, concurrent services, stale refresh, cancellation, G03 UUID and 4-8MiB document rewrite passed\n";
        return 0;
    } catch (const nickel::ErrorException& error) {
        std::cerr << "foundation adapter failure at " << stage << ": " << error.error().code << " " << error.what()
                  << " controlled-fixture details=" << error.error().details.dump() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "foundation adapter failure at " << stage << ": " << error.what() << '\n';
        return 1;
    }
}
