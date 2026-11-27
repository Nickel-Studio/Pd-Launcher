// SPDX-License-Identifier: AGPL-3.0-only
#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <stdexcept>

namespace {
QByteArray read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error((QStringLiteral("Cannot read resource: ") + path).toStdString());
    }
    return file.readAll();
}

void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}

QByteArray sha256(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

void verifyImmutable(const QString& root, const QString& brandRoot) {
    const QByteArray baseline = read(root + QStringLiteral("/tests/brand/functional-icons.sha256"));
    QByteArray normalized = baseline;
    normalized.replace("\r\n", "\n");
    require(sha256(normalized) == "585116fbc8852f10c649d64aabd74a19ea44b33b1c994ed703ed6bfbbabdbb8d",
            "The historical 18-hash baseline was edited");
    int count = 0;
    for (const QByteArray& line : baseline.split('\n')) {
        const QByteArray row = line.trimmed();
        if (row.isEmpty()) {
            continue;
        }
        const int separator = row.indexOf("  ");
        require(separator == 64, "Malformed immutable asset hash entry");
        const QString original = QString::fromUtf8(row.mid(separator + 2));
        const QByteArray expected = row.left(separator);
        require(sha256(read(root + QLatin1Char('/') + original)) == expected, "An old functional image changed");
        const QString alias = QStringLiteral(":/nickel/functional/") + original.mid(QStringLiteral("src/assets/").size());
        require(sha256(read(alias)) == expected, "Qt qrc changed functional image bytes");
        ++count;
    }
    require(count == 18, "All 18 historical functional images must be checked");
    QByteArray fixtureBytes = read(brandRoot + QStringLiteral("/native/tests/brand/functional-map.json"));
    fixtureBytes.replace("\r\n", "\n");
    require(sha256(fixtureBytes) == "95517ca822a7acafe750eaed93503e25dbee9f17f50e8248b1024c662c1dcdc4",
            "The immutable functional mapping contract was edited");
    const QJsonObject fixture = QJsonDocument::fromJson(fixtureBytes).object();
    const QJsonObject additional = fixture.value(QStringLiteral("additionalImmutable")).toObject();
    require(additional.size() == 5, "All default skin and credit hashes must be checked");
    for (auto it = additional.begin(); it != additional.end(); ++it) {
        require(sha256(read(root + QLatin1Char('/') + it.key())) == it.value().toString().toLatin1(), "Default skin or external mark changed");
        const QString alias = QStringLiteral(":/nickel/functional/") + it.key().mid(QStringLiteral("src/assets/").size());
        require(sha256(read(alias)) == it.value().toString().toLatin1(), "Qt qrc changed skin or external mark bytes");
    }
    const QJsonObject icons = fixture.value(QStringLiteral("iconKeys")).toObject();
    require(icons.size() == 12, "All original instance icon keys must be checked");
    for (auto it = icons.begin(); it != icons.end(); ++it) {
        require(read(QStringLiteral(":/nickel/functional/icons/") + it.key() + QStringLiteral(".png"))
                    == read(root + QLatin1Char('/') + it.value().toString()), "Native icon key mapping differs from the old UI");
    }
    require(read(QStringLiteral(":/nickel/functional/mod-placeholder.png"))
                == read(root + QLatin1Char('/') + fixture.value(QStringLiteral("defaultMod")).toString()), "Default Mod artwork changed");
    require(read(QStringLiteral(":/nickel/functional/icons/optifabric.png"))
                == read(root + QLatin1Char('/') + fixture.value(QStringLiteral("optifabricDedicated")).toString()), "OptiFabric dedicated badge changed");
}

void verifyBrand(const QString& root, const QString& oracleRoot) {
    const QString directory = root + QStringLiteral("/native/assets/brand/");
    const QJsonObject manifest = QJsonDocument::fromJson(read(directory + QStringLiteral("asset-manifest.json"))).object();
    require(manifest.value(QStringLiteral("product")).toString() == QStringLiteral("Palladium")
                && manifest.value(QStringLiteral("shortName")).toString() == QStringLiteral("Pd"), "Current brand is not Palladium/Pd");
    require(manifest.value(QStringLiteral("sourceSha256")).toString().toLatin1()
                == "685dc8657abbd73d0bb458dd90ea7507bf52a40f5c6129028c3bea78a911a066", "Palladium intake source changed");
    require(sha256(read(directory + manifest.value(QStringLiteral("preferredSource")).toString()))
                == manifest.value(QStringLiteral("sourceSha256")).toString().toLatin1(), "Palladium source inventory hash differs");
    require(manifest.value(QStringLiteral("artLicense")).toString() == QStringLiteral("CC-BY-SA-4.0")
                && manifest.value(QStringLiteral("artLicenseConfirmed")).toBool()
                && manifest.value(QStringLiteral("sourceCopyright")).toString() == QStringLiteral("Nickel Studio (ItzArona, ItzPlana)")
                && manifest.value(QStringLiteral("sourceLicenseDeclared")).toString() == QStringLiteral("CC-BY-SA-4.0"),
            "Palladium artwork must ship under CC-BY-SA-4.0 with attribution");
    require(manifest.value(QStringLiteral("qtResourceFiles")).toArray().size() == 20, "Palladium desktop resource subset changed");
    int embeddedImages = 0;
    for (const QJsonValue& entry : manifest.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject asset = entry.toObject();
        const QString path = asset.value(QStringLiteral("path")).toString();
        const QByteArray data = read(directory + path);
        require(sha256(data) == asset.value(QStringLiteral("sha256")).toString().toLatin1(), "Current brand asset hash mismatch");
        const QString alias = QStringLiteral(":/nickel/brand/") + path;
        if (asset.value(QStringLiteral("qtEmbedded")).toBool()) {
            require(read(alias) == data, "Compiled Palladium resource bytes differ");
            if (asset.value(QStringLiteral("format")).toString() == QStringLiteral("PNG")) ++embeddedImages;
        } else {
            require(!QFile::exists(alias), "Non-desktop brand material was embedded");
        }
        if (asset.value(QStringLiteral("format")).toString() == QStringLiteral("PNG")) {
            const QImage image = QImage::fromData(data, "PNG");
            require(!image.isNull(), "Supplied Palladium PNG does not decode through Qt");
            require(image.width() == asset.value(QStringLiteral("width")).toInt()
                        && image.height() == asset.value(QStringLiteral("height")).toInt(), "Supplied PNG dimensions differ");
        }
    }
    require(embeddedImages == 16, "All 16 desktop Palladium PNG aliases must be verified");
    require(manifest.value(QStringLiteral("runtimePngCompressedBytes")).toInt() == 269306
                && manifest.value(QStringLiteral("runtimePngDecodedBytes")).toInt() == 41549824, "Runtime artwork size accounting changed");
    require(read(directory + QStringLiteral("fonts/nickel-line.woff2"))
                == read(oracleRoot + QStringLiteral("/src/assets/fonts/nickel-line.woff2")), "Existing Noto subset changed");
    require(!QFile::exists(QStringLiteral(":/nickel/brand/fonts/nickel-line.woff2")), "Preferred-form font must not inflate desktop resources");
    require(!QFile::exists(QStringLiteral(":/nickel/brand/banners/palladium-launcher-hero-light.png")), "Website banners must not inflate desktop resources");
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral(":/nickel/brand/fonts/nickel-text-subset.ttf"));
    require(fontId >= 0 && !QFontDatabase::applicationFontFamilies(fontId).isEmpty(), "Qt cannot load the retained text font");
    QFontDatabase::removeApplicationFont(fontId);
    require(read(QStringLiteral(":/nickel/brand/fonts/OFL-1.1.txt")).contains("Reserved Font Name 'Source'"), "Font copyright must be embedded");
    const QByteArray brandNotice = read(QStringLiteral(":/nickel/brand/NOTICE.txt"));
    require(brandNotice.contains("Creative Commons Attribution-ShareAlike 4.0") && brandNotice.contains("CC-BY-SA-4.0"),
            "Brand NOTICE must carry the CC-BY-SA-4.0 attribution");
}
} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QGuiApplication application(argc, argv);
    try {
        const QString brandRoot = QString::fromUtf8(NICKEL_BRAND_CHECKOUT);
        const QString oracleRoot = QString::fromUtf8(ASSET_ORACLE_ROOT);
        verifyImmutable(oracleRoot, brandRoot);
        verifyBrand(brandRoot, oracleRoot);
        QTextStream(stdout) << "Palladium brand Qt resources: original functional hashes/mapping, supplied PNG bytes, font and CC-BY-SA-4.0 notice PASS\n";
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
}
