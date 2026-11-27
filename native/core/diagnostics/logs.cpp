#include "logs.hpp"
#include "core/instances/domain_io.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace nickel::diagnostics {
namespace {
using instances::Error;
namespace io = instances::detail;
constexpr std::size_t redaction_document_limit = 8 * 1024 * 1024;
constexpr std::size_t collection_limit = 1024 * 1024;
std::mutex lease_mutex;
std::set<std::string> leases;
struct Span { std::size_t start, end; std::string replacement; };
struct Incomplete { std::size_t start; };
struct Invalid {};
bool key_byte(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }
bool sensitive_key(std::string_view key, bool& token) {
    std::string lower;
    for (unsigned char c : key) if (c != '-' && c != '_') lower += static_cast<char>(std::tolower(c));
    constexpr std::array<std::string_view, 9> tokens{"accesstoken", "clienttoken", "refreshtoken", "sessiontoken", "authtoken", "bearertoken", "apitoken", "token", "credentialtoken"};
    constexpr std::array<std::string_view, 17> secrets{"password", "passwords", "passwd", "pwd", "secret", "secrets", "clientsecret", "authorization", "invitecode", "invitationcode", "apikey", "apikeys", "xapikey", "privatekey", "secretkey", "clientcredential", "credentials"};
    token = std::find(tokens.begin(), tokens.end(), lower) != tokens.end();
    return token || std::find(secrets.begin(), secrets.end(), lower) != secrets.end();
}
std::string token_mask(std::string_view input, bool token) {
    if (!token || input.size() < 10) return "***";
    if (input.find("***") != std::string_view::npos) return std::string(input);
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < input.size(); ++i)
        if ((static_cast<unsigned char>(input[i]) & 0xc0) != 0x80) offsets.push_back(i);
    if (offsets.size() < 10) return "***";
    return std::string(input.substr(0, offsets[4])) + "***" + std::string(input.substr(offsets[offsets.size() - 4]));
}
std::size_t skip_space(std::string_view text, std::size_t position, bool newlines = true) {
    while (position < text.size() && (newlines ? std::isspace(static_cast<unsigned char>(text[position])) : (text[position] == ' ' || text[position] == '\t'))) ++position;
    return position;
}
std::size_t line_end(std::string_view text, std::size_t position) {
    const auto end = text.find_first_of("\r\n", position);
    return end == std::string_view::npos ? text.size() : end;
}
std::size_t line_start(std::string_view text, std::size_t position) {
    const auto before = text.substr(0, position).find_last_of("\r\n");
    return before == std::string_view::npos ? 0 : before + 1;
}
std::size_t indent(std::string_view text, std::size_t position) {
    auto start = line_start(text, position);
    return skip_space(text, start, false) - start;
}
std::pair<std::size_t, std::size_t> quote_end(std::string_view text, std::size_t start) {
    const char delimiter = text[start];
    const auto width = delimiter != '`' && start + 2 < text.size() && text[start + 1] == delimiter && text[start + 2] == delimiter ? 3u : 1u;
    bool escape = false;
    for (std::size_t i = start + width; i < text.size(); ++i) {
        if (i - start > collection_limit) throw Invalid{};
        if (!escape && text[i] == delimiter && (width == 1 || (i + 2 < text.size() && text[i + 1] == delimiter && text[i + 2] == delimiter))) return {i, i + width};
        escape = text[i] == '\\' && !escape;
    }
    throw Incomplete{start};
}
std::string decode_quote(std::string_view inner, char delimiter) {
    if (delimiter == '"') {
        try { return Json::parse('"' + std::string(inner) + '"').get<std::string>(); }
        catch (...) {}
    }
    std::string result;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        const char c = inner[i];
        if (c == '\\' && i + 1 < inner.size()) {
            const char next = inner[++i];
            if (next == 'n') result += '\n';
            else if (next == 'r') result += '\r';
            else if (next == 't') result += '\t';
            else if (next == 'u' && i + 4 < inner.size()) {
                try {
                    const auto raw = "\"\\u" + std::string(inner.substr(i + 1, 4)) + "\"";
                    result += Json::parse(raw).get<std::string>();
                    i += 4;
                } catch (...) { throw Invalid{}; }
            } else result += next;
        } else result += c;
    }
    return result;
}
std::size_t collection_end(std::string_view text, std::size_t start) {
    std::vector<char> stack;
    for (std::size_t i = start; i < text.size(); ++i) {
        if (i - start > collection_limit) throw Invalid{};
        const auto c = text[i];
        if (c == '"' || c == '\'' || c == '`') {
            const auto quote = quote_end(text, i);
            i = quote.second - 1;
        } else if (c == '{' || c == '[') {
            if (stack.size() >= 64) throw Invalid{};
            stack.push_back(c);
        } else if (c == '}' || c == ']') {
            if (stack.empty() || (c == '}' && stack.back() != '{') || (c == ']' && stack.back() != '[')) throw Invalid{};
            stack.pop_back();
            if (stack.empty()) return i + 1;
        }
    }
    throw Incomplete{start};
}
std::size_t plain_end(std::string_view text, std::size_t start) {
    auto i = start;
    while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && std::string_view("\"'`,;&}])<>").find(text[i]) == std::string_view::npos) ++i;
    return i;
}
std::size_t yaml_end(std::string_view text, std::size_t key, std::size_t start, bool final) {
    const auto base_indent = indent(text, key);
    auto cursor = line_end(text, start);
    auto value_end = cursor;
    while (cursor < text.size()) {
        while (cursor < text.size() && (text[cursor] == '\n' || text[cursor] == '\r')) ++cursor;
        if (cursor == text.size()) break;
        auto end = line_end(text, cursor);
        auto content = skip_space(text, cursor, false);
        if (content < end && content - cursor <= base_indent) return value_end;
        if (end - start > collection_limit) throw Invalid{};
        value_end = end;
        cursor = end;
    }
    if (!final) throw Incomplete{key};
    return value_end;
}
Span sensitive_span(std::string_view text, std::size_t key, std::size_t value,
                    bool token, bool authorization, bool yaml, bool final) {
    if (value >= text.size()) {
        if (!final) throw Incomplete{key};
        return {value, value, ""};
    }
    if (text[value] == '[' || text[value] == '{') return {value, collection_end(text, value), "\"***\""};
    if (text[value] == '"' || text[value] == '\'' || text[value] == '`') {
        const auto [inner_end, end] = quote_end(text, value);
        const auto width = end - inner_end;
        const auto raw = text.substr(value + width, inner_end - value - width);
        if (width > 1 || raw.find_first_of("\r\n") != std::string_view::npos) return {value, end, "\"***\""};
        return {value + width, inner_end, token_mask(raw, token)};
    }
    if ((text[value] == '|' || text[value] == '>') && yaml) return {value, yaml_end(text, key, value, final), "\"***\""};
    if (yaml) return {value, yaml_end(text, key, value, final), "\"***\""};
    auto end = plain_end(text, value);
    if (authorization) {
        const auto lower = io::ascii_lower(text.substr(value));
        if (lower.starts_with("bearer ")) end = plain_end(text, skip_space(text, value + 7, false));
        else if (lower.starts_with("basic ")) end = plain_end(text, skip_space(text, value + 6, false));
        else end = line_end(text, value);
    }
    return {value, end, token_mask(text.substr(value, end - value), token)};
}
std::string apply_spans(std::string_view text, std::vector<Span> spans) {
    std::sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) {
        return a.start != b.start ? a.start < b.start : a.end > b.end;
    });
    std::vector<Span> merged;
    for (auto& span : spans) {
        if (span.end > text.size() || span.end <= span.start) continue;
        if (!merged.empty() && span.start < merged.back().end) {
            auto& previous = merged.back();
            previous.end = std::max(previous.end, span.end);
            previous.replacement = previous.replacement.starts_with('"') && previous.replacement.ends_with('"') ? "\"***\"" : "***";
        } else merged.push_back(std::move(span));
    }
    std::string result;
    result.reserve(text.size());
    std::size_t cursor = 0;
    for (const auto& span : merged) {
        result.append(text.substr(cursor, span.start - cursor));
        result += span.replacement;
        cursor = span.end;
    }
    result.append(text.substr(cursor));
    return result;
}
std::string mask_paths(std::string input) {
    const auto lower = io::ascii_lower(input);
    std::vector<Span> spans;
    const std::array<std::string_view, 10> markers{"\\\\Users\\\\", "\\\\Documents and Settings\\\\", "\\Users\\", "/Users/", "/home/", "\\Documents and Settings\\", "/invite/", "/join/", "/invite%2f", "/join%2f"};
    for (const auto marker : markers) {
        const auto normalized_marker = io::ascii_lower(marker);
        std::size_t cursor = 0;
        while ((cursor = lower.find(normalized_marker, cursor)) != std::string::npos) {
            const auto start = cursor + marker.size();
            auto end = start;
            while (end < input.size() && std::string_view("\\/\"'\r\n?&#)]}").find(input[end]) == std::string_view::npos) ++end;
            if (end > start && std::string_view(input).substr(start, end - start) != "***") spans.push_back({start, end, "***"});
            if (spans.size() > 262144) throw Invalid{};
            cursor = std::max(end, start);
        }
    }
    std::size_t cursor = 0;
    while ((cursor = input.find("://", cursor)) != std::string::npos) {
        const auto start = cursor + 3;
        auto end = start;
        while (end < input.size() && !std::isspace(static_cast<unsigned char>(input[end])) && std::string_view("/?#\"'").find(input[end]) == std::string_view::npos) ++end;
        const auto at = std::string_view(input).substr(start, end - start).rfind('@');
        if (at != std::string::npos && at != 0) spans.push_back({start, start + at, "***"});
        if (spans.size() > 262144) throw Invalid{};
        cursor = start;
    }
    static const std::regex jwt(R"(eyJ[A-Za-z0-9_-]{5,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{16,})");
    return std::regex_replace(apply_spans(input, std::move(spans)), jwt, "***");
}
// Registers the raw, percent-encoded (both hex cases) and JSON-escaped forms so a secret
// is masked whether it surfaces in a URL, a JSON field, or a plain log line.
void register_secret(std::vector<std::string>& values, std::string_view secret) {
    if (secret.empty()) return;
    if (secret.size() > 4096 || utf8_lossy(secret) != secret
        || std::any_of(secret.begin(), secret.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw Error("invalid_secret", "Launch secret must be bounded UTF-8 without control characters");
    std::vector<std::string> added{std::string(secret)};
    std::string encoded;
    constexpr std::string_view hex = "0123456789ABCDEF";
    for (unsigned char c : secret) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') encoded += static_cast<char>(c);
        else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
    }
    if (encoded != secret) {
        added.push_back(encoded);
        for (std::size_t i = 0; i + 2 < encoded.size(); ++i) if (encoded[i] == '%') {
            encoded[i + 1] = static_cast<char>(std::tolower(static_cast<unsigned char>(encoded[i + 1])));
            encoded[i + 2] = static_cast<char>(std::tolower(static_cast<unsigned char>(encoded[i + 2])));
            i += 2;
        }
        added.push_back(std::move(encoded));
    }
    const auto quoted = Json(std::string(secret)).dump();
    added.push_back(quoted.substr(1, quoted.size() - 2));
    auto result = values;
    for (auto& value : added) if (std::find(result.begin(), result.end(), value) == result.end()) result.push_back(std::move(value));
    std::size_t bytes = 0;
    for (const auto& value : result) bytes += value.size();
    if (result.size() > 128 || bytes > 128 * 1024) throw Error("limit", "Launch secret variants exceed their bounded quota");
    values = std::move(result);
}
// Bounded and fail-closed: oversized or too-deep input, or a failed pass, yields the
// redaction-failure marker instead of unredacted text; `final` controls partial tails.
RedactionResult redact_impl(std::string_view text, bool final, unsigned depth,
                            std::size_t document_limit = redaction_document_limit,
                            std::span<const std::string> secrets = {}) {
    if (text.size() > document_limit || depth > 8 || secrets.size() > 128 || text.find(redaction_failure) != std::string_view::npos) return {std::string(redaction_failure), false, {}};
    std::vector<Span> spans;
    auto incomplete = [&](std::size_t start) -> RedactionResult {
        if (final) return {std::string(redaction_failure), false, {}};
        for (const auto& span : spans) if (span.start < start && span.end > start) start = span.start;
        std::erase_if(spans, [&](const auto& span) { return span.end > start; });
        return {mask_paths(apply_spans(text.substr(0, start), std::move(spans))), true, start};
    };
    try {
        for (const auto& secret : secrets) {
            if (secret.empty()) continue;
            for (auto cursor = text.find(secret); cursor != std::string_view::npos; cursor = text.find(secret, cursor + secret.size())) {
                if (spans.size() >= 262144) throw Invalid{};
                spans.push_back({cursor, cursor + secret.size(), "***"});
            }
        }
        for (std::size_t i = 0; i < text.size();) {
            if (spans.size() > 262144) throw Invalid{};
            const auto key_start = i;
            std::string key;
            std::size_t after_key = i;
            bool quoted = false;
            if ((text[i] == '"' || text[i] == '\'' || text[i] == '`') && (i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1])))) {
                quoted = true;
                std::pair<std::size_t, std::size_t> quote;
                try { quote = quote_end(text, i); }
                catch (const Incomplete&) {
                    const auto remainder = text.substr(i);
                    const auto lower = io::ascii_lower(remainder);
                    bool possible = remainder.find("\\u") != std::string_view::npos;
                    for (auto word : {"token", "password", "secret", "authorization", "api_key", "apikey", "/users/", "/home/"}) possible = possible || lower.find(word) != std::string::npos;
                    if (possible) return incomplete(i);
                    ++i; continue;
                }
                const auto width = quote.second - quote.first;
                key = decode_quote(text.substr(i + width, quote.first - i - width), text[i]);
                after_key = quote.second;
                const auto separator = skip_space(text, after_key);
                bool token = false;
                const bool sensitive = sensitive_key(key, token);
                if (!sensitive || separator >= text.size() || (text[separator] != ':' && text[separator] != '=')) {
                    if (sensitive && separator == text.size() && !final) return incomplete(i);
                    const auto nested = redact_impl(key, true, depth + 1, document_limit, secrets);
                    if (!nested.safe) return incomplete(i);
                    if (nested.text != key) spans.push_back({i, quote.second, Json(nested.text).dump()});
                    i = quote.second;
                    continue;
                }
            } else if (key_byte(static_cast<unsigned char>(text[i]))) {
                while (after_key < text.size() && key_byte(static_cast<unsigned char>(text[after_key]))) ++after_key;
                key = text.substr(i, after_key - i);
            } else { ++i; continue; }
            i = after_key;
            if (key.size() > 256) continue;
            const auto separator = skip_space(text, after_key);
            const bool explicit_field = separator < text.size() && (text[separator] == ':' || text[separator] == '=');
            const bool cli = !quoted && key.starts_with("--") && (key_start == 0 || std::isspace(static_cast<unsigned char>(text[key_start - 1])));
            bool token = false;
            if (!sensitive_key(key, token) || (!explicit_field && !cli)) continue;
            if (explicit_field && separator + 1 < text.size() && (text[separator + 1] == '=' || text[separator + 1] == '>')) continue;
            const auto start = skip_space(text, explicit_field ? separator + 1 : after_key, explicit_field);
            const auto prefix = io::trim(text.substr(line_start(text, key_start), key_start - line_start(text, key_start)));
            const bool yaml = !quoted && explicit_field && text[separator] == ':' && (prefix.empty() || prefix == "-");
            const auto lower = io::ascii_lower(key);
            Span span;
            try { span = sensitive_span(text, key_start, start, token, lower == "authorization" || lower == "--authorization", yaml, final); }
            catch (const Incomplete&) { return incomplete(key_start); }
            spans.push_back(std::move(span));
            i = std::max(i, spans.back().end);
        }
        std::size_t cursor = 0;
        while ((cursor = text.find("-----BEGIN ", cursor)) != std::string_view::npos) {
            const auto header_end = text.find("-----", cursor + 11);
            if (header_end == std::string_view::npos || header_end - cursor > 128) return incomplete(cursor);
            const auto label = text.substr(cursor + 11, header_end - cursor - 11);
            if (label.find("PRIVATE KEY") == std::string_view::npos) { cursor = header_end + 5; continue; }
            const auto footer = "-----END " + std::string(label) + "-----";
            const auto ending = text.find(footer, header_end + 5);
            if (ending == std::string_view::npos) return incomplete(cursor);
            if (ending - cursor > collection_limit || spans.size() >= 262144) throw Invalid{};
            spans.push_back({cursor, ending + footer.size(), "***"});
            cursor = ending + footer.size();
        }
        return {mask_paths(apply_spans(text, std::move(spans))), true, {}};
    } catch (const Incomplete& partial) { return incomplete(partial.start); }
    catch (...) { return {std::string(redaction_failure), false, {}}; }
}
std::string normalized(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto c = text[i];
        if (c == '\r') { if (i + 1 < text.size() && text[i + 1] == '\n') ++i; result += '\n'; }
        else if (c == '\n' || c == '\t' || static_cast<unsigned char>(c) >= 32) result += c;
        else if (c != '\0') result += ' ';
    }
    return result;
}
std::string bounded_tail(std::string_view text, std::size_t lines, std::size_t bytes, bool& truncated) {
    std::size_t start = text.size() > bytes ? text.size() - bytes : 0;
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) ++start;
    std::size_t count = 0;
    auto line_start = text.size();
    for (std::size_t i = text.size(); i > start; --i) {
        if (text[i - 1] == '\n' && i != text.size() && ++count >= lines) { line_start = i; break; }
    }
    if (line_start != text.size()) start = std::max(start, line_start);
    if (start != 0) truncated = true;
    return std::string(text.substr(start));
}
std::optional<StoredLog> read_log(const fs::path& path, std::size_t lines, std::size_t bytes,
                                  bool head_tail, std::span<const std::string> secrets) {
    instances::reject_links(path);
    if (!fs::exists(path)) return {};
    if (!fs::is_regular_file(path)) throw Error("unsafe_log", "Log is not a regular file");
    if (fs::file_size(path) > redaction_document_limit) return StoredLog{instances::path_to_utf8(path.filename()), std::string(redaction_failure), true};
    const auto raw = io::read_bounded(path, redaction_document_limit);
    const auto safe = redact_checked(utf8_lossy(raw), true, secrets);
    if (!safe.safe) return StoredLog{instances::path_to_utf8(path.filename()), std::string(redaction_failure), false};
    const auto full = normalized(safe.text);
    bool truncated = false;
    auto tail = bounded_tail(full, lines, bytes, truncated);
    if (head_tail && full.size() > bytes) {
        auto head_size = bytes / 3;
        while (head_size > 0 && (static_cast<unsigned char>(full[head_size]) & 0xc0) == 0x80) --head_size;
        tail = full.substr(0, head_size) + "\n[nickel: log truncated]\n" + bounded_tail(full, lines - std::min(lines / 4, std::size_t(1000)), bytes - head_size - 32, truncated);
    }
    return StoredLog{instances::path_to_utf8(path.filename()), std::move(tail), truncated};
}
std::string lease_identity(const fs::path& path) {
    instances::reject_links(path);
    return instances::root_identity(instances::path_to_utf8(fs::weakly_canonical(path)));
}
void take_lease(const std::string& identity) {
    std::lock_guard lock(lease_mutex);
    if (!leases.insert(identity).second) throw Error("log_session_active", "Instance log session is still active");
}
void release_lease(const std::string& identity) {
    std::lock_guard lock(lease_mutex);
    leases.erase(identity);
}
std::size_t stream_index(ProcessStream stream) { return stream == ProcessStream::stdout_stream ? 0 : 1; }
} // namespace

