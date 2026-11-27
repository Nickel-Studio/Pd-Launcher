#include "Controls.hpp"
#include "BackgroundView.hpp"
#include "Icons.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QGraphicsDropShadowEffect>
#include <QHideEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QStyleOption>
#include <algorithm>
#include <cmath>

namespace nickel::ui {
namespace {
void roundFill(QPainter& p, const QRectF& r, qreal radius, const QColor& color) {
    p.setPen(Qt::NoPen); p.setBrush(color); p.drawRoundedRect(r, radius, radius);
}
bool keyboardReason(Qt::FocusReason reason) {
    return reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason;
}
}
void paintFocus(QPainter& p, const QRectF& rect, qreal radius, const ThemeTokens& t) {
    p.save(); p.setBrush(Qt::NoBrush); p.setPen(QPen(t.accentRing, 3));
    p.drawRoundedRect(rect.adjusted(1.5,1.5,-1.5,-1.5), radius, radius); p.restore();
}
Panel::Panel(Theme& theme, QWidget* parent, SurfaceKind surface) : QWidget(parent), theme_(theme), surface_(surface) {
    setAttribute(Qt::WA_TranslucentBackground); setAutoFillBackground(false);
    connect(&theme_, &Theme::changed, this, [this] { setShadow(shadow_); update(); });
}
void Panel::setRadius(qreal value) { radius_ = value; update(); }
void Panel::setSurface(SurfaceKind value) { surface_ = value; update(); }
void Panel::setShadow(int level) {
    shadow_ = level;
    if (!theme_.appearance().shadows || level <= 0) { if (graphicsEffect()) setGraphicsEffect(nullptr); return; }
    auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(graphicsEffect());
    if (!effect) { effect = new QGraphicsDropShadowEffect(this); setGraphicsEffect(effect); }
    effect->setBlurRadius(level >= 3 ? 40 : level == 2 ? 24 : 4);
    effect->setOffset(0, level >= 3 ? 16 : level == 2 ? 8 : 1);
    effect->setColor(theme_.tokens().dark ? QColor(0,0,0,100) : QColor(16,20,24,34));
}
QVBoxLayout* Panel::column(int padding, int spacing) {
    auto* result = new QVBoxLayout(this); result->setContentsMargins(padding,padding,padding,padding);
    result->setSpacing(spacing); return result;
}
void Panel::paintEvent(QPaintEvent*) {
    if (surface_ == SurfaceKind::None) return;
    const auto& t = theme_.tokens(); QColor fill = t.surface;
    if (surface_ == SurfaceKind::Fill) fill = t.fill1;
    if (surface_ == SurfaceKind::Glass) fill.setAlphaF(t.dark ? .66 : .70);
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    if (surface_ == SurfaceKind::Glass) {
        QPainterPath clip; clip.addRoundedRect(QRectF(rect()), radius_, radius_);
        p.save(); p.setClipPath(clip); paintWindowBackdrop(p, *this); p.restore();
    }
    roundFill(p, rect(), radius_, fill);
    if (shadow_ > 0) {
        p.setPen(QPen(t.dark ? QColor(255,255,255,11) : QColor(255,255,255,230), 1));
        p.drawLine(QPointF(radius_, .5), QPointF(width()-radius_, .5));
    }
}

Button::Button(Theme& theme, const QString& label, ButtonKind kind, QWidget* parent)
    : QPushButton(label, parent), theme_(theme), kind_(kind) {
    setFont(bodyFont(13,QFont::DemiBold)); setFlat(true); setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor); setMinimumHeight(34); setMouseTracking(true);
    connect(&theme_, &Theme::changed, this, qOverload<>(&QWidget::update));
    connect(this, &QPushButton::toggled, this, qOverload<>(&QWidget::update));
}
void Button::setIconName(const QString& name) { iconName_=name; updateGeometry(); update(); }
void Button::setSubtitle(const QString& text) { subtitle_=text; updateGeometry(); update(); }
void Button::setRadius(qreal r) { radius_=r; update(); }
void Button::setKind(ButtonKind kind) { kind_=kind; update(); }
void Button::setTextAlignment(Qt::Alignment a) { alignment_=a; update(); }
void Button::setAccentSwatch(const QColor& color) { swatch_=color; updateGeometry(); update(); }
void Button::setBusy(bool busy) { busy_=busy; setEnabled(!busy); update(); }
QSize Button::sizeHint() const {
    const int textWidth = QFontMetrics(font()).horizontalAdvance(text());
    const int icon = !iconName_.isEmpty() || swatch_.isValid() ? 24 : 0;
    return QSize(std::max(text().isEmpty() ? 32 : 60, textWidth+34+icon), subtitle_.isEmpty()?34:62);
}
void Button::enterEvent(QEnterEvent* event) { QPushButton::enterEvent(event); update(); }
void Button::leaveEvent(QEvent* event) { QPushButton::leaveEvent(event); update(); }
void Button::focusInEvent(QFocusEvent* event) { keyboardFocus_=keyboardReason(event->reason()); QPushButton::focusInEvent(event); update(); }
void Button::focusOutEvent(QFocusEvent* event) { keyboardFocus_=false; QPushButton::focusOutEvent(event); update(); }
void Button::paintEvent(QPaintEvent*) {
    const auto& t=theme_.tokens(); QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const bool hover=underMouse() && isEnabled(); QColor fill, fg=t.text;
    switch (kind_) {
    case ButtonKind::Primary: fill=hover?mix(t.accent,Qt::white,.12):t.accent; fg=t.accentForeground; break;
    case ButtonKind::Secondary: fill=hover?t.fill4:t.fill3; break;
    case ButtonKind::Outline: fill=hover?t.fill3:t.fill1; fg=hover?t.text:t.muted; break;
    case ButtonKind::Ghost: fill=hover?t.fill2:QColor(Qt::transparent); fg=hover?t.text:t.muted; break;
    case ButtonKind::Danger: fill=hover?t.danger:QColor(t.danger.red(),t.danger.green(),t.danger.blue(),38); fg=hover?QColor(Qt::white):t.danger; break;
    case ButtonKind::Navigation: fill=isChecked()?t.accentSoft:hover?t.fill2:QColor(Qt::transparent); fg=t.text; break;
    }
    if (isDown() && kind_==ButtonKind::Primary) fill=t.accentStrong;
    p.setOpacity(isEnabled()?1:.5);
    const QRectF box=QRectF(rect()).adjusted(2,2,-2,-2); const qreal radius=std::min(radius_,box.height()/2.);
    roundFill(p,box,radius,fill);
    if (kind_==ButtonKind::Primary || isChecked()) {
        p.setPen(QPen(QColor(255,255,255,60),1));
        p.drawLine(QPointF(box.left()+radius,box.top()+.5),QPointF(box.right()-radius,box.top()+.5));
    }
    const int iconSize=subtitle_.isEmpty()?16:22;
    const int gap=subtitle_.isEmpty()?8:14;
    const bool hasIcon=!iconName_.isEmpty() || swatch_.isValid() || busy_;
    const int widthText=QFontMetrics(font()).horizontalAdvance(text());
    const int total=widthText+(hasIcon?iconSize+gap:0);
    qreal x=alignment_.testFlag(Qt::AlignLeft)?14:alignment_.testFlag(Qt::AlignRight)?width()-14-total:(width()-total)/2.;
    if (isDown()) p.translate(0,1);
    if (hasIcon) {
        const QRectF iconRect(x,(height()-iconSize)/2.+.5,iconSize,iconSize);
        if (swatch_.isValid()) roundFill(p,iconRect,iconSize/2.,swatch_);
        else paintIcon(p,iconRect,busy_?QStringLiteral("refresh"):iconName_,fg);
        x+=iconSize+gap;
    }
    p.setPen(fg); p.setFont(font());
    const QRectF labelRect(x,0,std::max(0.,width()-x-10),height());
    if (subtitle_.isEmpty()) p.drawText(labelRect,Qt::AlignVCenter|Qt::AlignLeft,QFontMetrics(font()).elidedText(text(),Qt::ElideRight,int(labelRect.width())));
    else {
        p.setFont(displayFont(18,QFont::Bold)); p.drawText(QRectF(x,10,labelRect.width(),24),Qt::AlignLeft|Qt::AlignVCenter,text());
        p.setFont(bodyFont(12)); p.setOpacity(p.opacity()*.85);
        p.drawText(QRectF(x,34,labelRect.width(),18),Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(p.font()).elidedText(subtitle_,Qt::ElideRight,int(labelRect.width())));
    }
    p.setOpacity(1); if(hasFocus() && keyboardFocus_) paintFocus(p,rect(),radius,t);
}

