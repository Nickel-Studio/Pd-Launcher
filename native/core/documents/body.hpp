#pragma once

#include "core/resources/network.hpp"

#include <span>

namespace nickel::documents {

enum class BodyFormat { Markdown, Html, PlainText };

struct FullDescription {
    resources::Source source = resources::Source::Modrinth;
    std::string projectId;
    BodyFormat format = BodyFormat::PlainText;
    std::string text;
    std::string canonicalBase;
    std::string revision;
    resources::Freshness freshness = resources::Freshness::Fresh;
};

struct BodyLimits {
    std::size_t maximumInputBytes = 512 * 1024;
    std::size_t maximumOutputBytes = 2 * 1024 * 1024;
    std::size_t maximumNodes = 20000;
    unsigned maximumDepth = 64;
    std::size_t maximumImages = 128;
    std::size_t maximumLinks = 512;
    std::size_t maximumTableCells = 4096;
    std::chrono::milliseconds maximumParseTime{200};
};

struct BodyImage {
    std::string key;
    std::string alt;
    unsigned width = 160;
    unsigned height = 90;
};

struct BodyLink {
    std::string key;
    std::string label;
};

class SafeBody {
public:
    SafeBody() = default;
    bool valid() const noexcept;
    const std::string& html() const;
    const std::string& plainText() const;
    const std::string& documentId() const;
    std::vector<BodyImage> images() const;
    std::vector<BodyLink> links() const;
    std::string externalLink(const std::string& opaqueKey) const;
    std::size_t chargedBytes() const;
private:
    struct Data;
    explicit SafeBody(std::shared_ptr<const Data> data);
    std::shared_ptr<const Data> data_;
    friend class BodyReader;
    friend class BodyResources;
};

// Inject Qt's existing MarkdownNoHTML normalization at the native adapter boundary.
// The returned HTML STILL passes the mature structural allowlist, never setHtml directly.
using MarkdownNormalizer = std::function<std::string(std::string_view, const Cancellation&)>;

class BodyReader final {
public:
    explicit BodyReader(BodyLimits limits = {}, resources::UrlRules images = resources::descriptionImageRules(),
                        resources::UrlRules links = resources::externalLinkRules(), MarkdownNormalizer markdown = {});
    SafeBody convert(const FullDescription& description, const Cancellation& cancel = {}) const;
private:
    BodyLimits limits_;
    resources::UrlRules imageRules_;
    resources::UrlRules linkRules_;
    MarkdownNormalizer markdown_;
};

struct ImageLimits {
    std::size_t maximumEncodedBytes = 2 * 1024 * 1024;
    std::size_t maximumCacheBytes = 16 * 1024 * 1024;
    std::uint64_t maximumPixels = 4 * 1024 * 1024;
    unsigned maximumDimension = 4096;
    unsigned maximumDisplayDimension = 1024;
    unsigned maximumConcurrent = 4;
};

enum class ResourceState { Ready, Pending, Denied, Failed };

struct RasterImage {
    unsigned width = 1;
    unsigned height = 1;
    std::size_t stride = 4;
    resources::Bytes rgba{0, 0, 0, 0};
};

using ImageDecoder = std::function<RasterImage(std::span<const std::uint8_t>, const ImageLimits&, const Cancellation&)>;

std::shared_ptr<const RasterImage> resourcePlaceholder();

struct ResourceReply {
    ResourceState state = ResourceState::Pending;
    std::shared_ptr<const RasterImage> image = resourcePlaceholder();
};

void validateRasterImage(const RasterImage& image, const ImageLimits& limits);

// URLs stay private; all states contain a valid image. lookup never performs IO.
class BodyResources final {
public:
    explicit BodyResources(resources::CachedHttp& http, ImageLimits limits = {}, ImageDecoder decoder = {});
    ~BodyResources();
    BodyResources(const BodyResources&) = delete;
    BodyResources& operator=(const BodyResources&) = delete;
    void setBody(const SafeBody& body);
    bool approved(const std::string& opaqueKey) const;
    ResourceReply lookup(const std::string& opaqueKey) const;
    ResourceReply requestVisible(const std::string& opaqueKey, const Cancellation& cancel = {});
    void cancel();
    std::size_t decodedCacheBytes() const;
private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace nickel::documents
