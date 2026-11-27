#include "NativeMarkdown.hpp"
#include "core/documents/qt_body.hpp"

namespace nickel::ui {
std::string normalizeMarkdownNoIo(std::string_view markdown, const Cancellation& cancellation) {
    return documents::qt::normalizeMarkdownNoHtml(markdown, cancellation);
}
} // namespace nickel::ui