LineEdit::LineEdit(Theme& theme,QWidget* parent) : QLineEdit(parent),theme_(theme) {
    setFrame(false); setTextMargins(13,0,13,0); setMinimumHeight(36); setFont(bodyFont());
    setAttribute(Qt::WA_TranslucentBackground); setAutoFillBackground(false); setPalette(theme_.palette());
    connect(&theme_,&Theme::changed,this,[this]{setPalette(theme_.palette());update();});
}
void LineEdit::paintEvent(QPaintEvent* event) {
    {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        roundFill(p,rect(),13,hasFocus()||underMouse()?theme_.tokens().fill3:theme_.tokens().fill2);
        if(hasFocus()) paintFocus(p,rect(),13,theme_.tokens());
    }
    // The native editor owns IME/preedit, selection, cursor, clipboard and undo.
    QLineEdit::paintEvent(event);
}
void LineEdit::contextMenuEvent(QContextMenuEvent* event) {
    auto* menu=createStandardContextMenu(); menu->setPalette(theme_.palette()); menu->setFont(bodyFont());
    menu->exec(event->globalPos()); delete menu;
}

CheckBox::CheckBox(Theme& theme,const QString& text,QWidget* parent,bool switchStyle)
    : QCheckBox(text,parent),theme_(theme),switchStyle_(switchStyle) {
    setFont(bodyFont()); setFocusPolicy(Qt::StrongFocus); setCursor(Qt::PointingHandCursor);
    setMinimumHeight(28); connect(&theme_,&Theme::changed,this,qOverload<>(&QWidget::update));
}
QSize CheckBox::sizeHint() const { return QSize(QFontMetrics(font()).horizontalAdvance(text())+(switchStyle_?52:26),28); }
bool CheckBox::hitButton(const QPoint& position) const { return rect().contains(position); }
void CheckBox::paintEvent(QPaintEvent*) {
    const auto& t=theme_.tokens(); QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setOpacity(isEnabled()?1:.5);
    const int w=switchStyle_?44:18; const int h=switchStyle_?25:18;
    QRectF box(2,(height()-h)/2.,w,h);
    roundFill(p,box,switchStyle_?12.5:8,isChecked()?t.accent:underMouse()?t.fill4:t.fill3);
    if(switchStyle_) roundFill(p,QRectF(box.left()+(isChecked()?22:3),box.top()+3,19,19),9.5,isChecked()?t.accentForeground:QColor(Qt::white));
    else if(checkState()==Qt::PartiallyChecked) paintIcon(p,box.adjusted(2,2,-2,-2),"minus",t.accentForeground);
    else if(isChecked()) paintIcon(p,box.adjusted(2,2,-2,-2),"check",t.accentForeground);
    p.setPen(t.muted);p.setFont(font());p.drawText(QRectF(w+10,0,width()-w-10,height()),Qt::AlignLeft|Qt::AlignVCenter,text());
    if(hasFocus()) paintFocus(p,rect(),8,t);
}
RadioButton::RadioButton(Theme& theme,const QString& text,QWidget* parent):QRadioButton(text,parent),theme_(theme) {
    setFont(bodyFont(12));setMinimumHeight(36);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);
    connect(&theme_,&Theme::changed,this,qOverload<>(&QWidget::update));
}
void RadioButton::setSwatch(QColor color){swatch_=color;update();}
QSize RadioButton::sizeHint()const{return QSize(QFontMetrics(font()).horizontalAdvance(text())+54,38);}
void RadioButton::paintEvent(QPaintEvent*){
    const auto& t=theme_.tokens();QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.setOpacity(isEnabled()?1:.5);
    roundFill(p,QRectF(rect()).adjusted(2,2,-2,-2),18,isChecked()?t.accentSoft:underMouse()?t.fill3:t.fill1);
    QColor dot=swatch_.isValid()?swatch_:isChecked()?t.accent:t.faint;
    roundFill(p,QRectF(14,(height()-14)/2.,14,14),7,dot);
    if(isChecked() && !swatch_.isValid()) paintIcon(p,QRectF(14,(height()-14)/2.,14,14),"check",t.accentForeground);
    p.setFont(font());p.setPen(isChecked()?t.text:t.muted);p.drawText(QRectF(36,0,width()-46,height()),Qt::AlignLeft|Qt::AlignVCenter,text());
    if(hasFocus())paintFocus(p,rect(),18,t);
}

