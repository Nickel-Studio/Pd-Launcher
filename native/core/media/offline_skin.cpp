#include "offline_skin.hpp"
#include "../accounts/detail/util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <set>

namespace nickel::media {
namespace {
using accounts::IdentityError;
using accounts::IdentityErrorCode;
namespace ad = accounts::detail;
using Json = nickel::Json;
namespace fs = std::filesystem;
constexpr std::string_view owner_file = ".nickel-owner";
constexpr std::string_view token_file = ".nickel-owner-token";
constexpr std::string_view source_file = ".nickel-source.png";
constexpr std::string_view source_record = ".nickel-source.json";
constexpr std::string_view steve_texture = "assets/minecraft/textures/entity/steve.png";
constexpr std::string_view alex_texture = "assets/minecraft/textures/entity/alex.png";
constexpr std::string_view modern_steve = "assets/minecraft/textures/entity/player/wide/steve.png";
constexpr std::string_view modern_alex = "assets/minecraft/textures/entity/player/slim/alex.png";
constexpr std::string_view wide_prefix = "assets/minecraft/textures/entity/player/wide/";
constexpr std::string_view slim_prefix = "assets/minecraft/textures/entity/player/slim/";
constexpr std::string_view texture_base = "https://textures.minecraft.net/texture/";
constexpr std::size_t version_max_bytes = 64 * 1024;
constexpr std::size_t texture_property_max_bytes = 16 * 1024;

[[noreturn]] void fail(std::string_view code, std::string_view message) {
    throw nickel::ErrorException({"offline_skin_" + std::string(code), std::string(message), Json::object()});
}
std::string_view text(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
SkinBytes bytes(std::string_view value) { return {value.begin(), value.end()}; }
fs::path path(std::string_view value) { return fs::path(std::string(value)); }
bool hex(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
bool ours(std::string_view name) { return name == offline_skin_pack_folder || name == offline_skin_pack_id; }
std::optional<bool> texture_model(std::string_view name) {
    if (name == steve_texture) return false;
    if (name == alex_texture) return true;
    const bool slim = name.starts_with(slim_prefix);
    const auto prefix = slim ? slim_prefix : wide_prefix;
    if (!name.starts_with(prefix)) return std::nullopt;
    const auto file = name.substr(prefix.size());
    if (!file.ends_with(".png")) return std::nullopt;
    const auto stem = file.substr(0, file.size() - 4);
    if (stem.empty() || !std::all_of(stem.begin(), stem.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    })) return std::nullopt;
    return slim;
}
struct Target { std::string name; bool slim; };
struct Support {
    std::uint32_t format = 0;
    std::optional<std::uint32_t> minor;
    bool legacy = false;
    bool prefixed = true;
    std::vector<Target> targets;
};
std::uint32_t number(const Json& value) {
    if (!(value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0)))
        fail("unsupported_client", "Client resource-pack metadata is invalid. Reinstall the vanilla client or choose random skin.");
    const auto n = value.get<std::uint64_t>();
    if (n > UINT32_MAX) fail("unsupported_client", "Client resource-pack version is out of range. Reinstall the vanilla client.");
    return static_cast<std::uint32_t>(n);
}
Support old_support(std::string_view version) {
    const auto unsupported = [] {
        fail("unsupported_client", "Client JAR has no resource-pack metadata for this version. Reinstall the vanilla client or choose random skin.");
    };
    std::array<std::uint32_t, 3> parts{};
    std::size_t count{}, start{};
    while (start < version.size()) {
        if (count == parts.size()) unsupported();
        auto end = version.find('.', start);
        if (end == std::string_view::npos) end = version.size();
        const auto token = version.substr(start, end - start);
        if (token.empty() || !std::all_of(token.begin(), token.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) unsupported();
        const auto result = std::from_chars(token.data(), token.data() + token.size(), parts[count]);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) unsupported();
        ++count;
        start = end + 1;
    }
    if (version.empty() || version.back() == '.' || count < 2 || parts[0] != 1) unsupported();
    const auto minor = parts[1], patch = parts[2];
    std::uint32_t max_patch{};
    switch (minor) {
    case 6: max_patch = 4; break;
    case 7: max_patch = 10; break;
    case 8: max_patch = 9; break;
    case 9: case 14: max_patch = 4; break;
    case 10: case 11: case 12: case 13: case 15: case 18: case 19: max_patch = 2; break;
    case 16: max_patch = 5; break;
    case 17: max_patch = 1; break;
    default: unsupported();
    }
    if (patch > max_patch) unsupported();
    Support support;
    support.legacy = minor < 8; support.prefixed = minor >= 13;
    if (minor <= 8) support.format = 1;
    else if (minor <= 10) support.format = 2;
    else if (minor <= 12) support.format = 3;
    else if (minor <= 14) support.format = 4;
    else if (minor == 15 || (minor == 16 && patch <= 1)) support.format = 5;
    else support.format = minor - 10;
    return support;
}
SkinBytes read_entry(ClientSkinArchive& archive, const fs::path& jar,
                      const SkinArchiveEntry& entry, std::size_t limit, std::stop_token stop) {
    ad::check_stop(stop);
    if (entry.directory || entry.size > limit) fail("archive", "Client skin or metadata entry exceeds its size limit. Reinstall the client.");
    auto data = archive.read(jar, entry.name, limit, stop);
    if (data.size() > limit || data.size() != entry.size) fail("archive", "Client JAR entry length does not match its directory. Reinstall the client.");
    ad::check_stop(stop);
    return data;
}
struct Assets { Support support; std::vector<SkinArchiveEntry> entries; };
Assets read_assets(ClientSkinArchive& archive, const fs::path& jar,
                    std::string_view version, std::stop_token stop) {
    if (!jar.is_absolute()) fail("invalid_path", "Pass the installed, resolved absolute vanilla client JAR path; do not infer it from a display name.");
    auto entries = archive.entries(jar, stop);
    if (entries.size() > 100000) fail("archive", "Client JAR contains too many entries. Reinstall the client.");
    std::set<std::string> names;
    std::vector<Target> targets;
    std::optional<SkinArchiveEntry> metadata;
    std::size_t name_bytes{};
    for (const auto& entry : entries) {
        ad::check_stop(stop);
        if (entry.name.size() > 4096 || entry.name.size() > 8 * 1024 * 1024 - name_bytes)
            fail("archive", "Client JAR directory exceeds the allowed size. Reinstall the client.");
        name_bytes += entry.name.size();
        if (entry.name == "version.json") {
            if (metadata) fail("archive", "Client JAR has duplicate version metadata. Reinstall the client.");
            metadata = entry;
        }
        const auto slim = texture_model(entry.name);
        if (!slim) continue;
        if (entry.directory || !names.insert(entry.name).second || targets.size() == 128)
            fail("archive", "Client default-skin entries are invalid or duplicated. Reinstall the client.");
        targets.push_back({entry.name, *slim});
    }
    if (targets.empty()) fail("missing_texture", "Client JAR has no default player textures. Reinstall the vanilla client or choose random skin.");
    Support support;
    if (metadata) {
        const auto data = read_entry(archive, jar, *metadata, version_max_bytes, stop);
        const auto value = ad::parse_json(text(data), version_max_bytes);
        if (!value.is_object() || !value.contains("pack_version")) fail("unsupported_client", "Client version.json has no resource-pack version. Reinstall the client.");
        const auto& pack = value.at("pack_version");
        if (pack.is_number()) support.format = number(pack);
        else if (pack.is_object() && pack.contains("resource_major")) {
            support.format = number(pack.at("resource_major"));
            if (!pack.contains("resource_minor")) fail("unsupported_client", "Client resource-pack minor version is missing. Reinstall the client.");
            support.minor = number(pack.at("resource_minor"));
        } else if (pack.is_object() && pack.contains("resource")) support.format = number(pack.at("resource"));
        else fail("unsupported_client", "Client resource-pack metadata is invalid. Reinstall the client.");
        if (!support.format) fail("unsupported_client", "Client resource-pack major version must be positive. Reinstall the client.");
    } else support = old_support(version);
    std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    support.targets = std::move(targets);
    return {std::move(support), std::move(entries)};
}
SkinBytes default_source(ClientSkinArchive& archive, const fs::path& jar,
                          const Assets& assets, bool slim, std::stop_token stop) {
    const auto candidates = slim ? std::array{modern_alex, alex_texture} : std::array{modern_steve, steve_texture};
    for (const auto candidate : candidates) {
        const auto found = std::find_if(assets.entries.begin(), assets.entries.end(), [&](const auto& entry) { return entry.name == candidate; });
        if (found != assets.entries.end()) return read_entry(archive, jar, *found, skin_png_max_bytes, stop);
    }
    fail("missing_texture", slim ? "Client has no Alex texture. Choose Steve/custom or reinstall a client with Alex." :
                                   "Client has no Steve texture. Reinstall the vanilla client or choose custom skin.");
}
struct OwnedPack {
    fs::path directory;
    std::string identity;
    std::optional<SkinBytes> token;
    bool operator==(const OwnedPack&) const = default;
};
std::optional<OwnedPack> owned_pack(SkinFileStore& files, const fs::path& runtime) {
    const auto packs = runtime / "resourcepacks";
    if (!files.directory(packs)) return std::nullopt;
    const auto pack = packs / path(offline_skin_pack_folder);
    if (!files.directory(pack)) return std::nullopt;
    auto pin = files.pin_directory(pack);
    const auto identity = files.directory_identity(pack);
    const auto owner = files.read(pack / path(owner_file), offline_skin_record_max_bytes);
    if (!owner || text(*owner) != offline_skin_owner_content)
        fail("ownership", "Nickel-offline-skin exists without a valid Nickel owner marker. Rename that user-owned directory before retrying.");
    const auto token = files.read(pack / path(token_file), offline_skin_record_max_bytes);
    if (token && (token->size() != 33 || token->back() != '\n' || !hex(text(*token).substr(0, 32))))
        fail("ownership", "Nickel offline-skin owner token is invalid. Rename the pack before retrying; user files will not be claimed.");
    if (files.directory_identity(pack) != identity) fail("stale_owner", "Offline-skin pack was replaced while preparing. Retry after closing the game.");
    return OwnedPack{pack, identity, token};
}
void validate_owner(SkinFileStore& files, const fs::path& runtime,
                     std::string_view runtime_identity, const std::optional<OwnedPack>& expected) {
    if (files.directory_identity(runtime) != runtime_identity || owned_pack(files, runtime) != expected)
        fail("stale_owner", "Offline-skin ownership changed during preparation. Retry after closing the game.");
}
std::optional<std::pair<SkinBytes, bool>> cached_official(SkinFileStore& files,
                                                       const std::optional<OwnedPack>& pack,
                                                       std::string_view name, std::stop_token stop) {
    if (!pack) return std::nullopt;
    const auto data = files.read(pack->directory / path(source_record), offline_skin_record_max_bytes);
    if (!data) return std::nullopt;
    const auto value = ad::parse_json(text(*data), offline_skin_record_max_bytes);
    if (!value.is_object() || ad::string_field(value, "mode") != "official" ||
        ad::ascii_lower(ad::string_field(value, "name")) != ad::ascii_lower(name)) return std::nullopt;
    bool slim{};
    if (value.contains("sourceSlim")) {
        if (!value.at("sourceSlim").is_boolean()) fail("cache", "Offline skin cache model is invalid. Move the managed pack aside and retry.");
        slim = value.at("sourceSlim").get<bool>();
    }
    auto source = files.read(pack->directory / path(source_file), skin_png_max_bytes);
    if (!source) return std::nullopt;
    (void)decode_skin_png(*source, stop);
    return std::pair{std::move(*source), slim};
}
std::string texture_url(std::string_view url, bool allow_upgrade) {
    auto id = url.starts_with(texture_base) ? url.substr(texture_base.size()) : std::string_view{};
    constexpr std::string_view old_base = "http://textures.minecraft.net/texture/";
    if (id.empty() && allow_upgrade && url.starts_with(old_base)) id = url.substr(old_base.size());
    if (id.empty() || id.size() > 64 || !hex(id)) throw IdentityError(IdentityErrorCode::untrusted_download);
    return std::string(texture_base) + std::string(id);
}
SkinBytes get(accounts::IdentityHttp& http, std::string url, std::size_t limit,
               bool texture, std::stop_token stop, const accounts::IdentityLog& log) {
    for (unsigned redirects = 0;; ++redirects) {
        ad::check_stop(stop);
        accounts::IdentityHttpRequest request;
        request.method = "GET"; request.url = url; request.max_response_bytes = limit;
        request.headers.emplace_back("Accept", texture ? "image/png" : "application/json");
        accounts::IdentityHttpResponse response;
        try { response = http.perform(request, stop); }
        catch (const nickel::ErrorException&) { ad::check_stop(stop); throw; }
        catch (...) {
            ad::check_stop(stop);
            ad::safe_log(log, "offline_skin.fetch", "network");
            throw IdentityError(IdentityErrorCode::network);
        }
        ad::check_stop(stop);
        if (response.body.size() > limit) throw IdentityError(IdentityErrorCode::image_too_large);
        if (response.status == 301 || response.status == 302 || response.status == 303 || response.status == 307 || response.status == 308) {
            if (!texture || redirects >= 3) throw IdentityError(IdentityErrorCode::untrusted_download);
            const auto location = response.header("Location");
            if (location.empty()) throw IdentityError(IdentityErrorCode::untrusted_download);
            if (location.starts_with("/texture/")) url = texture_url("https://textures.minecraft.net" + location, false);
            else url = texture_url(location, false);
            continue;
        }
        if (response.status != 200) throw IdentityError(IdentityErrorCode::no_profile, response.status);
        return std::move(response.body);
    }
}
std::pair<SkinBytes, bool> fetch_official(accounts::IdentityHttp& http, std::string_view name,
                                        std::stop_token stop, const accounts::IdentityLog& log) {
    auto data = get(http, "https://api.mojang.com/users/profiles/minecraft/" + std::string(name), version_max_bytes, false, stop, log);
    auto lookup = ad::parse_json(text(data), version_max_bytes);
    const auto id = ad::string_field(lookup, "id", true, 32);
    if (id.size() != 32 || !hex(id)) throw IdentityError(IdentityErrorCode::invalid_response);
    data = get(http, "https://sessionserver.mojang.com/session/minecraft/profile/" + id, version_max_bytes, false, stop, log);
    const auto profile = ad::parse_json(text(data), version_max_bytes);
    if (!profile.is_object() || !profile.contains("properties") || !profile.at("properties").is_array()) throw IdentityError(IdentityErrorCode::invalid_response);
    std::string encoded;
    for (const auto& property : profile.at("properties")) {
        if (ad::string_field(property, "name") == "textures") {
            encoded = ad::string_field(property, "value", true, texture_property_max_bytes);
            break;
        }
    }
    if (encoded.empty()) throw IdentityError(IdentityErrorCode::invalid_response);
    const auto decoded = ad::base64_decode(encoded, texture_property_max_bytes);
    const auto textures = ad::parse_json(text(decoded), texture_property_max_bytes);
    const auto* skin = textures.contains("textures") && textures.at("textures").is_object() && textures.at("textures").contains("SKIN") ? &textures.at("textures").at("SKIN") : nullptr;
    if (!skin || !skin->is_object()) throw IdentityError(IdentityErrorCode::invalid_response);
    const auto url = texture_url(ad::string_field(*skin, "url", true), true);
    bool slim{};
    if (skin->contains("metadata") && skin->at("metadata").is_object() && skin->at("metadata").contains("model")) {
        const auto model = ad::string_field(skin->at("metadata"), "model", true, 16);
        if (model == "slim") slim = true;
        else if (model != "classic") throw IdentityError(IdentityErrorCode::invalid_response);
    }
    return {get(http, url, skin_png_max_bytes, true, stop, log), slim};
}
void parents(SkinFileStore& files, const fs::path& pack, std::string_view target,
               const std::function<void()>& validate) {
    auto directory = pack;
    std::size_t start{};
    for (;;) {
        const auto end = target.find('/', start);
        if (end == std::string_view::npos) break;
        validate();
        directory /= path(target.substr(start, end - start));
        files.ensure_directory(directory);
        start = end + 1;
    }
}
OfflineSkinResult clear(SkinFileStore& files, const fs::path& requested,
                        std::stop_token stop, const accounts::IdentityLog& log) {
    if (!requested.is_absolute()) fail("invalid_path", "Game runtime directory must be an absolute resolved path.");
    if (!files.directory(requested)) return {};
    const auto runtime = files.canonical_directory(requested);
    auto pin = files.pin_directory(runtime);
    const auto identity = files.directory_identity(runtime);
    auto lock = files.lock(runtime, stop);
    const auto validate = [&] {
        ad::check_stop(stop);
        if (files.directory_identity(runtime) != identity)
            fail("stale_owner", "Game runtime changed during offline skin cleanup. Close the game and retry.");
    };
    validate();
    // Recovery precedes observation, including a temporarily missing options.txt.
    // Disable only exact legacy identifiers; never claim a user-owned pack.
    auto before = files.observe_options(runtime / "options.txt", stop);
    if (!before) fail("filesystem", "Cannot observe game settings safely. Close the game and retry.");
    if (!before->exists()) return {};
    const auto original = text(before->bytes());
    if (!ad::valid_utf8(original)) {
        if (original.find(offline_skin_pack_folder) == std::string_view::npos) return {};
        fail("invalid_options", "options.txt is not UTF-8. Back it up and save resource-pack settings in-game before retrying.");
    }
    const auto updated = patch_offline_skin_options(original, std::nullopt);
    const bool changed = updated != original;
    if (changed) {
        files.check_write_capacity(runtime / "options.txt", updated.size());
        validate();
        before->replace(bytes(updated), stop);
    }
    ad::safe_log(log, "offline_skin.clear", changed ? "disabled" : "unchanged");
    return {false, changed, {}, 0};
}
} // namespace

OfflineSkinMode parse_offline_skin_mode(std::string_view mode) {
    if (mode == "random") return OfflineSkinMode::random;
    if (mode == "steve") return OfflineSkinMode::steve;
    if (mode == "alex") return OfflineSkinMode::alex;
    if (mode == "official") return OfflineSkinMode::official;
    if (mode == "custom") return OfflineSkinMode::custom;
    fail("invalid_mode", "Choose random, Steve, Alex, official player name, or a custom PNG in game settings.");
}
std::string_view offline_skin_mode_name(OfflineSkinMode mode) noexcept {
    switch (mode) {
    case OfflineSkinMode::random: return "random";
    case OfflineSkinMode::steve: return "steve";
    case OfflineSkinMode::alex: return "alex";
    case OfflineSkinMode::official: return "official";
    case OfflineSkinMode::custom: return "custom";
    }
    return {};
}
std::string patch_offline_skin_options(std::string_view source, std::optional<std::string_view> enable) {
    if (source.size() > offline_skin_options_max_bytes) fail("invalid_options", "options.txt exceeds 2 MiB. Back it up and save settings in-game.");
    if (!ad::valid_utf8(source) || (enable && !ours(*enable))) fail("invalid_options", "Resource-pack options or requested Nickel pack identifier are invalid.");
    std::string result;
    result.reserve(source.size() + offline_skin_pack_id.size() + 32);
    bool saw_resources{};
    std::size_t position{}, index{};
    while (position < source.size()) {
        auto end = source.find('\n', position);
        end = end == std::string_view::npos ? source.size() : end + 1;
        const auto line = source.substr(position, end - position);
        auto body = line;
        std::string_view ending;
        if (body.ends_with("\r\n")) { body.remove_suffix(2); ending = "\r\n"; }
        else if (body.ends_with("\n")) { body.remove_suffix(1); ending = "\n"; }
        std::string_view bom;
        if (!index && body.starts_with("\xEF\xBB\xBF")) { bom = body.substr(0, 3); body.remove_prefix(3); }
        const auto colon = body.find(':');
        const auto key = body.substr(0, colon);
        if (colon == std::string_view::npos || (key != "resourcePacks" && key != "incompatibleResourcePacks")) result.append(line);
        else {
            if (key == "resourcePacks") saw_resources = true;
            const auto value = body.substr(colon + 1);
            Json original;
            try { original = ad::parse_json(value, offline_skin_options_max_bytes); }
            catch (const IdentityError&) { original = nullptr; }
            const bool valid = original.is_array() && std::all_of(original.begin(), original.end(), [](const auto& item) { return item.is_string(); });
            if (!valid) {
                if (!enable && value.find(offline_skin_pack_folder) == std::string_view::npos) result.append(line);
                else fail("invalid_options", "Resource-pack list in options.txt is invalid. Back it up and save resource-pack settings in-game.");
            } else {
                Json list = Json::array();
                for (const auto& item : original) if (!ours(item.get_ref<const std::string&>())) list.push_back(item);
                if (key == "resourcePacks" && enable) list.push_back(std::string(*enable));
                if (list == original) result.append(line);
                else { result.append(bom); result.append(key); result.push_back(':'); result += list.dump(); result.append(ending); }
            }
        }
        position = end; ++index;
    }
    if (!saw_resources && enable) {
        const auto ending = source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
        if (!result.empty() && result.back() != '\n') result += ending;
        result += "resourcePacks:" + Json::array({std::string(*enable)}).dump() + ending;
    }
    if (result.size() > offline_skin_options_max_bytes) fail("invalid_options", "Updated resource-pack options exceed 2 MiB. Save settings in-game and retry.");
    return result;
}

OfflineSkinService::OfflineSkinService(SkinFileStore& files, ClientSkinArchive& archive,
                                       accounts::IdentityHttp* http, accounts::IdentityLog log)
    : files_(files), archive_(archive), http_(http), log_(std::move(log)) {}

std::optional<fs::path> OfflineSkinService::pick(const std::optional<fs::path>& selected,
                                               const fs::path& directory, std::stop_token stop) {
    if (!selected) return std::nullopt;
    ad::check_stop(stop);
    if (!selected->is_absolute() || !directory.is_absolute()) fail("invalid_path", "Select a local PNG and use the resolved absolute application offline-skins directory.");
    const auto source = files_.read(*selected, skin_png_max_bytes);
    if (!source) fail("missing_skin", "Selected skin file no longer exists. Choose a 64x64 or 64x32 PNG again.");
    const auto png = encode_skin_png(decode_skin_png(*source, stop), stop);
    ad::check_stop(stop);
    files_.check_write_capacity(directory, png.size());
    files_.ensure_directory(directory);
    const auto target = files_.canonical_directory(directory) / ("skin-" + ad::random_hex() + ".png");
    ad::check_stop(stop);
    files_.create_file(target, png);
    return target;
}

OfflineSkinResult OfflineSkinService::prepare(const OfflineSkinRequest& request, std::stop_token stop) {
    ad::check_stop(stop);
    if (request.mode == OfflineSkinMode::random) return clear(files_, request.runtime_directory, stop, log_);
    if (offline_skin_mode_name(request.mode).empty()) fail("invalid_mode", "Choose a valid offline skin mode in game settings.");
    if (!request.runtime_directory.is_absolute() || !files_.directory(request.runtime_directory))
        fail("invalid_path", "Installed game runtime directory must be an existing resolved absolute directory.");
    const auto runtime = files_.canonical_directory(request.runtime_directory);
    auto runtime_pin = files_.pin_directory(runtime);
    const auto runtime_identity = files_.directory_identity(runtime);
    std::optional<OwnedPack> expected;
    std::optional<std::pair<SkinBytes, bool>> cached;
    const auto name = ad::trim(request.name);
    if (name.size() > 1024 || !ad::valid_utf8(name)) fail("invalid_name", "Skin source name is invalid. Select the skin again.");
    if (request.mode == OfflineSkinMode::official && (name.empty() || name.size() > 16 ||
        !std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }))) fail("invalid_name", "Enter a 1-16 character official player name using ASCII letters, digits or underscores.");
    {
        auto lock = files_.lock(runtime, stop);
        expected = owned_pack(files_, runtime);
        if (request.mode == OfflineSkinMode::official) cached = cached_official(files_, expected, name, stop);
    }
    auto assets = read_assets(archive_, request.client_jar, request.minecraft_version, stop);
    SkinBytes source;
    bool source_slim{};
    switch (request.mode) {
    case OfflineSkinMode::steve: source = default_source(archive_, request.client_jar, assets, false, stop); break;
    case OfflineSkinMode::alex: source = default_source(archive_, request.client_jar, assets, true, stop); source_slim = true; break;
    case OfflineSkinMode::official: {
        if (cached) { source = std::move(cached->first); source_slim = cached->second; }
        else {
            if (!http_) fail("network", "Native HTTP service is required to fetch an official player's skin. Choose a local PNG to work offline.");
            auto fetched = fetch_official(*http_, name, stop, log_);
            source = std::move(fetched.first); source_slim = fetched.second;
        }
        break;
    }
    case OfflineSkinMode::custom: {
        if (!request.path || !request.path->is_absolute()) fail("invalid_path", "Select an absolute local PNG skin path in game settings.");
        auto selected = files_.read(*request.path, skin_png_max_bytes);
        if (!selected) fail("missing_skin", "Selected custom skin is missing. Select the PNG again.");
        source = std::move(*selected); source_slim = request.slim; break;
    }
    default: fail("invalid_mode", "Choose a valid offline skin mode in game settings.");
    }
    const auto decoded = decode_skin_png(source, stop);
    const auto steve = encode_skin_png(adapt_skin(decoded, source_slim, false, assets.support.legacy), stop);
    const bool needs_alex = std::any_of(assets.support.targets.begin(), assets.support.targets.end(), [](const auto& target) { return target.slim; });
    const auto alex = needs_alex ? encode_skin_png(adapt_skin(decoded, source_slim, true, false), stop) : SkinBytes{};
    Json metadata = {{"pack", {{"pack_format", assets.support.format}, {"description", "Palladium \u79bb\u7ebf\u76ae\u80a4"}}}};
    if (assets.support.minor) {
        metadata["pack"]["min_format"] = Json::array({assets.support.format, *assets.support.minor});
        metadata["pack"]["max_format"] = metadata["pack"]["min_format"];
    }
    const auto metadata_bytes = bytes(metadata.dump(2));
    const auto record = bytes(Json{{"mode", offline_skin_mode_name(request.mode)}, {"name", name}, {"sourceSlim", source_slim}}.dump());
    if (record.size() > offline_skin_record_max_bytes) fail("invalid_name", "Skin source record is too large. Select the skin again.");
    ad::check_stop(stop);
    auto lock = files_.lock(runtime, stop);
    validate_owner(files_, runtime, runtime_identity, expected);
    auto before = files_.observe_options(runtime / "options.txt", stop);
    if (!before) fail("filesystem", "Cannot observe game settings safely. Close the game and retry.");
    const auto original = text(before->bytes());
    const auto updated = patch_offline_skin_options(original, assets.support.prefixed ? offline_skin_pack_id : offline_skin_pack_folder);
    std::uint64_t write_bytes = source.size() + metadata_bytes.size() + record.size() + 2ULL * updated.size() + 4096;
    for (const auto& target : assets.support.targets) write_bytes += target.slim ? alex.size() : steve.size();
    files_.check_write_capacity(runtime, write_bytes);
    const auto packs = runtime / "resourcepacks";
    files_.ensure_directory(packs);
    auto packs_pin = files_.pin_directory(packs);
    const auto pack = packs / path(offline_skin_pack_folder);
    std::unique_ptr<SkinFileLock> pack_pin;
    if (!expected) {
        files_.create_directory(pack);
        pack_pin = files_.pin_directory(pack);
        const auto identity = files_.directory_identity(pack);
        const auto token = bytes(ad::random_hex() + "\n");
        files_.create_file(pack / path(owner_file), bytes(offline_skin_owner_content));
        files_.create_file(pack / path(token_file), token);
        expected = OwnedPack{pack, identity, token};
    } else {
        pack_pin = files_.pin_directory(pack);
        validate_owner(files_, runtime, runtime_identity, expected);
        if (!expected->token) {
            const auto token = bytes(ad::random_hex() + "\n");
            files_.create_file(pack / path(token_file), token);
            expected->token = token;
        }
    }
    const auto validate = [&] {
        ad::check_stop(stop);
        validate_owner(files_, runtime, runtime_identity, expected);
    };
    for (const auto& target : assets.support.targets) {
        parents(files_, pack, target.name, validate);
        validate();
        files_.replace_file(pack / path(target.name), target.slim ? alex : steve);
    }
    validate(); files_.replace_file(pack / "pack.mcmeta", metadata_bytes);
    validate(); files_.replace_file(pack / path(source_file), source);
    validate(); files_.replace_file(pack / path(source_record), record);
    validate();
    // Even unchanged bytes must still bind the observed identity and revision.
    // The adapter finishes any committed receipt before allowing launch.
    before->replace(bytes(updated), stop);
    ad::safe_log(log_, "offline_skin.prepare", "ready");
    return {true, updated != original, pack, assets.support.targets.size()};
}

} // namespace nickel::media
