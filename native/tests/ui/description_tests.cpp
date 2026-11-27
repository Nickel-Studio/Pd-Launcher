#include "TestServices.hpp"
#include "ui/SafeDescription.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>

using namespace nickel::ui;
using namespace nickel::ui::tests;

namespace {
QJsonObject body(const QString& document, int count = 1) {
    QJsonArray images;
    QString html = "<p>Native safe body</p>";
    for (int i = 0; i < count; ++i) {
        const QString key = "nickel-image://" + document + "/" + QString::number(i);
        images.append(QJsonObject{{"key", key}, {"width", 160}, {"height", 90}});
        html += "<p><img src=\"" + key + "\" width=\"160\" height=\"90\" /></p>";
    }
    return {{"documentKey", document}, {"sanitizedHtml", html}, {"images", images},
        {"links", QJsonArray{QJsonObject{{"key", "nickel-link://" + document + "/0"}, {"label", "Project"}}}}};
}
class ProviderGuard final {
public:
    explicit ProviderGuard(int& calls) : previous_(QTextDocument::defaultResourceProvider()) {
        QTextDocument::setDefaultResourceProvider([&calls](const QUrl&) {
            ++calls; return QVariant(QByteArray("unexpected provider"));
        });
    }
    ~ProviderGuard() { QTextDocument::setDefaultResourceProvider(previous_); }
private:
    QTextDocument::ResourceProvider previous_;
};
}

