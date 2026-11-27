#include "SafeDescription.hpp"
#include "Controls.hpp"
#include "core/documents/body.hpp"
#include <QAbstractTextDocumentLayout>
#include <QHideEvent>
#include <QJsonArray>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <utility>

namespace nickel::ui {
namespace {
std::atomic<RequestId> nextImageRequest{RequestId(1) << 62};
const documents::BodyLimits bodyLimits;
const documents::ImageLimits imageLimits;
}
SafeDescriptionDocument::SafeDescriptionDocument(UiServices& services, QObject* parent)
    : QTextDocument(parent), services_(services) {
    setUndoRedoEnabled(false);
    setDocumentMargin(0);
    setBaseUrl({});
    qRegisterMetaType<DescriptionImageReply>();
    connect(&services_, &UiServices::descriptionImageReady, this, [this](const DescriptionImageReply& reply) {
        Q_ASSERT(QThread::currentThread() == thread());
        const auto found = pending_.find(reply.id);
        if (found == pending_.end() || reply.documentKey != documentKey_
            || found->generation != generation_ || found->key != reply.opaqueKey) return;
        const QString key = found->key;
        pending_.erase(found);
        const auto bytes = reply.image.sizeInBytes();
        const auto maximumBytes = qsizetype(imageLimits.maximumCacheBytes);
        const quint64 maximumImageBytes = quint64(imageLimits.maximumDisplayDimension) * imageLimits.maximumDisplayDimension * 4;
        if (!reply.error.isError() && !reply.image.isNull() && quint64(bytes) <= maximumImageBytes
            && reply.image.width() <= int(imageLimits.maximumDisplayDimension)
            && reply.image.height() <= int(imageLimits.maximumDisplayDimension)
            && bytes <= maximumBytes - decodedBytes_) {
            images_.insert(key, reply.image);
            decodedBytes_ += bytes;
            addResource(QTextDocument::ImageResource, QUrl(key, QUrl::StrictMode), reply.image);
            markContentsDirty(0, characterCount());
            emit resourceUpdated();
        } else {
            addResource(QTextDocument::ImageResource, QUrl(key, QUrl::StrictMode), placeholder(QTextDocument::ImageResource));
        }
        requestVisibleImages();
    }, Qt::QueuedConnection);
}
SafeDescriptionDocument::~SafeDescriptionDocument() { releaseBody(); }
QString SafeDescriptionDocument::urlKey(const QUrl& url) const { return QString::fromLatin1(url.toEncoded(QUrl::FullyEncoded)); }
bool SafeDescriptionDocument::isOpaqueKey(const QUrl& url, const QString& scheme, const QString& documentKey) {
    if (documentKey.isEmpty() || !url.isValid() || url.isRelative() || url.scheme() != scheme || url.host() != documentKey
        || !url.userInfo().isEmpty() || url.port(-1) != -1 || url.hasQuery() || url.hasFragment()) return false;
    const QString path = url.path(QUrl::FullyEncoded);
    if (path.size() < 2 || path[0] != '/') return false;
    for (int i = 1; i < path.size(); ++i) if (path[i] < '0' || path[i] > '9') return false;
    return url.toString(QUrl::FullyEncoded) == scheme + "://" + documentKey + path;
}
QVariant SafeDescriptionDocument::placeholder(int type) {
    if (type == QTextDocument::ImageResource) {
        QImage image(1, 1, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent); return QVariant(image);
    }
    return QVariant(QByteArray(""));
}
void SafeDescriptionDocument::cancelPendingImages() {
    ++generation_;
    const auto pending = std::exchange(pending_, {});
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        requested_.remove(it->key);
        services_.cancelRequest(it.key());
    }
}
void SafeDescriptionDocument::releaseBody() {
    firstVisible_ = lastVisible_ = -1;
    cancelPendingImages();
    requested_.clear(); approvedImages_.clear(); approvedLinks_.clear(); images_.clear();
    decodedBytes_ = 0; sourceHtml_.clear();
    const QString oldDocument = std::exchange(documentKey_, {});
    clear();
    if (!oldDocument.isEmpty()) services_.releaseDescription(oldDocument);
}
void SafeDescriptionDocument::setBody(const QJsonObject& body) {
    const QString documentKey = body.value("documentKey").toString();
    const QString html = body.value("sanitizedHtml").toString();
    // An immutable SafeBody may be projected repeatedly; do not release its live lease.
    if (!documentKey_.isEmpty() && documentKey == documentKey_) {
        if (html == sourceHtml_) return;
        releaseBody(); setPlainText("正文标识与不可变内容不一致，请重试。"); return;
    }
    releaseBody(); deniedCount_ = 0; documentKey_ = documentKey;
    if (documentKey_.isEmpty() || !body.value("sanitizedHtml").isString()
        || std::size_t(html.toUtf8().size()) > bodyLimits.maximumOutputBytes
        || std::size_t(body.value("images").toArray().size()) > bodyLimits.maximumImages
        || std::size_t(body.value("links").toArray().size()) > bodyLimits.maximumLinks) {
        setPlainText("正文不可用或超出安全预算，请重试或显式打开项目主页。"); return;
    }
    for (const auto& value : body.value("images").toArray()) {
        const QUrl url(value.toObject().value("key").toString(), QUrl::StrictMode);
        if (isOpaqueKey(url, "nickel-image", documentKey_)) approvedImages_.insert(urlKey(url));
    }
    for (const auto& value : body.value("links").toArray()) {
        const QUrl url(value.toObject().value("key").toString(), QUrl::StrictMode);
        if (isOpaqueKey(url, "nickel-link", documentKey_)) approvedLinks_.insert(urlKey(url));
    }
    // Only the canonical Gumbo allowlist's immutable SafeBody projection reaches here.
    sourceHtml_ = html; setHtml(html); setBaseUrl({});
}
QVariant SafeDescriptionDocument::loadResource(int type, const QUrl& url) { return guardedResource(type, url); }
QVariant SafeDescriptionDocument::guardedResource(int type, const QUrl& url) {
    const QString key = urlKey(url); const QVariant fallback = placeholder(type);
    if (type != QTextDocument::ImageResource || !isOpaqueKey(url, "nickel-image", documentKey_) || !approvedImages_.contains(key)) {
        ++deniedCount_; addResource(type, url, fallback); emit resourceDenied(url); return fallback;
    }
    if (images_.contains(key)) return QVariant(images_.value(key));
    // Valid denial/pending/failed resources always terminate Qt's fallback chain.
    addResource(type, url, fallback); return fallback;
}
void SafeDescriptionDocument::setVisibleRange(int first, int last) {
    firstVisible_ = first;
    lastVisible_ = last;
    if (first < 0 || last < first) cancelPendingImages();
    else requestVisibleImages();
}
void SafeDescriptionDocument::requestVisibleImages() {
    if (documentKey_.isEmpty() || firstVisible_ < 0 || lastVisible_ < firstVisible_
        || decodedBytes_ >= qsizetype(imageLimits.maximumCacheBytes)) return;
    for (QTextBlock block = findBlock(firstVisible_); block.isValid() && block.position() <= lastVisible_; block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) continue;
            const QUrl url(fragment.charFormat().toImageFormat().name(), QUrl::StrictMode);
            const QString key = urlKey(url);
            if (!approvedImages_.contains(key) || requested_.contains(key)) continue;
            if (pending_.size() >= qsizetype(imageLimits.maximumConcurrent)) return;
            requested_.insert(key);
            const auto id = nextImageRequest.fetch_add(1);
            pending_.insert(id, {key, generation_});
            services_.resolveDescriptionImage(id, documentKey_, key);
        }
    }
}
bool SafeDescriptionDocument::isLinkApproved(const QUrl& url) const {
    return isOpaqueKey(url, "nickel-link", documentKey_) && approvedLinks_.contains(urlKey(url));
}
SafeDescriptionBrowser::SafeDescriptionBrowser(Theme& theme, UiServices& services, QWidget* parent)
    : QTextBrowser(parent), theme_(theme), document_(new SafeDescriptionDocument(services, this)) {
    setDocument(document_); setOpenLinks(false); setOpenExternalLinks(false); setSearchPaths({}); setReadOnly(true);
    setAcceptRichText(false); setFrameShape(QFrame::NoFrame); setFont(bodyFont()); setPalette(theme_.palette());
    viewport()->setAutoFillBackground(false); setAutoFillBackground(false);
    setVerticalScrollBar(new ScrollBar(theme_, Qt::Vertical, this));
    setHorizontalScrollBar(new ScrollBar(theme_, Qt::Horizontal, this)); setTextInteractionFlags(Qt::TextBrowserInteraction);
    connect(&theme_, &Theme::changed, this, [this] { setPalette(theme_.palette()); document_->setDefaultFont(bodyFont()); viewport()->update(); });
    connect(document_, &SafeDescriptionDocument::resourceUpdated, this, [this] { viewport()->update(); updateVisibleImages(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { updateVisibleImages(); });
    connect(this, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.isRelative() && url.toString().startsWith('#')) { scrollToAnchor(url.fragment()); return; }
        if (document_->isLinkApproved(url)) emit externalLinkRequested(document_->documentKey(), QString::fromLatin1(url.toEncoded()));
        else emit blockedLink(url);
    });
}
void SafeDescriptionBrowser::setBody(const QJsonObject& body) {
    document_->setBody(body); document_->setDefaultFont(bodyFont());
    QTimer::singleShot(0, this, [this] { updateVisibleImages(); });
}
void SafeDescriptionBrowser::releaseBody() { document_->releaseBody(); }
QVariant SafeDescriptionBrowser::loadResource(int type, const QUrl& url) { return document_->guardedResource(type, url); }
void SafeDescriptionBrowser::doSetSource(const QUrl& url, QTextDocument::ResourceType) { emit blockedLink(url); }
void SafeDescriptionBrowser::updateVisibleImages() {
    if (!isVisible()) { document_->setVisibleRange(-1, -1); return; }
    const int first = cursorForPosition(QPoint(0, -160)).position();
    const int last = cursorForPosition(QPoint(viewport()->width(), viewport()->height() + 160)).position();
    document_->setVisibleRange(first, last);
}
void SafeDescriptionBrowser::resizeEvent(QResizeEvent* event) { QTextBrowser::resizeEvent(event); updateVisibleImages(); }
void SafeDescriptionBrowser::showEvent(QShowEvent* event) { QTextBrowser::showEvent(event); updateVisibleImages(); }
void SafeDescriptionBrowser::hideEvent(QHideEvent* event) { document_->setVisibleRange(-1, -1); QTextBrowser::hideEvent(event); }
} // namespace nickel::ui