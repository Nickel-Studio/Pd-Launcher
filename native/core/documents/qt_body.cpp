#include "qt_body.hpp"

#include <QBuffer>
#include <QImageReader>
#include <QSet>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

namespace nickel::documents::qt {
namespace {

QVariant blockedResource(int type) {
    if (type == QTextDocument::ImageResource) return QVariant::fromValue(placeholder());
    if (type == QTextDocument::HtmlResource || type == QTextDocument::MarkdownResource || type == QTextDocument::StyleSheetResource) return QVariant(QStringLiteral(" "));
    return QVariant(QByteArray(" "));
}

class NoResourceDocument final : public QTextDocument {
public:
    NoResourceDocument() {
        setUndoRedoEnabled(false);
        setBaseUrl(QUrl());
        setResourceProvider([](const QUrl&) { return QVariant::fromValue(placeholder()); });
    }
protected:
    QVariant loadResource(int type, const QUrl&) override { return blockedResource(type); }
};

// Qt's HTML writer expresses Markdown emphasis as CSS. Serialize its existing
// document tree as semantic tags instead; Gumbo remains the only HTML parser.
class MarkdownHtmlWriter final {
public:
    MarkdownHtmlWriter(const Cancellation& cancel, std::chrono::steady_clock::time_point deadline)
        : cancel_(cancel), deadline_(deadline) {}

    std::string write(const QTextDocument& document) {
        append("<div>");
        range(document.rootFrame()->begin(), document.rootFrame()->end(), 1);
        append("</div>");
        check();
        return std::move(html_);
    }

private:
    struct ListState {
        const QTextList* list;
        int indent;
        std::string_view tag;
        bool itemOpen = false;
    };

    void check() const {
        cancel_.check();
        if (std::chrono::steady_clock::now() > deadline_)
            resources::fail("too_large", "Markdown normalization exceeds its time budget");
    }

    void visit(unsigned depth) {
        check();
        if (depth > 64 || ++nodes_ > 20000)
            resources::fail("too_large", "Markdown document exceeds its structural budget");
    }

    void append(std::string_view value) {
        if (value.size() > 2 * 1024 * 1024 - html_.size())
            resources::fail("too_large", "Markdown normalized HTML exceeds budget");
        html_.append(value);
    }

    void open(std::string_view tag) { append("<"); append(tag); append(">"); }
    void close(std::string_view tag) { append("</"); append(tag); append(">"); }

    void escaped(QString value, bool breaks = false) {
        value.replace(QChar::LineSeparator, QChar(u'\n'));
        const auto bytes = value.toUtf8();
        std::size_t count = 0;
        for (const auto character : bytes) {
            if ((++count & 1023U) == 0) check();
            switch (character) {
            case '&': append("&amp;"); break;
            case '<': append("&lt;"); break;
            case '>': append("&gt;"); break;
            case '"': append("&quot;"); break;
            case '\'': append("&#39;"); break;
            case '\n': append(breaks ? "<br />" : "\n"); break;
            default: append(std::string_view(&character, 1)); break;
            }
        }
    }

    void fragments(const QTextBlock& block, bool pre, unsigned depth) {
        QString link;
        for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const auto fragment = iterator.fragment();
            if (!fragment.isValid()) continue;
            visit(depth);
            const auto format = fragment.charFormat();
            const auto nextLink = format.isAnchor() ? format.anchorHref() : QString();
            if (nextLink != link) {
                if (!link.isEmpty()) close("a");
                link = nextLink;
                if (!link.isEmpty()) {
                    append("<a href=\""); escaped(link); append("\">");
                }
            }
            if (format.isImageFormat()) {
                const auto image = format.toImageFormat();
                append("<img src=\""); escaped(image.name());
                append("\" alt=\""); escaped(image.stringProperty(QTextFormat::ImageAltText));
                append("\" />");
                continue;
            }
            const std::array<std::pair<bool, std::string_view>, 7> tags{{
                {format.fontWeight() >= QFont::Bold, "strong"},
                {format.fontItalic(), "em"},
                {format.fontStrikeOut(), "s"},
                {format.fontUnderline() && link.isEmpty(), "u"},
                {!pre && format.fontFixedPitch(), "code"},
                {format.verticalAlignment() == QTextCharFormat::AlignSuperScript, "sup"},
                {format.verticalAlignment() == QTextCharFormat::AlignSubScript, "sub"}}};
            for (const auto& [enabled, tag] : tags) if (enabled) open(tag);
            escaped(fragment.text(), !pre);
            for (auto index = tags.size(); index; --index)
                if (tags[index - 1].first) close(tags[index - 1].second);
        }
        if (!link.isEmpty()) close("a");
    }