Slider::Slider(Theme& theme,QWidget* parent):QSlider(Qt::Horizontal,parent),theme_(theme){
    setMinimumHeight(44);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);
    connect(&theme_,&Theme::changed,this,qOverload<>(&QWidget::update));
}
void Slider::paintEvent(QPaintEvent*){
    const auto& t=theme_.tokens();QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.setOpacity(isEnabled()?1:.5);
    const double ratio=maximum()==minimum()?0:double(value()-minimum())/double(maximum()-minimum());
    const QRectF track(8,height()/2.-2,std::max(1,width()-16),4);
    roundFill(p,track,2,t.fill3);roundFill(p,QRectF(track.topLeft(),QSizeF(track.width()*ratio,4)),2,t.accentText);
    const qreal x=track.left()+track.width()*(invertedAppearance()?1-ratio:ratio);
    roundFill(p,QRectF(x-6,height()/2.-6,12,12),6,t.accentText);if(hasFocus())paintFocus(p,rect(),10,t);
}
void Slider::setFromPosition(qreal x){
    const double ratio=std::clamp((x-8)/std::max(1,width()-16),0.,1.);
    const int next=minimum()+int(std::lround((invertedAppearance()?1-ratio:ratio)*(maximum()-minimum())));
    setValue(next);
}
void Slider::mousePressEvent(QMouseEvent* event){
    if(event->button()==Qt::LeftButton && isEnabled()){setFocus(Qt::MouseFocusReason);setSliderDown(true);setFromPosition(event->position().x());event->accept();}
    else QSlider::mousePressEvent(event);
}
void Slider::mouseMoveEvent(QMouseEvent* event){if(isSliderDown()){setFromPosition(event->position().x());event->accept();}else QSlider::mouseMoveEvent(event);}
void Slider::mouseReleaseEvent(QMouseEvent* event){if(event->button()==Qt::LeftButton && isSliderDown()){setFromPosition(event->position().x());setSliderDown(false);event->accept();}else QSlider::mouseReleaseEvent(event);}

