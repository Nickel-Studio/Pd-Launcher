#include "mock_http.hpp"
#include "../../core/media/offline_skin_native.hpp"
#include "core/archive/archive.hpp"
#include "core/config/atomic_file.hpp"
#include "core/tasks/secure_fs.hpp"
#include "core/config/path_guard.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string_view>

namespace {
using namespace identity_test;
using namespace nickel::media;
namespace fs = std::filesystem;
namespace sd = nickel::tasks::detail;
SkinBytes skin_bytes(std::string_view value) { return {value.begin(),value.end()}; }
std::string skin_text(const SkinBytes& value) { return {value.begin(),value.end()}; }
template<class Operation>
void rejected(Operation operation, const char* message) {
    try { operation(); }
    catch (const nickel::ErrorException&) { return; }
    throw std::runtime_error(message);
}
template<class Operation>
void offline_error(std::string_view code, Operation operation) {
    try { operation(); }
    catch (const nickel::ErrorException& error) {
        require(error.error().code == "offline_skin_"+std::string(code),"unexpected native offline error code");
        return;
    }
    throw std::runtime_error("expected native offline skin error");
}
class NativeFixture final {
public:
    explicit NativeFixture(const fs::path& parent) {
        require(parent.is_absolute(),"native offline fixture parent must be absolute");
        auto existing = parent;
        while (!fs::exists(existing)) existing = existing.parent_path();
        std::error_code error;
        const auto capacity = fs::space(existing,error);
        require(!error && capacity.available >= 64ULL*1024*1024,"native offline fixtures require 64 MiB free before writing");
        sd::ParentGuard parent_guard(parent,true);
        root_ = parent/("offline-native-"+nickel::accounts::detail::random_hex(8));
        sd::create_directory_new(root_);
        identity_ = sd::directory_identity(root_);
    }
    ~NativeFixture() {
        try { sd::remove_owned_tree(root_,identity_); } catch (...) {}
    }
    const fs::path& root() const noexcept { return root_; }
private:
    fs::path root_;
    sd::FileIdentity identity_;
};
void test_native_file_bounds_and_exclusive_create(const fs::path& root) {
    NativeSkinFileStore files;
    files.ensure_directory(root/"files");
    const auto source = root/"files"/"data.bin";
    files.create_file(source,SkinBytes{1,2,3,4});
    require(files.read(source,4) == std::optional<SkinBytes>{{1,2,3,4}},"native bounded regular-file read");
    rejected([&] { files.create_file(source,SkinBytes{9}); },"exclusive create replaced a user file");
    rejected([&] { files.read(source,3); },"native read exceeded its bound");
    require(files.read(source,4) == std::optional<SkinBytes>{{1,2,3,4}},"failed native reads and creates preserve original bytes");
    files.replace_file(source,SkinBytes{5,6});
    require(files.read(source,2) == std::optional<SkinBytes>{{5,6}},"native atomic replacement publishes complete data");
    require(!files.read(root/"files"/"missing.bin",1024),"missing native file is distinct from empty bytes");
    rejected([&] { files.read(root/"files",1024); },"directory accepted as selected skin file");
    rejected([&] { files.directory(source); },"regular file accepted as a runtime directory");
    rejected([&] { files.create_directory(root/"files"); },"exclusive directory creation reused an existing path");
    const auto missing = root/"capacity-failure"/"new.bin";
    rejected([&] { files.check_write_capacity(missing,std::numeric_limits<std::uint64_t>::max()); },"impossible native capacity was accepted");
    require(!fs::exists(missing.parent_path()) && files.read(source,2) == std::optional<SkinBytes>{{5,6}},"capacity rejection occurs before filesystem publication");
}
void test_native_lock_cancellation_and_directory_pin(const fs::path& root) {
    NativeSkinFileStore files;
    const auto runtime = root/"lock-runtime";
    files.ensure_directory(runtime);
    auto first = files.lock(runtime,{});
    nickel::Cancellation cancellation;
    auto waiting = std::async(std::launch::async,[&] {
        try { auto second = files.lock(runtime,cancellation.stop_token()); return std::string("acquired"); }
        catch (const nickel::ErrorException& error) { return error.error().code+" "+error.error().details.dump(); }
    });
    const auto blocked = waiting.wait_for(std::chrono::milliseconds(30));
    cancellation.cancel();
    const auto outcome = waiting.get();
    first.reset();
    if (blocked != std::future_status::timeout || !outcome.starts_with("cancelled "))
        std::cerr << "controlled lock outcome: blocked=" << (blocked == std::future_status::timeout) << " result=" << outcome << '\n';
    require(blocked == std::future_status::timeout && outcome.starts_with("cancelled "),"actual same-runtime lock queues and canonical cancellation wakes it");
    auto next = files.lock(runtime,{}); next.reset();
    const auto directory = root/"pinned";
    files.ensure_directory(directory);
    const auto identity = files.directory_identity(directory);
    {
        auto pin = files.pin_directory(directory);
#ifdef _WIN32
        std::error_code error;
        fs::rename(directory,root/"pinned-moved",error);
        require(bool(error) && files.directory_identity(directory) == identity,"Windows directory pin prevents same-path replacement while publishing");
#endif
    }
    require(files.directory_identity(directory) == identity,"native directory identity is stable after pin release");
}
struct JarFixture {
    fs::path jar;
    SkinBytes steve;
    SkinBytes alex;
};
JarFixture create_client_jar(NativeSkinFileStore& files, const fs::path& root) {
    const auto sources = root/"jar-sources";
    files.ensure_directory(sources);
    const auto steve = encode_skin_png(image(64,64,{80,40,20,255}));
    const auto alex = encode_skin_png(image(64,64,{20,40,80,255}));
    files.create_file(sources/"steve.png",steve);
    files.create_file(sources/"alex.png",alex);
    files.create_file(sources/"version.json",skin_bytes(Json{{"pack_version",{{"resource_major",69},{"resource_minor",1}}}}.dump()));
    files.create_file(sources/"Main.class",SkinBytes{0xca,0xfe,0xba,0xbe,0,0,0,0x34});
    const auto jar = root/"vanilla-client.jar";
    const std::vector<nickel::archive::ZipSource> entries{
        {"assets/minecraft/textures/entity/player/wide/steve.png",sources/"steve.png"},
        {"assets/minecraft/textures/entity/player/slim/alex.png",sources/"alex.png"},
        {"version.json",sources/"version.json"},
        {"example/ABC.class",sources/"Main.class"}
    };
    nickel::archive::Limits limits;
    limits.max_entry_bytes = skin_png_max_bytes;
    limits.max_total_bytes = 4*skin_png_max_bytes;
    limits.max_archive_bytes = 4*skin_png_max_bytes;
    const nickel::Cancellation cancellation;
    nickel::archive::Archive::write_zip(jar,entries,limits,cancellation);
    // The canonical writer intentionally rejects disk-reserved names. Change only
    // equal-length fixture name bytes, preserving all ZIP sizes, offsets and CRCs.
    auto bytes = *files.read(jar,4*skin_png_max_bytes);
    constexpr std::string_view safe = "example/ABC.class", member = "example/CON.class";
    static_assert(safe.size() == member.size());
    auto cursor = bytes.begin();
    unsigned renamed = 0;
    while ((cursor = std::search(cursor,bytes.end(),safe.begin(),safe.end())) != bytes.end()) {
        std::copy(member.begin(),member.end(),cursor);
        cursor += member.size();
        ++renamed;
    }
    require(renamed == 2,"fixture changes exactly the local and central member names");
    files.replace_file(jar,bytes);
    return {jar,steve,alex};
}
void test_native_pick_prepare_random(const fs::path& root) {
    NativeSkinFileStore files;
    NativeClientSkinArchive archive;
    const auto jar = create_client_jar(files,root);
    const auto entries = archive.entries(jar.jar,{});
    require(entries.size() == 4 && archive.read(jar.jar,"version.json",64*1024,{}) == *files.read(root/"jar-sources"/"version.json",64*1024),"actual canonical JAR inspection and exact bounded read");
    require(std::any_of(entries.begin(),entries.end(),[](const SkinArchiveEntry& entry) { return entry.name == "example/CON.class"; }) &&
            archive.read(jar.jar,"example/CON.class",64,{}) == *files.read(root/"jar-sources"/"Main.class",64),
            "read-only JAR members allow a legitimate obfuscated class without resolving a disk output");
    const nickel::Cancellation cancellation;
    rejected([&] { nickel::archive::Archive::inspect(jar.jar,{},cancellation); },"disk extraction policy accepted a reserved output name");
    rejected([&] { archive.read(jar.jar,"version.json",1,{}); },"native client JAR read ignored selected-entry bound");
    rejected([&] { archive.read(jar.jar,"VERSION.JSON",64*1024,{}); },"native member read ignored exact case");
    rejected([&] { archive.read(jar.jar,"example/../version.json",64*1024,{}); },"native member read accepted traversal");
    const auto runtime = root/"game-runtime";
    const auto pack = runtime/"resourcepacks"/std::string(offline_skin_pack_folder);
    files.ensure_directory(pack);
    files.create_file(pack/".nickel-owner",skin_bytes(offline_skin_owner_content));
    files.create_file(pack/"user.keep",SkinBytes{7,8,9});
    const auto original = "\xEF\xBB\xBFgamma:0.5\r\nresourcePacks:[\"file/user.zip\"]\r\nunknown:keep\r\n";
    files.create_file(runtime/"options.txt",skin_bytes(original));
    const auto selected = root/"selected.png";
    const auto source = image(64,32,{125,55,25,171});
    files.create_file(selected,encode_skin_png(source));
    OfflineSkinService skins(files,archive);
    const auto copied = skins.pick(selected,root/"config"/"offline-skins");
    require(copied && copied->is_absolute() && *copied != selected && decode_skin_png(*files.read(*copied,skin_png_max_bytes)) == source,"actual native picker stores a complete validated managed PNG copy");
    OfflineSkinRequest request;
    request.runtime_directory = runtime;
    request.client_jar = jar.jar;
    request.minecraft_version = "1.21.9";
    request.mode = OfflineSkinMode::custom;
    request.path = copied;
    const auto installed = skins.prepare(request);
    require(installed.enabled && installed.options_changed && installed.texture_count == 2,"actual filesystem prepare enables only two present client default textures");
    const auto source_png = *files.read(*copied,skin_png_max_bytes);
    require(*files.read(pack/"assets/minecraft/textures/entity/player/wide/steve.png",skin_png_max_bytes) == adapt_skin_png(source_png,false,false,false),"actual wide client target contains full UV-converted PNG");
    require(*files.read(pack/"assets/minecraft/textures/entity/player/slim/alex.png",skin_png_max_bytes) == adapt_skin_png(source_png,false,true,false),"actual slim client target contains full UV-converted PNG");
    require(!fs::exists(pack/"example"),"native offline service never extracts unrelated JAR contents");
    const auto metadata = Json::parse(skin_text(*files.read(pack/"pack.mcmeta",4096)));
    require(metadata["pack"]["min_format"] == Json::array({69,1}),"actual client pack major/minor metadata copied into resource-pack range");
    require(files.read(pack/".nickel-owner-token",4096)->size() == 33,"actual legacy owner marker receives exclusive 128-bit token");
    request.mode = OfflineSkinMode::random;
    request.client_jar.clear(); request.minecraft_version.clear(); request.path.reset();
    const auto cleared = skins.prepare(request);
    require(!cleared.enabled && cleared.options_changed && skin_text(*files.read(runtime/"options.txt",4096)) == original,"actual random restores only enablement list, preserving all unrelated option bytes");
    require(*files.read(pack/"user.keep",3) == SkinBytes({7,8,9}) && fs::exists(pack/"pack.mcmeta"),"actual random preserves owned pack cache and user files");
    require(!skins.prepare(request).options_changed,"native repeated random performs no option replacement");
}
void test_native_missing_pack_cleanup(const fs::path& root) {
    NativeSkinFileStore files;
    NativeClientSkinArchive archive;
    const auto runtime = root/"missing-pack-runtime";
    files.ensure_directory(runtime);
    files.create_file(runtime/"options.txt",skin_bytes("resourcePacks:[\"file/Nickel-offline-skin\",\"user\"]\nincompatibleResourcePacks:[\"Nickel-offline-skin\"]\n"));
    OfflineSkinService skins(files,archive);
    OfflineSkinRequest request; request.runtime_directory = runtime;
    require(skins.prepare(request).options_changed,"actual nonoffline cleanup does not silently skip a missing pack");
    require(skin_text(*files.read(runtime/"options.txt",4096)) == "resourcePacks:[\"user\"]\nincompatibleResourcePacks:[]\n" && !fs::exists(runtime/"resourcepacks"),"missing-pack hardening deletes only exact enablement identifiers without claiming a pack directory");
}
void test_native_conditional_options(const fs::path& root) {
    NativeSkinFileStore files;
    const auto runtime = root/"conditional-options";
    files.ensure_directory(runtime);
    const auto target = runtime/"options.txt";
    auto pin = files.pin_directory(runtime);
    auto lock = files.lock(runtime,{});
    auto missing = files.observe_options(target,{});
    require(!missing->exists() && missing->bytes().empty(),"native options snapshot distinguishes missing");
    files.create_file(target,{});
    auto empty = files.observe_options(target,{});
    require(empty->exists() && empty->bytes().empty(),"native options snapshot distinguishes existing empty");
    offline_error("changing_options",[&] { missing->replace(skin_bytes("stale"),{}); });
    require(files.read(target,32)->empty(),"native missing expectation preserves a concurrent empty occupant");
    const auto first = skin_bytes("gamma:0.5\nresourcePacks:[]\n");
    empty->replace(first,{});
    require(files.read(target,128) == std::optional<SkinBytes>{first},"native conditional replacement works under real runtime pin and lock");
    auto stale = files.observe_options(target,{});
    const auto retained = runtime/"retained-options.txt";
    fs::rename(nickel::foundation_detail::native_filesystem_path(target),nickel::foundation_detail::native_filesystem_path(retained));
    files.create_file(target,first);
    offline_error("changing_options",[&] { stale->replace(skin_bytes("identity-lost"),{}); });
    require(files.read(target,128) == std::optional<SkinBytes>{first} && files.read(retained,128) == std::optional<SkinBytes>{first},
            "native identical bytes on a replaced file identity cannot overwrite either object");
    auto revision = files.observe_options(target,{});
    const auto native_target = nickel::foundation_detail::native_filesystem_path(target);
    fs::last_write_time(native_target,fs::last_write_time(native_target)+std::chrono::seconds(2));
    offline_error("changing_options",[&] { revision->replace(skin_bytes("revision-lost"),{}); });
    auto fresh = files.observe_options(target,{});
    nickel::Cancellation cancelled; cancelled.cancel();
    expect_error(nickel::accounts::IdentityErrorCode::cancelled,[&] { fresh->replace(skin_bytes("cancelled"),cancelled.stop_token()); });
    expect_error(nickel::accounts::IdentityErrorCode::cancelled,[&] { files.observe_options(target,cancelled.stop_token()); });
    rejected([&] { fresh->replace(SkinBytes(offline_skin_options_max_bytes+1,'x'),{}); },"native conditional output exceeded two-MiB quota");
    require(files.read(target,128) == std::optional<SkinBytes>{first},"revision, cancellation and quota failures preserve native options");
    for (const auto& entry : fs::directory_iterator(nickel::foundation_detail::native_filesystem_path(runtime))) {
        require(!nickel::path_to_utf8(entry.path().filename()).starts_with(".nickel-conditional-"),"successful native conditional options retain no receipt, backup or stage");
    }
    lock.reset(); pin.reset();
    const auto blocked_runtime = root/"conditional-recovery-blocked";
    files.ensure_directory(blocked_runtime);
    const auto receipt = blocked_runtime/(".nickel-conditional-"+std::string(32,'a')+".json");
    files.create_file(receipt,skin_bytes("{\"invalid\":true}"));
    NativeClientSkinArchive archive;
    OfflineSkinService skins(files,archive);
    OfflineSkinRequest request; request.runtime_directory = blocked_runtime;
    try {
        (void)skins.prepare(request);
        throw std::runtime_error("native cleanup allowed launch with an unresolved conditional recovery receipt");
    } catch (const nickel::ErrorException& error) {
        require(error.error().code == "RecoveryRequired", "native pending recovery fails closed instead of treating missing options as unchanged");
    }
    require(files.read(receipt,128) == std::optional<SkinBytes>{skin_bytes("{\"invalid\":true}")} &&
            !fs::exists(blocked_runtime/"options.txt") && !fs::exists(blocked_runtime/"resourcepacks"),
            "native failed recovery preserves the receipt and user namespace without pack publication");
}
}
int main(int argc, char** argv) {
    const char* stage = "arguments";
    try {
        require(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--options-only"),
                "native offline fixture parent and optional --options-only are required");
        NativeFixture fixture{fs::path(argv[1])};
        if (argc == 3) {
            stage = "conditional-options-recovery";
            test_native_conditional_options(fixture.root());
            std::cout << "native offline options: actual conditional snapshot, missing/empty, identity/revision conflicts, runtime pin/lock, cancellation, quotas and recovery gate passed\n";
            return 0;
        }
        stage = "file-bounds"; test_native_file_bounds_and_exclusive_create(fixture.root());
        stage = "locks-and-pins"; test_native_lock_cancellation_and_directory_pin(fixture.root());
        stage = "client-jar-and-pack"; test_native_pick_prepare_random(fixture.root());
        stage = "missing-pack-random"; test_native_missing_pack_cleanup(fixture.root());
        stage = "conditional-options-recovery"; test_native_conditional_options(fixture.root());
        std::cout << "native offline skin: canonical filesystem/archive, managed pick, UV pack/options, conditional recovery, owner token, capacity and cancellation passed\n";
        return 0;
    } catch (const nickel::ErrorException& error) {
        std::cerr << "native offline failure at " << stage << ": " << error.error().code << " " << error.what() << '\n'
                  << "synthetic fixture details: " << error.error().details.dump() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "native offline failure at " << stage << ": " << error.what() << '\n';
        return 1;
    }
}
