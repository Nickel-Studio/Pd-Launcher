#pragma once

#include "Theme.hpp"
#include "UiServices.hpp"
#include <QHash>
#include <QSet>
#include <QTextBrowser>
#include <QTextDocument>

namespace nickel::ui {

// Renders only service-owned SafeBody projections: links and images are allowlisted per document,
// and denied/pending resources terminate Qt's fallback chain instead of loading anything else.
class SafeDescriptionDocument final : public QTextDocument {
    Q_OBJECT
public:
    explicit SafeDescriptionDocument(UiServices& services, QObject* parent = nullptr);
    ~SafeDescriptionDocument() override;
    void setBody(const QJsonObject& safeBody);
    void releaseBody();
    void setVisibleRange(int firstPosition, int lastPosition);
    QVariant guardedResource(int type, const QUrl& url);
    QString documentKey() const { return documentKey_; }
    bool isLinkApproved(const QUrl& url) const;
    qsizetype deniedCount() const { return deniedCount_; }
    qsizetype pendingCount() const { return pending_.size(); }
    qsizetype decodedBytes() const { return decodedBytes_; }
    static bool isOpaqueKey(const QUrl& url, const QString& scheme, const QString& documentKey);
    static QVariant placeholder(int type);

signals:
    void resourceUpdated();
    void resourceDenied(const QUrl& url);

protected:
    QVariant loadResource(int type, const QUrl& url) override;

private:
    struct PendingImage { QString key; quint64 generation; };
    void cancelPendingImages();
    void requestVisibleImages();
    QString urlKey(const QUrl& url) const;
    UiServices& services_;
    QString documentKey_, sourceHtml_;
    QSet<QString> approvedImages_, approvedLinks_, requested_;
    QHash<RequestId, PendingImage> pending_;
    QHash<QString, QImage> images_;
    int firstVisible_ = -1, lastVisible_ = -1;
    qsizetype deniedCount_ = 0;
    qsizetype decodedBytes_ = 0;
    quint64 generation_ = 0;
};

class SafeDescriptionBrowser final : public QTextBrowser {
    Q_OBJECT
public:
    SafeDescriptionBrowser(Theme& theme, UiServices& services, QWidget* parent = nullptr);
    void setBody(const QJsonObject& safeBody);
    void releaseBody();
    SafeDescriptionDocument* safeDocument() const { return document_; }

signals:
    void externalLinkRequested(const QString& documentKey, const QString& opaqueKey);
    void blockedLink(const QUrl& url);

protected:
    QVariant loadResource(int type, const QUrl& url) override;
    void doSetSource(const QUrl& url, QTextDocument::ResourceType type) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updateVisibleImages();
    Theme& theme_;
    SafeDescriptionDocument* document_;
};

} // namespace nickel::ui