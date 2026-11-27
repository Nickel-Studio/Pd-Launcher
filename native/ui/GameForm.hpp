#pragma once

#include "PageShell.hpp"
#include <QJsonArray>

namespace nickel::ui {

class GameForm final : public QWidget {
    Q_OBJECT
public:
    GameForm(Theme& theme, bool perVersion, QWidget* parent = nullptr);
    void setValues(const QJsonObject& values, const QJsonObject& effective = {});
    void setJavaInstallations(const QJsonArray& items);
    void setRuntime(const QJsonObject& runtime);
    void setAvailable(bool available);

signals:
    void patchRequested(const QJsonObject& patch);
    void javaRefreshRequested();
    void offlineSkinRequested(const QJsonObject& arguments);

private:
    enum class Type { Text, Number, Boolean, Choice, Multiline };
    struct Field { QString key; Type type; QWidget* editor; CheckBox* inherit = nullptr; };
    void addField(FormGroup* group, const QString& key, const QString& label, Type type,
                  const QList<QPair<QString, QString>>& options = {}, const QString& description = {});
    void edit(const QString& key, const QJsonValue& value);
    Theme& theme_;
    bool perVersion_, applying_ = false, available_ = false;
    QHash<QString, Field> fields_;
    QJsonObject values_, effective_;
    QLabel* memoryInfo_;
    QWidget* memoryGraph_;
    QJsonObject runtime_;
};
} // namespace nickel::ui
