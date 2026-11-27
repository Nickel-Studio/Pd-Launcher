#include "app/service/public_dto.hpp"

#include "core/common/error.hpp"

#include <QJsonArray>
#include <QJsonObject>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace nickel::app {
namespace {

struct StaticError {
    std::string_view code;
    const char* message;
    bool retryable;
};

constexpr std::array errors{
    StaticError{"cancelled", "Operation cancelled", false},
    StaticError{"busy", "The operation is busy; please try again", true},
    StaticError{"invalid_argument", "The operation arguments are invalid", false},
    StaticError{"not_found", "The requested item is no longer available", true},
    StaticError{"unauthorized", "The requested operation is not authorized", false},
    StaticError{"unsupported", "This operation is not supported by this build", false},
    StaticError{"not_implemented", "This operation has not been connected", false},
    StaticError{"onboarding_required", "Complete the first-run agreement before continuing", false},
    StaticError{"authentication_required", "Sign in before continuing", false},
    StaticError{"network", "The network request could not be completed", true},
    StaticError{"integrity", "The downloaded content failed integrity verification", true},
    StaticError{"storage", "The local operation could not be committed", true},
    StaticError{"conflict", "The requested operation conflicts with the current state", true},
    StaticError{"missing_service", "A required service or release material is unavailable", false},
    StaticError{"internal", "The operation could not be completed safely", true},
};

const StaticError& error_entry(std::string_view code) {
    for (const auto& entry : errors) if (entry.code == code) return entry;
    if (code == "eula_required" || code == "changelog_required") return errors[7];
    if (code.starts_with("auth.") || code.starts_with("account.")) return errors[8];
    if (code.starts_with("http.") || code.starts_with("network.") || code.starts_with("transport.")) return errors[9];
    if (code.starts_with("hash.") || code.starts_with("archive.")) return errors[10];
    if (code.starts_with("path.") || code.starts_with("unsafe.") || code.starts_with("security.")) return errors[4];
    if (code.starts_with("config.") || code.starts_with("file.") || code.starts_with("io.")) return errors[11];
    return errors.back();
}

Json decode(const QJsonValue& value, unsigned depth) {
    if (depth > 32) fail_static("invalid_argument");
    switch (value.type()) {
    case QJsonValue::Null:
        return nullptr;
    case QJsonValue::Bool:
        return value.toBool();
    case QJsonValue::Double: {
        const double number = value.toDouble();
        if (!std::isfinite(number)) fail_static("invalid_argument");
        if (std::trunc(number) == number && number >= -9007199254740991.0 &&
            number <= 9007199254740991.0) {
            return value.toInteger();
        }
        return number;
    }
    case QJsonValue::String:
        return value.toString().toUtf8().toStdString();
    case QJsonValue::Array: {
        Json result = Json::array();
        const auto array = value.toArray();
        if (array.size() > 4096) fail_static("invalid_argument");
        for (const auto& entry : array) result.push_back(decode(entry, depth + 1));
        return result;
    }
    case QJsonValue::Object: {
        Json result = Json::object();
        const auto object = value.toObject();
        if (object.size() > 4096) fail_static("invalid_argument");
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            result[it.key().toUtf8().toStdString()] = decode(it.value(), depth + 1);
        }
        return result;
    }
    case QJsonValue::Undefined:
        fail_static("invalid_argument");
    }
    fail_static("invalid_argument");
}

QJsonValue encode(const Json& value, unsigned depth) {
    if (depth > 64) fail_static("internal");
    if (value.is_null()) return QJsonValue(QJsonValue::Null);
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number <= static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())) {
            return QJsonValue(static_cast<qint64>(number));
        }
        // QJson cannot store UINT64_MAX; a public string avoids a lossy snapshot.
        return QString::fromStdString(std::to_string(number));
    }
    if (value.is_number_integer()) return QJsonValue(value.get<qint64>());
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        return std::isfinite(number) ? QJsonValue(number) : QJsonValue(QJsonValue::Null);
    }
    if (value.is_string()) return QString::fromUtf8(value.get_ref<const std::string&>());
    if (value.is_array()) {
        QJsonArray result;
        for (const auto& entry : value) result.append(encode(entry, depth + 1));
        return result;
    }
    if (value.is_object()) {
        QJsonObject result;
        for (const auto& [key, entry] : value.items()) {
            result.insert(QString::fromUtf8(key), encode(entry, depth + 1));
        }
        return result;
    }
    fail_static("internal");
}

} // namespace

Json from_public_json(const QJsonValue& value, std::size_t max_bytes) {
    Json result = decode(value, 0);
    if (result.dump().size() > max_bytes) fail_static("invalid_argument");
    return result;
}

QJsonValue to_public_json(const Json& value) { return encode(value, 0); }

ui::UiError public_error(std::string_view code) {
    const auto& entry = error_entry(code);
    return {QString::fromLatin1(entry.code.data(), static_cast<qsizetype>(entry.code.size())),
            QString::fromLatin1(entry.message), {}, entry.retryable};
}

ui::UiError public_error(const std::exception& error) {
    const auto* canonical = dynamic_cast<const ErrorException*>(&error);
    return public_error(canonical ? std::string_view(canonical->error().code) : std::string_view("internal"));
}

void fail_static(std::string_view code) {
    const auto& entry = error_entry(code);
    throw ErrorException(Error{std::string(entry.code), entry.message});
}

std::string bounded_text(const Json& arguments, std::string_view field,
                         std::size_t max_bytes, bool required) {
    if (!arguments.is_object()) fail_static("invalid_argument");
    const auto found = arguments.find(std::string(field));
    if (found == arguments.end()) {
        if (required) fail_static("invalid_argument");
        return {};
    }
    if (!found->is_string()) fail_static("invalid_argument");
    const auto& text = found->get_ref<const std::string&>();
    if ((required && text.empty()) || text.size() > max_bytes || text.find('\0') != std::string::npos) {
        fail_static("invalid_argument");
    }
    return text;
}

std::string operation_id(const Json& arguments, std::string_view field) {
    auto id = bounded_text(arguments, field, 128);
    for (const auto character : id) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 33 || byte > 126) fail_static("invalid_argument");
    }
    return id;
}

} // namespace nickel::app
