#include "fixtures.hpp"
#include "core/documents/qt_body.hpp"

#include <QBuffer>
#include <QGuiApplication>
#include <QImageReader>
#include <QPainter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextFragment>

#include <array>

namespace {
using namespace nickel;
using namespace nickel::tests;
using namespace nickel::documents;
namespace adapter = nickel::documents::qt;

FullDescription description(std::string text, BodyFormat format = BodyFormat::Html) {
    FullDescription result;
    result.projectId = "p1";
    result.canonicalBase = "https://cdn.modrinth.com/data/p1/";
    result.text = std::move(text);
    result.format = format;
    return result;
}

QUrl keyUrl(std::string_view value) {
    return QUrl::fromEncoded(QByteArray(value.data(), qsizetype(value.size())), QUrl::StrictMode);
}

void realMarkdownNormalization() {
    BodyReader reader({}, resources::descriptionImageRules(), resources::externalLinkRules(), adapter::normalizeMarkdownNoHtml);
    const auto body = reader.convert(description("# Heading\n\n**strong** and *emphasis* and `code`\n\n- first\n- second\n\n| a | b |\n| - | - |\n| c | d |\n\n![image](https://cdn.modrinth.com/icon.png)\n\n[link](https://example.org/page)\n\n<img src='file:///secret' onerror='active()'>\n", BodyFormat::Markdown));
    require(body.valid() && body.html().find("<h1>") != std::string::npos && body.html().find("<table>") != std::string::npos,
        "Qt Markdown loses heading or GFM table structure");
    require(body.html().find("<strong>") != std::string::npos && body.html().find("<em>") != std::string::npos && body.html().find("<code>") != std::string::npos,
        "Qt Markdown formatting is discarded when generated CSS is sanitized");
    require(body.images().size() == 1 && body.links().size() == 1 && body.html().find("src=\"file:") == std::string::npos,
        "Qt Markdown accepts active HTML or loses opaque resource approvals");
    require(body.html().find("style=") == std::string::npos && body.html().find("src=\"https:") == std::string::npos && body.html().find("href=\"https:") == std::string::npos,
        "Normalized Markdown bypasses the sole Gumbo sanitizer");
    std::string longMarkdown;
    for (unsigned index = 0; index < 400; ++index) longMarkdown += "Paragraph **strong** with `code` and [link](https://example.org/page).\n\n";
    require(reader.convert(description(longMarkdown, BodyFormat::Markdown)).plainText().size() > 10000, "Long Markdown is truncated");
    expectError("too_large", [&] { (void)adapter::normalizeMarkdownNoHtml(std::string(512 * 1024 + 1, 'x')); });
    Cancellation cancel;
    cancel.cancel();
    expectError("cancelled", [&] { (void)adapter::normalizeMarkdownNoHtml("# cancelled", cancel); });
}

class ParentProvider final : public QTextDocument {
public:
    unsigned calls = 0;
protected:
    QVariant loadResource(int, const QUrl&) override { ++calls; return QVariant(QByteArray("parent must not load")); }
};

class GlobalProviderGuard {
public:
    GlobalProviderGuard() : previous_(QTextDocument::defaultResourceProvider()) {
        QTextDocument::setDefaultResourceProvider([this](const QUrl&) { ++calls; return QVariant(QByteArray("global must not load")); });
    }
    ~GlobalProviderGuard() { QTextDocument::setDefaultResourceProvider(previous_); }
    unsigned calls = 0;
private:
    QTextDocument::ResourceProvider previous_;
};

void opaqueResourcesNeverFallBack() {
    GlobalProviderGuard global;
    ParentProvider parent;
    adapter::SafeTextDocument document(&parent);
    unsigned lookups = 0, providerCalls = 0;
    document.setResourceProvider([&](const QUrl&) { ++providerCalls; return QVariant(QByteArray("document provider must not load")); });
    const auto body = BodyReader().convert(description("<h2>Safe</h2><img src='https://cdn.modrinth.com/icon.png'><a href='https://example.org/page'>link</a>"));
    document.setSafeBody(body, [&](const QUrl&) { ++lookups; return QImage(); });
    const auto imageKey = keyUrl(body.images().front().key);
    require(document.acceptsImage(imageKey), "Opaque image approval is not bound to the current document");
    const auto value = document.resource(QTextDocument::ImageResource, imageKey);
    require(value.isValid() && value.metaType().id() == QMetaType::QImage && !value.value<QImage>().isNull(), "Null decoder image escapes the valid placeholder boundary");
    const auto afterApproved = lookups;
    for (const auto* raw : {"https://cdn.modrinth.com/icon.png", "file:///C:/secret", "qrc:/secret", "data:image/png,x", "//server/share/secret", "nickel-image://foreign/0"}) {
        for (const auto type : {QTextDocument::ImageResource, QTextDocument::HtmlResource, QTextDocument::MarkdownResource, QTextDocument::StyleSheetResource}) {
            const auto blocked = document.resource(type, QUrl(QString::fromLatin1(raw)));
            require(blocked.isValid(), "Denied resource returns an invalid QVariant that can fall back");
            if (type == QTextDocument::ImageResource) require(!blocked.value<QImage>().isNull(), "Denied image placeholder is invalid");
        }
    }
    require(lookups == afterApproved && parent.calls == 0 && global.calls == 0 && providerCalls == 0, "Denied resource invokes an image, parent or global fallback");
    require(document.approvedExternalLink(keyUrl(body.links().front().key)) == "https://example.org/page", "Opaque link no longer resolves through approved policy");
    expectError("policy_denied", [&] { (void)document.approvedExternalLink(QUrl(QStringLiteral("https://example.org/page"))); });
    document.invalidateVisibleImages();
    require(lookups > afterApproved && parent.calls == 0 && global.calls == 0, "Explicit image invalidation bypasses the controlled lookup");
    document.setSafeBody(BodyReader().convert(description("<p>New generation</p>")));
    const auto old = document.resource(QTextDocument::ImageResource, imageKey);
    require(!document.acceptsImage(imageKey) && old.isValid() && old.value<QImage>().width() == 1, "New document retains old image approval or raster");
    QImage rendered(160, 80, QImage::Format_RGBA8888);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    document.drawContents(&painter);
    require(parent.calls == 0 && global.calls == 0 && providerCalls == 0, "Real QTextDocument rendering loads an implicit resource");
}

void markdownSemanticsAndEscapes() {
    GlobalProviderGuard global;
    BodyReader reader({}, resources::descriptionImageRules(), resources::externalLinkRules(), adapter::normalizeMarkdownNoHtml);
    const auto body = reader.convert(description(
        "***nested*** and ~~removed~~ and [**bold** *italic* `linked`](https://example.org/path?q=a&b=c)\n\n"
        "`<img src=\"https://cdn.modrinth.com/not-an-image.png\">`\n\n"
        "```cpp\nif (left < right && ready) {\n  return \"<img src='file:///secret'>\";\n}\n```\n\n"
        "> outer\n>\n> > inner **quoted**\n\n"
        "- parent\n  - child *emphasis*\n- sibling\n\n"
        "- [x] done\n- [ ] pending\n\n"
        "| **head** | other |\n| --- | --- |\n| `cell` | value |\n\n"
        "![alt](https://cdn.modrinth.com/icon.png)\n\n", BodyFormat::Markdown));
    const auto& html = body.html();
    require(html.find("<strong><em>nested</em></strong>") != std::string::npos && html.find("<s>removed</s>") != std::string::npos,
        "Nested emphasis or strikethrough is lost");
    require(html.find("<pre><code>if (left &lt; right &amp;&amp; ready) {\n  return") != std::string::npos,
        "Multiline fenced code is split or interpreted as HTML");
    require(html.find("<code>&lt;img src=") != std::string::npos && body.images().size() == 1 && body.links().size() == 1,
        "Literal code injects an image or formatted link splits its approval");
    require(body.externalLink(body.links().front().key) == "https://example.org/path?q=a&b=c" && body.images().front().alt == "alt",
        "Attribute escaping changes approved link queries or image alternate text");
    require(html.find("<blockquote><p>outer</p><blockquote>") != std::string::npos &&
        html.find("<li><p>parent</p><ul>") != std::string::npos && html.find("<code>cell</code>") != std::string::npos,
        "Nested quotes, lists or table formatting is flattened");
    require(body.plainText().find("[x] done") != std::string::npos && body.plainText().find("[ ] pending") != std::string::npos,
        "GFM task markers are lost");
    require(html.find("style=") == std::string::npos && html.find("src=\"file:") == std::string::npos && global.calls == 0,
        "Markdown normalization adds style, active HTML or resource-provider IO");
    expectError("too_large", [&] { (void)adapter::normalizeMarkdownNoHtml(std::string(80, '>') + " deep\n"); });
    expectError("too_large", [&] { (void)adapter::normalizeMarkdownNoHtml(std::string("\xc0\xaf", 2)); });
    std::string largeTable = "| a | b |\n| --- | --- |\n";
    for (unsigned row = 0; row < 2050; ++row) largeTable += "| a | b |\n";
    expectError("too_large", [&] { (void)adapter::normalizeMarkdownNoHtml(largeTable); });
}

resources::Bytes encodedImage(const char* format, int width = 4, int height = 2) {
    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(QColor(220, 30, 10, 255));
    QByteArray storage;
    QBuffer buffer(&storage);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, format), "Approved static image plugin is unavailable");
    const auto* first = reinterpret_cast<const std::uint8_t*>(storage.constData());
    return {first, first + storage.size()};
}

