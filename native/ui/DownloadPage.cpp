#include "Pages.hpp"
#include "Dialogs.hpp"
#include "ListModel.hpp"
#include "ProductIdentity.hpp"

#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <cmath>

namespace nickel::ui {
namespace {
// Read-only projection of download tasks. Cancel/dismiss/export are explicit service commands;
// progress and task state are never mutated locally.
class DownloadPage final : public RoutePage {
public:
    DownloadPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent)
        : RoutePage(theme, bindings, navigation, parent) {
        setObjectName("downloadDetailPage"); auto* column = pageColumn(this, 16); auto* head = new QWidget(this); auto* headRow = rowLayout(head, 12);
        auto* back = actionButton(theme, "返回", "back", ButtonKind::Ghost, head); headRow->addWidget(back); headRow->addWidget(textLabel(theme, "下载详情", 19)); headRow->addStretch(); count_ = textLabel(theme, {}, 11, true); headRow->addWidget(count_);
        auto* refresh = actionButton(theme, "刷新", "refresh", ButtonKind::Secondary, head); headRow->addWidget(refresh); column->addWidget(head);
        auto* main = new QWidget(this); mainRow_ = rowLayout(main, 24); auto* sideHost = new QWidget(main); auto* sideColumn = pageColumn(sideHost, 20); side_ = sideHost;
        auto* stats = new Panel(theme, sideHost); stats->setObjectName("downloadOverview"); auto* statsColumn = stats->column(20, 24);
        const QStringList names{"总进度", "下载速度", "剩余文件"};
        for (int i = 0; i < 3; ++i) { auto* cell = new QWidget(stats); auto* cellColumn = pageColumn(cell, 8); cellColumn->addWidget(textLabel(theme, names.at(i), 11, true)); values_[size_t(i)] = textLabel(theme, "—", 24); values_[size_t(i)]->setFont(monoFont(24)); cellColumn->addWidget(values_[size_t(i)]); statsColumn->addWidget(cell); }
        sideColumn->addWidget(stats); sideColumn->addStretch(); mainRow_->addWidget(sideHost, 3);
        auto* right = new QWidget(main); auto* rightColumn = pageColumn(right, 12); alert_ = textLabel(theme, {}, 12, true); alert_->hide(); rightColumn->addWidget(alert_);
        auto* tools = new QWidget(right); auto* toolsRow = rowLayout(tools, 8); auto* copy = actionButton(theme, "复制详情", {}, ButtonKind::Ghost, tools); auto* report = actionButton(theme, "导出错误报告", "download", ButtonKind::Ghost, tools); toolsRow->addWidget(copy); toolsRow->addWidget(report); toolsRow->addStretch(); rightColumn->addWidget(tools);
        state_ = new QStackedWidget(right); model_ = new ListModel(ListKind::Downloads, this); view_ = new ListView(theme, ListKind::Downloads, state_); view_->setObjectName("downloadTasks"); view_->setSource(model_);
        loading_ = new LoadingCard(theme, "正在读取下载状态…", state_); empty_ = emptyNotice(theme, "没有正在进行的任务", "下载或安装任务开始后会显示在这里。", state_); state_->addWidget(view_); state_->addWidget(loading_); state_->addWidget(empty_); state_->setCurrentWidget(loading_); rightColumn->addWidget(state_, 1); mainRow_->addWidget(right, 7); column->addWidget(main, 1);
        connect(back, &QPushButton::clicked, this, [this] { navigation_.returnFromDownloads(); }); connect(refresh, &QPushButton::clicked, this, [this] { command(UiAction::RefreshDownloads, {}, [this](const UiReply& reply) { reportError(reply.error); reload(); }); });
        connect(copy, &QPushButton::clicked, this, [this] { const auto item = view_->currentObject(); if (item.isEmpty()) return; QString text = item.value("title").toString() + "\n" + item.value("message").toString(); for (const auto& stage : item.value("stages").toArray()) text += "\n" + stage.toObject().value("name").toString() + " " + stage.toObject().value("detail").toString(); QApplication::clipboard()->setText(text); });
        connect(report, &QPushButton::clicked, this, [this, report] {
            const auto item = view_->currentObject(); if (item.isEmpty() || !report->isEnabled()) return;
            QString path = saveFile(this, "导出完整错误报告", errorReportFileName(), "ZIP (*.zip)");
            if (path.isEmpty()) return;
            if (!path.endsWith(".zip", Qt::CaseInsensitive)) path += ".zip";
            report->setBusy(true);
            command(UiAction::ExportErrorReport, withFields(item.value("reportTarget").toObject(), {{"taskId", item.value("id")}, {"path", path}}),
                [this, report](const UiReply& reply) { report->setBusy(false); reportError(reply.error); });
        });
        connect(view_->nickelDelegate(), &ListDelegate::actionRequested, this, [this](const QString& action, const QJsonObject& item) { if (action == "cancel") command(UiAction::CancelDownload, {{"id", item.value("id")}}); else if (action == "dismiss") command(UiAction::DismissDownload, {{"id", item.value("id")}}); });
    }
    void enter() override { reload(); }
    void snapshot(UiTopic topic, const QJsonObject& state) override { if (topic == UiTopic::Downloads) { invalidate("downloads"); apply(state); } }
private:
    void reload() { query(UiQuery::Downloads, {}, "downloads", [this](const UiReply& reply) { if (reply.error.isError()) { loading_->setError(true); loading_->setLabel(reply.error.message); reportError(reply.error); } else apply(reply.data.toObject()); }); }
    void reportError(const UiError& error) { if (error.isError()) { alert_->setText(error.message); alert_->show(); report(error); } }
    void apply(const QJsonObject& state) {
        const auto items = state.value("items").toArray(); view_->preserveSelection([this, items] { model_->setItems(items); }); count_->setText(QString::number(items.size()) + " 个任务");
        if (state.value("ready").isBool() && !state.value("ready").toBool()) state_->setCurrentWidget(loading_); else state_->setCurrentWidget(items.isEmpty() ? empty_ : view_);
        side_->setVisible(!items.isEmpty()); const auto overview = state.value("overview").toObject();
        values_[0]->setText(overview.value("percent").isDouble() ? QString::number(overview.value("percent").toDouble(), 'f', 1) + "%" : "—");
        values_[1]->setText(overview.value("speedBps").isDouble() ? formatBytes(overview.value("speedBps").toDouble()) + "/s" : "—");
        values_[2]->setText(overview.value("remainingFiles").isDouble() ? QString::number(overview.value("remainingFiles").toInteger()) : "—");
        const QString error = state.value("error").toString(); alert_->setText(error); alert_->setVisible(!error.isEmpty());
    }
    QLabel *count_, *alert_; std::array<QLabel*, 3> values_; QWidget* side_; QHBoxLayout* mainRow_; ListModel* model_; ListView* view_; QStackedWidget* state_; LoadingCard* loading_; QWidget* empty_;
};
}
RoutePage* createDownloadDetailPage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent) { return new DownloadPage(theme, bindings, navigation, parent); }
} // namespace nickel::ui
