#include "core/instances/icons.hpp"
#include "core/instances/domain_io.hpp"
#include "core/instances/settings.hpp"
#include "core/config/detail.hpp"
#include "core/media/images.hpp"
#include "core/media/skin_png.hpp"
#include "core/packs/internal.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace nickel;
namespace io = instances::detail;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

void require(bool value, std::string_view reason) {
    if (!value) throw std::runtime_error(std::string(reason));
}
template<class Function> void rejected(Function&& function, std::string_view expected = {}) {
    try { function(); }
    catch (const ErrorException& error) {
        if (expected.empty() || error.error().code == expected) return;
        throw std::runtime_error("Expected " + std::string(expected) + ", got " + error.error().code);
    }
    throw std::runtime_error("Unsafe instance icon operation was accepted");
}
std::uint32_t be32(std::span<const std::uint8_t> data) {
    return (std::uint32_t(data[0]) << 24) | (std::uint32_t(data[1]) << 16) | (std::uint32_t(data[2]) << 8) | data[3];
}
void append32(Bytes& bytes, std::uint32_t value) {
    for (const auto shift : {24U, 16U, 8U, 0U}) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void write(const fs::path& path, std::span<const std::uint8_t> bytes) {
    io::create_directories(path.parent_path());
    io::write_exclusive(path, {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}
std::string digest(std::span<const std::uint8_t> bytes) {
    foundation_detail::Sha256 sha; sha.update(bytes); return sha.finish();
}
Bytes source_image(std::size_t exact_size = 0, std::uint8_t tint = 0) {
    media::RgbaImage image{64, 32, Bytes(64 * 32 * 4)};
    for (std::uint32_t y = 0; y < image.height; ++y) for (std::uint32_t x = 0; x < image.width; ++x) {
        const auto at = (y * image.width + x) * 4;
        image.pixels[at] = static_cast<std::uint8_t>(x + tint);
        image.pixels[at + 1] = static_cast<std::uint8_t>(y);
        image.pixels[at + 2] = 71;
        image.pixels[at + 3] = 255;
    }
    auto encoded = media::encode_skin_png(image);
    if (!exact_size) return encoded;
    require(exact_size > encoded.size() + 12, "Exact source fixture is too small");
    const auto payload = exact_size - encoded.size() - 12;
    Bytes chunk; chunk.reserve(payload + 12); append32(chunk, static_cast<std::uint32_t>(payload));
    chunk.insert(chunk.end(), {'t', 'E', 'X', 't'}); chunk.insert(chunk.end(), payload, 'x');
    append32(chunk, static_cast<std::uint32_t>(crc32(0, chunk.data() + 4, static_cast<uInt>(payload + 4))));
    encoded.insert(encoded.begin() + 33, chunk.begin(), chunk.end());
    require(encoded.size() == exact_size, "Exact source fixture size changed");
    return encoded;
}

// A controlled implementation of the existing decoder interface, never a production
// fallback. It decodes a real PNG after omitting ancillary text, and counts the one call.
class Decoder final : public media::ImageDecoder {
public:
    unsigned probes = 0, decodes = 0;
    std::string expected_sha;
    std::size_t seen = 0;
    std::function<void()> during_decode;
    media::ImageProbe probe(std::span<const std::uint8_t> bytes) override {
        ++probes; seen = bytes.size();
        require(bytes.size() >= 33 && be32(bytes.subspan(16, 4)) == 64 && be32(bytes.subspan(20, 4)) == 32,
            "Controlled decoder did not receive the complete original PNG");
        if (!expected_sha.empty()) require(digest(bytes) == expected_sha, "Domain changed or shortened selected source bytes");
        return {64, 32, "image/png", 1, 64 * 32 * 4};
    }
    media::RgbaImage decode(std::span<const std::uint8_t> bytes, const media::ImageProbe&,
                            const media::ImageLimits&, std::stop_token) override {
        ++decodes;
        if (during_decode) during_decode();
        Bytes compact(bytes.begin(), bytes.begin() + 8);
        for (std::size_t at = 8; at < bytes.size();) {
            require(bytes.size() - at >= 12, "Controlled source PNG is incomplete");
            const auto size = be32(bytes.subspan(at, 4));
            require(size <= bytes.size() - at - 12, "Controlled source PNG chunk exceeds input");
            const std::string_view type(reinterpret_cast<const char*>(bytes.data() + at + 4), 4);
            if (type != "tEXt") compact.insert(compact.end(), bytes.begin() + at, bytes.begin() + at + size + 12);
            at += size + 12;
        }
        return media::decode_skin_png(compact);
    }
};
class NoHttp final : public accounts::IdentityHttp {
public:
    accounts::IdentityHttpResponse perform(const accounts::IdentityHttpRequest&, std::stop_token) override {
        throw std::runtime_error("Selected instance icon unexpectedly used HTTP");
    }
};
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("pd-instance-icons-" + io::random_id());
    fs::path game = root / "game", instance = game / "versions/Pack", source = root / "selected.png";
    std::string descriptor = "{\"id\":\"Pack\",\"nickelCustom\":{\"name\":\"User Name\",\"future\":true},\"untouched\":7}";
    Fixture() {
        io::create_directories(instance);
        io::write_exclusive(instance / "Pack.json", descriptor);
        io::write_exclusive(instance / instances::settings_file, "{\"icon\":\"grass\",\"memoryMb\":4096,\"unknown\":{\"kept\":true}}");
    }
    ~Fixture() { try { if (io::path_exists(root)) io::remove_owned_tree(root); } catch (...) {} }
    void input(std::size_t size = 0, std::uint8_t tint = 0) { const auto bytes = source_image(size, tint); write(source, bytes); }
};
void leases_released(const media::ImageService& images) {
    const auto stats = images.cache_stats();
    require(!stats.active_decodes && !stats.active_bytes && !stats.active_requests && !stats.entries,
        "Instance icon retained a decoder/cache/network lease");
}
void check_png(std::span<const std::uint8_t> bytes, std::uint8_t tint = 0) {
    require(bytes.size() <= instances::managed_icon_max_bytes && be32(bytes.subspan(16, 4)) == 32
        && be32(bytes.subspan(20, 4)) == 32, "Managed icon is not a bounded 32x32 image");
    const auto length = be32(bytes.subspan(33, 4));
    require(std::string_view(reinterpret_cast<const char*>(bytes.data() + 37), 4) == "IDAT", "Managed icon retained source metadata");
    std::array<std::uint8_t, 32 * (32 * 4 + 1)> raw{};
    uLongf output = raw.size(); uLong input = length;
    require(uncompress2(raw.data(), &output, bytes.data() + 41, &input) == Z_OK
        && output == raw.size() && input == length, "Managed PNG did not consume its exact zlib scanlines");
    for (std::size_t y = 0; y < 32; ++y) {
        const auto at = y * 129;
        require(raw[at] == 0, "Managed icon encoder changed its sanitized scanline format");
        for (std::size_t x = 0; x < 32; ++x) {
            const auto pixel = at + 1 + x * 4;
            require(raw[pixel] == static_cast<std::uint8_t>(2 * x + 1 + tint) && raw[pixel + 1] == y
                && raw[pixel + 2] == 71 && raw[pixel + 3] == 255,
                "Instance icon cropped the source instead of resizing its complete rectangle");
        }
    }
}
void roundtrip() {
    Fixture f; f.input(); NoHttp http; Decoder decoder; decoder.expected_sha = digest(source_image());
    media::ImageService images(http, decoder);
    const auto ref = instances::set_instance_icon(f.game, "Pack", f.source, images);
    constexpr std::string_view prefix = ".nickel-icons/";
    require(ref.starts_with(prefix) && ref.size() == prefix.size() + 64 + 4,
        "Managed reference is not a relative content-addressed token");
    const auto icon = instances::read_instance_icon(f.game, "Pack");
    require(icon && icon->reference == ref && digest(icon->png) == ref.substr(prefix.size(), 64),
        "Approved icon read lost its digest binding");
    check_png(icon->png);
    const auto settings = instances::get_version_settings(f.game, "Pack");
    require(settings["icon"] == ref && settings["memoryMb"] == 4096 && settings["unknown"]["kept"] == true,
        "Selected icon replaced unrelated local settings");
    require(io::read_bounded(f.instance / "Pack.json", 4096) == f.descriptor, "Selected icon changed instance identity/display descriptor");
    const auto target = f.instance / instances::path_from_utf8(ref);
    const auto before = fs::last_write_time(io::native_path(target));
    require(instances::set_instance_icon(f.game, "Pack", f.source, images) == ref
        && fs::last_write_time(io::native_path(target)) == before, "Same sanitized icon overwrote its managed file");
    require(decoder.probes == 2 && decoder.decodes == 2, "Domain duplicated image decoding");
    instances::patch_instance_metadata(f.game, "Pack", {{"icon", "auto"}});
    require(!instances::read_instance_icon(f.game, "Pack"), "Automatic built-in icon read a managed image");
    rejected([&] { instances::patch_instance_metadata(f.game, "Pack", {{"icon", ref}}); }, "invalid_icon");
    leases_released(images);
    std::cout << "Pd managed 32x32 PNG/one probe and decode/full rectangle/unknown settings/no-overwrite/read-only token: PASS\n";
}
void boundaries() {
    Fixture f; f.input(instances::selected_icon_max_bytes); NoHttp http; Decoder decoder;
    decoder.expected_sha = digest(source_image(instances::selected_icon_max_bytes));
    media::ImageService images(http, decoder);
    const auto ref = instances::set_instance_icon(f.game, "Pack", f.source, images);
    require(!ref.empty() && decoder.seen == instances::selected_icon_max_bytes && decoder.decodes == 1,
        "Domain did not consume the complete exact 4 MiB selection");
    const auto settings = io::read_bounded(f.instance / instances::settings_file, instances::settings_max_bytes);
    const auto too_big = f.root / "too-large.png"; write(too_big, source_image(instances::selected_icon_max_bytes + 1));
    rejected([&] { instances::set_instance_icon(f.game, "Pack", too_big, images); }, "limit");
    rejected([&] { instances::set_instance_icon(f.game, "Pack", "relative.png", images); }, "unsafe_path");
    rejected([&] { instances::set_instance_icon(f.game, "Pack", f.root, images); });
    require(decoder.probes == 1 && decoder.decodes == 1
        && io::read_bounded(f.instance / instances::settings_file, instances::settings_max_bytes) == settings,
        "Rejected selection allocated a decoder or changed metadata");
    leases_released(images);
    std::cout << "actual PNG full 4 MiB selection/4 MiB+1 pre-decode rejection/relative and special source rejection: PASS\n";
}
void cancellation() {
    Fixture f; f.input(); NoHttp http; Decoder decoder; media::ImageService images(http, decoder);
    const auto before = io::read_bounded(f.instance / instances::settings_file, instances::settings_max_bytes);
    std::stop_source stopped; stopped.request_stop();
    rejected([&] { instances::set_instance_icon(f.game, "Pack", f.source, images, stopped.get_token()); }, "cancelled");
    require(!decoder.probes && !decoder.decodes, "Pre-cancelled selection reached media decoding");
    std::stop_source during;
    decoder.during_decode = [&] { during.request_stop(); };
    rejected([&] { instances::set_instance_icon(f.game, "Pack", f.source, images, during.get_token()); }, "cancelled");
    require(io::read_bounded(f.instance / instances::settings_file, instances::settings_max_bytes) == before
        && !io::path_exists(f.instance / ".nickel-icons"), "Cancelled selection published file or settings");
    auto transaction = instances::InstanceTransaction::reserve(f.game, "Pending", "1.20.1");
    rejected([&] { instances::set_instance_icon(f.game, "Pending", f.source, images); }, "pending_instance");
    require(decoder.decodes == 1, "Pending instance reached image decoding");
    leases_released(images);
    std::cout << "pre/during decode cancellation/pending instance/no-publication/media lease cleanup: PASS\n";
}
void security() {
    Fixture f; f.input(); NoHttp http; Decoder decoder; media::ImageService images(http, decoder);
    const auto alias = f.root / "hardlink.png";
    fs::create_hard_link(io::native_path(f.source), io::native_path(alias));
    rejected([&] { instances::set_instance_icon(f.game, "Pack", f.source, images); }, "unsafe_path");
    fs::remove(io::native_path(alias));
    require(!decoder.probes, "Multi-hardlink selected source reached decoding");
#ifdef _WIN32
    struct Handle { HANDLE value; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
    {
        Handle writer{CreateFileW(io::native_path(f.source).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(writer.value != INVALID_HANDLE_VALUE, "Cannot create actual open-writer selection fixture");
        rejected([&] { instances::set_instance_icon(f.game, "Pack", f.source, images); });
    }
    decoder.during_decode = [&] {
        const auto handle = CreateFileW(io::native_path(f.source).c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "Pinned source allowed a concurrent writer");
        require(!MoveFileExW(io::native_path(f.source).c_str(), io::native_path(f.root / "renamed.png").c_str(), 0),
            "Pinned selected source allowed pathname substitution");
        require(!MoveFileExW(io::native_path(f.instance).c_str(), io::native_path(f.game / "versions/Replaced").c_str(), 0),
            "Pinned instance directory allowed target substitution");
    };
    (void)instances::set_instance_icon(f.game, "Pack", f.source, images);
#else
    decoder.during_decode = [&] { io::atomic_write(f.source, "changed after read"); };
    rejected([&] { instances::set_instance_icon(f.game, "Pack", f.source, images); }, "source_changed");
#endif
    auto value = instances::get_version_settings(f.game, "Pack"); value["icon"] = instances::path_to_utf8(f.source);
    instances::set_version_settings(f.game, "Pack", value);
    require(!instances::read_instance_icon(f.game, "Pack"), "Legacy external icon reference was treated as source authorization");
    value["icon"] = ".nickel-icons/../selected.png"; instances::set_version_settings(f.game, "Pack", value);
    rejected([&] { instances::read_instance_icon(f.game, "Pack"); }, "invalid_icon");
    leases_released(images);
    std::cout << "real hardlink/writer/source and instance pinning/malformed token/no external read authorization: PASS\n";
}
void publication_failure() {
    Fixture f; f.input(); NoHttp http; Decoder decoder; media::ImageService images(http, decoder);
    const auto ref = instances::set_instance_icon(f.game, "Pack", f.source, images);
    const auto icon = *instances::read_instance_icon(f.game, "Pack");
    const auto path = f.instance / instances::path_from_utf8(ref);
    io::atomic_write(path, "PREEXISTING-DIFFERENT-BYTES");
    const auto before = io::read_bounded(f.instance / instances::settings_file, instances::settings_max_bytes);
    rejected([&] { instances::set_instance_icon(f.game, "Pack", f.source, images); }, "conflict");
    require(io::read_bounded(path, 4096) == "PREEXISTING-DIFFERENT-BYTES"
        && io::read_bounded(f.instance / instances::settings_file, instances::settings_max_bytes) == before,
        "Destination conflict overwrote file or metadata");
    rejected([&] { instances::read_instance_icon(f.game, "Pack"); }, "source_changed");
    io::atomic_write(path, {reinterpret_cast<const char*>(icon.png.data()), icon.png.size()});
#ifdef _WIN32
    const auto other = f.root / "other.png"; write(other, source_image(0, 17));
    const auto count = io::directory_entries(f.instance / ".nickel-icons").size();
    const auto settings_path = io::native_path(f.instance / instances::settings_file);
    const auto blocker = CreateFileW(settings_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(blocker != INVALID_HANDLE_VALUE, "Cannot create actual settings publication sharing-failure fixture");
    try { rejected([&] { instances::set_instance_icon(f.game, "Pack", other, images); }); }
    catch (...) { CloseHandle(blocker); throw; }
    CloseHandle(blocker);
    require(io::directory_entries(f.instance / ".nickel-icons").size() == count
        && io::read_bounded(f.instance / instances::settings_file, instances::settings_max_bytes) == before,
        "Failed settings publication left an unreferenced new icon or changed old metadata");
#endif
    leases_released(images);
    std::cout << "content-addressed conflict/no-overwrite/digest tamper/actual settings sharing-fault cleanup: PASS\n";
}
void long_paths() {
    Fixture f; NoHttp http; Decoder decoder; media::ImageService images(http, decoder);
    auto base = f.root;
    while (instances::path_to_utf8(base).size() <= 365) base /= "original-logical-instance-icon-long-component";
    const auto game = base / "game", instance = game / "versions/DeepIcon", selected = base / "source/selected.png";
    io::create_directories(instance); io::write_exclusive(instance / "DeepIcon.json", "{\"id\":\"DeepIcon\"}");
    write(selected, source_image());
    const auto ref = instances::set_instance_icon(game, "DeepIcon", selected, images);
    const auto icon = instances::read_instance_icon(game, "DeepIcon");
    require(icon && icon->reference == ref && instances::path_to_utf8(instance / instances::path_from_utf8(ref)).size() > 440,
        "Original logical deep icon did not preserve its full managed path");
    check_png(icon->png); leases_released(images);
    std::cout << "original logical >365 source/root and >440 managed path/pinned native read/PNG publication: PASS\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        const std::string group = argc > 1 ? argv[1] : "all";
        if (group == "roundtrip" || group == "all") roundtrip();
        if (group == "boundary" || group == "all") boundaries();
        if (group == "cancel" || group == "all") cancellation();
        if (group == "security" || group == "all") security();
        if (group == "publication" || group == "all") publication_failure();
        if (group == "longpath" || group == "all") long_paths();
        return 0;
    } catch (const ErrorException& error) { std::cerr << error.error().code << ": " << error.error().message << '\n'; return 1; }
      catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
