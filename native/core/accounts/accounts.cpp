#include "accounts.hpp"
#include "auth.hpp"
#include "detail/internal.hpp"
#include "detail/protocol.hpp"

#include <algorithm>
#include <iomanip>
#include <streambuf>
#include <limits>
#include <set>
#include <utility>

namespace nickel::accounts {
namespace {
std::string_view error_message(IdentityErrorCode code) noexcept {
    switch (code) {
    case IdentityErrorCode::invalid_input: return "Invalid identity input";
    case IdentityErrorCode::invalid_server: return "Invalid authentication server; sign in with the correct server";
    case IdentityErrorCode::invalid_response: return "Invalid identity service response";
    case IdentityErrorCode::network: return "Identity service request failed";
    case IdentityErrorCode::storage: return "Account repository operation failed";
    case IdentityErrorCode::vault: return "Local credential vault operation failed";
    case IdentityErrorCode::missing_account: return "Account does not exist";
    case IdentityErrorCode::missing_credentials: return "Account needs sign-in credentials";
    case IdentityErrorCode::unauthorized: return "Sign-in credentials are invalid or expired";
    case IdentityErrorCode::declined: return "Sign-in was declined";
    case IdentityErrorCode::expired: return "Sign-in session has expired";
    case IdentityErrorCode::cancelled: return "Identity operation was cancelled";
    case IdentityErrorCode::busy: return "Identity operation is already running";
    case IdentityErrorCode::stale_commit: return "Account changed while authentication was running";
    case IdentityErrorCode::no_entitlement: return "Account does not own Minecraft Java Edition";
    case IdentityErrorCode::no_profile: return "Account has no Minecraft profile";
    case IdentityErrorCode::invalid_profile: return "Selected profile is not available";
    case IdentityErrorCode::xbox_account_missing: return "Create an Xbox account before signing in";
    case IdentityErrorCode::xbox_child_account: return "A family organizer must authorize this child account";
    case IdentityErrorCode::xbox_region: return "Xbox Live is not supported in this region";
    case IdentityErrorCode::xbox_banned: return "Xbox account is banned";
    case IdentityErrorCode::xbox_verification: return "Complete account verification at Xbox before signing in";
    case IdentityErrorCode::quota: return "Identity resource limit reached";
    case IdentityErrorCode::unsupported_image: return "Unsupported or corrupt image";
    case IdentityErrorCode::image_too_large: return "Image exceeds its memory budget";
    case IdentityErrorCode::untrusted_download: return "Texture download origin is not trusted";
    }
    return "Identity operation failed";
}

void validate_public(const PublicAccount& account) {
    const auto valid = [](const std::string& value, std::size_t maximum) {
        return value.size() <= maximum && detail::valid_utf8(value);
    };
    if (!valid(account.id, 256) || !valid(account.name, 256) || !valid(account.uuid, 64) ||
        (account.email && !valid(*account.email, 512)) ||
        (account.server && !valid(*account.server, 4096)) ||
        (account.avatar_url && !valid(*account.avatar_url, 4096))) {
        throw IdentityError(IdentityErrorCode::invalid_input);
    }
}

std::optional<std::string> optional_field(const detail::Json& value, std::string_view name,
                                        std::size_t limit = 16384) {
    const auto it = value.find(std::string(name));
    if (it == value.end() || it->is_null()) return std::nullopt;
    return detail::string_field(value, name, false, limit);
}

std::string read_active_id(AccountStorage& storage) {
    std::string text;
    try { text = storage.read_active(); } catch (...) { throw IdentityError(IdentityErrorCode::storage); }
    detail::EraseString erase{text};
    if (detail::trim(text).empty()) return {};
    try {
        const auto parsed = detail::parse_json(text, 4096, 4);
        return detail::string_field(parsed, "id", false, 256);
    } catch (const IdentityError& error) {
        if (error.code() != IdentityErrorCode::invalid_response) throw;
        return {};
    }
}
}

IdentityError::IdentityError(IdentityErrorCode code, int http_status)
    : nickel::ErrorException(nickel::Error{std::string(identity_error_code_name(code)), std::string(error_message(code)),
          http_status ? nickel::Json{{"httpStatus", http_status}} : nickel::Json::object()}),
      code_(code), http_status_(http_status) {}
IdentityErrorCode IdentityError::code() const noexcept { return code_; }
int IdentityError::http_status() const noexcept { return http_status_; }

std::string_view account_type_name(AccountType type) noexcept {
    switch (type) {
    case AccountType::offline: return "offline";
    case AccountType::microsoft: return "microsoft";
    case AccountType::littleskin: return "littleskin";
    case AccountType::yggdrasil: return "yggdrasil";
    }
    return "offline";
}

AccountType parse_account_type(std::string_view value) {
    if (value.empty() || value == "offline") return AccountType::offline;
    if (value == "microsoft") return AccountType::microsoft;
    if (value == "littleskin") return AccountType::littleskin;
    if (value == "yggdrasil") return AccountType::yggdrasil;
    throw IdentityError(IdentityErrorCode::invalid_input);
}

std::string normalize_uuid(std::string_view raw) {
    auto result = detail::ascii_lower(detail::trim(raw));
    result.erase(std::remove(result.begin(), result.end(), '-'), result.end());
    return result;
}

std::string dash_uuid(std::string_view raw) {
    const auto compact = normalize_uuid(raw);
    if (compact.size() != 32 || !std::all_of(compact.begin(), compact.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) return detail::trim(raw);
    return compact.substr(0, 8) + "-" + compact.substr(8, 4) + "-" + compact.substr(12, 4) + "-" +
           compact.substr(16, 4) + "-" + compact.substr(20, 12);
}

std::string offline_uuid(std::string_view utf8_name) {
    if (!detail::valid_utf8(utf8_name) || utf8_name.size() > 256)
        throw IdentityError(IdentityErrorCode::invalid_input);
    // Preserve the vendored Rust launcher identity, not Java's MD5 name UUID.
    std::uint64_t a = 0xcbf29ce484222325ULL;
    std::uint64_t b = 0x14650fb0739d0383ULL;
    for (const char character : utf8_name) {
        a ^= static_cast<unsigned char>(character);
        a *= 0x100000001b3ULL;
        b ^= a;
        b *= 0x517cc1b727220a95ULL;
    }
    const auto append_hex = [](std::string& output, std::uint64_t value, unsigned digits) {
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned i = digits; i != 0; --i)
            output.push_back(hex[(value >> ((i - 1) * 4)) & 15]);
    };
    std::string result;
    result.reserve(36);
    append_hex(result, b >> 32, 8);
    result.push_back('-');
    append_hex(result, (b >> 16) & 0xffff, 4);
    result += "-3";
    append_hex(result, b & 0x0fff, 3);
    result.push_back('-');
    append_hex(result, ((a >> 48) & 0x3fff) | 0x8000, 4);
    result.push_back('-');
    append_hex(result, a & 0x0000ffffffffffffULL, 12);
    return result;
}

std::string validate_server(const std::optional<std::string>& server, bool stored) {
    const auto raw = server ? detail::trim(*server) : std::string{};
    if (raw.empty()) {
        if (stored && server) throw IdentityError(IdentityErrorCode::invalid_server);
        return "https://littleskin.cn";
    }
    const auto lower = detail::ascii_lower(raw);
    if ((lower.starts_with("http:") || lower.starts_with("https:")) && raw.find("://") == std::string::npos)
        throw IdentityError(IdentityErrorCode::invalid_server);
    auto base = raw.find("://") == std::string::npos ? "https://" + raw : raw;
    while (base.ends_with('/')) base.pop_back();
    auto parsed = detail::parse_url(base);
    base = parsed.str();
    // Legacy storage strips only the long Blessing Skin suffix; /yggdrasil is a custom API root.
    constexpr std::string_view suffix = "/api/yggdrasil";
    if (parsed.path.ends_with(suffix)) {
        base.resize(base.size() - suffix.size());
        while (base.ends_with('/')) base.pop_back();
    }
    return base;
}

std::string yggdrasil_base(std::string_view server) {
    const auto parsed = detail::parse_url(server);
    auto base = parsed.str();
    while (base.ends_with('/')) base.pop_back();
    if (parsed.path.ends_with("/api/yggdrasil") || parsed.path.ends_with("/yggdrasil")) return base;
    return detail::join_url(base, "/api/yggdrasil");
}

std::string site_base(std::string_view server) {
    const auto parsed = detail::parse_url(server);
    auto base = parsed.str();
    while (base.ends_with('/')) base.pop_back();
    for (const auto suffix : {std::string_view("/api/yggdrasil"), std::string_view("/yggdrasil")}) {
        if (parsed.path.ends_with(suffix)) { base.resize(base.size() - suffix.size()); break; }
    }
    while (base.ends_with('/')) base.pop_back();
    return base;
}

std::string public_account_json(const PublicAccount& account) {
    validate_public(account);
    return detail::account_json(account).dump();
}

std::string public_accounts_json(const std::vector<PublicAccount>& accounts) {
    detail::Json output = detail::Json::array();
    for (const auto& account : accounts) { validate_public(account); output.push_back(detail::account_json(account)); }
    return output.dump();
}

Secret::Secret(std::string_view value) : bytes_(value.begin(), value.end()) {}
Secret::~Secret() { clear(); }
Secret::Secret(Secret&& other) noexcept : bytes_(std::move(other.bytes_)) {}
Secret& Secret::operator=(Secret&& other) noexcept {
    if (this != &other) { clear(); bytes_ = std::move(other.bytes_); }
    return *this;
}
std::string_view Secret::view() const noexcept { return bytes_.empty() ? std::string_view{} : std::string_view(bytes_.data(), bytes_.size()); }
bool Secret::empty() const noexcept { return bytes_.empty(); }
void Secret::clear() noexcept {
    if (!bytes_.empty()) detail::erase_bytes(bytes_.data(), bytes_.size());
    bytes_.clear();
}
std::ostream& operator<<(std::ostream& out, const Secret&) { return out << "<redacted>"; }

std::optional<MsaConfig> parse_msa_config(std::string_view text) {
    if (detail::trim(text).empty()) return std::nullopt;
    const auto parsed = detail::parse_json(text, 64 * 1024, 16);
    const auto found = parsed.is_object() ? parsed.find("microsoft") : parsed.end();
    if (found == parsed.end() || !found->is_object()) return std::nullopt;
    MsaConfig config;
    config.client_id = detail::trim(detail::string_field(*found, "clientId", false, 256));
    if (config.client_id.empty()) return std::nullopt;
    auto authority = detail::trim(detail::string_field(*found, "authority", false, 256));
    auto redirect = detail::trim(detail::string_field(*found, "redirectUri", false, 2048));
    if (!authority.empty()) config.authority = std::move(authority);
    if (!redirect.empty()) config.redirect_uri = std::move(redirect);
    return config;
}

AccountService::AccountService(AccountStorage& storage, SecretVault& vault, IdentityLog log)
    : impl_(std::make_unique<Impl>(storage, vault, std::move(log))) {}
AccountService::~AccountService() = default;

std::vector<PublicAccount> AccountService::list() const {
    std::lock_guard lock(impl_->mutex);
    const auto stored = impl_->load_locked();
    std::vector<PublicAccount> result;
    result.reserve(stored.size());
    for (const auto& account : stored) result.push_back(account.public_account);
    return result;
}

std::optional<PublicAccount> AccountService::find(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto stored = impl_->load_locked();
    for (const auto& account : stored) if (account.public_account.id == id) return account.public_account;
    return std::nullopt;
}

PublicAccount AccountService::upsert_public(PublicAccount incoming) {
    validate_public(incoming);
    const auto supplied_uuid = detail::trim(incoming.uuid);
    if (detail::trim(incoming.id).empty()) incoming.id = "acc_" + detail::random_hex();
    if (detail::trim(incoming.name).empty()) throw IdentityError(IdentityErrorCode::invalid_input);
    if (incoming.server && incoming.type == AccountType::littleskin)
        incoming.server = validate_server(incoming.server);
    else if (incoming.server && incoming.type == AccountType::yggdrasil)
        incoming.server = detail::parse_url(*incoming.server).str();
    std::lock_guard lock(impl_->mutex);
    impl_->update_locked([&](auto& list) {
        const auto existing = std::find_if(list.begin(), list.end(), [&](const auto& entry) { return entry.public_account.id == incoming.id; });
        if (existing != list.end()) {
            const auto stored_uuid = detail::trim(existing->public_account.uuid);
            if (!stored_uuid.empty() && !detail::valid_profile_id(stored_uuid))
                throw IdentityError(IdentityErrorCode::invalid_profile);
        }
        if (!supplied_uuid.empty() && !detail::valid_profile_id(supplied_uuid))
            throw IdentityError(IdentityErrorCode::invalid_input);
        detail::StoredAccount updated;
        if (existing != list.end()) {
            const auto& current = existing->public_account;
            if (current.type != incoming.type) throw IdentityError(IdentityErrorCode::invalid_input);
            if (!incoming.email) incoming.email = current.email;
            if (!incoming.server) incoming.server = current.server;
            if (!incoming.avatar_url) incoming.avatar_url = current.avatar_url;
            if (!detail::trim(current.uuid).empty()) {
                if (!supplied_uuid.empty() && normalize_uuid(incoming.uuid) != normalize_uuid(current.uuid))
                    throw IdentityError(IdentityErrorCode::invalid_input);
                // Ordinary edits cannot migrate a player's UUID, including already saved MD5 values.
                incoming.uuid = current.uuid;
            } else if (supplied_uuid.empty()) {
                incoming.uuid = current.uuid;
            }
            if (incoming.type != AccountType::offline) {
                if (normalize_uuid(incoming.uuid) != normalize_uuid(current.uuid) || incoming.email != current.email)
                    throw IdentityError(IdentityErrorCode::invalid_input);
                const auto canonical_server = [&](const std::optional<std::string>& server) {
                    if (incoming.type == AccountType::littleskin) return validate_server(server, true);
                    if (incoming.type == AccountType::yggdrasil)
                        return server ? detail::parse_url(*server).str() : std::string{};
                    return server.value_or("");
                };
                // A public rename/avatar edit cannot retarget hidden credentials.
                if (canonical_server(incoming.server) != canonical_server(current.server))
                    throw IdentityError(IdentityErrorCode::invalid_server);
            }
            updated = *existing;
        } else if (incoming.type != AccountType::offline) {
            throw IdentityError(IdentityErrorCode::missing_credentials);
        }
        if (detail::trim(incoming.uuid).empty()) {
            if (incoming.type != AccountType::offline) throw IdentityError(IdentityErrorCode::invalid_profile);
            incoming.uuid = offline_uuid(detail::trim(incoming.name));
        }
        updated.public_account = incoming;
        if (existing == list.end()) list.push_back(std::move(updated)); else *existing = std::move(updated);
    });
    return incoming;
}

PublicAccount AccountService::add_offline(std::string_view name, std::optional<std::string> id) {
    PublicAccount account;
    account.name = detail::trim(name);
    if (id) account.id = std::move(*id);
    // Resolve a missing UUID inside the transaction that observes an existing account.
    return upsert_public(std::move(account));
}

std::vector<PublicAccount> AccountService::remove(std::string_view id) {
    if (id.empty()) throw IdentityError(IdentityErrorCode::invalid_input);
    std::lock_guard lock(impl_->mutex);
    const auto list = impl_->update_locked([&](auto& accounts) {
        accounts.erase(std::remove_if(accounts.begin(), accounts.end(), [&](const auto& account) { return account.public_account.id == id; }), accounts.end());
        if (read_active_id(impl_->storage) == id) impl_->storage.write_active("{\"id\":\"\"}");
    });
    std::vector<PublicAccount> result;
    for (const auto& account : list) result.push_back(account.public_account);
    return result;
}

std::string AccountService::active_id() const {
    std::lock_guard lock(impl_->mutex);
    return read_active_id(impl_->storage);
}

std::optional<PublicAccount> AccountService::active() const {
    std::lock_guard lock(impl_->mutex);
    const auto id = read_active_id(impl_->storage);
    for (const auto& account : impl_->load_locked()) if (account.public_account.id == id) return account.public_account;
    return std::nullopt;
}

void AccountService::set_active(std::string_view id) {
    const nickel::Cancellation cancellation;
    set_active(id, {}, cancellation);
}

void AccountService::set_active(std::string_view id,
                                const std::function<void()>& sibling_commit_callback,
                                const nickel::Cancellation& cancellation) {
    if (id.size() > 256 || !detail::valid_utf8(id)) throw IdentityError(IdentityErrorCode::invalid_input);
    detail::check_stop(cancellation.stop_token());
    std::lock_guard lock(impl_->mutex);
    try {
        impl_->storage.transaction([&] {
            detail::check_stop(cancellation.stop_token());
            const auto list = impl_->load_locked();
            if (!id.empty() && std::none_of(list.begin(), list.end(), [&](const auto& account) { return account.public_account.id == id; }))
                throw IdentityError(IdentityErrorCode::missing_account);
            impl_->storage.write_active(detail::Json{{"id", id}}.dump());
            if (sibling_commit_callback) sibling_commit_callback();
            detail::check_stop(cancellation.stop_token());
        }, &cancellation);
    } catch (const IdentityError&) { throw; }
    catch (const nickel::ErrorException& error) {
        if (error.error().code == "cancelled") throw IdentityError(IdentityErrorCode::cancelled);
        throw IdentityError(IdentityErrorCode::storage);
    }
    catch (...) { throw IdentityError(IdentityErrorCode::storage); }
}

} // namespace nickel::accounts

