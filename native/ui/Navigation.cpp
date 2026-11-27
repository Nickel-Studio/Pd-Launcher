#include "Navigation.hpp"

namespace nickel::ui {
int Navigation::tabIndex() const {
    switch (route_) {
    case Route::Home: return 0;
    case Route::Resources: return 1;
    case Route::Settings: return 2;
    case Route::More: return 3;
    default: return -1;
    }
}
void Navigation::navigate(Route next) {
    if (next == route_) return;
    if (next == Route::DownloadDetail && route_ != Route::DownloadDetail) downloadOrigin_ = route_;
    route_ = next; emit routeChanged(route_);
}
void Navigation::returnFromDownloads() {
    navigate(downloadOrigin_ == Route::DownloadDetail ? Route::Resources : downloadOrigin_);
}
void Navigation::openVersionSettings(const InstanceIdentity& target) {
    if (!target.valid()) return;
    const bool sameRoute = route_ == Route::VersionSettings;
    versionTarget_ = target;
    if (sameRoute) emit versionTargetChanged();
    else navigate(Route::VersionSettings);
}
void Navigation::backToVersions() { versionTarget_ = {}; navigate(Route::Versions); }
void Navigation::openResourceMods(const InstanceIdentity& target) {
    if (!target.valid()) return;
    resourceTarget_ = target; memory(Route::Resources).section = "mod";
    clearDetails(); navigate(Route::Resources); emit resourceTargetChanged();
}
DetailFrame* Navigation::currentDetail() { return details_.isEmpty() ? nullptr : &details_.last(); }
DetailFrame* Navigation::detail(const QString& key) {
    for (auto& frame : details_) if (frame.key == key) return &frame;
    return nullptr;
}
void Navigation::pushDetail(DetailFrame frame) {
    if (frame.projectId.isEmpty() || frame.provider.isEmpty()) return;
    details_.push_back(std::move(frame)); emit detailChanged();
}
void Navigation::popDetail() { if (!details_.isEmpty()) { details_.pop_back(); emit detailChanged(); } }
void Navigation::clearDetails() { if (!details_.isEmpty()) { details_.clear(); emit detailChanged(); } }
QString routeKey(Route route) {
    static const char* keys[] = {"home", "instances", "versionSettings", "resources", "settings", "more", "downloads"};
    return QString::fromLatin1(keys[static_cast<int>(route)]);
}
} // namespace nickel::ui
