#include "app/service/public_dto.hpp"
#include "core/common/error.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using nickel::Error;
using nickel::ErrorException;
using nickel::Json;
using namespace nickel::app;

namespace {
template<class Work>
void invalid(Work&& work) {
    bool caught = false;
    try { work(); } catch (const ErrorException& error) { caught = error.error().code == "invalid_argument"; }
    assert(caught);
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QJsonObject fields{{"nested", QJsonObject{{"inherit", QJsonValue(QJsonValue::Null)},
                                                   {"value", 42}}},
                             {"list", QJsonArray{true, "item"}}};
    const auto decoded = from_public_json(fields);
    assert(decoded["nested"]["inherit"].is_null());
    assert(decoded["nested"]["value"] == 42);
    assert(decoded["list"][0] == true);
    assert(to_public_json(decoded).toObject() == fields);
    invalid([&] { from_public_json(fields, 2); });
    invalid([&] { from_public_json(QJsonValue(QJsonValue::Undefined)); });
    QJsonValue deep = true;
    for (unsigned i = 0; i < 34; ++i) deep = QJsonArray{deep};
    invalid([&] { from_public_json(deep); });

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const Json canonical{{"unknown", maximum}, {"nested", Json{{"unknown", maximum}}}};
    const auto snapshot = to_public_json(canonical).toObject();
    assert(snapshot.value("unknown").toString() == "18446744073709551615");
    assert(canonical["nested"]["unknown"].get<std::uint64_t>() == maximum);
    assert(to_public_json(Json(std::numeric_limits<std::int64_t>::max())).toInteger() ==
           std::numeric_limits<std::int64_t>::max());

    const std::string secret = "synthetic-secret-DO-NOT-PUBLISH";
    const ErrorException provider(Error{"auth.remote." + secret, secret, Json{{"token", secret}}});
    const auto safe = public_error(provider);
    assert(safe.code == "authentication_required");
    assert(safe.detail.isEmpty());
    assert(!safe.message.contains(QString::fromStdString(secret)));
    for (const auto code : {"eula_required", "changelog_required"}) {
        const ErrorException gate(Error{code, secret, Json{{"token", secret}}});
        const auto rejected = public_error(gate);
        assert(rejected.code == "onboarding_required");
        assert(!rejected.retryable);
        assert(rejected.detail.isEmpty());
        assert(!rejected.message.contains(QString::fromStdString(secret)));
        assert(public_error(code).code == "onboarding_required");
        bool normalized = false;
        try { fail_static(code); }
        catch (const ErrorException& error) { normalized = error.error().code == "onboarding_required"; }
        assert(normalized);
    }
    const auto unknown = public_error(std::runtime_error(secret));
    assert(unknown.code == "internal");
    assert(!unknown.message.contains(QString::fromStdString(secret)));
    assert(public_error(secret).code == "internal");
    assert(public_error("eula_required.private").code == "internal");
    assert(public_error("changelog_required.private").code == "internal");

    assert(operation_id(Json{{"launchId", "launch-123_abc"}}, "launchId") == "launch-123_abc");
    invalid([&] { operation_id(Json{{"launchId", std::string(129, 'a')}}, "launchId"); });
    invalid([&] { operation_id(Json{{"launchId", "line\nsecret"}}, "launchId"); });
    invalid([&] { operation_id(Json{{"launchId", "name with space"}}, "launchId"); });
    invalid([&] { bounded_text(Json{{"path", std::string("bad\0path", 8)}}, "path", 100); });
    std::cout << "public DTO: null/limits/uint64/static-error/onboarding/operation-id controls passed\n";
}