namespace nickel::accounts::detail {

StoredAccount::~StoredAccount() {
    for (auto* field : {&access_token, &client_token, &refresh_token, &password}) if (*field) erase_string(**field);
}

Json account_json(const PublicAccount& account) {
    Json value{{"id", account.id}, {"type", account_type_name(account.type)},
               {"name", account.name}, {"uuid", account.uuid},
               {"email", account.email ? Json(*account.email) : Json(nullptr)},
               {"server", account.server ? Json(*account.server) : Json(nullptr)},
               {"avatarUrl", account.avatar_url ? Json(*account.avatar_url) : Json(nullptr)}};
    return value;
}

Json stored_json(const StoredAccount& account) {
    auto value = account_json(account.public_account);
    if (account.access_token) value["accessToken"] = *account.access_token;
    if (account.client_token) value["clientToken"] = *account.client_token;
    if (account.refresh_token) value["refreshToken"] = *account.refresh_token;
    if (account.expires_at) value["expiresAt"] = *account.expires_at;
    if (account.password) value["password"] = *account.password;
    return value;
}

std::vector<StoredAccount> parse_accounts(std::string_view text) {
    if (trim(text).empty()) return {};
    try {
        auto parsed = parse_json(text, account_document_max_bytes, 16);
        EraseJson erase{parsed};
        if (!parsed.is_array() || parsed.size() > account_document_max_entries) return {};
        std::vector<StoredAccount> result;
        result.reserve(parsed.size());
        for (const auto& value : parsed) {
            if (!value.is_object()) return {};
            StoredAccount account;
            auto& pub = account.public_account;
            pub.id = string_field(value, "id", false, 256);
            pub.type = parse_account_type(string_field(value, "type", false, 32));
            pub.name = string_field(value, "name", false, 256);
            pub.uuid = string_field(value, "uuid", false, 64);
            pub.email = optional_field(value, "email", 512);
            pub.server = optional_field(value, "server", 4096);
            pub.avatar_url = optional_field(value, "avatarUrl", 4096);
            account.access_token = optional_field(value, "accessToken");
            account.client_token = optional_field(value, "clientToken");
            account.refresh_token = optional_field(value, "refreshToken");
            account.password = optional_field(value, "password", 64 * 1024);
            if (value.contains("expiresAt") && !value["expiresAt"].is_null()) account.expires_at = uint_field(value, "expiresAt");
            result.push_back(std::move(account));
        }
        return result;
    } catch (const IdentityError&) { return {}; }
}

std::string stored_accounts_json(const std::vector<StoredAccount>& accounts) {
    if (accounts.size() > account_document_max_entries) throw IdentityError(IdentityErrorCode::quota);
    Json value = Json::array();
    EraseJson erase{value};
    for (const auto& account : accounts) value.push_back(stored_json(account));
    class BoundedBuffer final : public std::streambuf {
        std::string text_;
    protected:
        std::streamsize xsputn(const char* data, std::streamsize size) override {
            if (size < 0 || static_cast<std::uint64_t>(size) > account_document_max_bytes - text_.size()) return 0;
            text_.append(data, static_cast<std::size_t>(size));
            return size;
        }
        int_type overflow(int_type character) override {
            if (traits_type::eq_int_type(character, traits_type::eof())) return traits_type::not_eof(character);
            if (text_.size() == account_document_max_bytes) return traits_type::eof();
            text_.push_back(traits_type::to_char_type(character));
            return character;
        }
    public:
        ~BoundedBuffer() override { erase_string(text_); }
        std::string take() { return std::move(text_); }
    } buffer;
    std::ostream output(&buffer);
    // Keep stream errors nonthrowing: the JSON stream adapter uses noexcept writes.
    output << std::setw(2) << value;
    if (!output) throw IdentityError(IdentityErrorCode::quota);
    return buffer.take();
}

AccountState::AccountState(AccountStorage& storage_, SecretVault& vault_, IdentityLog log_)
    : storage(storage_), vault(vault_), log(std::move(log_)) {}

void AccountState::observe_locked(const std::vector<StoredAccount>& accounts) const {
    std::set<std::string> present;
    for (const auto& account : accounts) {
        const auto& id = account.public_account.id;
        if (id.empty()) continue;
        auto encoded = stored_json(account);
        EraseJson erase_json_value{encoded};
        auto canonical = encoded.dump();
        EraseString erase_canonical{canonical};
        const auto hash = md5(canonical);
        const auto known = fingerprints.find(id);
        if (known == fingerprints.end() || known->second != hash) {
            fingerprints[id] = hash;
            revisions[id] = ++sequence;
        }
        present.insert(id);
    }
    std::erase_if(fingerprints, [&](const auto& item) { return !present.contains(item.first); });
    std::erase_if(revisions, [&](const auto& item) { return !present.contains(item.first); });
}

std::vector<StoredAccount> AccountState::load_locked() const {
    std::string text;
    try { text = storage.read_accounts(); } catch (...) { throw IdentityError(IdentityErrorCode::storage); }
    EraseString erase{text};
    auto list = parse_accounts(text);
    observe_locked(list);
    return list;
}

std::vector<StoredAccount> AccountState::update_locked(const std::function<void(std::vector<StoredAccount>&)>& mutator) {
    std::vector<StoredAccount> list;
    try {
        storage.transaction([&] {
            list = load_locked();
            mutator(list);
            auto text = stored_accounts_json(list);
            EraseString erase{text};
            storage.write_accounts(text);
        });
    } catch (const IdentityError&) { throw; }
    catch (...) {
        safe_log(log, "account.commit", "storage_failure");
        throw IdentityError(IdentityErrorCode::storage);
    }
    // Foundation's transaction must finish durably before revision publication.
    observe_locked(list);
    safe_log(log, "account.commit", "success");
    return list;
}

AccountSnapshot AccountState::snapshot(std::string_view id) const {
    std::lock_guard lock(mutex);
    const auto list = load_locked();
    for (const auto& account : list) if (account.public_account.id == id) return {account, revisions.at(std::string(id))};
    throw IdentityError(IdentityErrorCode::missing_account);
}

PublicAccount AccountState::commit(StoredAccount incoming, const std::optional<AccountSnapshot>& expected, bool deduplicate) {
    std::lock_guard lock(mutex);
    PublicAccount public_account;
    update_locked([&](auto& list) {
        auto found = list.end();
        if (expected) {
            const auto& id = expected->account.public_account.id;
            found = std::find_if(list.begin(), list.end(), [&](const auto& item) { return item.public_account.id == id; });
            if (found == list.end() || revisions[id] != expected->revision) throw IdentityError(IdentityErrorCode::stale_commit);
            incoming.public_account.id = id;
        } else if (deduplicate) {
            const auto& candidate = incoming.public_account;
            found = std::find_if(list.begin(), list.end(), [&](const auto& item) {
                const auto& existing = item.public_account;
                if (existing.id.empty() || existing.type != candidate.type) return false;
                if (candidate.type == AccountType::microsoft) return normalize_uuid(existing.uuid) == normalize_uuid(candidate.uuid);
                return existing.server == candidate.server && existing.email == candidate.email;
            });
            if (found != list.end()) {
                incoming.public_account.id = found->public_account.id;
                if (!incoming.public_account.email) incoming.public_account.email = found->public_account.email;
                if (!incoming.public_account.avatar_url) incoming.public_account.avatar_url = found->public_account.avatar_url;
            }
        }
        if (incoming.public_account.id.empty()) incoming.public_account.id = "acc_" + random_hex();
        public_account = incoming.public_account;
        if (found == list.end()) list.push_back(std::move(incoming)); else *found = std::move(incoming);
    });
    return public_account;
}

std::string AccountState::seal(std::string_view plaintext) {
    try {
        auto result = vault.seal(plaintext);
        if (!result.starts_with("v1:") || result.size() > 64 * 1024) throw IdentityError(IdentityErrorCode::vault);
        return result;
    } catch (...) { throw IdentityError(IdentityErrorCode::vault); }
}

Secret AccountState::open(std::string_view ciphertext) {
    try {
        auto result = vault.open(ciphertext);
        EraseString erase{result};
        return Secret(result);
    } catch (...) { throw IdentityError(IdentityErrorCode::vault); }
}

} // namespace nickel::accounts::detail
