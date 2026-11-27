#include "mock_http.hpp"
#include "../../core/media/offline_skin.hpp"

#include <algorithm>
#include <map>
#include <string_view>

namespace identity_test {
namespace {
using namespace nickel::media;
using nickel::accounts::IdentityErrorCode;
namespace fs = std::filesystem;
SkinBytes skin_bytes(std::string_view value) { return {value.begin(),value.end()}; }
std::string skin_text(const SkinBytes& value) { return {value.begin(),value.end()}; }
[[noreturn]] void fixture_error(std::string_view code) {
    throw nickel::ErrorException({"offline_skin_"+std::string(code),"Controlled offline file failure",Json::object()});
}
template<class Operation>
void offline_error(std::string_view code, Operation operation) {
    try { operation(); }
    catch (const nickel::ErrorException& error) {
        require(error.error().code == "offline_skin_"+std::string(code),"unexpected offline error code");
        return;
    }
    throw std::runtime_error("expected offline skin error");
}
class EmptySkinLock final : public SkinFileLock {};
class MemorySkinFiles final : public SkinFileStore {
public:
    fs::path root = fs::absolute("offline-skin-fixture");
    std::map<fs::path,std::string> directories;
    std::map<fs::path,SkinBytes> contents;
    std::map<fs::path,unsigned> revisions;
    unsigned operations{}, writes{}, sequence{}, options_observations{}, options_commits{};
    bool no_space{};
    std::function<void(const fs::path&)> before_read, before_replace;
    std::function<void()> before_capacity, before_observe, after_options_commit;
    MemorySkinFiles() { add_directory(root); }
    void add_directory(const fs::path& path) {
        if (path.empty() || directories.contains(path)) return;
        const auto parent = path.parent_path();
        if (parent != path) add_directory(parent);
        directories.emplace(path,"directory-"+std::to_string(++sequence));
    }
    void put(const fs::path& path, SkinBytes value) {
        add_directory(path.parent_path()); contents[path] = std::move(value);
        revisions[path] = ++sequence;
    }
    fs::path pack() const { return root/"resourcepacks"/std::string(offline_skin_pack_folder); }
    void owned(bool with_token = true) {
        add_directory(pack());
        put(pack()/".nickel-owner",skin_bytes(offline_skin_owner_content));
        if (with_token) put(pack()/".nickel-owner-token",skin_bytes(std::string(32,'a')+"\n"));
    }
    bool directory(const fs::path& path) override {
        ++operations;
        if (contents.contains(path)) fixture_error("unsafe_path");
        return directories.contains(path);
    }
    fs::path canonical_directory(const fs::path& path) override {
        ++operations;
        if (!path.is_absolute() || !directories.contains(path)) fixture_error("invalid_path");
        return path;
    }
    std::string directory_identity(const fs::path& path) override {
        ++operations;
        const auto found = directories.find(path);
        if (found == directories.end()) fixture_error("invalid_path");
        return found->second;
    }
    std::unique_ptr<SkinFileLock> pin_directory(const fs::path& path) override {
        ++operations;
        if (!directories.contains(path)) fixture_error("invalid_path");
        return std::make_unique<EmptySkinLock>();
    }
    std::optional<SkinBytes> read(const fs::path& path, std::size_t max_bytes) override {
        ++operations;
        if (before_read) before_read(path);
        if (directories.contains(path)) fixture_error("unsafe_path");
        const auto found = contents.find(path);
        if (found == contents.end()) return std::nullopt;
        if (found->second.size() > max_bytes) fixture_error("file_too_large");
        return found->second;
    }
    std::unique_ptr<SkinOptionsSnapshot> observe_options(const fs::path& path, std::stop_token stop) override {
        nickel::accounts::detail::check_stop(stop);
        ++operations; ++options_observations;
        if (before_observe) before_observe();
        nickel::accounts::detail::check_stop(stop);
        auto before = read(path,offline_skin_options_max_bytes);
        const auto revision = revisions.contains(path) ? revisions.at(path) : 0;
        class Snapshot final : public SkinOptionsSnapshot {
        public:
            Snapshot(MemorySkinFiles& files, fs::path path, std::optional<SkinBytes> before, unsigned revision)
                : files_(files), path_(std::move(path)), before_(std::move(before)), revision_(revision) {}
            bool exists() const noexcept override { return before_.has_value(); }
            std::span<const std::uint8_t> bytes() const noexcept override {
                return before_ ? std::span<const std::uint8_t>(*before_) : std::span<const std::uint8_t>{};
            }
            void replace(std::span<const std::uint8_t> value, std::stop_token stop) override {
                nickel::accounts::detail::check_stop(stop);
                ++files_.operations;
                if (value.size() > offline_skin_options_max_bytes) fixture_error("file_too_large");
                if (files_.before_replace) files_.before_replace(path_);
                nickel::accounts::detail::check_stop(stop);
                if (files_.directories.contains(path_) || !files_.directories.contains(path_.parent_path())) fixture_error("unsafe_path");
                const auto found = files_.contents.find(path_);
                const auto current = found == files_.contents.end() ? std::optional<SkinBytes>{} : std::optional<SkinBytes>{found->second};
                const auto revision = files_.revisions.contains(path_) ? files_.revisions.at(path_) : 0;
                if (current != before_ || revision != revision_) fixture_error("changing_options");
                files_.contents[path_] = SkinBytes(value.begin(),value.end());
                files_.revisions[path_] = ++files_.sequence;
                ++files_.writes; ++files_.options_commits;
                if (files_.after_options_commit) files_.after_options_commit();
            }
        private:
            MemorySkinFiles& files_;
            fs::path path_;
            std::optional<SkinBytes> before_;
            unsigned revision_;
        };
        return std::make_unique<Snapshot>(*this,path,std::move(before),revision);
    }
    void check_write_capacity(const fs::path&, std::uint64_t) override {
        ++operations;
        if (before_capacity) before_capacity();
        if (no_space) fixture_error("disk_full");
    }
    void ensure_directory(const fs::path& path) override {
        ++operations;
        if (contents.contains(path)) fixture_error("unsafe_path");
        if (!directories.contains(path)) { add_directory(path); ++writes; }
    }
    void create_directory(const fs::path& path) override {
        ++operations;
        if (directories.contains(path) || contents.contains(path)) fixture_error("conflict");
        if (!directories.contains(path.parent_path())) fixture_error("invalid_path");
        add_directory(path); ++writes;
    }
    void create_file(const fs::path& path, std::span<const std::uint8_t> value) override {
        ++operations;
        if (contents.contains(path) || directories.contains(path)) fixture_error("conflict");
        if (!directories.contains(path.parent_path())) fixture_error("invalid_path");
        contents[path] = SkinBytes(value.begin(),value.end()); revisions[path] = ++sequence; ++writes;
    }
    void replace_file(const fs::path& path, std::span<const std::uint8_t> value) override {
        ++operations;
        if (before_replace) before_replace(path);
        if (directories.contains(path) || !directories.contains(path.parent_path())) fixture_error("unsafe_path");
        contents[path] = SkinBytes(value.begin(),value.end()); revisions[path] = ++sequence; ++writes;
    }
    std::unique_ptr<SkinFileLock> lock(const fs::path&, std::stop_token stop) override {
        ++operations;
        nickel::accounts::detail::check_stop(stop);
        return std::make_unique<EmptySkinLock>();
    }
};
class FixtureSkinArchive final : public ClientSkinArchive {
public:
    std::map<std::string,SkinBytes> contents;
    std::vector<SkinArchiveEntry> extras;
    unsigned calls{};
    std::function<void()> before_entries;
    FixtureSkinArchive() {
        contents["version.json"] = skin_bytes(Json{{"pack_version",{{"resource",9}}}}.dump());
        contents["assets/minecraft/textures/entity/steve.png"] = encode_skin_png(image(64,64,{80,40,20,255}));
        contents["assets/minecraft/textures/entity/alex.png"] = encode_skin_png(image(64,64,{20,40,80,255}));
        contents["net/minecraft/CON.class"] = {1,2,3};
    }
    std::vector<SkinArchiveEntry> entries(const fs::path& jar, std::stop_token stop) override {
        ++calls;
        require(jar.is_absolute(),"resolved client JAR is absolute");
        nickel::accounts::detail::check_stop(stop);
        if (before_entries) before_entries();
        auto result = extras;
        for (const auto& [name,value] : contents) result.push_back({name,value.size(),false});
        return result;
    }
    SkinBytes read(const fs::path&, std::string_view name, std::size_t max_bytes, std::stop_token stop) override {
        ++calls;
        nickel::accounts::detail::check_stop(stop);
        const auto found = contents.find(std::string(name));
        require(found != contents.end() && found->second.size() <= max_bytes,"bounded exact archive fixture read");
        return found->second;
    }
};
OfflineSkinRequest custom_request(MemorySkinFiles& files) {
    OfflineSkinRequest request;
    request.runtime_directory = files.root;
    request.client_jar = files.root/"inherited-vanilla-client.jar";
    request.minecraft_version = "1.19.2";
    request.mode = OfflineSkinMode::custom;
    request.path = files.root/"selected.png";
    files.put(*request.path,encode_skin_png(image(64,64,{120,60,30,255})));
    return request;
}
void test_options_exact_preservation() {
    const std::string original = "\xEF\xBB\xBFgamma:0.75\r\nresourcePacks:[\"vanilla\",\"file/user.zip\",\"file/Nickel-offline-skin\"]\r\n"
        "incompatibleResourcePacks:[\"file/old.zip\",\"Nickel-offline-skin\"]\r\nkey_key.forward:87";
    const auto enabled = patch_offline_skin_options(original,offline_skin_pack_id);
    require(enabled.starts_with("\xEF\xBB\xBFgamma:0.75\r\n") && enabled.ends_with("key_key.forward:87"),"BOM, CRLF and unrelated option lines stay byte-identical");
    require(enabled.find("incompatibleResourcePacks:[\"file/old.zip\"]\r\n") != std::string::npos,"incompatible array loses only our exact identifier");
    require(patch_offline_skin_options(enabled,offline_skin_pack_id) == enabled,"offline enablement is idempotent");
    const auto cleared = patch_offline_skin_options(enabled,std::nullopt);
    require(cleared.find("resourcePacks:[\"vanilla\",\"file/user.zip\"]\r\n") != std::string::npos,"random retains other enabled packs");
    require(patch_offline_skin_options(cleared,std::nullopt) == cleared,"random disablement is idempotent");
    require(patch_offline_skin_options("resourcePacks:[\"Nickel-offline-skin\",\"file/Nickel-offline-skin\",\"file/Nickel-offline-skin-user.zip\",\"mine\",\"mine\"]\n",std::nullopt) ==
            "resourcePacks:[\"file/Nickel-offline-skin-user.zip\",\"mine\",\"mine\"]\n","no prefix deletion or unrelated deduplication");
    require(patch_offline_skin_options("resourcePacks:broken\n",std::nullopt) == "resourcePacks:broken\n","malformed unrelated random options stay untouched");
    offline_error("invalid_options",[&] { patch_offline_skin_options("resourcePacks:broken\n",offline_skin_pack_id); });
    offline_error("invalid_options",[&] { patch_offline_skin_options("resourcePacks:[Nickel-offline-skin]\n",std::nullopt); });
    offline_error("invalid_options",[&] { patch_offline_skin_options(std::string(offline_skin_options_max_bytes+1,'x'),std::nullopt); });
    require(patch_offline_skin_options("gamma:1",offline_skin_pack_folder) == "gamma:1\nresourcePacks:[\"Nickel-offline-skin\"]\n","old clients use unprefixed identifier without rewriting other lines");
}
void test_pick_managed_copy_and_zero_io_cancel() {
    MemorySkinFiles files;
    FixtureSkinArchive archive;
    OfflineSkinService skins(files,archive);
    nickel::Cancellation cancelled; cancelled.cancel();
    require(!skins.pick(std::nullopt,fs::path("invalid-relative"),cancelled.stop_token()) && files.operations == 0 && archive.calls == 0,
            "cancelled native picker selection performs zero I/O including path validation");
    const auto selected = files.root/"selection.png";
    const auto source = image(64,32,{70,20,10,123});
    files.put(selected,encode_skin_png(source));
    const auto managed = files.root/"config"/"offline-skins";
    const auto copied = skins.pick(selected,managed);
    require(copied && copied->is_absolute() && copied->parent_path() == managed && copied->filename().string().starts_with("skin-") &&
            decode_skin_png(files.contents.at(*copied)) == source,"picker publishes a validated re-encoded managed PNG rather than returning the user path");
    files.contents[selected] = {1,2,3};
    require(decode_skin_png(files.contents.at(*copied)) == source,"managed copy survives source edits");
    const auto writes = files.writes;
    expect_error(IdentityErrorCode::unsupported_image,[&] { skins.pick(selected,managed); });
    require(files.writes == writes,"bad picker PNG does not create or truncate files");
    files.contents[selected] = encode_skin_png(source); files.no_space = true;
    offline_error("disk_full",[&] { skins.pick(selected,files.root/"new-managed"); });
    require(files.writes == writes && !files.directories.contains(files.root/"new-managed"),"insufficient picker space fails before directory creation");
}
void test_prepare_real_targets_and_random() {
    MemorySkinFiles files;
    FixtureSkinArchive archive;
    OfflineSkinService skins(files,archive);
    const auto request = custom_request(files);
    files.put(files.root/"options.txt",skin_bytes("gamma:0.75\nresourcePacks:[\"file/user.zip\"]\ncustom:keep\n"));
    const auto result = skins.prepare(request);
    require(result.enabled && result.options_changed && result.texture_count == 2 && result.pack_directory == files.pack(),"custom prepare returns actual target count and pack directory");
    const auto source = files.contents.at(*request.path);
    for (const auto& [name,value] : archive.contents) {
        (void)value;
        if (name.starts_with("assets/")) require(files.contents.at(files.pack()/name) == adapt_skin_png(source,false,name.ends_with("alex.png"),false),"all and only present JAR default textures use the correct arm UV model");
    }
    require(!files.contents.contains(files.pack()/"net/minecraft/CON.class"),"unrelated class is never copied or extracted by media service");
    const auto metadata = Json::parse(skin_text(files.contents.at(files.pack()/"pack.mcmeta")));
    require(metadata["pack"]["pack_format"] == 9,"client version.json resource pack format is authoritative");
    require(metadata["pack"]["description"] == "Palladium \u79bb\u7ebf\u76ae\u80a4" &&
            files.contents.at(files.pack()/".nickel-owner") == skin_bytes(offline_skin_owner_content),"Pd visible pack description preserves legacy owner identity");
    require(files.contents.at(files.pack()/".nickel-owner-token").size() == 33 && files.options_commits == 1,"managed pack has an owner token and conditionally committed options");
    const auto sentinel = files.pack()/"user.keep"; files.put(sentinel,{4,5,6});
    const auto packed = files.contents;
    const auto archive_calls = archive.calls;
    OfflineSkinRequest random;
    random.runtime_directory = files.root;
    random.mode = OfflineSkinMode::random;
    random.name = "not/a/player"; random.path = fs::path("invalid-relative");
    const auto cleared = skins.prepare(random);
    require(!cleared.enabled && cleared.options_changed && archive.calls == archive_calls,"random has no client JAR, version, UUID or network precondition");
    require(skin_text(files.contents.at(files.root/"options.txt")) == "gamma:0.75\nresourcePacks:[\"file/user.zip\"]\ncustom:keep\n","random removes enablement rather than replacing options wholesale");
    for (const auto& [path,value] : packed) if (path != files.root/"options.txt") require(files.contents.at(path) == value,"random keeps pack textures, source cache and user files");
    require(!skins.prepare(random).options_changed && files.options_commits == 2,"second random does not rewrite options");
}
void test_random_missing_and_unowned_pack() {
    for (const bool unowned : {false,true}) {
        MemorySkinFiles files;
        FixtureSkinArchive archive;
        if (unowned) files.put(files.pack()/"user.keep",{7,8,9});
        files.put(files.root/"options.txt",skin_bytes("resourcePacks:[\"Nickel-offline-skin\",\"file/other.zip\"]\nincompatibleResourcePacks:[\"file/Nickel-offline-skin\"]\n"));
        const auto before = files.contents;
        OfflineSkinService skins(files,archive);
        OfflineSkinRequest request; request.runtime_directory = files.root;
        const auto result = skins.prepare(request);
        require(!result.enabled && result.options_changed && archive.calls == 0,"stale offline enablement is disabled even when pack is missing or user-owned");
        require(skin_text(files.contents.at(files.root/"options.txt")) == "resourcePacks:[\"file/other.zip\"]\nincompatibleResourcePacks:[]\n","nonoffline random cleanup cannot silently succeed with our items retained");
        for (const auto& [path,value] : before) if (path != files.root/"options.txt") require(files.contents.at(path) == value,"random never claims or edits unowned pack files");
    }
}
void test_metadata_formats_and_legacy_boundaries() {
    for (const auto& version : std::array<Json,3>{Json(9),Json{{"resource",9}},Json{{"resource_major",69},{"resource_minor",1}}}) {
        MemorySkinFiles files;
        FixtureSkinArchive archive;
        auto request = custom_request(files);
        archive.contents["version.json"] = skin_bytes(Json{{"pack_version",version}}.dump());
        OfflineSkinService skins(files,archive);
        skins.prepare(request);
        const auto pack = Json::parse(skin_text(files.contents.at(files.pack()/"pack.mcmeta"))).at("pack");
        if (version.is_object() && version.contains("resource_major")) require(pack["min_format"] == Json::array({69,1}) && pack["max_format"] == pack["min_format"],"major/minor client metadata emits exact supported range");
        else require(pack["pack_format"] == 9,"scalar and resource metadata formats remain supported");
    }
    for (const auto& [version,format] : std::array<std::pair<std::string_view,unsigned>,8>{{{"1.6.4",1},{"1.7.10",1},{"1.8.9",1},{"1.10.2",2},{"1.12.2",3},{"1.16.1",5},{"1.16.5",6},{"1.18.2",8}}}) {
        MemorySkinFiles files;
        FixtureSkinArchive archive;
        archive.contents.erase("version.json"); archive.contents.erase("assets/minecraft/textures/entity/alex.png");
        auto request = custom_request(files); request.minecraft_version = std::string(version);
        OfflineSkinService skins(files,archive); skins.prepare(request);
        const auto metadata = Json::parse(skin_text(files.contents.at(files.pack()/"pack.mcmeta")));
        require(metadata["pack"]["pack_format"] == format,"known release fallback preserves version boundaries");
        const auto texture = decode_skin_png(files.contents.at(files.pack()/"assets/minecraft/textures/entity/steve.png"));
        require(texture.height == (version.starts_with("1.6") || version.starts_with("1.7") ? 32U : 64U),"legacy client uses real 64x32 UV output");
        const auto options = skin_text(files.contents.at(files.root/"options.txt"));
        require(options.find(version.starts_with("1.6") || version.starts_with("1.7") || version.starts_with("1.8") || version.starts_with("1.10") || version.starts_with("1.12") ?
                "[\"Nickel-offline-skin\"]" : "[\"file/Nickel-offline-skin\"]") != std::string::npos,"old and new options pack IDs differ at 1.13 boundary");
    }
}
void test_default_modes_and_errors_before_writes() {
    for (const auto mode : {OfflineSkinMode::steve,OfflineSkinMode::alex}) {
        MemorySkinFiles files;
        FixtureSkinArchive archive;
        OfflineSkinService skins(files,archive);
        auto request = custom_request(files); request.mode = mode;
        skins.prepare(request);
        const bool slim = mode == OfflineSkinMode::alex;
        const auto source = archive.contents.at(slim ? "assets/minecraft/textures/entity/alex.png" : "assets/minecraft/textures/entity/steve.png");
        require(files.contents.at(files.pack()/"assets/minecraft/textures/entity/steve.png") == adapt_skin_png(source,slim,false,false),"Steve/Alex mode consumes complete actual client texture, not built-in head asset");
    }
    const auto invalid_metadata = std::array<Json,5>{Json(-1),Json(0),Json{{"resource_major",69}},Json::object(),Json("9")};
    for (const auto& metadata : invalid_metadata) {
        MemorySkinFiles files; FixtureSkinArchive archive; auto request = custom_request(files);
        archive.contents["version.json"] = skin_bytes(Json{{"pack_version",metadata}}.dump());
        OfflineSkinService skins(files,archive);
        offline_error("unsupported_client",[&] { skins.prepare(request); });
        require(files.writes == 0,"invalid metadata publishes neither pack nor options");
    }
    MemorySkinFiles files; FixtureSkinArchive archive; auto request = custom_request(files);
    archive.contents.erase("version.json"); request.minecraft_version = "latest-snapshot";
    OfflineSkinService skins(files,archive);
    offline_error("unsupported_client",[&] { skins.prepare(request); });
    request.minecraft_version = "1.19.2";
    archive.extras.push_back({"assets/minecraft/textures/entity/steve.png",1,false});
    offline_error("archive",[&] { skins.prepare(request); });
    archive.extras.clear(); archive.contents.erase("assets/minecraft/textures/entity/steve.png"); archive.contents.erase("assets/minecraft/textures/entity/alex.png");
    offline_error("missing_texture",[&] { skins.prepare(request); });
    require(files.writes == 0,"unsupported, duplicated or missing skin assets never write");
}
void test_ownership_space_cancellation_and_latest_options() {
    MemorySkinFiles files; FixtureSkinArchive archive; auto request = custom_request(files);
    files.owned(false); files.put(files.pack()/"user.keep",{1,9,1});
    files.put(files.root/"options.txt",skin_bytes("gamma:0.25\nresourcePacks:[]\n"));
    OfflineSkinService skins(files,archive);
    archive.before_entries = [&] { files.contents[files.root/"options.txt"] = skin_bytes("gamma:0.85\nresourcePacks:[\"user-added\"]\nunknown:keep\n"); };
    skins.prepare(request);
    require(files.contents.at(files.pack()/".nickel-owner-token").size() == 33 && files.contents.at(files.pack()/"user.keep") == SkinBytes({1,9,1}),"v1 marker migrates to owner token without deleting user files");
    require(skin_text(files.contents.at(files.root/"options.txt")).starts_with("gamma:0.85\nresourcePacks:[\"user-added\","),"publication patches latest options rather than a network-time snapshot");
    const auto writes = files.writes;
    archive.before_entries = [&] { files.directories[files.pack()] = "same-path-replacement"; };
    offline_error("stale_owner",[&] { skins.prepare(request); });
    require(files.writes == writes,"same-path pack replacement retaining owner marker and token is rejected before publication");
    archive.before_entries = {}; files.no_space = true;
    offline_error("disk_full",[&] { skins.prepare(request); });
    require(files.writes == writes,"disk-space preflight rejects before any pack writes");
    files.no_space = false;
    nickel::Cancellation cancelled; cancelled.cancel();
    const auto operations = files.operations;
    expect_error(IdentityErrorCode::cancelled,[&] { skins.prepare(request,cancelled); });
    require(files.operations == operations,"pre-cancelled prepare performs zero file I/O");
    nickel::Cancellation during;
    archive.before_entries = [&] { during.cancel(); };
    expect_error(IdentityErrorCode::cancelled,[&] { skins.prepare(request,during); });
    require(files.writes == writes,"cancellation during client inspection writes nothing");
    archive.before_entries = {};
    files.before_replace = [&](const auto& path) { if (path.filename() == ".nickel-source.json") files.contents[files.root/"options.txt"] = skin_bytes("gamma:0.95\nresourcePacks:[\"external\"]\n"); };
    offline_error("changing_options",[&] { skins.prepare(request); });
    require(skin_text(files.contents.at(files.root/"options.txt")) == "gamma:0.95\nresourcePacks:[\"external\"]\n","detected concurrent settings change is never overwritten");
    MemorySkinFiles unowned; FixtureSkinArchive safe_archive; auto unowned_request = custom_request(unowned);
    unowned.put(unowned.pack()/"user.keep",{1,2});
    OfflineSkinService protected_skins(unowned,safe_archive);
    offline_error("ownership",[&] { protected_skins.prepare(unowned_request); });
    require(unowned.writes == 0 && safe_archive.calls == 0,"unowned pack blocks enablement before archive/network work");
}
void add_official(MockHttp& http, const SkinBytes& png, std::function<void()> before_texture = {}) {
    const std::string id(32,'b'), hash(64,'c');
    const auto property = Json{{"textures",{{"SKIN",{{"url","http://textures.minecraft.net/texture/"+hash},{"metadata",{{"model","slim"}}}}}}}}.dump();
    const auto encoded = nickel::accounts::detail::base64_encode({reinterpret_cast<const std::uint8_t*>(property.data()),property.size()});
    const auto check = [](const IdentityHttpRequest& request, std::size_t limit) {
        require(request.method == "GET" && !request.sensitive && request.body.empty() && request.max_response_bytes == limit,"offline official fetch is a bounded public GET, not authenticated skin upload");
        for (const auto& [key,value] : request.headers) { (void)value; require(key != "Authorization" && key != "Cookie","official skin lookup carries no credentials"); }
    };
    http.add("GET","https://api.mojang.com/users/profiles/minecraft/FixturePlayer",[id,check](const auto& request,auto) {
        check(request,64*1024); return json_response(200,Json{{"id",id}});
    });
    http.add("GET","https://sessionserver.mojang.com/session/minecraft/profile/"+id,[encoded,check](const auto& request,auto) {
        check(request,64*1024); return json_response(200,Json{{"properties",Json::array({{{"name","textures"},{"value",encoded}}})}});
    });
    http.add("GET","https://textures.minecraft.net/texture/"+hash,[png,check,before_texture](const auto& request,auto) {
        check(request,skin_png_max_bytes); if (before_texture) before_texture(); return IdentityHttpResponse{200,{},png};
    });
}
void test_official_public_lookup_cache_and_network_owner_race() {
    MemorySkinFiles files; FixtureSkinArchive archive; MockHttp http;
    auto request = custom_request(files); request.mode = OfflineSkinMode::official; request.name = "FixturePlayer";
    const auto png = encode_skin_png(image(64,64,{25,50,75,255}));
    add_official(http,png);
    OfflineSkinService skins(files,archive,&http);
    require(skins.prepare(request).enabled && http.calls == 3,"cold official offline mode uses legacy public lookup/session/texture flow");
    require(skins.prepare(request).enabled && http.calls == 3,"same-name official source cache supports repeated offline launch without network");
    http.verify();
    MemorySkinFiles raced; FixtureSkinArchive raced_archive; MockHttp race_http;
    auto race_request = custom_request(raced); race_request.mode = OfflineSkinMode::official; race_request.name = "FixturePlayer";
    raced.owned();
    add_official(race_http,png,[&] { raced.directories[raced.pack()] = "network-time-replacement"; });
    OfflineSkinService racing(raced,raced_archive,&race_http);
    offline_error("stale_owner",[&] { racing.prepare(race_request); });
    require(raced.writes == 0,"network-time same-path replacement cannot be reclaimed even with identical marker/token");
    race_http.verify();
}
void test_conditional_options_transactions() {
    {
        MemorySkinFiles files;
        const auto path = files.root/"options.txt";
        auto missing = files.observe_options(path,{});
        require(!missing->exists() && missing->bytes().empty(),"options missing observation is explicit");
        files.put(path,{});
        auto empty = files.observe_options(path,{});
        require(empty->exists() && empty->bytes().empty(),"existing empty options are not missing");
        offline_error("changing_options",[&] { missing->replace(skin_bytes("stale"),{}); });
        require(files.contents.at(path).empty() && files.options_commits == 0,"missing expectation cannot overwrite a new empty file");
        empty->replace(skin_bytes("gamma:1\n"),{});
        offline_error("changing_options",[&] { empty->replace(skin_bytes("stale"),{}); });
        require(skin_text(files.contents.at(path)) == "gamma:1\n" && files.options_commits == 1,"snapshot cannot be replayed after publication");
        auto unchanged = files.observe_options(path,{});
        files.put(path,files.contents.at(path));
        offline_error("changing_options",[&] { unchanged->replace(skin_bytes("lost"),{}); });
        require(files.options_commits == 1,"same bytes with replaced identity or revision fail closed");
    }
    for (const bool cleanup : {false,true}) {
        MemorySkinFiles files;
        FixtureSkinArchive archive;
        auto request = custom_request(files);
        if (cleanup) request.mode = OfflineSkinMode::random;
        const auto path = files.root/"options.txt";
        const auto original = skin_bytes(cleanup ? "resourcePacks:[\"file/Nickel-offline-skin\"]\n" : "resourcePacks:[]\n");
        const auto external = skin_bytes("gamma:0.95\nresourcePacks:[\"external\"]\n");
        files.put(path,original);
        files.before_replace = [&](const fs::path& target) { if (target == path) files.put(path,external); };
        OfflineSkinService skins(files,archive);
        offline_error("changing_options",[&] { skins.prepare(request); });
        require(files.contents.at(path) == external && files.options_commits == 0,"change at the publication boundary is preserved for enable and random cleanup");
    }
    {
        MemorySkinFiles files;
        FixtureSkinArchive archive;
        const auto request = custom_request(files);
        const auto path = files.root/"options.txt";
        const auto original = skin_bytes("resourcePacks:[\"file/Nickel-offline-skin\"]\n");
        files.put(path,original);
        files.before_replace = [&](const fs::path& target) { if (target.filename() == ".nickel-source.json") files.put(path,original); };
        OfflineSkinService skins(files,archive);
        offline_error("changing_options",[&] { skins.prepare(request); });
        require(files.contents.at(path) == original && files.options_commits == 0,"already-enabled settings still require the original identity and revision");
    }
    for (const bool cleanup : {false,true}) {
        MemorySkinFiles files;
        FixtureSkinArchive archive;
        auto request = custom_request(files);
        if (cleanup) request.mode = OfflineSkinMode::random;
        const auto path = files.root/"options.txt";
        const auto original = skin_bytes(cleanup ? "resourcePacks:[\"file/Nickel-offline-skin\"]\n" : "resourcePacks:[]\n");
        files.put(path,original);
        nickel::Cancellation cancellation;
        files.before_replace = [&](const fs::path& target) { if (target == path) cancellation.cancel(); };
        OfflineSkinService skins(files,archive);
        expect_error(IdentityErrorCode::cancelled,[&] { skins.prepare(request,cancellation); });
        require(files.contents.at(path) == original && files.options_commits == 0,"prepublication cancellation preserves options in both modes");
    }
    {
        MemorySkinFiles files;
        FixtureSkinArchive archive;
        const auto path = files.root/"options.txt";
        unsigned recovered{};
        files.before_observe = [&] {
            ++recovered;
            files.put(path,skin_bytes("resourcePacks:[\"file/Nickel-offline-skin\",\"user\"]\n"));
        };
        nickel::Cancellation cancellation;
        files.after_options_commit = [&] { cancellation.cancel(); };
        OfflineSkinService skins(files,archive);
        OfflineSkinRequest request; request.runtime_directory = files.root;
        const auto result = skins.prepare(request,cancellation);
        require(result.options_changed && recovered == 1 && archive.calls == 0 &&
                skin_text(files.contents.at(path)) == "resourcePacks:[\"user\"]\n","recovery precedes missing-file cleanup and committed publication is not cancelled");
    }
    {
        MemorySkinFiles files;
        FixtureSkinArchive archive;
        files.before_observe = [] { fixture_error("recovery_required"); };
        OfflineSkinService skins(files,archive);
        OfflineSkinRequest request; request.runtime_directory = files.root;
        offline_error("recovery_required",[&] { skins.prepare(request); });
        require(files.writes == 0 && archive.calls == 0,"incomplete recovery cannot allow a game preparation result");
    }
}
}
void run_offline_skin_tests() {
    test_options_exact_preservation();
    test_pick_managed_copy_and_zero_io_cancel();
    test_prepare_real_targets_and_random();
    test_random_missing_and_unowned_pack();
    test_metadata_formats_and_legacy_boundaries();
    test_default_modes_and_errors_before_writes();
    test_ownership_space_cancellation_and_latest_options();
    test_official_public_lookup_cache_and_network_owner_race();
    test_conditional_options_transactions();
    std::cout << "offline skin: managed pick, target/UV metadata, recoverable conditional options, Pd description, ownership, cancellation and public cache passed\n";
}
} // namespace identity_test
