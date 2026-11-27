#pragma once

#include "Theme.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QVBoxLayout>

namespace nickel::ui {

enum class ButtonKind { Primary, Secondary, Outline, Ghost, Danger, Navigation };
enum class SurfaceKind { Card, Glass, Fill, None };

// All custom controls paint from ThemeTokens on every paint; there is no stylesheet layer,
// so an appearance change only needs a repaint rather than a QSS reload.
class Panel : public QWidget {
    Q_OBJECT
public:
    explicit Panel(Theme& theme, QWidget* parent = nullptr, SurfaceKind surface = SurfaceKind::Card);
    void setRadius(qreal radius);
    void setSurface(SurfaceKind kind);
    void setShadow(int level);
    QVBoxLayout* column(int padding = 20, int spacing = 16);
protected:
    void paintEvent(QPaintEvent*) override;
    Theme& theme_;
    SurfaceKind surface_;
    qreal radius_ = 18;
    int shadow_ = 1;
};

class Button : public QPushButton {
    Q_OBJECT
public:
    Button(Theme& theme, const QString& text = {}, ButtonKind kind = ButtonKind::Secondary, QWidget* parent = nullptr);
    void setIconName(const QString& name);
    void setSubtitle(const QString& text);
    void setRadius(qreal radius);
    void setKind(ButtonKind kind);
    void setTextAlignment(Qt::Alignment alignment);
    void setAccentSwatch(const QColor& color);
    void setBusy(bool busy);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
private:
    Theme& theme_;
    ButtonKind kind_;
    QString iconName_, subtitle_;
    QColor swatch_;
    qreal radius_ = 999;
    Qt::Alignment alignment_ = Qt::AlignCenter;
    bool keyboardFocus_ = false, busy_ = false;
};

class LineEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit LineEdit(Theme& theme, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
private:
    Theme& theme_;
};

class CheckBox : public QCheckBox {
    Q_OBJECT
public:
    CheckBox(Theme& theme, const QString& text = {}, QWidget* parent = nullptr, bool switchStyle = false);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent*) override;
    bool hitButton(const QPoint& position) const override;
private:
    Theme& theme_;
    bool switchStyle_;
};

class RadioButton : public QRadioButton {
    Q_OBJECT
public:
    RadioButton(Theme& theme, const QString& text, QWidget* parent = nullptr);
    void setSwatch(QColor color);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent*) override;
private:
    Theme& theme_;
    QColor swatch_;
};

class Slider : public QSlider {
    Q_OBJECT
public:
    explicit Slider(Theme& theme, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
private:
    void setFromPosition(qreal x);
    Theme& theme_;
};

class PopupDelegate final : public QStyledItemDelegate {
public:
    PopupDelegate(Theme& theme, QObject* parent = nullptr) : QStyledItemDelegate(parent), theme_(theme) {}
    void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
private:
    Theme& theme_;
};

class ComboBox : public QComboBox {
    Q_OBJECT
public:
    explicit ComboBox(Theme& theme, QWidget* parent = nullptr);
    void showPopup() override;
protected:
    void paintEvent(QPaintEvent*) override;
private:
    Theme& theme_;
};

class ScrollBar : public QScrollBar {
    Q_OBJECT
public:
    ScrollBar(Theme& theme, Qt::Orientation orientation, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
private:
    QRectF thumbRect() const;
    Theme& theme_;
    qreal dragOffset_ = -1;
};

class ScrollArea : public QScrollArea {
    Q_OBJECT
public:
    explicit ScrollArea(Theme& theme, QWidget* parent = nullptr);
};

class TextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit TextEdit(Theme& theme, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    Theme& theme_;
};

class TabStrip : public QTabBar {
    Q_OBJECT
public:
    explicit TabStrip(Theme& theme, QWidget* parent = nullptr);
    int addItem(const QString& text, const QString& iconName = {});
    void setHighlightVisible(bool visible);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent*) override;
    QSize tabSizeHint(int index) const override;
private:
    Theme& theme_;
    QStringList icons_;
    bool highlightVisible_ = true;
    QRectF pill_;
    QVariantAnimation animation_;
};

class LoadingCard : public QWidget {
    Q_OBJECT
public:
    explicit LoadingCard(Theme& theme, const QString& label, QWidget* parent = nullptr);
    void setLabel(const QString& text);
    void setError(bool error);
protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    Theme& theme_;
    QString label_;
    QTimer timer_;
    int frame_ = 0;
    bool error_ = false;
};

QLabel* textLabel(Theme& theme, const QString& text, int size = 13, bool muted = false, QWidget* parent = nullptr);
void paintFocus(QPainter& painter, const QRectF& rect, qreal radius, const ThemeTokens& tokens);

} // namespace nickel::ui
