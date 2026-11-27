#pragma once

#include "ui/UiServices.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

namespace nickel::production_smoke {

inline const char* port_name(ui::UiQuery query) {
    switch (query) {
#define SMOKE_QUERY(name) case ui::UiQuery::name: return #name;
        SMOKE_QUERY(Bootstrap) SMOKE_QUERY(Accounts) SMOKE_QUERY(Characters)
        SMOKE_QUERY(InstanceRoots) SMOKE_QUERY(Instances) SMOKE_QUERY(VersionDetail)
        SMOKE_QUERY(VersionSettings) SMOKE_QUERY(JavaInstallations) SMOKE_QUERY(VanillaCatalog)
        SMOKE_QUERY(LoaderCatalog) SMOKE_QUERY(CommunitySearch) SMOKE_QUERY(ProjectDetail)
        SMOKE_QUERY(ProjectVersions) SMOKE_QUERY(RequiredDependencies) SMOKE_QUERY(InstalledResources)
        SMOKE_QUERY(Mods) SMOKE_QUERY(Settings) SMOKE_QUERY(Downloads) SMOKE_QUERY(Logs)
        SMOKE_QUERY(ExportTree) SMOKE_QUERY(JunkPreview) SMOKE_QUERY(RuntimeInformation)
        SMOKE_QUERY(Onboarding)
#undef SMOKE_QUERY
    }
    throw std::runtime_error("unknown production query enum");
}

inline const char* port_name(ui::UiAction action) {
    switch (action) {
#define SMOKE_ACTION(name) case ui::UiAction::name: return #name;
        SMOKE_ACTION(SetAccountType) SMOKE_ACTION(SetActiveAccount) SMOKE_ACTION(RemoveAccount)
        SMOKE_ACTION(BeginMicrosoftLogin) SMOKE_ACTION(CancelMicrosoftLogin) SMOKE_ACTION(LoginSkinServer)
        SMOKE_ACTION(SelectCharacter) SMOKE_ACTION(CancelCharacterSelection) SMOKE_ACTION(RefreshCharacters)
        SMOKE_ACTION(UpdateOfflineSkin) SMOKE_ACTION(ResetOfflineSkin) SMOKE_ACTION(UploadSkin)
        SMOKE_ACTION(ChangePlayerName) SMOKE_ACTION(ActivateRoot) SMOKE_ACTION(AddRoot)
        SMOKE_ACTION(CreateRoot) SMOKE_ACTION(RenameRoot) SMOKE_ACTION(RemoveRoot) SMOKE_ACTION(DeleteRoot)
        SMOKE_ACTION(SelectInstance) SMOKE_ACTION(RenameInstance) SMOKE_ACTION(SetInstanceIcon)
        SMOKE_ACTION(SetInstanceFavorite) SMOKE_ACTION(DeleteInstance) SMOKE_ACTION(PatchVersionSettings)
        SMOKE_ACTION(ResetVersionSettings) SMOKE_ACTION(InstallVersion) SMOKE_ACTION(ImportPack)
        SMOKE_ACTION(InstallProject) SMOKE_ACTION(DownloadProjectFile) SMOKE_ACTION(ImportResources)
        SMOKE_ACTION(DeleteResource) SMOKE_ACTION(SetModEnabled) SMOKE_ACTION(RenameMod)
        SMOKE_ACTION(DeleteMods) SMOKE_ACTION(CheckModUpdates) SMOKE_ACTION(UpdateMods)
        SMOKE_ACTION(ExportMods) SMOKE_ACTION(ExportPack) SMOKE_ACTION(PatchSettings)
        SMOKE_ACTION(LaunchGame) SMOKE_ACTION(CancelLaunch) SMOKE_ACTION(RefreshDownloads)
        SMOKE_ACTION(CancelDownload) SMOKE_ACTION(DismissDownload) SMOKE_ACTION(DismissDownloadBubble)
        SMOKE_ACTION(ClearDownloadError) SMOKE_ACTION(OpenPath) SMOKE_ACTION(OpenExternalUrl)
        SMOKE_ACTION(ExportLogs) SMOKE_ACTION(ExportErrorReport) SMOKE_ACTION(ScanJunk)
        SMOKE_ACTION(CleanJunk) SMOKE_ACTION(ClearLaunchError) SMOKE_ACTION(AcceptEula)
        SMOKE_ACTION(AcknowledgeChangelog) SMOKE_ACTION(FinishOnboarding) SMOKE_ACTION(RejectEula)
#undef SMOKE_ACTION
    }
    throw std::runtime_error("unknown production action enum");
}


class Probe final {
public:
    explicit Probe(ui::UiServices& services) : services_(services) {
        if (services_.thread() != QThread::currentThread()) {
            throw std::runtime_error("AppServices constructed on the wrong QObject thread");
        }
        QObject::connect(&services_, &ui::UiServices::queryReady, &context_,
                         [this](const ui::UiReply& reply) { receive(false, reply); });
        QObject::connect(&services_, &ui::UiServices::commandFinished, &context_,
                         [this](const ui::UiReply& reply) { receive(true, reply); });
        QObject::connect(&services_, &ui::UiServices::stateChanged, &context_,
                         [this](ui::UiTopic topic, const QJsonObject& snapshot) {
                             on_thread();
                             if (events_.size() >= 2048) {
                                 overflow_ = true;
                                 return;
                             }
                             events_.append(QJsonObject{{"topic", static_cast<int>(topic)},
                                                        {"snapshot", snapshot}});
                         });
        const auto monitor = monitor_;
        const auto expected = services_.thread();
        QObject::connect(&services_, &ui::UiServices::queryReady, &context_,
                         [monitor, expected](const ui::UiReply&) {
                             if (QThread::currentThread() != expected) {
                                 monitor->wrongThread.store(true);
                             }
                         }, Qt::DirectConnection);
        QObject::connect(&services_, &ui::UiServices::commandFinished, &context_,
                         [monitor, expected](const ui::UiReply&) {
                             if (QThread::currentThread() != expected) {
                                 monitor->wrongThread.store(true);
                             }
                         }, Qt::DirectConnection);
    }

    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    [[nodiscard]] ui::RequestId query(ui::UiQuery queryKind, const QJsonObject& scope = {}) {
        const auto id = next_++;
        request_with_id(id, queryKind, scope);
        return id;
    }