void PopupDelegate::paint(QPainter* p,const QStyleOptionViewItem& option,const QModelIndex& index)const{
    const auto& t=theme_.tokens();p->save();p->setRenderHint(QPainter::Antialiasing);p->fillRect(option.rect,t.surface2);
    if(option.state.testFlag(QStyle::State_Selected))roundFill(*p,QRectF(option.rect).adjusted(4,2,-4,-2),8,t.accentSoft);
    p->setPen(index.flags().testFlag(Qt::ItemIsEnabled)?t.text:t.faint);p->setFont(bodyFont());
    p->drawText(option.rect.adjusted(13,0,-28,0),Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(p->font()).elidedText(index.data().toString(),Qt::ElideRight,option.rect.width()-41));
    if(option.state.testFlag(QStyle::State_Selected))paintIcon(*p,QRectF(option.rect.right()-24,option.rect.center().y()-7,14,14),"check",t.accentText);
    p->restore();
}
QSize PopupDelegate::sizeHint(const QStyleOptionViewItem&,const QModelIndex&)const{return QSize(180,36);}
ComboBox::ComboBox(Theme& theme,QWidget* parent):QComboBox(parent),theme_(theme){
    setFrame(false);setMinimumHeight(36);setFont(bodyFont());setPalette(theme_.palette());setFocusPolicy(Qt::StrongFocus);
    setItemDelegate(new PopupDelegate(theme,this));setMaxVisibleItems(12);
    view()->setFrameShape(QFrame::NoFrame);view()->setVerticalScrollBar(new ScrollBar(theme,Qt::Vertical,view()));
    connect(&theme_,&Theme::changed,this,[this]{setPalette(theme_.palette());view()->setPalette(theme_.palette());update();});
}
void ComboBox::paintEvent(QPaintEvent*){
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const auto& t=theme_.tokens();p.setOpacity(isEnabled()?1:.5);
    roundFill(p,rect(),10,underMouse()||hasFocus()?t.fill3:t.fill2);
    p.setFont(font());p.setPen(currentIndex()<0?t.faint:t.text);
    p.drawText(QRectF(13,0,width()-42,height()),Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(font()).elidedText(currentIndex()<0?placeholderText():currentText(),Qt::ElideRight,width()-42));
    paintIcon(p,QRectF(width()-27,(height()-15)/2.+.5,15,15),"chevronDown",t.faint);if(hasFocus())paintFocus(p,rect(),10,t);
}
void ComboBox::showPopup(){QComboBox::showPopup();view()->window()->setPalette(theme_.palette());view()->setPalette(theme_.palette());}