class DescriptionTests final : public QObject {
    Q_OBJECT
private slots:
    void deniedResourcesNeverUseProviders() {
        int providers = 0; ProviderGuard guard(providers);
        TestServices services; SafeDescriptionDocument document(services);
        document.setBody(body("doc-a"));
        const QStringList urls{"https://127.0.0.1/private", "file:///C:/Windows/win.ini", "qrc:/nickel/brand/icon.png",
            "data:image/png;base64,AA==", "relative.png", "nickel-image://other/0", "nickel-image://doc-a/1"};
        for (const auto& text : urls) {
            const QVariant value = document.resource(QTextDocument::ImageResource, QUrl(text, QUrl::StrictMode));
            QVERIFY(value.isValid()); QVERIFY(value.canConvert<QImage>()); QVERIFY(!value.value<QImage>().isNull());
        }
        const auto style = document.resource(QTextDocument::StyleSheetResource, QUrl("https://127.0.0.1/style.css"));
        QVERIFY(style.isValid()); QCOMPARE(style.toByteArray(), QByteArray{});
        QCOMPARE(providers, 0); QVERIFY(services.imageRequests.isEmpty());
    }
    void localImageIsNotRead() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QImage secret(13, 7, QImage::Format_RGBA8888); secret.fill(Qt::red);
        const QString file = temp.filePath("secret.png"); QVERIFY(secret.save(file));
        TestServices services; SafeDescriptionDocument document(services); document.setBody(body("doc-a"));
        const auto denied = document.resource(QTextDocument::ImageResource, QUrl::fromLocalFile(file)).value<QImage>();
        QCOMPARE(denied.size(), QSize(1, 1)); QCOMPARE(denied.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(document.decodedBytes(), qsizetype(0));
    }
    void opaqueKeysMustBeExact() {
        QVERIFY(SafeDescriptionDocument::isOpaqueKey(QUrl("nickel-image://doc-a/0"), "nickel-image", "doc-a"));
        const QStringList invalid{"nickel-image://doc-a/0?x=1", "nickel-image://doc-a/0#x", "nickel-image://doc-a:80/0",
            "nickel-image://user@doc-a/0", "nickel-image://doc-a/-1", "nickel-image://doc-a/0/1", "https://doc-a/0"};
        for (const auto& key : invalid) QVERIFY(!SafeDescriptionDocument::isOpaqueKey(QUrl(key, QUrl::StrictMode), "nickel-image", "doc-a"));
    }
    void requestsAreLazyBoundedAndQueued() {
        TestServices services; SafeDescriptionDocument document(services); document.setBody(body("doc-a", 10));
        QVERIFY(services.imageRequests.isEmpty());
        document.setVisibleRange(0, document.characterCount());
        QCOMPARE(document.pendingCount(), qsizetype(4)); QCOMPARE(services.imageRequests.size(), qsizetype(4));
        const auto request = services.imageRequests.first();
        const QVariant pending = document.resource(QTextDocument::ImageResource, QUrl(request.opaqueKey));
        QVERIFY(pending.isValid()); QVERIFY(!pending.value<QImage>().isNull());
        QImage image(32, 16, QImage::Format_RGBA8888); image.fill(Qt::green);
        emit services.descriptionImageReady({request.id, request.documentKey, request.opaqueKey, image, {}});
        QCOMPARE(document.decodedBytes(), qsizetype(0));
        QTRY_COMPARE(document.decodedBytes(), image.sizeInBytes());
        QCOMPARE(document.pendingCount(), qsizetype(4)); QCOMPARE(services.imageRequests.size(), qsizetype(5));
        QCOMPARE(document.resource(QTextDocument::ImageResource, QUrl(request.opaqueKey)).value<QImage>(), image);
    }
    void hidingCancelsAndOldRepliesAreIgnored() {
        TestServices services; SafeDescriptionDocument document(services); document.setBody(body("doc-a"));
        document.setVisibleRange(0, document.characterCount()); const auto old = services.imageRequests.last();
        document.setVisibleRange(-1, -1); QCOMPARE(document.pendingCount(), qsizetype(0));
        QVERIFY(services.cancellations.contains(old.id));
        QImage image(16, 16, QImage::Format_RGBA8888); image.fill(Qt::red);
        emit services.descriptionImageReady({old.id, old.documentKey, old.opaqueKey, image, {}});
        QTest::qWait(10); QCOMPARE(document.decodedBytes(), qsizetype(0));
        document.setVisibleRange(0, document.characterCount());
        const auto resumed = services.imageRequests.last(); QVERIFY(resumed.id != old.id);
        document.setBody(body("doc-b")); QVERIFY(services.cancellations.contains(resumed.id));
        QVERIFY(services.releasedDocuments.contains("doc-a"));
        emit services.descriptionImageReady({resumed.id, resumed.documentKey, resumed.opaqueKey, image, {}});
        QTest::qWait(10); QCOMPARE(document.decodedBytes(), qsizetype(0)); QCOMPARE(document.documentKey(), QString("doc-b"));
    }
    void repeatingImmutableBodyRetainsLeaseAndCache() {
        TestServices services; SafeDescriptionDocument document(services); const auto projection = body("doc-a");
        document.setBody(projection); document.setVisibleRange(0, document.characterCount());
        const auto request = services.imageRequests.last(); QImage image(16, 8, QImage::Format_RGBA8888); image.fill(Qt::green);
        emit services.descriptionImageReady({request.id, request.documentKey, request.opaqueKey, image, {}});
        QTRY_COMPARE(document.decodedBytes(), image.sizeInBytes());
        document.setBody(projection); document.setVisibleRange(0, document.characterCount());
        QVERIFY(services.releasedDocuments.isEmpty()); QCOMPARE(services.imageRequests.size(), qsizetype(1));
        QCOMPARE(document.resource(QTextDocument::ImageResource, QUrl(request.opaqueKey)).value<QImage>(), image);
        document.setVisibleRange(-1, -1); document.setVisibleRange(0, document.characterCount());
        QCOMPARE(document.decodedBytes(), image.sizeInBytes()); QCOMPARE(services.imageRequests.size(), qsizetype(1));
        document.releaseBody(); QCOMPARE(services.releasedDocuments.count("doc-a"), 1);
        document.releaseBody(); QCOMPARE(services.releasedDocuments.count("doc-a"), 1);
    }
    void sameDocumentWithDifferentHtmlFailsClosed() {
        TestServices services; SafeDescriptionDocument document(services); document.setBody(body("doc-a"));
        document.setVisibleRange(0, document.characterCount()); const auto request = services.imageRequests.last();
        auto changed = body("doc-a"); changed.insert("sanitizedHtml", "<p>Changed immutable body</p>");
        document.setBody(changed); QVERIFY(document.documentKey().isEmpty()); QCOMPARE(document.decodedBytes(), qsizetype(0));
        QVERIFY(services.cancellations.contains(request.id)); QCOMPARE(services.releasedDocuments.count("doc-a"), 1);
        QVERIFY(!document.toPlainText().contains("Changed immutable body"));
    }
    void mismatchedReplyCannotConsumePendingRequest() {
        TestServices services; SafeDescriptionDocument document(services); document.setBody(body("doc-a"));
        document.setVisibleRange(0, document.characterCount()); const auto request = services.imageRequests.last();
        QImage image(16, 8, QImage::Format_RGBA8888); image.fill(Qt::green);
        emit services.descriptionImageReady({request.id + 1, request.documentKey, request.opaqueKey, image, {}});
        emit services.descriptionImageReady({request.id, "doc-b", request.opaqueKey, image, {}});
        emit services.descriptionImageReady({request.id, request.documentKey, "nickel-image://doc-a/1", image, {}});
        QTest::qWait(10); QCOMPARE(document.pendingCount(), qsizetype(1)); QCOMPARE(document.decodedBytes(), qsizetype(0));
        emit services.descriptionImageReady({request.id, request.documentKey, request.opaqueKey, image, {}});
        QTRY_COMPARE(document.pendingCount(), qsizetype(0)); QCOMPARE(document.decodedBytes(), image.sizeInBytes());
    }
    void failedImageKeepsValidPlaceholder() {
        TestServices services; SafeDescriptionDocument document(services); document.setBody(body("doc-a"));
        document.setVisibleRange(0, document.characterCount()); const auto request = services.imageRequests.last();
        emit services.descriptionImageReady({request.id, request.documentKey, request.opaqueKey, {}, {"denied", "Denied", {}, false}});
        QTRY_COMPARE(document.pendingCount(), qsizetype(0));
        const auto result = document.resource(QTextDocument::ImageResource, QUrl(request.opaqueKey));
        QVERIFY(result.isValid()); QVERIFY(!result.value<QImage>().isNull()); QCOMPARE(document.decodedBytes(), qsizetype(0));
        document.setVisibleRange(0, document.characterCount()); QCOMPARE(services.imageRequests.size(), qsizetype(1));
    }
    void browserNeverLoadsSourcesAndLinksStayOpaque() {
        Theme theme; TestServices services; SafeDescriptionBrowser browser(theme, services); browser.setBody(body("doc-a"));
        QSignalSpy opened(&browser, &SafeDescriptionBrowser::externalLinkRequested);
        QSignalSpy blocked(&browser, &SafeDescriptionBrowser::blockedLink);
        const QString previous = browser.toPlainText();
        browser.setSource(QUrl("file:///C:/Windows/win.ini"));
        QCOMPARE(blocked.count(), 1); QCOMPARE(browser.toPlainText(), previous);
        emit browser.anchorClicked(QUrl("https://127.0.0.1/private")); QCOMPARE(opened.count(), 0);
        emit browser.anchorClicked(QUrl("nickel-link://doc-a/0")); QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.first().at(0).toString(), QString("doc-a"));
        QCOMPARE(opened.first().at(1).toString(), QString("nickel-link://doc-a/0"));
        QVERIFY(services.commands.isEmpty());
    }
    void releaseOnDestruction() {
        TestServices services; auto* document = new SafeDescriptionDocument(services); document->setBody(body("doc-a"));
        document->setVisibleRange(0, document->characterCount()); const auto request = services.imageRequests.last();
        delete document; QVERIFY(services.cancellations.contains(request.id)); QCOMPARE(services.releasedDocuments.count("doc-a"), 1);
    }
};
QTEST_MAIN(DescriptionTests)
#include "description_tests.moc"
