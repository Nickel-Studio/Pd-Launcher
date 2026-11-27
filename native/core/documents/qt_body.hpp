#pragma once

#include "body.hpp"

#include <QImage>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>

namespace nickel::documents::qt {

std::string normalizeMarkdownNoHtml(std::string_view markdown, const Cancellation& cancel = {});
RasterImage decodeRaster(std::span<const std::uint8_t> bytes, const ImageLimits& limits, const Cancellation& cancel = {});
QImage toImage(const RasterImage& image);
QImage placeholder();

// All misses return a valid typed placeholder. Neither a parent browser nor a
// global resource provider can load the original document's external resources.
class SafeTextDocument final : public QTextDocument {
public:
    using ImageLookup = std::function<QImage(const QUrl&)>;
    explicit SafeTextDocument(QObject* parent = nullptr);
    void setSafeBody(const SafeBody& body, ImageLookup images = {});
    bool acceptsImage(const QUrl& key) const;
    std::string approvedExternalLink(const QUrl& key) const;
    void invalidateVisibleImages();
protected:
    QVariant loadResource(int type, const QUrl& name) override;
private:
    SafeBody body_;
    ImageLookup images_;
};

} // namespace nickel::documents::qt