ScrollBar::ScrollBar(Theme& theme,Qt::Orientation orientation,QWidget* parent):QScrollBar(orientation,parent),theme_(theme){
    if(orientation==Qt::Vertical)setFixedWidth(11);else setFixedHeight(11);
    setAttribute(Qt::WA_TranslucentBackground);connect(&theme_,&Theme::changed,this,qOverload<>(&QWidget::update));
}
QRectF ScrollBar::thumbRect()const{
    const bool vertical=orientation()==Qt::Vertical;const qreal length=vertical?height():width();
    const qreal range=maximum()-minimum();if(range<=0)return {};
    const qreal thumb=std::clamp(length*double(pageStep())/std::max(1.,range+pageStep()),24.,length);
    const qreal pos=(length-thumb)*(value()-minimum())/range;
    return vertical?QRectF(3,pos,5,thumb):QRectF(pos,3,thumb,5);
}
void ScrollBar::paintEvent(QPaintEvent*){
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);auto c=theme_.tokens().text;c.setAlphaF(underMouse()?.28:.15);roundFill(p,thumbRect(),2.5,c);
}
void ScrollBar::mousePressEvent(QMouseEvent* event){
    if(event->button()!=Qt::LeftButton){QScrollBar::mousePressEvent(event);return;}
    const QRectF thumb=thumbRect();const bool vertical=orientation()==Qt::Vertical;const qreal pos=vertical?event->position().y():event->position().x();
    if(thumb.contains(event->position()))dragOffset_=pos-(vertical?thumb.top():thumb.left());
    else setValue(value()+(pos<(vertical?thumb.top():thumb.left())?-pageStep():pageStep()));
    event->accept();
}
void ScrollBar::mouseMoveEvent(QMouseEvent* event){
    if(dragOffset_<0){QScrollBar::mouseMoveEvent(event);return;}
    const bool vertical=orientation()==Qt::Vertical;const auto thumb=thumbRect();
    const qreal track=(vertical?height()-thumb.height():width()-thumb.width());
    const qreal pos=(vertical?event->position().y():event->position().x())-dragOffset_;
    if(track>0)setValue(minimum()+int(std::lround(std::clamp(pos/track,0.,1.)*(maximum()-minimum()))));event->accept();
}
void ScrollBar::mouseReleaseEvent(QMouseEvent* event){dragOffset_=-1;event->accept();}
ScrollArea::ScrollArea(Theme& theme,QWidget* parent):QScrollArea(parent){
    setFrameShape(QFrame::NoFrame);setWidgetResizable(true);setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBar(new ScrollBar(theme,Qt::Vertical,this));setAutoFillBackground(false);
    viewport()->setAutoFillBackground(false);setPalette(theme.palette());
    connect(&theme,&Theme::changed,this,[this,&theme]{setPalette(theme.palette());viewport()->update();});
}
TextEdit::TextEdit(Theme& theme,QWidget* parent):QPlainTextEdit(parent),theme_(theme){
    setFrameShape(QFrame::NoFrame);setFont(bodyFont());setPalette(theme_.palette());setAutoFillBackground(false);
    viewport()->setAutoFillBackground(false);setVerticalScrollBar(new ScrollBar(theme,Qt::Vertical,this));
    setHorizontalScrollBar(new ScrollBar(theme,Qt::Horizontal,this));setTabChangesFocus(true);
    connect(&theme_,&Theme::changed,this,[this]{setPalette(theme_.palette());viewport()->update();});
}
void TextEdit::paintEvent(QPaintEvent* event){
    {QPainter p(viewport());p.setRenderHint(QPainter::Antialiasing);roundFill(p,viewport()->rect(),10,theme_.tokens().fill2);}
    QPlainTextEdit::paintEvent(event);
}