void realRasterBoundaries() {
    const auto png = encodedImage("PNG");
    ImageLimits limits;
    limits.maximumDisplayDimension = 2;
    const auto image = adapter::decodeRaster(png, limits);
    require(image.width == 2 && image.height == 1 && image.stride == 8 && image.rgba.size() == 8 && !adapter::toImage(image).isNull(),
        "Real PNG decode/scaling/RGBA contract differs");
    const auto jpeg = encodedImage("JPEG");
    require(adapter::decodeRaster(jpeg, {}).width == 4, "Approved static JPEG decoder is not linked");
    limits.maximumDimension = 1;
    expectError("too_large", [&] { (void)adapter::decodeRaster(png, limits); });
    limits = {};
    limits.maximumEncodedBytes = 2;
    expectError("too_large", [&] { (void)adapter::decodeRaster(png, limits); });
    expectError("policy_denied", [&] { (void)adapter::decodeRaster(resources::Bytes{'G','I','F','8','9','a'}, {}); });
    expectError("policy_denied", [&] { (void)adapter::decodeRaster(bytes("<svg><script>active</script></svg>"), {}); });
    auto animated = png;
    const resources::Bytes animationChunk{0,0,0,0,'a','c','T','L',0,0,0,0};
    animated.insert(animated.begin() + 8, animationChunk.begin(), animationChunk.end());
    expectError("policy_denied", [&] { (void)adapter::decodeRaster(animated, {}); });
    const resources::Bytes animatedWebp{'R','I','F','F',12,0,0,0,'W','E','B','P','A','N','I','M',0,0,0,0};
    expectError("policy_denied", [&] { (void)adapter::decodeRaster(animatedWebp, {}); });
    Cancellation cancel;
    cancel.cancel();
    expectError("cancelled", [&] { (void)adapter::decodeRaster(png, {}, cancel); });
    RasterImage invalid;
    invalid.width = 10000;
    require(adapter::toImage(invalid).width() == 1 && !adapter::placeholder().isNull(), "Malformed raster does not become a valid Qt placeholder");
}
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << "START Qt offscreen application\n";
    QGuiApplication application(argc, argv);
    const std::array<std::pair<const char*, void(*)()>, 4> cases{{
        {"unique real Qt Markdown normalization", realMarkdownNormalization},
        {"Markdown structure, semantics and escaping", markdownSemanticsAndEscapes},
        {"opaque resources and zero provider fallback", opaqueResourcesNeverFallBack},
        {"real raster decode and expansion budgets", realRasterBoundaries}}};
    unsigned failures = 0;
    for (const auto& [name, test] : cases) {
        std::cerr << "START " << name << '\n';
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const ErrorException& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.error().code << ": " << error.what() << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failures ? 1 : 0;
}
