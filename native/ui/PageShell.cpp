#include "PageShell.hpp"
#include "Icons.hpp"

#include <QHideEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QPointer>

namespace nickel::ui {
RoutePage::RoutePage(Theme& theme, Bindings& bindings, Navigation& navigation, QWidget* parent)
    : QWidget(parent), theme_(theme), bindings_(bindings), navigation_(navigation) {
    setAutoFillBackground(false);
}
RoutePage::~RoutePage() { bindings_.forget(this); }
void RoutePage::enter() {}
void RoutePage::leave() { cancelQueries(); }
void RoutePage::snapshot(UiTopic, const QJsonObject&) {}
void RoutePage::query(UiQuery operation, const QJsonObject& scope, const QString& channel, Bindings::Callback callback) {
    invalidate(channel);
    const quint64 generation = generations_.value(channel);
    const auto id = bindings_.query(operation, scope, this, [this, channel, generation, callback = std::move(callback)](const UiReply& reply) {
        if (generations_.value(channel) != generation) return;
        queryRequests_.remove(channel);
        if (callback) callback(reply);
    });
    queryRequests_.insert(channel, id);
}
void RoutePage::command(UiAction operation, const QJsonObject& args, Bindings::Callback callback) {
    bindings_.command(operation, args, this, [this, callback = std::move(callback)](const UiReply& reply) {
        if (callback) callback(reply);
        else if (reply.error.isError()) report(reply.error);
    });
}
void RoutePage::invalidate(const QString& channel) {
    ++generations_[channel];
    const auto id = queryRequests_.take(channel);
    if (id) bindings_.cancel(id);
}
void RoutePage::cancelQueries() {
    const auto channels = queryRequests_.keys();
    for (const auto& channel : channels) invalidate(channel);
}
void RoutePage::hideEvent(QHideEvent* event) {
    cancelQueries();
    QWidget::hideEvent(event);
}
void RoutePage::report(const UiError& error) { if (error.isError()) emit errorReported(error); }

Pager::Pager(Theme& theme, const QString& title, const QList<NavItem>& items, QWidget* parent)
    : QWidget(parent), theme_(theme), title_(textLabel(theme, title, 11, true)), content_(new QStackedWidget(this)) {
    auto* row = rowLayout(this, 20);
    auto* sidebar = new QWidget(this); sidebar->setObjectName("pagerSidebar"); sidebar->setFixedWidth(208);
    auto* column = pageColumn(sidebar, 8); title_->setContentsMargins(12, 8, 12, 0); column->addWidget(title_);
    auto* list = new Panel(theme, sidebar, SurfaceKind::Fill); list->setShadow(0);
    auto* listColumn = list->column(8, 4); QString group;
    for (const auto& item : items) {
        if (!item.group.isEmpty() && item.group != group) {
            auto* label = textLabel(theme, item.group, 10, true, list); label->setContentsMargins(12, 8, 12, 4);
            listColumn->addWidget(label); group = item.group;
        }
        auto* button = actionButton(theme, item.label, item.icon, ButtonKind::Navigation, list);
        button->setObjectName("section-" + item.id); button->setCheckable(true); button->setMinimumHeight(38);
        button->setRadius(10); button->setTextAlignment(Qt::AlignLeft); listColumn->addWidget(button); buttons_.insert(item.id, button);
        connect(button, &QPushButton::clicked, this, [this, id = item.id] { select(id); });
    }
    column->addWidget(list); column->addStretch(); row->addWidget(sidebar);
    auto* right = new QWidget(this); auto* rightRow = rowLayout(right, 0); rightRow->setContentsMargins(0, 0, 8, 0);
    content_->setObjectName("pagerContent"); rightRow->addWidget(content_); row->addWidget(right, 1);
}
void Pager::addPage(const QString& id, QWidget* page, bool scroll) {
    if (pages_.contains(id)) return;
    pages_.insert(id, page); QWidget* host = page;
    if (scroll) { auto* area = new ScrollArea(theme_, this); area->setWidget(page); host = area; }
    hosts_.insert(id, host); content_->addWidget(host);
    if (selected_.isEmpty()) select(id);
}
void Pager::select(const QString& id) {
    if (!hosts_.contains(id) || !buttons_.contains(id)) return;
    const bool changed = selected_ != id; selected_ = id;
    for (auto it = buttons_.begin(); it != buttons_.end(); ++it) it.value()->setChecked(it.key() == id);
    content_->setCurrentWidget(hosts_.value(id));
    if (changed) emit sectionChanged(id);
}
void Pager::setTitle(const QString& title) { title_->setText(title); }
void Pager::setItemVisible(const QString& id, bool visible) {
    if (auto* button = buttons_.value(id)) button->setVisible(visible);
    if (!visible && selected_ == id) select("appearance");
}
QWidget* Pager::page(const QString& id) const { return pages_.value(id); }

FormGroup::FormGroup(Theme& theme, const QString& title, const QString& description, QWidget* parent)
    : Panel(theme, parent), body_(column(20, 12)) {
    body_->addWidget(textLabel(theme, title, 15));
    if (!description.isEmpty()) body_->addWidget(textLabel(theme, description, 12, true));
}
QWidget* FormGroup::addRow(const QString& label, QWidget* control, const QString& description, bool stacked) {
    auto* row = new QWidget(this); auto* names = new QWidget(row); auto* namesColumn = pageColumn(names, 3);
    namesColumn->addWidget(textLabel(theme_, label));
    if (!description.isEmpty()) namesColumn->addWidget(textLabel(theme_, description, 11, true));
    control->setAccessibleName(label);
    if (stacked) { auto* column = pageColumn(row, 8); column->addWidget(names); column->addWidget(control); }
    else { auto* layout = rowLayout(row, 16); names->setMinimumWidth(148); names->setMaximumWidth(180); layout->addWidget(names); layout->addWidget(control, 1); }
    body_->addWidget(row); return row;
}
ImageView::ImageView(Theme& theme, QSize size, QWidget* parent) : QWidget(parent), theme_(theme) {
    setFixedSize(size); setAutoFillBackground(false);
    connect(&theme_, &Theme::changed, this, qOverload<>(&QWidget::update));
}
void ImageView::setImage(const QImage& image, bool pixelated) { image_ = image; pixelated_ = pixelated; update(); }
void ImageView::setResource(const QString& key, bool pixelated) { setImage(functionalImage(key), pixelated); }
void ImageView::setFallbackIcon(const QString& key) { fallback_ = key; update(); }
void ImageView::paintEvent(QPaintEvent*) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    if (!image_.isNull()) paintContainedImage(painter, rect(), image_, pixelated_);
    else if (!fallback_.isEmpty()) paintIcon(painter, rect(), fallback_, theme_.tokens().faint);
}
Disclosure::Disclosure(Theme& theme, const QString& title, QWidget* content, QWidget* parent)
    : QWidget(parent), header_(actionButton(theme, title, "chevronDown", ButtonKind::Ghost, this)), content_(content) {
    auto* column = pageColumn(this, 8); header_->setCheckable(true); header_->setTextAlignment(Qt::AlignLeft); header_->setRadius(10);
    column->addWidget(header_); column->addWidget(content_); content_->hide();
    connect(header_, &QPushButton::toggled, this, [this](bool open) { content_->setVisible(open); emit openChanged(open); });
}
void Disclosure::setOpen(bool open) { header_->setChecked(open); content_->setVisible(open); }
void Disclosure::setTitle(const QString& title) { header_->setText(title); }
QVBoxLayout* pageColumn(QWidget* widget, int spacing) {
    auto* layout = new QVBoxLayout(widget); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(spacing); return layout;
}
QHBoxLayout* rowLayout(QWidget* widget, int spacing) {
    auto* layout = new QHBoxLayout(widget); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(spacing); return layout;
}
Button* actionButton(Theme& theme, const QString& text, const QString& icon, ButtonKind kind, QWidget* parent) {
    auto* result = new Button(theme, text, kind, parent); result->setIconName(icon); result->setAccessibleName(text); return result;
}
QWidget* emptyNotice(Theme& theme, const QString& title, const QString& hint, QWidget* parent) {
    auto* result = new QWidget(parent); auto* column = pageColumn(result, 12); column->setContentsMargins(24, 48, 24, 48);
    auto* heading = textLabel(theme, title, 19); heading->setAlignment(Qt::AlignCenter);
    auto* description = textLabel(theme, hint, 13, true); description->setAlignment(Qt::AlignCenter);
    column->addStretch(); column->addWidget(heading); column->addWidget(description); column->addStretch(); return result;
}
QJsonObject withFields(QJsonObject object, const QJsonObject& fields) {
    for (auto it = fields.begin(); it != fields.end(); ++it) object.insert(it.key(), it.value()); return object;
}
bool matchesInstance(const QJsonObject& object, const InstanceIdentity& target) {
    return target.valid() && object.value("root").toString() == target.root && object.value("versionId").toString() == target.versionId;
}
} // namespace nickel::ui