    static std::string_view listTag(const QTextList& list) {
        switch (list.format().style()) {
        case QTextListFormat::ListDecimal:
        case QTextListFormat::ListLowerAlpha:
        case QTextListFormat::ListUpperAlpha:
        case QTextListFormat::ListLowerRoman:
        case QTextListFormat::ListUpperRoman: return "ol";
        default: return "ul";
        }
    }

    void table(const QTextTable& value, unsigned depth) {
        visit(depth);
        const auto rows = value.rows(), columns = value.columns();
        if (rows <= 0 || columns <= 0 || std::uint64_t(rows) * std::uint64_t(columns) > 4096 - cells_)
            resources::fail("too_large", "Markdown table exceeds its cell budget");
        cells_ += std::size_t(rows) * std::size_t(columns);
        open("table");
        for (int row = 0; row < rows; ++row) {
            open("tr");
            for (int column = 0; column < columns; ++column) {
                const auto cell = value.cellAt(row, column);
                if (!cell.isValid() || cell.row() != row || cell.column() != column) continue;
                visit(depth + 1);
                const auto tag = row < value.format().headerRowCount() ? "th" : "td";
                open(tag);
                range(cell.begin(), cell.end(), depth + 2);
                close(tag);
            }
            close("tr");
        }
        close("table");
    }

    void range(QTextFrame::iterator iterator, const QTextFrame::iterator& end, unsigned depth) {
        std::vector<ListState> lists;
        unsigned quotes = 0;
        bool pre = false;
        const auto closePre = [&] {
            if (pre) { close("code"); close("pre"); pre = false; }
        };
        const auto closeList = [&] {
            if (lists.back().itemOpen) close("li");
            close(lists.back().tag);
            lists.pop_back();
        };
        const auto closeLists = [&] { while (!lists.empty()) closeList(); };
        const auto setQuotes = [&](unsigned next) {
            while (quotes > next) { close("blockquote"); --quotes; }
            while (quotes < next) { visit(depth + quotes); open("blockquote"); ++quotes; }
        };
        for (; iterator != end; ++iterator) {
            visit(depth);
            if (const auto frame = iterator.currentFrame()) {
                closePre(); closeLists(); setQuotes(0);
                if (const auto nested = qobject_cast<QTextTable*>(frame)) table(*nested, depth + 1);
                else { open("div"); range(frame->begin(), frame->end(), depth + 1); close("div"); }
                continue;
            }
            const auto block = iterator.currentBlock();
            if (!block.isValid()) continue;
            const auto format = block.blockFormat();
            const auto nextQuotes = unsigned(std::max(0, format.intProperty(QTextFormat::BlockQuoteLevel)));
            if (nextQuotes != quotes) { closePre(); closeLists(); setQuotes(nextQuotes); }
            const auto list = block.textList();
            if (list) {
                closePre();
                const auto indent = std::max(1, list->format().indent());
                if (indent > 64) resources::fail("too_large", "Markdown list exceeds its depth budget");
                while (!lists.empty() && (lists.back().indent > indent ||
                    (lists.back().indent == indent && lists.back().list != list))) closeList();
                if (lists.empty() || lists.back().indent < indent) {
                    const auto tag = listTag(*list);
                    open(tag); lists.push_back({list, indent, tag});
                }
                if (lists.back().itemOpen) close("li");
                open("li"); lists.back().itemOpen = true;
            } else if (format.indent() > 0 && !lists.empty()) {
                while (!lists.empty() && lists.back().indent > format.indent()) { closePre(); closeList(); }
            } else if (!lists.empty()) { closePre(); closeLists(); }
            const auto contentDepth = depth + quotes + unsigned(lists.size()) * 2 + 2;
            visit(contentDepth);
            if (format.hasProperty(QTextFormat::BlockCodeLanguage)) {
                if (!pre) { open("pre"); open("code"); pre = true; }
                else append("\n");
                fragments(block, true, contentDepth + 1);
                continue;
            }
            closePre();
            if (format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) { append("<hr />"); continue; }
            static constexpr std::array<std::string_view, 7> tags{"p", "h1", "h2", "h3", "h4", "h5", "h6"};
            const auto tag = tags[std::clamp(format.headingLevel(), 0, 6)];
            open(tag);
            if (format.marker() == QTextBlockFormat::MarkerType::Checked) append("[x] ");
            else if (format.marker() == QTextBlockFormat::MarkerType::Unchecked) append("[ ] ");
            fragments(block, false, contentDepth + 1);
            close(tag);
        }
        closePre(); closeLists(); setQuotes(0);
    }

