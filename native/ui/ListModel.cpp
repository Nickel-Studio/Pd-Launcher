#include "ListModel.hpp"
#include "Icons.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <algorithm>
#include <cmath>

namespace nickel::ui {
namespace {
QString title(const QJsonObject& item){
    for(const char* key:{"displayName","title","name","fileName","id","label","dir"}){
        const QString value=item.value(QLatin1String(key)).toString();if(!value.isEmpty())return value;
    }
    return {};
}
struct SavedAccountText { QString name, type, subtitle; };
SavedAccountText savedAccountText(const QJsonObject& item) {
    SavedAccountText text{item.value("name").toString(), {}, {}};
    if (text.name.isEmpty()) text.name = QStringLiteral("未命名");
    const QString type = item.value("type").toString();
    if (type == "microsoft") {
        text.type = QStringLiteral("正版账号"); text.subtitle = item.value("emailMasked").toString();
        if (text.subtitle.isEmpty()) text.subtitle = QStringLiteral("Microsoft 账户");
    } else if (type == "littleskin") {
        text.type = QStringLiteral("皮肤站"); text.subtitle = item.value("server").toString();
        if (text.subtitle.isEmpty()) text.subtitle = QStringLiteral("LittleSkin");
    } else if (type == "offline") {
        text.type = QStringLiteral("离线"); text.subtitle = QStringLiteral("离线 · 仅本地");
    } else text.type = QStringLiteral("账号");
    return text;
}
QString stableId(const QJsonObject& item) {
    QString id = item.value("id").toString();
    if (id.isEmpty()) id = item.value("versionId").toString();
    if (id.isEmpty()) id = item.value("dir").toString();
    if (id.isEmpty()) id = item.value("projectId").toString();
    if (id.isEmpty()) id = item.value("path").toString();
    if (id.isEmpty()) id = item.value("kind").toString() + QChar(0) + item.value("version").toString();
    const QString root = item.value("root").toString();
    if (!root.isEmpty()) return root + QChar(0) + id;
    const QString provider = item.value("provider").toString();
    return provider.isEmpty() ? id : provider + QChar(0) + id;
}
void rounded(QPainter& p,const QRectF& r,qreal radius,const QColor& c){p.setPen(Qt::NoPen);p.setBrush(c);p.drawRoundedRect(r,radius,radius);}
const ListModel* sourceModel(const QAbstractItemModel* model){
    if(auto* proxy=qobject_cast<const QSortFilterProxyModel*>(model))return qobject_cast<const ListModel*>(proxy->sourceModel());
    return qobject_cast<const ListModel*>(model);
}
}
QString joinJsonStrings(const QJsonValue& value,const QString& separator){
    QStringList strings;for(const auto& entry:value.toArray())strings.append(entry.toString());return strings.join(separator);
}
QString formatBytes(double bytes){
    if(!std::isfinite(bytes)||bytes<0)return QStringLiteral("—");
    static const char* units[]={"B","KiB","MiB","GiB"};int unit=0;while(bytes>=1024&&unit<3){bytes/=1024;++unit;}
    return QString::number(bytes,'f',unit==0?0:bytes<10?2:1)+" "+QLatin1String(units[unit]);
}
ListModel::ListModel(ListKind kind,QObject* parent):QAbstractListModel(parent),kind_(kind){}
int ListModel::rowCount(const QModelIndex& parent)const{return parent.isValid()?0:int(items_.size());}
QJsonObject ListModel::object(int row)const{return row>=0&&row<items_.size()?items_[row]:QJsonObject{};}
QString ListModel::identity(int row)const{return stableId(object(row));}
QModelIndex ListModel::find(const QString& id)const{auto it=rows_.constFind(id);return it==rows_.end()?QModelIndex{}:index(it.value());}
QVariant ListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= items_.size()) return {};
    const auto& item = items_[index.row()];
    if (role == ObjectRole) return item;
    if (role == IdentityRole) return stableId(item);
    if (role == KindRole) return int(kind_);
    if (kind_ == ListKind::Accounts && item.value("accountSwitchRow").toBool()) {
        const auto text = savedAccountText(item);
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) return text.name;
        if (role == Qt::ToolTipRole || role == Qt::AccessibleDescriptionRole)
            return text.type + "\n" + text.subtitle + (item.value("selected").toBool() ? QStringLiteral("\n当前") : QString{})
                + QStringLiteral("\n删除 ") + text.name;
    }
    if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) return title(item);
    if (role == Qt::ToolTipRole || role == Qt::AccessibleDescriptionRole)
        return title(item) + "\n" + item.value("description").toString() + "\n" + item.value("path").toString();
    if (role == Qt::CheckStateRole) return checked_.contains(stableId(item)) ? Qt::Checked : Qt::Unchecked;
    return {};
}
Qt::ItemFlags ListModel::flags(const QModelIndex& index)const{
    if(!index.isValid())return Qt::NoItemFlags;
    Qt::ItemFlags result=Qt::ItemIsSelectable|Qt::ItemIsEnabled;
    if(kind_==ListKind::Mods||kind_==ListKind::Resources)result|=Qt::ItemIsUserCheckable;
    if(kind_==ListKind::Loader && object(index.row()).value("compatible").isBool() && !object(index.row()).value("compatible").toBool())result&=~Qt::ItemIsEnabled;
    return result;
}
bool ListModel::setData(const QModelIndex& index,const QVariant& value,int role){
    if(role!=Qt::CheckStateRole||!index.isValid())return false;setChecked(identity(index.row()),value.toInt()==Qt::Checked);return true;
}
void ListModel::setItems(const QJsonArray& items) {
    beginResetModel();
    items_.clear(); rows_.clear(); mediaKeys_.clear(); items_.reserve(items.size());
    for (const auto& value : items) {
        if (!value.isObject()) continue;
        const auto item = value.toObject();
        rows_.insert(stableId(item), int(items_.size()));
        items_.push_back(item);
        for (const char* field : {"iconKey", "avatarKey"}) {
            const QString key = item.value(QLatin1String(field)).toString();
            if (!key.isEmpty()) mediaKeys_.insert(key);
        }
    }
    for (const auto& key : images_.keys()) if (!mediaKeys_.contains(key)) images_.remove(key);
    QSet<QString> retained;
    for (const auto& id : checked_) if (rows_.contains(id)) retained.insert(id);
    checked_ = std::move(retained);
    endResetModel();
    emit checkedChanged();
}
void ListModel::setChecked(const QString& id,bool checked){
    if(checked)checked_.insert(id);else checked_.remove(id);const auto row=find(id);if(row.isValid())emit dataChanged(row,row,{Qt::CheckStateRole});emit checkedChanged();
}
void ListModel::clearChecked(){checked_.clear();if(rowCount()>0)emit dataChanged(index(0),index(rowCount()-1),{Qt::CheckStateRole});emit checkedChanged();}
void ListModel::setImage(const QString& key, const QImage& image) {
    if (!mediaKeys_.contains(key) || image.isNull() || image.width() > 1024 || image.height() > 1024
        || image.sizeInBytes() > 4 * 1024 * 1024) return;
    images_.insert(key, new QImage(image), int(image.sizeInBytes()));
    for (int row = 0; row < items_.size(); ++row) {
        if (items_[row].value("iconKey").toString() == key || items_[row].value("avatarKey").toString() == key)
            emit dataChanged(index(row), index(row), {Qt::DecorationRole});
    }
}
QImage ListModel::image(const QString& key) const {
    const auto* found = images_.object(key);
    return found ? *found : QImage{};
}
void ListModel::clearImages() {
    images_.clear();
    if (rowCount() > 0) emit dataChanged(index(0), index(rowCount() - 1), {Qt::DecorationRole});
}
void ListFilter::setSearch(const QString& text){search_=text.trimmed();invalidateFilter();}
void ListFilter::setShowHidden(bool show){showHidden_=show;invalidateFilter();}
void ListFilter::setKindFilter(const QString& kind){kind_=kind;invalidateFilter();}
bool ListFilter::filterAcceptsRow(int row,const QModelIndex& parent)const{
    const auto item=sourceModel()->index(row,0,parent).data(ListModel::ObjectRole).toJsonObject();
    if(!showHidden_&&item.value("category").toString()=="hidden")return false;
    if(!kind_.isEmpty()&&item.value("kind").toString()!=kind_&&item.value("type").toString()!=kind_)return false;
    return search_.isEmpty()||QString(title(item)+" "+item.value("description").toString()+" "+item.value("fileName").toString()).contains(search_,Qt::CaseInsensitive);
}

