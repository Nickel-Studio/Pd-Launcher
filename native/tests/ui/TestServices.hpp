#pragma once

#include "ui/UiServices.hpp"
#include <QHash>
#include <QJsonArray>
#include <QTimer>
#include <QVector>

namespace nickel::ui::tests {
// Deterministic public DTOs only. This class is compiled exclusively into QtTest.
class TestServices final : public UiServices {
public:
    struct Query { RequestId id; UiQuery operation; QJsonObject arguments; };
    struct Command { RequestId id; UiAction operation; QJsonObject arguments; };
    QVector<Query> queries;
    QVector<Command> commands;
    QHash<UiQuery, QJsonObject> queryReplies;
    bool automatic = true;
    bool holdFinish = false;
    QJsonObject onboarding{{"eulaVersion", "minecraft-eula-v1"}, {"eulaUrl", "https://www.minecraft.net/zh-hans/eula"},
        {"eulaAcceptedAt", 1700000000}, {"currentVersion", "test-native-fixture"}, {"needsEula", false}, {"needsChangelog", false}, {"exiting", false}};
    QJsonObject settings{{"themeMode", "dark"}, {"accent", "mono"}, {"offlineUsername", "FixturePlayer"}, {"accountType", "offline"},
        {"backgroundImage", QJsonValue(QJsonValue::Null)}, {"backgroundOpacity", .5}, {"reducedMotion", true}, {"shadows", false}};
    QJsonObject accounts{{"items", QJsonArray{}}, {"activeId", ""}, {"accountType", "offline"}, {"offlineUsername", "FixturePlayer"}};
    QJsonObject roots{{"items", QJsonArray{QJsonObject{{"dir", "fixture-root"}, {"label", "Fixture"}, {"isDefault", true}}}}, {"active", "fixture-root"}};
    QJsonObject selected{{"root", "fixture-root"}, {"versionId", "1.20.1"}, {"displayName", "Native fixture"}};
    QJsonObject downloads{{"ready", true}, {"items", QJsonArray{}}, {"overview", QJsonObject{}}};

    void request(RequestId id, UiQuery query, const QJsonObject& scope) override {
        queries.append({id, query, scope}); if (!automatic) return;
        QJsonObject data;
        switch (query) {
        case UiQuery::Bootstrap: data = {{"settings", settings}, {"accounts", accounts}, {"roots", roots}, {"selectedInstance", selected}, {"downloads", downloads}}; break;
        case UiQuery::Onboarding: data = onboarding; break;
        case UiQuery::Accounts: data = accounts; break;
        case UiQuery::InstanceRoots: data = roots; break;
        case UiQuery::Settings: data = {{"values", settings}}; break;
        case UiQuery::Downloads: data = downloads; break;
        case UiQuery::VersionDetail: data = scope; data.insert("name", "Native fixture"); data.insert("minecraftVersion", "1.20.1"); break;
        case UiQuery::VersionSettings: data = scope; data.insert("values", QJsonObject{}); data.insert("effective", QJsonObject{}); break;
        default: data = scope; data.insert("items", QJsonArray{}); break;
        }
        if (queryReplies.contains(query)) {
            data = scope;
            const auto snapshot = queryReplies.value(query);
            for (auto field = snapshot.begin(); field != snapshot.end(); ++field) data.insert(field.key(), field.value());
        }
        QTimer::singleShot(0, this, [this, id, data] { emit queryReady({id, data, {}}); });
    }
    void submit(RequestId id, UiAction action, const QJsonObject& args) override {
        commands.append({id, action, args}); if (!automatic || (action == UiAction::FinishOnboarding && holdFinish)) return;
        QJsonObject data;
        if (action == UiAction::AcceptEula) { onboarding.insert("needsEula", false); data = onboarding; }
        if (action == UiAction::AcknowledgeChangelog) { onboarding.insert("needsChangelog", false); data = onboarding; }
        QTimer::singleShot(0, this, [this, id, data] { emit commandFinished({id, data, {}}); });
    }
    struct ImageRequest { RequestId id; QString documentKey; QString opaqueKey; };
    QVector<ImageRequest> imageRequests;
    QVector<RequestId> cancellations;
    QStringList releasedDocuments;
    void resolveDescriptionImage(RequestId id, const QString& documentKey, const QString& opaqueKey) override { imageRequests.append({id, documentKey, opaqueKey}); }
    void releaseDescription(const QString& key) override { releasedDocuments.append(key); }
    void cancelRequest(RequestId id) override { cancellations.append(id); }
    void deliverQuery(RequestId id, const QJsonObject& data) { emit queryReady({id, data, {}}); }
    void deliverCommand(RequestId id, const QJsonObject& data = {}) { emit commandFinished({id, data, {}}); }
};
} // namespace nickel::ui::tests