    const Cancellation& cancel_;
    std::chrono::steady_clock::time_point deadline_;
    std::string html_;
    std::size_t nodes_ = 0, cells_ = 0;
};

bool imageEnvelope(std::span<const std::uint8_t> bytes, QByteArray& format) {
    static constexpr std::uint8_t png[]{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    if (bytes.size() >= 8 && std::equal(std::begin(png), std::end(png), bytes.begin())) format = "png";
    else if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) format = "jpeg";
    else if (bytes.size() >= 12 && std::equal(bytes.begin(), bytes.begin() + 4, "RIFF") && std::equal(bytes.begin() + 8, bytes.begin() + 12, "WEBP")) format = "webp";
    else return false;
    return true;
}

std::uint32_t bigEndian(std::span<const std::uint8_t> bytes) {
    return std::uint32_t(bytes[0]) << 24 | std::uint32_t(bytes[1]) << 16 | std::uint32_t(bytes[2]) << 8 | bytes[3];
}

void rejectAnimatedContainer(std::span<const std::uint8_t> bytes, const QByteArray& format) {
    if (format == "png") {
        for (std::size_t offset = 8; offset + 12 <= bytes.size();) {
            const auto size = bigEndian(bytes.subspan(offset, 4));
            if (size > bytes.size() - offset - 12) resources::fail("invalid_response", "PNG chunk exceeds encoded image budget");
            if (std::equal(bytes.begin() + offset + 4, bytes.begin() + offset + 8, "acTL")) resources::fail("policy_denied", "Animated PNG descriptions are disabled");
            offset += std::size_t(size) + 12;
        }
    } else if (format == "webp") {
        for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
            const auto size = std::uint32_t(bytes[offset + 4]) | std::uint32_t(bytes[offset + 5]) << 8 |
                std::uint32_t(bytes[offset + 6]) << 16 | std::uint32_t(bytes[offset + 7]) << 24;
            if (size > bytes.size() - offset - 8) resources::fail("invalid_response", "WebP chunk exceeds encoded image budget");
            if (std::equal(bytes.begin() + offset, bytes.begin() + offset + 4, "ANIM") || std::equal(bytes.begin() + offset, bytes.begin() + offset + 4, "ANMF")) resources::fail("policy_denied", "Animated WebP descriptions are disabled");
            offset += std::size_t(size) + 8 + (size & 1U);
        }
    }
}

std::string keyBytes(const QUrl& key) {
    const auto bytes = key.toEncoded(QUrl::FullyEncoded);
    return {bytes.constData(), std::size_t(bytes.size())};
}

} // namespace

