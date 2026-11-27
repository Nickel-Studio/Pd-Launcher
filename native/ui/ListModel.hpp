#pragma once

#include "Controls.hpp"
#include <QAbstractListModel>
#include <QCache>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QListView>
#include <QPersistentModelIndex>
#include <QSet>
#include <QSortFilterProxyModel>
#include <functional>

namespace nickel::ui {

enum class ListKind { Instances, Roots, Community, Vanilla, Loader, ProjectVersions, Mods, Resources, Downloads, Accounts, Logs };

// Stable identity is the DTO-provided key, never a row number: the proxy model reorders rows.
// setItems() keeps checked ids and decoded images by identity and drops entries no longer present.
class ListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { ObjectRole = Qt::UserRole + 1, IdentityRole, KindRole };
    explicit ListModel(ListKind kind, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    void setItems(const QJsonArray& items);
    QJsonObject object(int row) const;
    QString identity(int row) const;
    QModelIndex find(const QString& identity) const;
    ListKind kind() const { return kind_; }
    void setChecked(const QString& identity, bool checked);
    QSet<QString> checked() const { return checked_; }
    void clearChecked();
    // Only keys referenced by an item's iconKey/avatarKey are accepted, at most 1024x1024 and
    // 4 MiB per image; the cache itself is bounded to 8 MiB, so evicted images come back null.
    void setImage(const QString& key, const QImage& image);
    QImage image(const QString& key) const;
    qsizetype decodedImageBytes() const { return images_.totalCost(); }
    void clearImages();

signals:
    void checkedChanged();

private:
    ListKind kind_;
    QVector<QJsonObject> items_;
    QHash<QString, int> rows_;
    QSet<QString> checked_;
    QSet<QString> mediaKeys_;
    mutable QCache<QString, QImage> images_{8 * 1024 * 1024};
};

class ListFilter final : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit ListFilter(QObject* parent=nullptr):QSortFilterProxyModel(parent){}
    void setSearch(const QString& text);
    void setShowHidden(bool show);
    void setKindFilter(const QString& kind);
protected:
    bool filterAcceptsRow(int row,const QModelIndex& parent)const override;
private:
    QString search_,kind_;
    bool showHidden_=false;
};

class ListDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    ListDelegate(Theme& theme, ListKind kind, QObject* parent = nullptr);
    void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
    bool editorEvent(QEvent*, QAbstractItemModel*, const QStyleOptionViewItem&, const QModelIndex&) override;
    QRect accountDeleteRect(const QRect& row) const;

signals:
    void actionRequested(const QString& action, const QJsonObject& item);

private:
    Theme& theme_;
    ListKind kind_;
};

struct ListViewState {
    QString currentId;
    QStringList selectedIds;
    int verticalScroll = 0;
    int horizontalScroll = 0;
};

class ListView final : public QListView {
    Q_OBJECT
public:
    ListView(Theme& theme, ListKind kind, QWidget* parent = nullptr);
    ListDelegate* nickelDelegate() const { return delegate_; }
    void setSource(ListModel* model, ListFilter* filter = nullptr);
    QJsonObject currentObject() const;
    QJsonArray selectedObjects() const;
    ListViewState viewState() const;
    void restoreViewState(const ListViewState& state);
    void preserveSelection(const std::function<void()>& update);
protected:
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
private:
    ListKind kind_;
    ListDelegate* delegate_;
    QPersistentModelIndex pressedAccountDelete_;
    bool accountDeletePressed_ = false;
};

QString formatBytes(double bytes);
QString joinJsonStrings(const QJsonValue& value,const QString& separator=QStringLiteral(" · "));

} // namespace nickel::ui
