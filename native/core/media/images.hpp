#pragma once

#include "../accounts/accounts.hpp"
#include "../accounts/http_port.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::transport { class HttpClient; }

namespace nickel::media {

struct ImageLimits {
    std::size_t max_encoded_bytes = 4 * 1024 * 1024;
    std::uint32_t max_dimension = 2048;
    std::uint64_t max_pixels = 4 * 1024 * 1024;
    std::size_t max_decoded_bytes = 16 * 1024 * 1024;
    std::size_t max_cache_bytes = 32 * 1024 * 1024;
    std::size_t max_cache_entries = 128;
    std::size_t max_concurrent_decodes = 2;
    // Decoder working sets plus output stay charged through publication.
    std::size_t max_active_bytes = 32 * 1024 * 1024;
};

struct ImageProbe {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string mime;
    std::uint32_t frames = 1;
    // Peak simultaneous decoder pixel storage, including the returned RGBA plane.
    // Zero declares a single-plane decoder with no native/conversion temporaries.
    // Multi-plane adapters MUST report a conservative, metadata-only upper bound.
    std::size_t decoded_working_bytes = 0;
};

struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
    bool operator==(const RgbaImage&) const = default;
};

// GUI supplies a Qt QImageReader adapter. Probe MUST NOT allocate pixel planes.
// Decode stays within decoded_working_bytes and returns one tightly packed,
// exactly reserved RGBA8888 plane; no SVG/HTML, hidden copies or extra frames.
class ImageDecoder {
public:
    virtual ~ImageDecoder() = default;
    virtual ImageProbe probe(std::span<const std::uint8_t> encoded) = 0;
    virtual RgbaImage decode(std::span<const std::uint8_t> encoded,
                             const ImageProbe& expected,
                             const ImageLimits& limits,
                             std::stop_token stop) = 0;
};

enum class ImageKind { skin, avatar };
struct EncodedImage {
    std::vector<std::uint8_t> bytes;
    std::string mime;
    ImageKind kind = ImageKind::skin;
};
struct PixelRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};
struct HeadGeometry {
    PixelRect face_source;
    PixelRect face_destination;
    std::optional<PixelRect> hat_source;
    std::optional<PixelRect> hat_destination;
};

HeadGeometry head_geometry(std::uint32_t width, std::uint32_t height,
                           std::uint32_t output_size, ImageKind kind);
RgbaImage compose_head(const RgbaImage& image, std::uint32_t output_size = 256,
                       ImageKind kind = ImageKind::skin,
                       const ImageLimits& limits = {});
RgbaImage resize_image(const RgbaImage& image, std::uint32_t width,
                       std::uint32_t height, const ImageLimits& limits = {});
// Exact original functional PNG pixels; builtin_head adds the shared face margin.
RgbaImage builtin_head_asset(bool alex, const ImageLimits& limits = {});
RgbaImage builtin_head(bool alex, std::uint32_t output_size = 256,
                       const ImageLimits& limits = {});
RgbaImage adapt_skin(const RgbaImage& skin, bool source_slim,
                     bool target_slim, bool legacy,
                     const ImageLimits& limits = {});

struct SkinRequest {
    std::string account_id;
    accounts::AccountType source = accounts::AccountType::offline;
    std::string name;
    std::optional<std::string> uuid;
    std::optional<std::string> server;
    std::uint32_t output_size = 256;
    std::uint64_t revision = 0;
    // Explicitly approved additional origins; profile-supplied URLs grant no trust.
    std::vector<std::string> trusted_texture_origins;
};
struct ImageResult {
    RgbaImage head;
    bool fallback = false;
    bool cached = false;
    std::uint64_t revision = 0;
};
struct ImageCacheStats {
    std::size_t entries = 0;
    std::size_t bytes = 0;
    std::size_t active_decodes = 0;
    std::size_t active_bytes = 0;
    std::size_t active_requests = 0;
    std::size_t tracked_accounts = 0;
};

class ImageService final {
public:
    ImageService(transport::HttpClient& http, ImageDecoder& decoder,
                 ImageLimits limits = {}, accounts::IdentityLog log = {});
    ImageService(accounts::IdentityHttp& controlled_http, ImageDecoder& decoder,
                 ImageLimits limits = {}, accounts::IdentityLog log = {});
    ImageService(std::shared_ptr<accounts::IdentityHttp> http, ImageDecoder& decoder,
                 ImageLimits limits = {}, accounts::IdentityLog log = {});
    ~ImageService();
    ImageService(const ImageService&) = delete;
    ImageService& operator=(const ImageService&) = delete;

    std::optional<EncodedImage> fetch_skin(const SkinRequest& request,
                                          std::stop_token stop = {});
    ImageResult head(const SkinRequest& request, std::stop_token stop = {});
    RgbaImage decode_bounded(const EncodedImage& encoded,
                             std::stop_token stop = {});
    // Decode once, resize the complete image to 32x32 and emit metadata-free PNG.
    EncodedImage normalize_icon(const EncodedImage& encoded,
                                 std::stop_token stop = {});
    EncodedImage read_selected_file(const std::filesystem::path& selected_file,
                                    std::stop_token stop = {});
    void invalidate(std::string_view account_id);
    void clear();
    ImageCacheStats cache_stats() const;

    std::optional<EncodedImage> fetch_skin(const SkinRequest& request,
                                          const nickel::Cancellation& cancellation) {
        return fetch_skin(request, cancellation.stop_token());
    }
    ImageResult head(const SkinRequest& request, const nickel::Cancellation& cancellation) {
        return head(request, cancellation.stop_token());
    }
    RgbaImage decode_bounded(const EncodedImage& encoded, const nickel::Cancellation& cancellation) {
        return decode_bounded(encoded, cancellation.stop_token());
    }
    EncodedImage normalize_icon(const EncodedImage& encoded, const nickel::Cancellation& cancellation) {
        return normalize_icon(encoded, cancellation.stop_token());
    }
    EncodedImage read_selected_file(const std::filesystem::path& selected,
                                    const nickel::Cancellation& cancellation) {
        return read_selected_file(selected, cancellation.stop_token());
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nickel::media
