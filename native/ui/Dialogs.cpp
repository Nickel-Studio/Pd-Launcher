#include "Dialogs.hpp"

#include <QCloseEvent>
#include <QFileDialog>
#include <QPainter>
#include <QTimer>

namespace nickel::ui {
Dialog::Dialog(Theme& theme, const QString& title, QWidget* parent)
    : QDialog(parent), theme_(theme), body_(new QVBoxLayout), actions_(new QHBoxLayout), error_(textLabel(theme, {}, 12, true)),
      confirm_(actionButton(theme, "确定", {}, ButtonKind::Primary)), cancel_(actionButton(theme, "取消", {}, ButtonKind::Ghost)) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint); setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose); setWindowModality(Qt::WindowModal); setWindowTitle(title);
    setMinimumWidth(420); resize(460, 240); setFont(bodyFont()); setPalette(theme_.palette());
    auto* column = new QVBoxLayout(this); column->setContentsMargins(24, 24, 24, 20); column->setSpacing(16);
    column->addWidget(textLabel(theme, title, 19)); body_->setContentsMargins(0, 0, 0, 0); body_->setSpacing(12); column->addLayout(body_, 1);
    error_->setObjectName("dialogError"); error_->hide(); column->addWidget(error_);
    actions_->setContentsMargins(0, 0, 0, 0); actions_->setSpacing(8); actions_->addStretch(); actions_->addWidget(cancel_); actions_->addWidget(confirm_); column->addLayout(actions_);
    confirm_->setDefault(true); connect(confirm_, &QPushButton::clicked, this, &Dialog::confirmed);
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    connect(&theme_, &Theme::changed, this, [this] { setPalette(theme_.palette()); update(); });
}
void Dialog::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy; confirm_->setBusy(busy); cancel_->setEnabled(!busy);
    emit busyChanged(busy);
}
void Dialog::showError(const UiError& error) { error_->setText(error.message + (error.detail.isEmpty() ? QString{} : "\n" + error.detail)); error_->setVisible(error.isError()); setBusy(false); }
void Dialog::setConfirmText(const QString& text) { confirm_->setText(text); }
void Dialog::paintEvent(QPaintEvent*) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(Qt::NoPen); painter.setBrush(theme_.tokens().surface2);
    painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 18, 18);
}
void Dialog::reject() { if (!busy_) QDialog::reject(); }
void Dialog::closeEvent(QCloseEvent* event) { if (busy_) event->ignore(); else QDialog::closeEvent(event); }
void confirmAction(Theme& theme, QWidget* parent, const QString& title, const QString& message, std::function<void()> action) {
    auto* dialog = new Dialog(theme, title, parent); dialog->body()->addWidget(textLabel(theme, message));
    QObject::connect(dialog, &Dialog::confirmed, dialog, [dialog, action = std::move(action)] {
        dialog->accept(); action();
    });
    dialog->open();
}
void promptText(Theme& theme, QWidget* parent, const QString& title, const QString& label, const QString& initial,
                std::function<void(const QString&, Dialog*)> action) {
    auto* dialog = new Dialog(theme, title, parent); dialog->body()->addWidget(textLabel(theme, label, 12, true));
    auto* input = new LineEdit(theme, dialog); input->setObjectName("dialogInput"); input->setAccessibleName(label);
    input->setText(initial); dialog->body()->addWidget(input);
    QObject::connect(input, &QLineEdit::textChanged, dialog, [dialog](const QString& text) {
        dialog->confirmButton()->setEnabled(!text.trimmed().isEmpty());
    });
    dialog->confirmButton()->setEnabled(!initial.trimmed().isEmpty());
    QObject::connect(dialog, &Dialog::confirmed, dialog, [dialog, input, action = std::move(action)] {
        if (!input->text().trimmed().isEmpty()) action(input->text().trimmed(), dialog);
    });
    dialog->open(); input->setFocus(); input->selectAll();
}
QString pickDirectory(QWidget* parent, const QString& title) { return QFileDialog::getExistingDirectory(parent, title, {}, QFileDialog::ShowDirsOnly); }
QStringList pickFiles(QWidget* parent, const QString& title, const QString& filter) { return QFileDialog::getOpenFileNames(parent, title, {}, filter); }
QString saveFile(QWidget* parent, const QString& title, const QString& proposed, const QString& filter) { return QFileDialog::getSaveFileName(parent, title, proposed, filter); }
} // namespace nickel::ui