    void request_with_id(ui::RequestId id, ui::UiQuery queryKind, const QJsonObject& scope = {}) {
        remember(false, id, static_cast<int>(queryKind), port_name(queryKind), scope);
        services_.request(id, queryKind, scope);
    }

    void supersede_pending(ui::RequestId id, ui::UiQuery queryKind, const QJsonObject& scope) {
        on_thread();
        const auto key = std::make_pair(false, id);
        if (!requests_.contains(key) || replies_.contains(key) || replyCounts_.contains(key)) {
            throw std::runtime_error("cannot supersede a delivered or unknown production request");
        }
        requests_.erase(key);
        request_with_id(id, queryKind, scope);
    }

    [[nodiscard]] ui::RequestId submit(ui::UiAction actionKind, const QJsonObject& arguments = {}) {
        const auto id = next_++;
        remember(true, id, static_cast<int>(actionKind), port_name(actionKind), arguments);
        services_.submit(id, actionKind, arguments);
        return id;
    }

    void cancel(ui::RequestId id) {
        on_thread();
        services_.cancelRequest(id);
    }

    void reuse_completed(ui::RequestId id, bool command) {
        healthy();
        const auto key = std::make_pair(command, id);
        if (!replies_.contains(key) || replyCounts_.at(key) != 1) {
            throw std::runtime_error("cannot reuse an incomplete production request");
        }
        replies_.erase(key);
        replyCounts_.erase(key);
        requests_.erase(key);
    }

    [[nodiscard]] QJsonObject submitted(ui::RequestId id, bool command) const {
        return requests_.at(std::make_pair(command, id));
    }

