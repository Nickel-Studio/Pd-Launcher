#pragma once

#include "PageShell.hpp"
#include <QDialog>
#include <functional>

namespace nickel::ui {

class Dialog final : public QDialog {
    Q_OBJECT
public:
    Dialog(Theme& theme, const QString& title, QWidget* parent = nullptr);
    QVBoxLayout* body() const { return body_; }
    QHBoxLayout* actions() const { return actions_; }
    Button* confirmButton() const { return confirm_; }
    void setBusy(bool busy);
    bool isBusy() const { return busy_; }
    void showError(const UiError& error);
    void setConfirmText(const QString& text);
    void reject() override;

signals:
    void confirmed();
    void busyChanged(bool busy);

protected:
    void paintEvent(QPaintEvent*) override;
    void closeEvent(QCloseEvent*) override;

private:
    Theme& theme_;
    QVBoxLayout* body_;
    QHBoxLayout* actions_;
    QLabel* error_;
    Button* confirm_;
    Button* cancel_;
    bool busy_ = false;
};

void confirmAction(Theme& theme, QWidget* parent, const QString& title, const QString& message, std::function<void()> action);
void promptText(Theme& theme, QWidget* parent, const QString& title, const QString& label, const QString& initial, std::function<void(const QString&, Dialog*)> action);
QString pickDirectory(QWidget* parent, const QString& title);
QStringList pickFiles(QWidget* parent, const QString& title, const QString& filter);
QString saveFile(QWidget* parent, const QString& title, const QString& proposed, const QString& filter);

} // namespace nickel::ui
