#pragma once

#include "core/common/json.hpp"
#include "ui/UiServices.hpp"

#include <QJsonValue>
#include <QString>

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

namespace nickel::app {

// Conversion is one-way for canonical snapshots; UI sends changed fields only.
// from_public_json() rejects payloads above its byte budget instead of truncating them.
[[nodiscard]] Json from_public_json(const QJsonValue& value, std::size_t max_bytes = 256 * 1024);
[[nodiscard]] QJsonValue to_public_json(const Json& value);
[[nodiscard]] ui::UiError public_error(const std::exception& error);
[[nodiscard]] ui::UiError public_error(std::string_view code);
[[noreturn]] void fail_static(std::string_view code);
[[nodiscard]] std::string bounded_text(const Json& arguments, std::string_view field,
                                       std::size_t max_bytes, bool required = true);
[[nodiscard]] std::string operation_id(const Json& arguments, std::string_view field);

} // namespace nickel::app
