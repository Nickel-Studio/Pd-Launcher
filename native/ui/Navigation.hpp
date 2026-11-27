#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <array>

namespace nickel::ui {

enum class Route { Home, Versions, VersionSettings, Resources, Settings, More, DownloadDetail };
struct InstanceIdentity {
    QString root, versionId;
    [[nodiscard]] bool valid() const { return !root.trimmed().isEmpty() && !versionId.trimmed().isEmpty(); }
    [[nodiscard]] QJsonObject scope() const { return {{"root", root}, {"versionId", versionId}}; }
    bool operator==(const InstanceIdentity&) const = default;
};
struct DetailFrame {
    QString key, provider, projectId, kind;
    int scroll = 0;
    int descriptionScroll = 0;
    int versionsScroll = 0;
    QString selectedVersionId;
    QJsonObject filters;
    QStringList openGroups;
    QJsonObject dependency;
};
// Per-route scroll/filter memory survives section switches; details_ is a stack so a nested
// project page returns to the exact predecessor instead of a hardcoded parent route.
struct PageMemory { QString section; int scroll = 0; QJsonObject filters; QString selectedId; };

class Navigation final : public QObject {
    Q_OBJECT
public:
    explicit Navigation(QObject* parent = nullptr) : QObject(parent) {}
    Route route() const { return route_; }
    int tabIndex() const;
    void navigate(Route route);
    void returnFromDownloads();
    void openVersionSettings(const InstanceIdentity& target);
    void backToVersions();
    void openResourceMods(const InstanceIdentity& target);
    const InstanceIdentity& versionTarget() const { return versionTarget_; }
    const InstanceIdentity& resourceTarget() const { return resourceTarget_; }
    PageMemory& memory(Route route) { return memory_.at(static_cast<size_t>(route)); }
    const QVector<DetailFrame>& details() const { return details_; }
    DetailFrame* currentDetail();
    DetailFrame* detail(const QString& key);
    void pushDetail(DetailFrame frame);
    void popDetail();
    void clearDetails();

signals:
    void routeChanged(nickel::ui::Route route);
    void detailChanged();
    void versionTargetChanged();
    void resourceTargetChanged();

private:
    Route route_ = Route::Home;
    Route downloadOrigin_ = Route::Home;
    InstanceIdentity versionTarget_, resourceTarget_;
    std::array<PageMemory, 7> memory_;
    QVector<DetailFrame> details_;
};

QString routeKey(Route route);

} // namespace nickel::ui
Q_DECLARE_METATYPE(nickel::ui::Route)