QImage placeholder() {
    static const QImage value = [] {
        QImage image(1, 1, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        return image;
    }();
    return value;
}

std::string normalizeMarkdownNoHtml(std::string_view markdown, const Cancellation& cancel) {
    cancel.check();
    if (markdown.size() > 512 * 1024 || !resources::validUtf8(markdown)) resources::fail("too_large", "Markdown exceeds native normalization budget");
    const auto deadline = std::chrono::steady_clock::now() + BodyLimits{}.maximumParseTime;
    NoResourceDocument document;
    const QTextDocument::MarkdownFeatures features{QTextDocument::MarkdownDialectGitHub, QTextDocument::MarkdownNoHTML};
    document.setMarkdown(QString::fromUtf8(markdown.data(), qsizetype(markdown.size())), features);
    cancel.check();
    if (document.characterCount() > 2 * 1024 * 1024 || document.blockCount() > 20000) resources::fail("too_large", "Markdown normalized document exceeds budget");
    return MarkdownHtmlWriter(cancel, deadline).write(document);
}

RasterImage decodeRaster(std::span<const std::uint8_t> bytes, const ImageLimits& limits, const Cancellation& cancel) {
    cancel.check();
    if (bytes.empty() || bytes.size() > limits.maximumEncodedBytes || bytes.size() > std::size_t(std::numeric_limits<qsizetype>::max()) ||
        !limits.maximumPixels || !limits.maximumDimension || !limits.maximumDisplayDimension || limits.maximumDimension > 4096 || limits.maximumPixels > 4 * 1024 * 1024) resources::fail("too_large", "Raster image exceeds native decode budget");
    QByteArray format;
    if (!imageEnvelope(bytes, format)) resources::fail("policy_denied", "Only bounded PNG, JPEG or WebP images are accepted");
    rejectAnimatedContainer(bytes, format);
    QByteArray storage(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size()));
    QBuffer buffer(&storage);
    if (!buffer.open(QIODevice::ReadOnly)) resources::fail("io", "Cannot open bounded raster bytes");
    QImageReader reader(&buffer, format);
    reader.setAutoDetectImageFormat(false);
    reader.setDecideFormatFromContent(false);
    reader.setAutoTransform(false);
    const auto size = reader.size();
    if (!size.isValid() || unsigned(size.width()) > limits.maximumDimension || unsigned(size.height()) > limits.maximumDimension ||
        std::uint64_t(size.width()) * std::uint64_t(size.height()) > limits.maximumPixels) resources::fail("too_large", "Raster dimensions exceed native image budget");
    if (reader.supportsAnimation() && reader.imageCount() != 1) resources::fail("policy_denied", "Animated document images are disabled");
    cancel.check();
    auto image = reader.read();
    cancel.check();
    if (image.isNull() || image.size() != size || image.sizeInBytes() > qsizetype(limits.maximumPixels * 8)) resources::fail("invalid_response", "Invalid raster image");
    if (unsigned(image.width()) > limits.maximumDisplayDimension || unsigned(image.height()) > limits.maximumDisplayDimension) image = image.scaled(int(limits.maximumDisplayDimension), int(limits.maximumDisplayDimension), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) resources::fail("invalid_response", "Raster conversion failed");
    RasterImage result;
    result.width = unsigned(image.width());
    result.height = unsigned(image.height());
    result.stride = std::size_t(result.width) * 4;
    result.rgba.resize(result.stride * result.height);
    for (unsigned row = 0; row < result.height; ++row) {
        cancel.check();
        std::memcpy(result.rgba.data() + row * result.stride, image.constScanLine(int(row)), result.stride);
    }
    validateRasterImage(result, limits);
    return result;
}

QImage toImage(const RasterImage& image) {
    try {
        validateRasterImage(image, {});
        const QImage view(image.rgba.data(), int(image.width), int(image.height), qsizetype(image.stride), QImage::Format_RGBA8888);
        const auto copy = view.copy();
        return copy.isNull() ? placeholder() : copy;
    } catch (...) { return placeholder(); }
}

SafeTextDocument::SafeTextDocument(QObject* parent) : QTextDocument(parent) {
    setUndoRedoEnabled(false);
    setBaseUrl(QUrl());
    setResourceProvider([](const QUrl&) { return QVariant::fromValue(placeholder()); });
}

void SafeTextDocument::setSafeBody(const SafeBody& body, ImageLookup images) {
    clear();
    body_ = body;
    images_ = std::move(images);
    setBaseUrl(QUrl());
    const auto& html = body_.html();
    setHtml(QString::fromUtf8(html.data(), qsizetype(html.size())));
}

bool SafeTextDocument::acceptsImage(const QUrl& key) const {
    if (!body_.valid()) return false;
    const auto encoded = keyBytes(key);
    for (const auto& image : body_.images()) if (image.key == encoded) return true;
    return false;
}

std::string SafeTextDocument::approvedExternalLink(const QUrl& key) const {
    return body_.externalLink(keyBytes(key));
}

QVariant SafeTextDocument::loadResource(int type, const QUrl& name) {
    if (type != QTextDocument::ImageResource || !acceptsImage(name)) return blockedResource(type);
    try {
        const auto image = images_ ? images_(name) : placeholder();
        return QVariant::fromValue(image.isNull() ? placeholder() : image);
    } catch (...) { return QVariant::fromValue(placeholder()); }
}

void SafeTextDocument::invalidateVisibleImages() {
    if (!body_.valid()) return;
    for (const auto& image : body_.images()) {
        const auto key = QUrl::fromEncoded(QByteArray(image.key.data(), qsizetype(image.key.size())), QUrl::StrictMode);
        addResource(QTextDocument::ImageResource, key, loadResource(QTextDocument::ImageResource, key));
    }
    markContentsDirty(0, characterCount());
}

} // namespace nickel::documents::qt
