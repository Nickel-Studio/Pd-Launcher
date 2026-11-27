#include "app/service/snapshots.hpp"

#include "BuildInfo.hpp"
#include "app/service/public_dto.hpp"

#include <QFile>
#include <QResource>

static void initialize_application_resources() {
    Q_INIT_RESOURCE(application_resources);
}

namespace nickel::app {

std::string build_version() {
    if (build::version.empty() || build::version.size() > 128) fail_static("integrity");
    return std::string(build::version);
}

Json build_changelog() {
    static const Json changelog = [] {
        initialize_application_resources();
        QFile source(QStringLiteral(":/nickel/application/changelog.json"));
        if (!source.open(QIODevice::ReadOnly) || source.size() > 256 * 1024) fail_static("integrity");
        const auto bytes = source.readAll();
        const auto value = Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
        if (!value.is_object() || value.value("version", std::string{}) != build_version()
            || !value.contains("sections") || !value["sections"].is_array()
            || value["sections"].size() > 64) fail_static("integrity");
        for (const auto& section : value["sections"]) {
            if (!section.is_object() || !section.contains("title") || !section["title"].is_string()
                || !section.contains("items") || !section["items"].is_array()
                || section["items"].size() > 256) fail_static("integrity");
            for (const auto& item : section["items"])
                if (!item.is_string()) fail_static("integrity");
        }
        return value;
    }();
    return changelog;
}

} // namespace nickel::app
