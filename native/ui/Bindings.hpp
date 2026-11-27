#pragma once

#include "UiServices.hpp"
#include <QHash>
#include <QPointer>
#include <functional>

namespace nickel::ui {

// Marshals UI queries/commands to UiServices on the GUI thread. Each pending callback is
// bound to an owner QObject: destroying the owner cancels its requests and drops late replies.
class Bindings final : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const UiReply&)>;
    explicit Bindings(UiServices& services, QObject* parent = nullptr);
    ~Bindings() override;
    RequestId query(UiQuery query, const QJsonObject& scope, QObject* owner, Callback callback);
    RequestId command(UiAction action, const QJsonObject& arguments, QObject* owner, Callback callback = {});
    void cancel(RequestId id);
    void forget(QObject* owner);
    UiServices& services() const { return services_; }

signals:
    void failure(const nickel::ui::UiError& error);

private:
    struct Pending {
        QPointer<QObject> owner;
        QObject* identity;
        Callback callback;
    };
    void watchOwner(QObject* owner);
    void receive(const UiReply& reply, QHash<RequestId, Pending>& pending, bool command);
    UiServices& services_;
    QHash<RequestId, Pending> queries_, commands_;
    QHash<QObject*, QMetaObject::Connection> owners_;
};

} // namespace nickel::ui