TabStrip::TabStrip(Theme& theme,QWidget* parent):QTabBar(parent),theme_(theme){
    setFont(bodyFont(13,QFont::DemiBold));setExpanding(false);setDrawBase(false);setFocusPolicy(Qt::StrongFocus);
    setElideMode(Qt::ElideRight);setUsesScrollButtons(false);setMinimumHeight(38);
    animation_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&animation_,&QVariantAnimation::valueChanged,this,[this](const QVariant& value){pill_=value.toRectF();update();});
    connect(this,&QTabBar::currentChanged,this,[this](int index){
        if(index<0)return;const QRectF target=QRectF(tabRect(index)).adjusted(3,3,-3,-3);
        animation_.stop();if(theme_.reducedMotion() || pill_.isEmpty()){pill_=target;update();return;}
        animation_.setStartValue(pill_);animation_.setEndValue(target);animation_.setDuration(180);animation_.start();
    });
    connect(&theme_,&Theme::changed,this,qOverload<>(&QWidget::update));
    connect(&theme_,&Theme::motionChanged,this,[this](bool reduced){if(reduced){animation_.stop();if(currentIndex()>=0)pill_=QRectF(tabRect(currentIndex())).adjusted(3,3,-3,-3);update();}});
}
int TabStrip::addItem(const QString& text,const QString& icon){icons_.append(icon);return addTab(text);}
void TabStrip::setHighlightVisible(bool visible){highlightVisible_=visible;update();}
QSize TabStrip::tabSizeHint(int index)const{return QSize(QFontMetrics(font()).horizontalAdvance(tabText(index))+32+(icons_.value(index).isEmpty()?0:22),38);}
QSize TabStrip::sizeHint()const{int width=0;for(int i=0;i<count();++i)width+=tabSizeHint(i).width();return QSize(width,38);}
void TabStrip::paintEvent(QPaintEvent*){
    const auto& t=theme_.tokens();QPainter p(this);p.setRenderHint(QPainter::Antialiasing);roundFill(p,rect(),19,t.fill1);
    if(currentIndex()>=0 && !animation_.state())pill_=QRectF(tabRect(currentIndex())).adjusted(3,3,-3,-3);
    if(highlightVisible_)roundFill(p,pill_,16,t.accent);
    for(int i=0;i<count();++i){
        const QRectF r=QRectF(tabRect(i));const bool active=i==currentIndex()&&highlightVisible_;const QColor fg=active?t.accentForeground:t.muted;
        const bool icon=!icons_.value(i).isEmpty();const int textWidth=QFontMetrics(font()).horizontalAdvance(tabText(i));
        const qreal start=r.center().x()-(textWidth+(icon?22:0))/2.;
        if(icon)paintIcon(p,QRectF(start,r.center().y()-7+.5,15,15),icons_[i],fg);
        p.setPen(fg);p.setFont(font());p.drawText(QRectF(start+(icon?22:0),r.top(),textWidth,r.height()),Qt::AlignCenter,tabText(i));
        if(hasFocus()&&i==currentIndex())paintFocus(p,r.adjusted(1,1,-1,-1),16,t);
    }
}

