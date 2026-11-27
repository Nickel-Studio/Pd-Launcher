#include "LaunchError.hpp"
#include "ProductIdentity.hpp"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QPointer>
#include <QStringList>

namespace nickel::ui {
namespace {
QString briefReason(const QString& value) {
    QString first;
    for (const auto& line : value.split('\n')) { first = line.trimmed(); if (!first.isEmpty()) break; }
    for (auto& character : first) if (character.unicode() < 0x20 || character.unicode() == 0x7f) character = ' ';
    auto codepoints = first.toStdU32String();
    if (codepoints.size() > 240) { codepoints.resize(237); codepoints += U"..."; }
    return QString::fromUcs4(codepoints.data(), qsizetype(codepoints.size()));
}
QJsonObject failedScope(const QJsonObject& launch) {
    const auto target = launch.value("reportTarget").toObject();
    return {{"root", target.value("root").toString()}, {"versionId", target.value("versionId").toString()},
        {"launchId", target.value("launchId").toString()}};
}
}
// Launch-failure dialog. The failure key hashes target, error and diagnostic summary so one
// failure is shown once; report export is delegated to the service instead of reading logs here.
QString launchFailureKey(const QJsonObject& launch) {
    const auto diagnostic = launch.value("diagnostic").toObject();
    if (launch.value("error").toString().trimmed().isEmpty() && diagnostic.isEmpty()) return {};
    const QJsonObject identity{{"target", failedScope(launch)}, {"error", launch.value("error")},
        {"diagnostic", QJsonObject{{"category", diagnostic.value("category")}, {"severity", diagnostic.value("severity")},
            {"reason", diagnostic.value("reason")}, {"fallbackReason", diagnostic.value("fallbackReason")}}}};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
Dialog* showLaunchErrorDialog(Theme& theme, Bindings& bindings, const QJsonObject& launch, QWidget* parent) {
    const QString failureKey = launchFailureKey(launch); if (failureKey.isEmpty()) return nullptr;
    const QJsonObject diagnostic = launch.value("diagnostic").toObject(); const bool crashed = !diagnostic.isEmpty();
    const QJsonObject scope = failedScope(launch);
    const bool validTarget = !scope.value("root").toString().isEmpty() && !scope.value("versionId").toString().isEmpty()
        && !scope.value("launchId").toString().isEmpty();
    auto* dialog = new Dialog(theme, crashed ? "游戏意外退出了" : "游戏没能顺利启动", parent);
    dialog->setObjectName("launchErrorDialog"); dialog->resize(560, 380);
    dialog->setProperty("launchFailureKey", failureKey); dialog->setProperty("failedLaunchId", scope.value("launchId"));
    dialog->setProperty("failedRoot", scope.value("root")); dialog->setProperty("failedVersionId", scope.value("versionId"));
    dialog->body()->addWidget(textLabel(theme, "这次启动遇到了一点问题，先看看下面的简略原因。", 13, true, dialog));
    auto* reasonPanel = new Panel(theme, dialog, SurfaceKind::Fill); reasonPanel->setShadow(0);
    auto* reasonColumn = reasonPanel->column(16, 8); reasonColumn->addWidget(textLabel(theme, "简略原因", 12, true, reasonPanel));
    const QString fallback = briefReason(diagnostic.value("fallbackReason").toString());
    QString reason = briefReason(crashed ? diagnostic.value("reason").toString() : launch.value("error").toString());
    if (reason.isEmpty()) reason = fallback;
    if (reason.isEmpty()) reason = "暂时没有找到明确原因，请导出完整错误报告进一步检查。";
    auto* reasonLabel = textLabel(theme, reason, 13, false, reasonPanel); reasonLabel->setObjectName("launchFailureReason");
    reasonColumn->addWidget(reasonLabel); dialog->body()->addWidget(reasonPanel);
    if (!fallback.isEmpty() && fallback != reason) dialog->body()->addWidget(textLabel(theme, fallback, 13, true, dialog));
    dialog->body()->addWidget(textLabel(theme, "寻求帮助时，请导出错误报告并发送完整 ZIP 文件，不要只截图此窗口，也不要只发送其中一份日志。", 13, true, dialog));
    auto* exported = textLabel(theme, "完整报告已导出。请将保存的 ZIP 文件完整发送给帮助你的人，发送前可检查 Mod 写入的个人内容。", 12, true, dialog);
    exported->setObjectName("launchReportExported"); exported->hide(); dialog->body()->addWidget(exported);
    auto* logs = actionButton(theme, "打开日志目录", "folder", ButtonKind::Ghost, dialog); logs->setObjectName("openFailedLogs"); logs->setEnabled(validTarget);
    dialog->actions()->insertWidget(1, logs);
    auto* report = actionButton(theme, "导出错误报告 ZIP", "download", ButtonKind::Primary, dialog); report->setObjectName("exportLaunchReport"); report->setEnabled(validTarget);
    dialog->actions()->addWidget(report); dialog->setConfirmText("知道了"); dialog->confirmButton()->setKind(ButtonKind::Ghost);
    QObject::connect(dialog, &Dialog::confirmed, dialog, &QDialog::accept);
    QObject* commandOwner = parent ? static_cast<QObject*>(parent) : &bindings;
    QObject::connect(dialog, &QDialog::finished, commandOwner, [&bindings, commandOwner, scope, validTarget, owner = QPointer<Dialog>(dialog)] {
        if (validTarget && owner && !owner->property("obsoleteLaunchFailure").toBool())
            bindings.command(UiAction::ClearLaunchError, scope, commandOwner);
    });
    QObject::connect(logs, &QPushButton::clicked, dialog, [&bindings, dialog, logs, report, scope] {
        if (dialog->isBusy()) return;
        dialog->setBusy(true); logs->setBusy(true); report->setEnabled(false);
        bindings.command(UiAction::OpenPath, withFields(scope, {{"location", "logs"}}), dialog,
            [owner = QPointer<Dialog>(dialog), logs, report](const UiReply& reply) {
                if (!owner) return;
                logs->setBusy(false); report->setEnabled(true); owner->setBusy(false);
                if (reply.error.isError()) owner->showError(reply.error);
            });
    });
    QObject::connect(report, &QPushButton::clicked, dialog, [&bindings, dialog, report, logs, exported, scope] {
        if (dialog->isBusy()) return;
        exported->hide(); dialog->showError({}); dialog->setBusy(true); report->setBusy(true); logs->setEnabled(false);
        const QPointer<Dialog> owner(dialog);
        QString path = saveFile(dialog, "导出完整错误报告", errorReportFileName(), "ZIP (*.zip)");
        if (!owner) return;
        if (path.isEmpty()) { report->setBusy(false); logs->setEnabled(true); dialog->setBusy(false); return; }
        if (!path.endsWith(".zip", Qt::CaseInsensitive)) path += ".zip";
        bindings.command(UiAction::ExportErrorReport, withFields(scope, {{"path", path}}), dialog,
            [owner, report, logs, exported](const UiReply& reply) {
                if (!owner) return;
                report->setBusy(false); logs->setEnabled(true); owner->setBusy(false);
                if (reply.error.isError()) owner->showError(reply.error); else exported->show();
            });
    });
    dialog->open(); return dialog;
}
} // namespace nickel::ui
