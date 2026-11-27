#pragma once

#include "Bindings.hpp"
#include "Controls.hpp"
#include "Navigation.hpp"
#include <QHash>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <functional>

namespace nickel::ui {

struct NavItem { QString id, label, icon, group; };

// Page base for one route. Named query channels carry a generation token: re-querying or
// leaving the page invalidates the old channel, so a late reply cannot overwrite newer state.
class RoutePage : public QWidget {
    Q_OBJECT
public:
    RoutePage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent = nullptr);
    ~RoutePage() override;
    virtual void enter();
    virtual void leave();
    virtual void snapshot(UiTopic topic, const QJsonObject& state);

signals:
    void errorReported(const nickel::ui::UiError& error);

protected:
    void query(UiQuery query, const QJsonObject& scope, const QString& channel, Bindings::Callback callback);
    void command(UiAction action, const QJsonObject& args, Bindings::Callback callback = {});
    void invalidate(const QString& channel);
    void cancelQueries();
    void hideEvent(QHideEvent* event) override;
    void report(const UiError& error);
    Theme& theme_;
    Bindings& bindings_;
    Navigation& navigation_;
    QHash<QString, quint64> generations_;
    QHash<QString, RequestId> queryRequests_;
};

// Sidebar plus stacked content. addPage() wraps the page in a ScrollArea unless scroll is
// false, which is meant for pages that already own their scrolling (for example a list).
class Pager final : public QWidget {
    Q_OBJECT
public:
    Pager(Theme& theme, const QString& title, const QList<NavItem>& items, QWidget* parent = nullptr);
    void addPage(const QString& id, QWidget* page, bool scroll = true);
    void select(const QString& id);
    void setTitle(const QString& title);
    void setItemVisible(const QString& id, bool visible);
    QString selected() const { return selected_; }
    QWidget* page(const QString& id) const;
    QStackedWidget* content() const { return content_; }

signals:
    void sectionChanged(const QString& id);

private:
    Theme& theme_;
    QLabel* title_;
    QHash<QString, Button*> buttons_;
    QHash<QString, QWidget*> pages_;
    QHash<QString, QWidget*> hosts_;
    QStackedWidget* content_;
    QString selected_;
};

// Caption/description column plus one control per row. stacked=true puts the control below
// the caption for wide editors such as text areas.
class FormGroup final : public Panel {
    Q_OBJECT
public:
    FormGroup(Theme& theme, const QString& title, const QString& description = {}, QWidget* parent = nullptr);
    QWidget* addRow(const QString& label, QWidget* control, const QString& description = {}, bool stacked = false);
    QVBoxLayout* body() const { return body_; }
private:
    QVBoxLayout* body_;
};

// Draws a pre-decoded image; the fallback icon is shown only while no image has been set,
// so a failed resource load still has a visible placeholder.
class ImageView final : public QWidget {
    Q_OBJECT
public:
    ImageView(Theme& theme, QSize size, QWidget* parent = nullptr);
    void setImage(const QImage& image, bool pixelated = false);
    void setResource(const QString& key, bool pixelated = false);
    void setFallbackIcon(const QString& key);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    Theme& theme_;
    QImage image_;
    QString fallback_;
    bool pixelated_ = false;
};

class Disclosure final : public QWidget {
    Q_OBJECT
public:
    Disclosure(Theme& theme, const QString& title, QWidget* content, QWidget* parent = nullptr);
    void setOpen(bool open);
    bool isOpen() const { return header_->isChecked(); }
    void setTitle(const QString& title);

signals:
    void openChanged(bool open);
private:
    Button* header_;
    QWidget* content_;
};

QVBoxLayout* pageColumn(QWidget* widget, int spacing = 16);
QHBoxLayout* rowLayout(QWidget* widget, int spacing = 12);
Button* actionButton(Theme& theme, const QString& text, const QString& icon = {}, ButtonKind kind = ButtonKind::Secondary, QWidget* parent = nullptr);
// Empty-state block; the hint should name the next action, not repeat the title.
QWidget* emptyNotice(Theme& theme, const QString& title, const QString& hint, QWidget* parent = nullptr);
// Shallow merge: keys from fields replace values in object, including nested objects.
QJsonObject withFields(QJsonObject object, const QJsonObject& fields);
bool matchesInstance(const QJsonObject& object, const InstanceIdentity& target);

} // namespace nickel::ui
