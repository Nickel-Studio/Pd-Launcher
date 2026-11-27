#pragma once

#include <QImage>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QUrl>

namespace nickel::ui {

using RequestId = quint64;

// UI-only port. AppServices implements these operations using the native domains.
// JSON fields are the public DTO contract, never account tokens or a UI store.
enum class UiQuery {
    Bootstrap, Accounts, Characters, InstanceRoots, Instances, VersionDetail,
    VersionSettings, JavaInstallations, VanillaCatalog, LoaderCatalog,
    CommunitySearch, ProjectDetail, ProjectVersions, RequiredDependencies,
    InstalledResources, Mods, Settings, Downloads, Logs, ExportTree,
    JunkPreview, RuntimeInformation, Onboarding
};

enum class UiAction {
    SetAccountType, SetActiveAccount, RemoveAccount, BeginMicrosoftLogin,
    CancelMicrosoftLogin, LoginSkinServer, SelectCharacter, CancelCharacterSelection,
    RefreshCharacters, UpdateOfflineSkin, ResetOfflineSkin, UploadSkin, ChangePlayerName,
    ActivateRoot, AddRoot, CreateRoot, RenameRoot, RemoveRoot, DeleteRoot,
    SelectInstance, RenameInstance, SetInstanceIcon, SetInstanceFavorite,
    DeleteInstance, PatchVersionSettings, ResetVersionSettings,
    InstallVersion, ImportPack, InstallProject, DownloadProjectFile,
    ImportResources, DeleteResource, SetModEnabled, RenameMod, DeleteMods,
    CheckModUpdates, UpdateMods, ExportMods, ExportPack, PatchSettings,
    LaunchGame, CancelLaunch, RefreshDownloads, CancelDownload,
    DismissDownload, DismissDownloadBubble, ClearDownloadError,
    OpenPath, OpenExternalUrl, ExportLogs, ExportErrorReport,
    ScanJunk, CleanJunk, ClearLaunchError,
    AcceptEula, AcknowledgeChangelog, FinishOnboarding, RejectEula
};

enum class UiTopic {
    Accounts, Instances, Version, Settings, Downloads, Launch, Resources, Mods, Logs,
    Onboarding
};

struct UiError {
    QString code;
    QString message;
    QString detail;
    bool retryable = false;
    [[nodiscard]] bool isError() const { return !code.isEmpty() || !message.isEmpty(); }
};

struct UiReply {
    RequestId id = 0;
    QJsonValue data;
    UiError error;
};

struct DescriptionImageReply {
    RequestId id = 0;
    QString documentKey;
    QString opaqueKey;
    QImage image;
    UiError error;
};

class UiServices : public QObject {
    Q_OBJECT
public:
    explicit UiServices(QObject* parent = nullptr) : QObject(parent) {}
    ~UiServices() override = default;

    virtual void request(RequestId id, UiQuery query, const QJsonObject& scope) = 0;
    virtual void submit(RequestId id, UiAction action, const QJsonObject& arguments) = 0;
    // Opaque keys resolve only against the immutable, service-owned SafeBody map.
    virtual void resolveDescriptionImage(RequestId id, const QString& documentKey,
                                         const QString& opaqueKey) = 0;
    virtual void releaseDescription(const QString& documentKey) { Q_UNUSED(documentKey); }
    virtual void cancelRequest(RequestId id) { Q_UNUSED(id); }

signals:
    void queryReady(const nickel::ui::UiReply& reply);
    void commandFinished(const nickel::ui::UiReply& reply);
    void stateChanged(nickel::ui::UiTopic topic, const QJsonObject& snapshot);
    void mediaReady(const QString& key, const QImage& image);
    void descriptionImageReady(const nickel::ui::DescriptionImageReply& reply);
};

} // namespace nickel::ui

Q_DECLARE_METATYPE(nickel::ui::UiQuery)
Q_DECLARE_METATYPE(nickel::ui::UiAction)
Q_DECLARE_METATYPE(nickel::ui::UiTopic)
Q_DECLARE_METATYPE(nickel::ui::UiError)
Q_DECLARE_METATYPE(nickel::ui::UiReply)
Q_DECLARE_METATYPE(nickel::ui::DescriptionImageReply)
