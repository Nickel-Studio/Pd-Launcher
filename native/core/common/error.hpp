#pragma once

#include "core/common/json.hpp"
#include <stdexcept>
#include <string>
#include <utility>

namespace nickel {
struct Error {
    std::string code;
    std::string message;
    Json details = Json::object();
};

class ErrorException : public std::runtime_error {
public:
    explicit ErrorException(Error error)
        : std::runtime_error(error.message), error_(std::move(error)) {}
    [[nodiscard]] const Error& error() const noexcept { return error_; }
private:
    Error error_;
};
}
