#include "Bindings.hpp"

#include <QThread>
#include <QVector>
#include <atomic>

namespace nickel::ui {
namespace {
std::atomic<RequestId> nextBindingRequest{1};
}
Bindings::Bindings(UiServices& services, QObject* parent) : QObject(parent), services_(services) {
    qRegisterMetaType<UiReply>();
    qRegisterMetaType<UiError>();
    qRegisterMetaType<UiTopic>();
    connect(&services_, &UiServices::queryReady, this, [this](const UiReply& reply) {
        receive(reply, queries_, false);
    }, Qt::QueuedConnection);
    connect(&services_, &UiServices::commandFinished, this, [this](const UiReply& reply) {
        receive(reply, commands_, true);
    }, Qt::QueuedConnection);
}
Bindings::~Bindings() {
    for (const auto& connection : owners_) disconnect(connection);
    owners_.clear();
    const auto queryIds = queries_.keys();
    const auto commandIds = commands_.keys();
    queries_.clear();
    commands_.clear();
    for (const auto id : queryIds) services_.cancelRequest(id);
    for (const auto id : commandIds) services_.cancelRequest(id);
}
void Bindings::watchOwner(QObject* owner) {
    Q_ASSERT(owner && owner->thread() == thread());
    if (owners_.contains(owner)) return;
    owners_.insert(owner, connect(owner, &QObject::destroyed, this, [this, owner] {
        // QPointer is already null during destroyed(); retain only the identity for cleanup.
        owners_.remove(owner);
        forget(owner);
    }));
}
RequestId Bindings::query(UiQuery operation, const QJsonObject& scope, QObject* owner, Callback callback) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!owner) return 0;
    watchOwner(owner);
    const auto id = nextBindingRequest.fetch_add(1);
    queries_.insert(id, {owner, owner, std::move(callback)});
    services_.request(id, operation, scope);
    return id;
}
RequestId Bindings::command(UiAction action, const QJsonObject& args, QObject* owner, Callback callback) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!owner) return 0;
    watchOwner(owner);
    const auto id = nextBindingRequest.fetch_add(1);
    commands_.insert(id, {owner, owner, std::move(callback)});
    services_.submit(id, action, args);
    return id;
}
void Bindings::cancel(RequestId id) {
    const bool query = queries_.remove(id) != 0;
    const bool command = commands_.remove(id) != 0;
    if (query || command) services_.cancelRequest(id);
}
void Bindings::forget(QObject* owner) {
    QVector<RequestId> ids;
    for (auto it = queries_.begin(); it != queries_.end();) {
        if (it->identity == owner) { ids.append(it.key()); it = queries_.erase(it); }
        else ++it;
    }
    for (auto it = commands_.begin(); it != commands_.end();) {
        if (it->identity == owner) { ids.append(it.key()); it = commands_.erase(it); }
        else ++it;
    }
    for (const auto id : ids) services_.cancelRequest(id);
}
void Bindings::receive(const UiReply& reply, QHash<RequestId, Pending>& pending, bool command) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto it = pending.find(reply.id);
    if (it == pending.end()) return;
    Pending entry = std::move(it.value());
    pending.erase(it);
    if (entry.owner.isNull()) return;
    if (entry.callback) entry.callback(reply);
    else if (command && reply.error.isError()) emit failure(reply.error);
}
} // namespace nickel::ui
