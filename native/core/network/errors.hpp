#pragma once

#include "core/common/error.hpp"
#include <functional>
#include <string_view>
#include <system_error>
#include <utility>

namespace nickel::transport::detail {
[[noreturn]] inline void fail(std::string_view code, std::string_view message) {
    throw ErrorException(Error{std::string(code), std::string(message)});
}

template<class Function>
decltype(auto) boundary(std::string_view code, std::string_view message, Function&& function) {
    try {
        return std::invoke(std::forward<Function>(function));
    } catch (const ErrorException&) {
        throw;
    } catch (const std::system_error& error) {
        throw ErrorException(Error{std::string(code), std::string(message),
            Json{{"nativeError", error.code().value()}}});
    } catch (const std::exception&) {
        fail(code, message);
    }
}
} // namespace nickel::transport::detail
