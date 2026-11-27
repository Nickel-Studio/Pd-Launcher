#include "DownloadBubble.hpp"
#include "ListModel.hpp"
#include <QJsonArray>

namespace nickel::ui {
DownloadBubble::DownloadBubble(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent)
    : Panel(theme, parent, SurfaceKind::Glass), label_(textLabel(theme, {}, 12, true, this)),
      open_(actionButton(theme, "下载详情", "downloadTray", ButtonKind::Ghost, this)),
      dismiss_(actionButton(theme, {}, "close", ButtonKind::Ghost, this)) {
    setObjectName("downloadBubble"); setAccessibleName("全局下载状态"); setRadius(18); setShadow(2);
    auto* row = rowLayout(this, 8); row->setContentsMargins(16, 10, 10, 10);
    row->addWidget(label_, 1); row->addWidget(open_); row->addWidget(dismiss_);
    dismiss_->setFixedSize(28, 28); dismiss_->setAccessibleName("收起下载状态");
    connect(open_, &QPushButton::clicked, this, [&navigation] { navigation.navigate(Route::DownloadDetail); });
    connect(dismiss_, &QPushButton::clicked, this, [this, &bindings] { bindings.command(UiAction::DismissDownloadBubble, {}, this); });
    hide();
}
void DownloadBubble::apply(const QJsonObject& snapshot) {
    const auto items = snapshot.value("items").toArray(); int active = 0;
    for (const auto& value : items) {
        const auto status = value.toObject().value("status").toString();
        if (status == "queued" || status == "running" || status == "downloading" || status == "installing" || status == "cancelling") ++active;
    }
    const auto overview = snapshot.value("overview").toObject(); QString text = QString::number(active) + " 个任务";
    if (overview.value("percent").isDouble()) text += " · " + QString::number(overview.value("percent").toDouble(), 'f', 0) + "%";
    if (overview.value("speedBps").isDouble()) text += " · " + formatBytes(overview.value("speedBps").toDouble()) + "/s";
    const QString error = snapshot.value("startError").toString().isEmpty() ? snapshot.value("error").toString() : snapshot.value("startError").toString();
    label_->setText(error.isEmpty() ? text : error);
    setVisible(!snapshot.value("bubbleDismissed").toBool() && (active > 0 || !error.isEmpty()));
}
} // namespace nickel::ui