    [[nodiscard]] int reply_count(ui::RequestId id, bool command) const {
        const auto found = replyCounts_.find(std::make_pair(command, id));
        return found == replyCounts_.end() ? 0 : found->second;
    }


    [[nodiscard]] ui::UiReply await(ui::RequestId id, bool command, int timeoutMs = 15000) {
        on_thread();
        const auto key = std::make_pair(command, id);
        if (!replies_.contains(key)) {
            QEventLoop loop;
            QTimer poll;
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                if (replies_.contains(key)) {
                    loop.quit();
                }
            });
            QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
            poll.start(2);
            deadline.start(timeoutMs);
            loop.exec();
        }
        if (!replies_.contains(key)) {
            throw std::runtime_error("production QObject reply timeout");
        }
        healthy();
        if (replyCounts_.at(key) != 1) {
            throw std::runtime_error("duplicate production QObject reply");
        }
        return replies_.at(key);
    }

    [[nodiscard]] ui::UiReply request(ui::UiQuery queryKind, const QJsonObject& scope = {}) {
        return await(query(queryKind, scope), false);
    }

    [[nodiscard]] ui::UiReply action(ui::UiAction actionKind, const QJsonObject& arguments = {}) {
        return await(submit(actionKind, arguments), true);
    }

    [[nodiscard]] const QJsonArray& events() const { return events_; }

    void drain(int milliseconds = 30) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
        healthy();
    }

    void healthy() const {
        if (monitor_->wrongThread.load() || wrongSlotThread_ || overflow_) {
            throw std::runtime_error("wrong QObject signal thread or unbounded notifications");
        }
        for (const auto& [key, count] : replyCounts_) {
            static_cast<void>(key);
            if (count != 1) {
                throw std::runtime_error("duplicate production reply after delivery");
            }
        }
    }

private:
    struct Monitor { std::atomic_bool wrongThread{false}; };

    void remember(bool command, ui::RequestId id, int operation, const char* name, const QJsonObject& arguments) {
        on_thread();
        const auto key = std::make_pair(command, id);
        if (id == 0 || requests_.contains(key) || requests_.size() >= 2048) {
            throw std::runtime_error("invalid, reused or unbounded probe request");
        }
        requests_.emplace(key, QJsonObject{{"id", static_cast<qint64>(id)},
            {"channel", command ? "command" : "query"}, {"operation", operation},
            {"name", QString::fromLatin1(name)}, {"arguments", arguments}});
    }

    void on_thread() {
        if (context_.thread() != QThread::currentThread()) {
            wrongSlotThread_ = true;
        }
    }

    void receive(bool command, const ui::UiReply& reply) {
        on_thread();
        const auto key = std::make_pair(command, reply.id);
        if (replyCounts_.size() >= 2048 && !replyCounts_.contains(key)) {
            overflow_ = true;
            return;
        }
        ++replyCounts_[key];
        replies_.insert_or_assign(key, reply);
    }

    ui::UiServices& services_;
    std::shared_ptr<Monitor> monitor_ = std::make_shared<Monitor>();
    ui::RequestId next_ = 1000;
    std::map<std::pair<bool, ui::RequestId>, ui::UiReply> replies_;
    std::map<std::pair<bool, ui::RequestId>, int> replyCounts_;
    std::map<std::pair<bool, ui::RequestId>, QJsonObject> requests_;
    QJsonArray events_;
    bool overflow_ = false;
    bool wrongSlotThread_ = false;
    // Destroy the connection context before captured state is released.
    QObject context_;
};

inline QJsonObject public_reply(const ui::UiReply& reply) {
    return {{"id", static_cast<qint64>(reply.id)}, {"data", reply.data},
            {"error", QJsonObject{{"code", reply.error.code}, {"message", reply.error.message},
                                  {"detail", reply.error.detail}, {"retryable", reply.error.retryable}}}};
}

inline void require_ok(const ui::UiReply& reply) {
    if (reply.error.isError()) {
        throw std::runtime_error("production action/query failed: " + reply.error.code.toStdString());
    }
}

} // namespace nickel::production_smoke
