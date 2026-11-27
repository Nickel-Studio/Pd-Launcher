#pragma once

#include "core/common/cancellation.hpp"
#include <string>
#include <string_view>

namespace nickel::ui {
// Inject into documents::BodyReader. The returned HTML still requires its Gumbo allowlist.
std::string normalizeMarkdownNoIo(std::string_view markdown, const Cancellation& cancellation);
} // namespace nickel::ui