ListDelegate::ListDelegate(Theme& theme,ListKind kind,QObject* parent):QStyledItemDelegate(parent),theme_(theme),kind_(kind){}
QSize ListDelegate::sizeHint(const QStyleOptionViewItem&,const QModelIndex& index)const{
    switch(kind_){
    case ListKind::Instances:return QSize(272,120);
    case ListKind::Roots:return QSize(236,60);
    case ListKind::Community:return QSize(300,122);
    case ListKind::Vanilla:return QSize(300,48);
    case ListKind::Loader:return QSize(300,64);
    case ListKind::ProjectVersions:return QSize(300,50);
    case ListKind::Mods:return QSize(300,76);
    case ListKind::Resources:return QSize(260,86);
    case ListKind::Accounts:return QSize(300,index.data(ListModel::ObjectRole).toJsonObject().value("profileRow").toBool()?56:70);
    case ListKind::Logs:return QSize(300,26);
    case ListKind::Downloads:{const auto object=index.data(ListModel::ObjectRole).toJsonObject();return QSize(400,86+int(object.value("stages").toArray().size())*26+(object.value("message").toString().isEmpty()?0:46));}
    }
    return QSize(300,60);
}
QRect ListDelegate::accountDeleteRect(const QRect& row) const {
    const QRect box = row.adjusted(2, 2, -2, -4);
    const int width = QFontMetrics(bodyFont(12, QFont::DemiBold)).horizontalAdvance(QStringLiteral("删除")) + 14 + 8 + 26;
    return QRect(box.right() - 4 - width + 1, box.center().y() - 13, width, 28);
}
void ListDelegate::paint(QPainter* p,const QStyleOptionViewItem& option,const QModelIndex& index)const{
    const auto item=index.data(ListModel::ObjectRole).toJsonObject();const auto& t=theme_.tokens();
    const auto* model=sourceModel(index.model());const bool selected=option.state.testFlag(QStyle::State_Selected)||item.value("selected").toBool();
    const bool checked=index.data(Qt::CheckStateRole).toInt()==Qt::Checked;const bool hover=option.state.testFlag(QStyle::State_MouseOver);
    p->save();p->setRenderHint(QPainter::Antialiasing);p->setClipRect(option.rect);
    const QRectF box=QRectF(option.rect).adjusted(2,2,-2,-4);
    QColor fill=t.surface;if(kind_==ListKind::Roots||kind_==ListKind::Vanilla||kind_==ListKind::ProjectVersions||kind_==ListKind::Mods)fill=hover?t.fill2:QColor(Qt::transparent);
    if(selected||checked)fill=t.accentSoft;else if(hover)fill=kind_==ListKind::Community||kind_==ListKind::Instances?t.surface2:t.fill2;
    if(kind_!=ListKind::Logs)rounded(*p,box,kind_==ListKind::Vanilla?13:18,fill);
    qreal x=box.left()+16;const qreal right=box.right()-16;
    if(kind_==ListKind::Mods||kind_==ListKind::Resources){
        QRectF check(x,box.center().y()-9,18,18);rounded(*p,check,8,checked?t.accent:t.fill4);
        if(checked)paintIcon(*p,check.adjusted(2,2,-2,-2),"check",t.accentForeground);x+=30;
    }
    const bool profileRow=kind_==ListKind::Accounts&&item.value("profileRow").toBool();
    const bool accountSwitchRow=kind_==ListKind::Accounts&&item.value("accountSwitchRow").toBool();
    const int iconSize=profileRow?36:accountSwitchRow?34:kind_==ListKind::Vanilla?28:kind_==ListKind::ProjectVersions?32:kind_==ListKind::Mods?44:40;
    QString imageKey=item.value("iconKey").toString();if(kind_==ListKind::Accounts)imageKey=item.value("avatarKey").toString();
    if(imageKey.isEmpty())imageKey=item.value("icon").toString();
    QImage image=model?model->image(imageKey):QImage{};if(image.isNull())image=functionalImage(imageKey);
    bool pixelated=isPixelArtResource(imageKey);
    if(image.isNull()&&(kind_==ListKind::Instances||kind_==ListKind::Vanilla||kind_==ListKind::Loader)){
        image=functionalImage("defaultredstone");pixelated=true;
    }else if(image.isNull()&&kind_==ListKind::Mods)image=functionalImage("defaultmod");
    const bool drawImage=kind_!=ListKind::Logs&&kind_!=ListKind::Downloads&&kind_!=ListKind::Roots;
    if(drawImage){
        const QRectF target(x,kind_==ListKind::Instances?box.top()+16:box.center().y()-iconSize/2.,iconSize,iconSize);
        if(!image.isNull())paintContainedImage(*p,target,image,pixelated);
        else paintIcon(*p,target,kind_==ListKind::Mods?QStringLiteral("mods"):kind_==ListKind::Accounts?QStringLiteral("user"):QStringLiteral("resources"),t.faint);
        x+=iconSize+12;
    }
    if(kind_==ListKind::Roots){paintIcon(*p,QRectF(x,box.top()+11,18,18),"folder",selected?t.accentText:t.faint);x+=30;}
    if(profileRow){
        p->setFont(bodyFont(14,QFont::DemiBold));p->setPen(t.text);
        p->drawText(QRectF(x,box.top(),std::max(0.,right-x-40),box.height()),Qt::AlignLeft|Qt::AlignVCenter,
            QFontMetrics(p->font()).elidedText(title(item),Qt::ElideRight,int(std::max(0.,right-x-40))));
        if(selected)paintIcon(*p,QRectF(right-20,box.center().y()-8,16,16),"check",t.accentText);
        if(option.state.testFlag(QStyle::State_HasFocus))paintFocus(*p,box,13,t);
        p->restore();return;
    }
    if(accountSwitchRow){
        const auto text=savedAccountText(item);const QRectF deletion=accountDeleteRect(option.rect);
        p->setFont(bodyFont(12,QFont::DemiBold));p->setPen(t.muted);
        paintIcon(*p,QRectF(deletion.left()+13,deletion.center().y()-6.5,14,14),"trash",t.muted);
        p->drawText(deletion.adjusted(35,0,-13,0),Qt::AlignLeft|Qt::AlignVCenter,QStringLiteral("删除"));
        qreal textRight=deletion.left()-8;
        if(item.value("selected").toBool()){
            p->setFont(bodyFont(12));const qreal width=QFontMetrics(p->font()).horizontalAdvance(QStringLiteral("当前"));
            p->setPen(t.accentText);p->drawText(QRectF(textRight-width,box.top(),width,box.height()),Qt::AlignVCenter|Qt::AlignRight,QStringLiteral("当前"));
            textRight-=width+8;
        }
        const qreal textWidth=std::max(0.,textRight-x);const qreal nameY=box.center().y()-20;
        p->setFont(bodyFont(13,QFont::DemiBold));p->setPen(t.text);
        p->drawText(QRectF(x,nameY,textWidth,22),Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(p->font()).elidedText(text.name,Qt::ElideRight,int(textWidth)));
        p->setFont(bodyFont(12));const qreal badgeWidth=QFontMetrics(p->font()).horizontalAdvance(text.type)+12;
        const QRectF badge(x,nameY+24,std::min(badgeWidth,textWidth),18);
        rounded(*p,badge,9,t.fill2);p->setPen(t.muted);
        p->drawText(badge.adjusted(6,0,-6,0),Qt::AlignCenter,text.type);
        p->setPen(t.faint);const qreal subtitleX=badge.right()+6;
        p->drawText(QRectF(subtitleX,nameY+24,std::max(0.,textRight-subtitleX),18),Qt::AlignLeft|Qt::AlignVCenter,
            QFontMetrics(p->font()).elidedText(text.subtitle,Qt::ElideRight,int(std::max(0.,textRight-subtitleX))));
        if(option.state.testFlag(QStyle::State_HasFocus))paintFocus(*p,box,18,t);
        p->restore();return;
    }
    if(kind_==ListKind::Downloads){
        p->setFont(displayFont(15));p->setPen(t.text);p->drawText(QRectF(x,box.top()+12,right-x-34,24),Qt::AlignLeft|Qt::AlignVCenter,title(item));
        paintIcon(*p,QRectF(right-20,box.top()+16,16,16),"close",t.muted);
        qreal y=box.top()+50;const auto stages=item.value("stages").toArray();
        for(const auto& stageValue:stages){const auto stage=stageValue.toObject();p->setFont(bodyFont(12));p->setPen(stage.value("completed").toBool()?t.faint:t.text);
            const QRectF status(x,y,44,26);if(stage.value("completed").toBool())paintIcon(*p,QRectF(status.center()-QPointF(7,7),QSizeF(14,14)),"check",t.faint);
            else {QString progress=item.value("percent").isDouble()?QString::number(std::floor(item.value("percent").toDouble()))+"%":item.value("status").toString()=="queued"?QString::number(item.value("queuePosition").toInt()):QStringLiteral("…");p->drawText(status,Qt::AlignCenter,progress);}
            p->drawText(QRectF(x+56,y,right-x-56,26),Qt::AlignLeft|Qt::AlignVCenter,stage.value("name").toString());y+=26;
        }
        if(stages.isEmpty()){p->setFont(bodyFont(12));p->setPen(t.muted);p->drawText(QRectF(x,y,right-x,26),Qt::AlignLeft|Qt::AlignVCenter,item.value("status").toString()=="queued"?QStringLiteral("等待前一项完成"):QStringLiteral("等待状态更新"));y+=26;}
        if(!item.value("message").toString().isEmpty()){p->setPen(item.value("status").toString()=="error"?t.danger:t.warning);p->drawText(QRectF(x,y+8,right-x,36),Qt::AlignLeft|Qt::AlignTop|Qt::TextWordWrap,item.value("message").toString());}
        p->restore();return;
    }
    if(kind_==ListKind::Logs){p->setFont(monoFont(12));p->setPen(item.value("level").toString()=="error"?t.danger:t.muted);p->drawText(QRectF(x,box.top(),right-x,26),Qt::AlignLeft|Qt::AlignVCenter,item.value("time").toString()+" "+item.value("text").toString());p->restore();return;}
    const QString name=title(item);p->setPen(item.value("enabled").isBool()&&!item.value("enabled").toBool()?t.muted:t.text);
    p->setFont(kind_==ListKind::Community||kind_==ListKind::Instances?displayFont(15):bodyFont(13,QFont::DemiBold));
    const qreal nameY=kind_==ListKind::Instances?box.top()+15:kind_==ListKind::Community?box.top()+15:box.center().y()-20;
    const qreal toolsWidth=kind_==ListKind::Instances?0:kind_==ListKind::Mods?100:kind_==ListKind::Roots?0:28;
    p->drawText(QRectF(x,nameY,std::max(0.,right-x-toolsWidth),24),Qt::AlignVCenter|Qt::AlignLeft,QFontMetrics(p->font()).elidedText(name,Qt::ElideRight,int(right-x-toolsWidth)));
    QString subtitle=item.value("description").toString();
    if(kind_==ListKind::Instances)subtitle=item.value("minecraftVersion").toString()+" · "+item.value("loader").toString();
    if(kind_==ListKind::Roots)subtitle=item.value("dir").toString();
    if(kind_==ListKind::Vanilla)subtitle=item.value("releaseTime").toString();
    if(kind_==ListKind::Loader)subtitle=item.value("reason").toString().isEmpty()?item.value("version").toString():item.value("reason").toString();
    if(kind_==ListKind::ProjectVersions)subtitle=item.value("number").toString()+" · "+joinJsonStrings(item.value("loaders"))+" · "+item.value("date").toString();
    if(kind_==ListKind::Accounts){
        const auto text=savedAccountText(item);subtitle=text.type+(text.subtitle.isEmpty()?QString{}:QStringLiteral(" · ")+text.subtitle);
    }
    if(kind_==ListKind::Resources)subtitle=item.value("kind").toString()+" · "+formatBytes(item.value("size").toDouble());
    if(kind_==ListKind::Mods)subtitle=item.value("version").toString()+" · "+item.value("description").toString();
    p->setFont(bodyFont(kind_==ListKind::Roots?10:11));p->setPen(t.faint);
    p->drawText(QRectF(x,nameY+25,std::max(0.,right-x-toolsWidth),22),Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(p->font()).elidedText(subtitle,Qt::ElideRight,int(right-x-toolsWidth)));
    if(kind_==ListKind::Community){p->setFont(bodyFont(11));p->setPen(t.muted);p->drawText(QRectF(box.left()+16,box.bottom()-34,box.width()-32,22),Qt::AlignLeft|Qt::AlignVCenter,item.value("provider").toString()+" · "+item.value("author").toString()+" · "+QString::number(item.value("downloads").toDouble(),'g',5));}
    if(kind_==ListKind::Instances){
        for(int tool=0;tool<3;++tool){const QString icon=tool==0?QStringLiteral("pencil"):tool==1?QStringLiteral("trash"):QStringLiteral("settings");
            const QRectF r(right-92+tool*34,box.bottom()-32,28,26);rounded(*p,r,10,t.fill1);paintIcon(*p,r.adjusted(6,5,-6,-5),icon,tool==0&&item.value("favorite").toBool()?t.accentText:t.muted);}
        if(!item.value("hasJson").toBool()){p->setPen(t.danger);p->drawText(QRectF(box.left()+16,box.bottom()-30,box.width()-128,22),Qt::AlignLeft|Qt::AlignVCenter,QStringLiteral("版本信息不完整"));}
    }
    if(kind_==ListKind::Mods){
        for(int tool=0;tool<3;++tool){const QString icon=tool==0?QStringLiteral("check"):tool==1?QStringLiteral("pencil"):QStringLiteral("trash");paintIcon(*p,QRectF(right-88+tool*32,box.center().y()-8,16,16),icon,tool==0&&item.value("enabled").toBool()?t.accentText:t.muted);}
    }
    if(option.state.testFlag(QStyle::State_HasFocus))paintFocus(*p,box,18,t);
    p->restore();
}
bool ListDelegate::editorEvent(QEvent* event,QAbstractItemModel* model,const QStyleOptionViewItem& option,const QModelIndex& index){
    if(event->type()!=QEvent::MouseButtonRelease)return QStyledItemDelegate::editorEvent(event,model,option,index);
    auto* mouse=static_cast<QMouseEvent*>(event);if(mouse->button()!=Qt::LeftButton)return false;
    const auto item=index.data(ListModel::ObjectRole).toJsonObject();const qreal relativeX=mouse->position().x()-option.rect.left();
    if((kind_==ListKind::Mods||kind_==ListKind::Resources)&&relativeX<46){model->setData(index,index.data(Qt::CheckStateRole).toInt()==Qt::Checked?Qt::Unchecked:Qt::Checked,Qt::CheckStateRole);return true;}
    if(kind_==ListKind::Accounts&&item.value("accountSwitchRow").toBool()&&accountDeleteRect(option.rect).contains(mouse->position().toPoint())){emit actionRequested("delete",item);return true;}
    if(kind_==ListKind::Instances && mouse->position().y()>option.rect.bottom()-40 && relativeX>option.rect.width()-120){
        const int tool=std::clamp(int((relativeX-(option.rect.width()-110))/34),0,2);emit actionRequested(tool==0?"favorite":tool==1?"delete":"settings",item);return true;
    }
    if(kind_==ListKind::Mods&&relativeX>option.rect.width()-116){const int tool=std::clamp(int((relativeX-(option.rect.width()-106))/32),0,2);emit actionRequested(tool==0?"toggle":tool==1?"rename":"delete",item);return true;}
    if(kind_==ListKind::Downloads&&relativeX>option.rect.width()-52&&mouse->position().y()<option.rect.top()+50){emit actionRequested(item.value("status").toString()=="error"?"dismiss":"cancel",item);return true;}
    return false;
}
ListView::ListView(Theme& theme,ListKind kind,QWidget* parent):QListView(parent),kind_(kind),delegate_(new ListDelegate(theme,kind,this)){
    setFrameShape(QFrame::NoFrame);setAutoFillBackground(false);viewport()->setAutoFillBackground(false);setPalette(theme.palette());setFont(bodyFont());
    setItemDelegate(delegate_);setSelectionMode(kind==ListKind::Mods||kind==ListKind::Resources?QAbstractItemView::ExtendedSelection:QAbstractItemView::SingleSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setVerticalScrollBar(new ScrollBar(theme,Qt::Vertical,this));
    setMouseTracking(true);setUniformItemSizes(kind!=ListKind::Downloads);setEditTriggers(QAbstractItemView::NoEditTriggers);
    if(kind==ListKind::Instances){setViewMode(QListView::IconMode);setFlow(QListView::LeftToRight);setWrapping(true);setResizeMode(QListView::Adjust);setSpacing(8);setMovement(QListView::Static);setGridSize(QSize(280,128));}
    connect(&theme,&Theme::changed,this,[this,&theme]{setPalette(theme.palette());viewport()->update();});
}
void ListView::setSource(ListModel* model,ListFilter* filter){if(filter){filter->setSourceModel(model);setModel(filter);}else setModel(model);}
QJsonObject ListView::currentObject()const{return currentIndex().data(ListModel::ObjectRole).toJsonObject();}
QJsonArray ListView::selectedObjects()const{QJsonArray result;if(!selectionModel())return result;for(const auto& row:selectionModel()->selectedRows())result.append(row.data(ListModel::ObjectRole).toJsonObject());return result;}
ListViewState ListView::viewState() const {
    ListViewState state;
    state.currentId = currentIndex().data(ListModel::IdentityRole).toString();
    if (selectionModel()) for (const auto& row : selectionModel()->selectedRows())
        state.selectedIds.append(row.data(ListModel::IdentityRole).toString());
    state.verticalScroll = verticalScrollBar()->value();
    state.horizontalScroll = horizontalScrollBar()->value();
    return state;
}
void ListView::restoreViewState(const ListViewState& state) {
    if (!model() || !selectionModel()) return;
    const QSet<QString> selected(state.selectedIds.cbegin(), state.selectedIds.cend());
    QItemSelection selection;
    QModelIndex current;
    for (int row = 0; row < model()->rowCount(); ++row) {
        const auto item = model()->index(row, 0);
        const QString id = item.data(ListModel::IdentityRole).toString();
        if (id == state.currentId) current = item;
        if (selected.contains(id)) selection.select(item, item);
    }
    selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selectionModel()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
    doItemsLayout();
    verticalScrollBar()->setValue(state.verticalScroll);
    horizontalScrollBar()->setValue(state.horizontalScroll);
}
void ListView::preserveSelection(const std::function<void()>& update) {
    const auto state = viewState();
    update();
    restoreViewState(state);
}
void ListView::keyPressEvent(QKeyEvent* event){
    const auto item=currentObject();if(!item.isEmpty()){
        if(event->key()==Qt::Key_Delete){emit delegate_->actionRequested("delete",item);event->accept();return;}
        if(event->key()==Qt::Key_F2){emit delegate_->actionRequested("rename",item);event->accept();return;}
        if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter){emit activated(currentIndex());event->accept();return;}
    }
    QListView::keyPressEvent(event);
}
void ListView::resizeEvent(QResizeEvent* event){
    if(kind_==ListKind::Instances){const int columns=std::max(1,(viewport()->width()+16)/(272+16));const int width=(viewport()->width()-16*(columns-1))/columns;setGridSize(QSize(width,128));}
    QListView::resizeEvent(event);
}
void ListView::mousePressEvent(QMouseEvent* event) {
    const auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && kind_ == ListKind::Accounts && index.isValid()
        && index.data(ListModel::ObjectRole).toJsonObject().value("accountSwitchRow").toBool()
        && delegate_->accountDeleteRect(visualRect(index)).contains(event->position().toPoint())) {
        pressedAccountDelete_ = index; accountDeletePressed_ = true; event->accept(); return;
    }
    pressedAccountDelete_ = QPersistentModelIndex{}; accountDeletePressed_ = false;
    QListView::mousePressEvent(event);
}
void ListView::mouseReleaseEvent(QMouseEvent* event) {
    const auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && kind_ == ListKind::Accounts) {
        const bool deletion = index.isValid() && index.data(ListModel::ObjectRole).toJsonObject().value("accountSwitchRow").toBool()
            && delegate_->accountDeleteRect(visualRect(index)).contains(event->position().toPoint());
        // A model reset invalidates the index, but must not turn a delete press into a row click.
        if (accountDeletePressed_ || deletion) {
            const auto pressed = pressedAccountDelete_; pressedAccountDelete_ = QPersistentModelIndex{}; accountDeletePressed_ = false;
            if (deletion && pressed == index) emit delegate_->actionRequested("delete", index.data(ListModel::ObjectRole).toJsonObject());
            event->accept(); return;
        }
    }
    QListView::mouseReleaseEvent(event);
}
} // namespace nickel::ui