LoadingCard::LoadingCard(Theme& theme,const QString& text,QWidget* parent):QWidget(parent),theme_(theme),label_(text){
    setMinimumHeight(200);setAccessibleName(text);timer_.setInterval(100);
    connect(&timer_,&QTimer::timeout,this,[this]{frame_=(frame_+1)%18;update();});
    connect(&theme_,&Theme::changed,this,qOverload<>(&QWidget::update));
    connect(&theme_,&Theme::motionChanged,this,[this](bool reduced){if(reduced)timer_.stop();else if(isVisible()&&!error_)timer_.start();update();});
}
void LoadingCard::setLabel(const QString& text){label_=text;setAccessibleName(text);update();}
void LoadingCard::setError(bool error){error_=error;if(error)timer_.stop();else if(isVisible()&&!theme_.reducedMotion())timer_.start();update();}
void LoadingCard::showEvent(QShowEvent* event){QWidget::showEvent(event);if(!theme_.reducedMotion()&&!error_)timer_.start();}
void LoadingCard::hideEvent(QHideEvent* event){timer_.stop();QWidget::hideEvent(event);}
void LoadingCard::paintEvent(QPaintEvent*){
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const auto& t=theme_.tokens();
    const QPointF center(width()/2.,height()/2.-20);const QColor ink=error_?t.danger:t.accentText;
    if(error_)paintIcon(p,QRectF(center-QPointF(20,20),QSizeF(40,40)),"alert",ink);
    else {
        QImage block=functionalImage("blocks/CobbleStone.png");paintContainedImage(p,QRectF(center-QPointF(24,24),QSizeF(48,48)),block,true);
        p.setPen(QPen(ink,2.5,Qt::SolidLine,Qt::RoundCap));p.setBrush(Qt::NoBrush);
        const int start=theme_.reducedMotion()?90:frame_*20;
        p.drawArc(QRectF(center-QPointF(34,34),QSizeF(68,68)),start*16,90*16);
    }
    p.setFont(bodyFont());p.setPen(t.muted);p.drawText(QRectF(20,center.y()+48,width()-40,40),Qt::AlignHCenter|Qt::AlignTop|Qt::TextWordWrap,label_);
}
QLabel* textLabel(Theme& theme,const QString& text,int size,bool muted,QWidget* parent){
    auto* result=new QLabel(text,parent);result->setTextFormat(Qt::PlainText);result->setFont(size>=15?displayFont(size):bodyFont(size));
    result->setWordWrap(true);result->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);
    auto apply=[result,&theme,muted]{auto palette=result->palette();palette.setColor(QPalette::WindowText,muted?theme.tokens().muted:theme.tokens().text);result->setPalette(palette);};
    apply();QObject::connect(&theme,&Theme::changed,result,apply);return result;
}
} // namespace nickel::ui