std::string utf8_lossy(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        const auto c = static_cast<unsigned char>(input[i]);
        const auto width = c < 0x80 ? 1u : c >= 0xc2 && c <= 0xdf ? 2u : c >= 0xe0 && c <= 0xef ? 3u : c >= 0xf0 && c <= 0xf4 ? 4u : 0u;
        bool valid = width != 0 && i + width <= input.size();
        for (unsigned j = 1; valid && j < width; ++j) valid = (static_cast<unsigned char>(input[i + j]) & 0xc0) == 0x80;
        if (valid && width >= 3) {
            const auto second = static_cast<unsigned char>(input[i + 1]);
            valid = !(c == 0xe0 && second < 0xa0) && !(c == 0xed && second >= 0xa0) && !(c == 0xf0 && second < 0x90) && !(c == 0xf4 && second >= 0x90);
        }
        if (valid) { result.append(input.substr(i, width)); i += width; }
        else { result += "\xef\xbf\xbd"; ++i; }
    }
    return result;
}
RedactionResult redact_checked(std::string_view text, bool final, std::span<const std::string> secrets) {
    return redact_impl(text, final, 0, redaction_document_limit, secrets);
}
RedactionResult redact_report_checked(std::string_view text, std::span<const std::string> secrets) {
    return redact_impl(text, true, 0, 64ULL * 1024 * 1024, secrets);
}
std::string redact(std::string_view text) { return redact_checked(text).text; }
std::string bounded_safe_line(std::string_view raw, std::size_t bytes) {
    auto text = utf8_lossy(raw);
    if (text.size() <= bytes) return text;
    constexpr std::string_view marker = " [nickel: line truncated]";
    const auto size = bytes > marker.size() ? bytes - marker.size() : 0;
    auto end = size;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return text.substr(0, end) + std::string(marker.substr(0, bytes - end));
}
std::string StreamRedactor::reject() {
    pending_.clear(); since_.reset(); pending_lines_ = 0;
    const bool first = !failed_;
    failed_ = true;
    return first ? std::string(redaction_failure) + '\n' : "";
}
void StreamRedactor::add_secret(std::string_view secret) {
    if (started_ || closed_ || failed_) throw Error("log_session_started", "Launch secrets must be registered before stream input");
    register_secret(secrets_, secret);
}
std::string StreamRedactor::push_line(std::string_view line, bool discarded) {
    if (closed_ || failed_) return {};
    started_ = true;
    if (discarded || line.size() > stream_line_max_bytes) return reject();
    if (pending_.size() + line.size() + 1 > collection_limit + stream_line_max_bytes || ++pending_lines_ > 16384) return reject();
    pending_.append(utf8_lossy(line));
    pending_ += '\n';
    const auto checked = redact_checked(pending_, false, secrets_);
    if (!checked.safe) return reject();
    if (checked.pending_start) {
        pending_.erase(0, *checked.pending_start);
        if (!since_) since_ = std::chrono::steady_clock::now();
    } else { pending_.clear(); since_.reset(); pending_lines_ = 0; }
    return checked.text;
}
std::string StreamRedactor::finish(bool failed) {
    if (closed_) return {};
    closed_ = true;
    if (failed) return reject();
    if (failed_) return {};
    const auto safe = redact_checked(pending_, true, secrets_);
    pending_.clear(); since_.reset(); pending_lines_ = 0;
    if (!safe.safe) return reject();
    return safe.text;
}
std::string StreamRedactor::expire(std::chrono::steady_clock::time_point now) {
    if (since_ && now - *since_ >= std::chrono::seconds(30)) return reject();
    return {};
}
bool StreamRedactor::failed() const noexcept { return failed_; }
void OutputCapture::push_safe(std::string_view raw) {
    auto line = bounded_safe_line(raw);
    while (!lines_.empty() && (lines_.size() >= capture_max_lines || bytes_ + line.size() + 1 > capture_max_bytes)) {
        bytes_ -= lines_.front().size() + 1;
        lines_.pop_front();
    }
    bytes_ += line.size() + 1;
    lines_.push_back(std::move(line));
}
std::string OutputCapture::text() const {
    std::string result;
    result.reserve(bytes_);
    for (const auto& line : lines_) { if (!result.empty()) result += '\n'; result += line; }
    return result;
}
std::size_t OutputCapture::size() const noexcept { return lines_.size(); }
std::size_t OutputCapture::bytes() const noexcept { return bytes_; }
LogPaths resolve_log_paths(const fs::path& input) {
    const auto root = fs::absolute(input);
    const auto logs = root / "logs";
    return {root, logs, logs / log_file_name, logs / "latest.log", root / "crash-reports"};
}
struct LogSecretState {
    mutable std::mutex mutex;
    std::vector<std::string> values;
    ~LogSecretState() { for (auto& value : values) std::fill(value.begin(), value.end(), '\0'); }
};
LaunchLogStore::LaunchLogStore(fs::path root, std::size_t bytes, std::size_t archives)
    : paths_(resolve_log_paths(root)), max_bytes_(std::clamp<std::size_t>(bytes, 1024, capture_max_bytes)),
      archives_(std::min<std::size_t>(archives, 3)), secrets_(std::make_shared<LogSecretState>()) {
    instances::reject_links(paths_.logs);
}
void LaunchLogStore::add_secret(std::string_view secret) {
    std::lock_guard lock(secrets_->mutex);
    register_secret(secrets_->values, secret);
}
std::vector<std::string> LaunchLogStore::secrets() const {
    std::lock_guard lock(secrets_->mutex);
    return secrets_->values;
}
RedactionResult LaunchLogStore::redact_report(std::string_view raw) const {
    const auto known = secrets();
    return redact_report_checked(raw, known);
}
void LaunchLogStore::rotate() const {
    if (archives_ == 0) { std::error_code ec; fs::remove(paths_.launcher, ec); return; }
    const auto archive_path = [&](std::size_t index) { return paths_.logs / (std::string(log_file_name) + '.' + std::to_string(index)); };
    instances::reject_links(archive_path(archives_));
    std::error_code ec;
    fs::remove(archive_path(archives_), ec);
    for (auto index = archives_; index > 1; --index) {
        const auto previous = archive_path(index - 1);
        instances::reject_links(previous);
        if (fs::exists(previous)) io::rename_no_replace(previous, archive_path(index));
    }
    instances::reject_links(paths_.launcher);
    if (fs::exists(paths_.launcher)) io::rename_no_replace(paths_.launcher, archive_path(1));
}
void LaunchLogStore::start_session() {
    instances::reject_links(paths_.logs);
    io::DirectoryLock lock(paths_.logs);
    rotate();
    io::atomic_write(paths_.launcher, "");
}
void LaunchLogStore::append(std::string_view raw) { append_safe(raw); }
void LaunchLogStore::append_safe(std::string_view raw) {
    instances::reject_links(paths_.logs);
    io::DirectoryLock lock(paths_.logs);
    const auto known = secrets();
    std::string prior;
    if (fs::exists(paths_.launcher)) {
        if (fs::file_size(paths_.launcher) > redaction_document_limit) prior = std::string(redaction_failure) + '\n';
        else {
            const auto checked = redact_checked(io::read_bounded(paths_.launcher, redaction_document_limit), true, known);
            prior = checked.safe ? normalized(checked.text) : std::string(redaction_failure) + '\n';
        }
    }
    const auto checked = redact_checked(raw, true, known);
    auto safe = normalized(checked.safe ? checked.text : redaction_failure);
    if (!safe.empty() && safe.back() != '\n') safe += '\n';
    bool truncated = false;
    safe = bounded_tail(safe, capture_max_lines, std::min<std::size_t>(max_bytes_, 512 * 1024), truncated);
    if (prior.size() + safe.size() > max_bytes_) { rotate(); prior.clear(); }
    io::atomic_write(paths_.launcher, prior + safe);
}
std::optional<StoredLog> LaunchLogStore::tail(std::size_t lines, std::size_t bytes) const {
    const auto known = secrets();
    return read_log(paths_.launcher, std::clamp<std::size_t>(lines, 1, 10000), std::clamp<std::size_t>(bytes, 1024, redaction_document_limit), false, known);
}
std::optional<StoredLog> LaunchLogStore::latest(std::size_t bytes) const {
    const auto known = secrets();
    return read_log(paths_.latest, capture_max_lines, std::clamp<std::size_t>(bytes, 1024, redaction_document_limit), false, known);
}
std::vector<StoredLog> LaunchLogStore::crashes(std::size_t count, std::size_t bytes) const {
    std::vector<StoredLog> result;
    const auto known = secrets();
    instances::reject_links(paths_.crash_reports);
    if (!fs::is_directory(paths_.crash_reports)) return result;
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(paths_.crash_reports)) {
        instances::reject_links(entry.path(), false);
        const auto name = instances::path_to_utf8(entry.path().filename());
        if (entry.is_regular_file() && name.starts_with("crash-") && name.ends_with(".txt")) paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) { return fs::last_write_time(a) > fs::last_write_time(b); });
    for (const auto& path : paths) {
        if (result.size() >= std::min<std::size_t>(count, 3)) break;
        if (auto log = read_log(path, capture_max_lines, std::clamp<std::size_t>(bytes, 1024, redaction_document_limit), true, known)) result.push_back(std::move(*log));
    }
    return result;
}
const LogPaths& LaunchLogStore::paths() const noexcept { return paths_; }
OutputSession::OutputSession(LaunchLogStore store, SafeOutputSink sink, bool start_new_log)
    : store_(std::move(store)), sink_(std::move(sink)), lease_(lease_identity(store_.paths().launcher)) {
    take_lease(lease_);
    try { if (start_new_log) store_.start_session(); }
    catch (...) { release_lease(lease_); throw; }
}
OutputSession::~OutputSession() {
    try { finish(std::chrono::milliseconds(0)); } catch (...) {}
    release_lease(lease_);
}
void OutputSession::add_secret(std::string_view secret) {
    std::lock_guard lock(mutex_);
    if (complete_ || observed_[0] || observed_[1] || closed_[0] || closed_[1])
        throw Error("log_session_started", "Launch secrets must be registered before any process output");
    auto updated = redactors_;
    for (auto& redactor : updated) redactor.add_secret(secret);
    store_.add_secret(secret);
    redactors_ = std::move(updated);
}
void OutputSession::publish_locked(ProcessStream stream, std::string_view text) {
    if (text.empty()) return;
    store_.append_safe(text);
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = bounded_safe_line(text.substr(start, end - start));
        capture_.push_safe(line);
        if (sink_) sink_({stream, std::move(line)});
        start = end + 1;
    }
}
void OutputSession::bytes(ProcessStream stream, std::string_view input) {
    std::lock_guard lock(mutex_);
    const auto index = stream_index(stream);
    if (closed_[index] || complete_) return;
    observed_[index] = true;
    for (char c : input) {
        if (c == '\n') {
            if (!line_buffers_[index].empty() && line_buffers_[index].back() == '\r') line_buffers_[index].pop_back();
            publish_locked(stream, redactors_[index].push_line(line_buffers_[index], discarded_[index]));
            line_buffers_[index].clear(); discarded_[index] = false;
        } else if (line_buffers_[index].size() < stream_line_max_bytes) line_buffers_[index] += c;
        else discarded_[index] = true;
    }
    publish_locked(stream, redactors_[index].expire());
}
void OutputSession::line(ProcessStream stream, std::string_view line, bool discarded) {
    std::lock_guard lock(mutex_);
    const auto index = stream_index(stream);
    if (closed_[index] || complete_) return;
    observed_[index] = true;
    publish_locked(stream, redactors_[index].push_line(line, discarded));
}
void OutputSession::close_locked(std::size_t index, bool failed) {
    if (closed_[index]) return;
    const auto stream = index == 0 ? ProcessStream::stdout_stream : ProcessStream::stderr_stream;
    if (!line_buffers_[index].empty() || discarded_[index]) {
        publish_locked(stream, redactors_[index].push_line(line_buffers_[index], discarded_[index]));
        line_buffers_[index].clear(); discarded_[index] = false;
    }
    publish_locked(stream, redactors_[index].finish(failed));
    closed_[index] = true;
}
void OutputSession::eof(ProcessStream stream, bool failed) {
    std::lock_guard lock(mutex_);
    close_locked(stream_index(stream), failed);
    changed_.notify_all();
}
void OutputSession::producer_disappeared() {
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < 2; ++i) if (!closed_[i]) close_locked(i, true);
    changed_.notify_all();
}
bool OutputSession::finish(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (complete_) return !redactors_[0].failed() && !redactors_[1].failed();
    const bool drained = changed_.wait_for(lock, timeout, [&] { return closed_[0] && closed_[1]; });
    if (!drained) for (std::size_t i = 0; i < 2; ++i) if (!closed_[i]) close_locked(i, true);
    complete_ = true;
    release_lease(lease_);
    lease_.clear();
    return drained && !redactors_[0].failed() && !redactors_[1].failed();
}
bool OutputSession::finished() const { std::lock_guard lock(mutex_); return complete_; }
bool OutputSession::safe() const { std::lock_guard lock(mutex_); return !redactors_[0].failed() && !redactors_[1].failed(); }
std::string OutputSession::capture_text() const { std::lock_guard lock(mutex_); return capture_.text(); }
LaunchLogStore OutputSession::log_store() const {
    std::lock_guard lock(mutex_);
    return store_;
}
bool drain_owned_installer(OutputSession& session, const std::function<void()>& join, std::chrono::milliseconds timeout) {
    try { join(); }
    catch (...) { session.producer_disappeared(); session.finish(std::chrono::milliseconds(0)); throw; }
    return session.finish(timeout);
}
FinishedLogLease::FinishedLogLease(const fs::path& path) : identity_(lease_identity(path)) { take_lease(identity_); }
FinishedLogLease::~FinishedLogLease() { release_lease(identity_); }
} // namespace nickel::diagnostics
