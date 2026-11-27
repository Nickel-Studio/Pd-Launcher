#include "internal.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <set>
#include <unordered_map>
#include <yaml.h>
#include <toml++/toml.hpp>

namespace nickel::packs::detail {
bool valid_utf8(std::string_view text) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    for (std::size_t i = 0; i < text.size();) {
        const auto first = bytes[i++];
        if (first < 0x80) continue;
        const auto count = first >= 0xc2 && first <= 0xdf ? 1 : first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        if (!count || i + count > text.size()) return false;
        const auto next = bytes[i];
        if ((first == 0xe0 && next < 0xa0) || (first == 0xed && next >= 0xa0)
            || (first == 0xf0 && next < 0x90) || (first == 0xf4 && next >= 0x90)) return false;
        for (int k = 0; k < count; ++k) if ((bytes[i++] & 0xc0) != 0x80) return false;
    }
    return true;
}
bool credential_key(std::string_view key) {
    key = key.substr(key.rfind('.') == std::string_view::npos ? 0 : key.rfind('.') + 1);
    std::string folded;
    for (unsigned char c : key) if (std::isalnum(c)) folded += static_cast<char>(std::tolower(c));
    constexpr std::string_view keys[]{"accesstoken", "clienttoken", "refreshtoken", "authtoken", "bearertoken", "sessiontoken", "apitoken", "clientsecret", "apikey", "apikeys", "xapikey", "password", "passwords", "passwd", "pwd", "authorization", "privatekey", "secretkey", "secret", "secrets", "credential", "credentials"};
    return std::find(std::begin(keys), std::end(keys), folded) != std::end(keys);
}
bool environment_name(std::string_view value) {
    const auto initial = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    return !value.empty() && initial(static_cast<unsigned char>(value.front()))
        && std::all_of(value.begin(), value.end(), [&](unsigned char c) { return initial(c) || (c >= '0' && c <= '9'); });
}
bool credential_value(std::string_view text) {
    const auto value = io::trim(text);
    std::string folded;
    for (unsigned char c : value) if (c != '_' && c != '-' && c != ' ') folded += static_cast<char>(std::tolower(c));
    constexpr std::string_view empty[]{"", "null", "none", "true", "false", "disabled", "unset", "notset", "changeme", "placeholder", "redacted", "yourapikey", "yourapikeyhere", "yourtoken", "yourtokenhere", "yourpassword", "yourpasswordhere", "yourclientsecret", "<apikey>", "<token>", "<password>", "~"};
    if (std::find(std::begin(empty), std::end(empty), folded) != std::end(empty)
        || std::all_of(value.begin(), value.end(), [](char c) { return c == '*'; })) return false;
    std::string_view name;
    if (value.starts_with("${") && value.ends_with('}')) name = std::string_view(value).substr(2, value.size() - 3);
    else if (value.starts_with('%') && value.ends_with('%') && value.size() > 2) name = std::string_view(value).substr(1, value.size() - 2);
    return !environment_name(name);
}
namespace {
std::string percent_decode(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') { result += text[i]; continue; }
        if (i + 2 >= text.size()) throw Error("unsafe_content", "Invalid encoded content");
        unsigned int value = 0;
        const auto parsed = std::from_chars(text.data() + i + 1, text.data() + i + 3, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + i + 3) throw Error("unsafe_content", "Invalid encoded content");
        result += static_cast<char>(value);
        i += 2;
    }
    return result;
}
bool private_url_credentials(std::string_view text) {
    for (auto at = text.find("://"); at != std::string_view::npos; at = text.find("://", at + 3)) {
        const auto end = text.find_first_of("/\r\n\t \"'", at + 3);
        if (text.substr(at + 3, end == std::string_view::npos ? text.size() - at - 3 : end - at - 3).find('@') != std::string_view::npos) return true;
    }
    for (auto offset = text.find_first_of("?&"); offset != std::string_view::npos; offset = text.find_first_of("?&", offset + 1)) {
        const auto equals = text.find('=', offset + 1);
        const auto end = text.find_first_of("&#\r\n\t \"'", offset + 1);
        if (equals == std::string_view::npos || (end != std::string_view::npos && equals > end)) continue;
        try {
            const auto key = percent_decode(text.substr(offset + 1, equals - offset - 1));
            const auto value = percent_decode(text.substr(equals + 1, end == std::string_view::npos ? text.size() - equals - 1 : end - equals - 1));
            if (credential_key(key) && credential_value(value)) return true;
        } catch (...) { return true; }
    }
    return false;
}
void append_codepoint(std::string& result, std::uint32_t code) {
    if (code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) throw Error("unsafe_content", "Invalid encoded scalar");
    if (code < 0x80) result += static_cast<char>(code);
    else if (code < 0x800) { result += static_cast<char>(0xc0 | (code >> 6)); result += static_cast<char>(0x80 | (code & 63)); }
    else if (code < 0x10000) { result += static_cast<char>(0xe0 | (code >> 12)); result += static_cast<char>(0x80 | ((code >> 6) & 63)); result += static_cast<char>(0x80 | (code & 63)); }
    else { result += static_cast<char>(0xf0 | (code >> 18)); result += static_cast<char>(0x80 | ((code >> 12) & 63)); result += static_cast<char>(0x80 | ((code >> 6) & 63)); result += static_cast<char>(0x80 | (code & 63)); }
}
std::string decode_string(std::string_view raw) {
    std::string result;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\') { result += raw[i]; continue; }
        if (++i >= raw.size()) throw Error("unsafe_content", "Incomplete encoded scalar");
        const auto code = raw[i];
        if (code == '\n') continue;
        if (code == 'u' || code == 'x') {
            const auto count = code == 'u' ? 4 : 2;
            if (i + count >= raw.size()) throw Error("unsafe_content", "Incomplete encoded scalar");
            unsigned int point = 0;
            const auto parsed = std::from_chars(raw.data() + i + 1, raw.data() + i + 1 + count, point, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + i + 1 + count) throw Error("unsafe_content", "Invalid encoded scalar");
            i += count;
            append_codepoint(result, point);
        } else if (code >= '0' && code <= '7') {
            unsigned int point = static_cast<unsigned int>(code - '0');
            for (int k = 1; k < 3 && i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '7'; ++k) point = point * 8 + static_cast<unsigned int>(raw[++i] - '0');
            append_codepoint(result, point);
        } else {
            switch (code) {
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            default: result += code; break;
            }
        }
    }
    if (!valid_utf8(result)) throw Error("unsafe_content", "Invalid UTF-8 scalar");
    return result;
}
struct Token {
    enum Kind { text, open, close, field, assignment, end, other } kind = other;
    std::string value;
    bool quoted = false;
    char delimiter = 0;
    std::size_t begin = 0;
    std::size_t finish = 0;
    Token(Kind token_kind, std::string text_value = {}, bool is_quoted = false, char separator = 0)
        : kind(token_kind), value(std::move(text_value)), quoted(is_quoted), delimiter(separator) {}
};
std::vector<Token> tokenize(std::string_view text, bool script, bool lua = false) {
    std::vector<Token> result;
    std::vector<char> stack;
    const auto emit = [&](Token token, std::size_t begin, std::size_t finish) {
        if (result.size() >= 200000) throw Error("limit", "Configuration token quota exceeded");
        token.begin = begin; token.finish = finish; result.push_back(std::move(token));
    };
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (std::isspace(c)) {
            if (c == '\n' || c == '\r') emit({Token::end, {}, false, static_cast<char>(c)}, i, i + 1);
            ++i; continue;
        }
        const auto rest = text.substr(i);
        if (rest.starts_with("//") || (!script && rest.starts_with('#')) || (lua && rest.starts_with("--"))) {
            const auto end = text.find('\n', i); i = end == std::string_view::npos ? text.size() : end; continue;
        }
        if (rest.starts_with("/*")) {
            const auto end = text.find("*/", i + 2);
            if (end == std::string_view::npos) throw Error("unsafe_content", "Unclosed configuration comment");
            i = end + 2; continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            const auto delimiter = static_cast<char>(c);
            const auto begin = i++;
            const auto start = i;
            bool escaped = false;
            for (; i < text.size(); ++i) {
                if (!escaped && text[i] == delimiter) break;
                if (!escaped && text[i] == '\\') escaped = true; else escaped = false;
            }
            if (i == text.size()) throw Error("unsafe_content", "Unclosed configuration string");
            emit({Token::text, decode_string(text.substr(start, i - start)), true, delimiter}, begin, i + 1);
            ++i; continue;
        }
        if (rest.starts_with("${")) {
            const auto end = text.find('}', i + 2);
            if (end == std::string_view::npos) throw Error("unsafe_content", "Unclosed placeholder");
            emit({Token::text, std::string(text.substr(i, end - i + 1))}, i, end + 1); i = end + 1; continue;
        }
        if (c == '{' || c == '[' || c == '(') {
            if (stack.size() >= 64) throw Error("limit", "Configuration nesting quota exceeded");
            stack.push_back(static_cast<char>(c)); emit({Token::open, {}, false, static_cast<char>(c)}, i, i + 1); ++i; continue;
        }
        if (c == '}' || c == ']' || c == ')') {
            const auto expected = c == '}' ? '{' : c == ']' ? '[' : '(';
            if (stack.empty() || stack.back() != expected) throw Error("unsafe_content", "Unbalanced configuration structure");
            stack.pop_back(); emit({Token::close, {}, false, static_cast<char>(c)}, i, i + 1); ++i; continue;
        }
        if (c == ':') { emit({Token::field}, i, i + 1); ++i; continue; }
        if (c == ',' || c == ';') { emit({Token::end, {}, false, static_cast<char>(c)}, i, i + 1); ++i; continue; }
        if (std::string_view("=!|&?+-*/%<>^").find(static_cast<char>(c)) != std::string_view::npos) {
            const auto start = i++;
            while (i < text.size() && (text[i] == '=' || text[i] == text[start] || text[i] == '?')) ++i;
            const auto value = text.substr(start, i - start);
            const bool assignment = value == "=" || (value.ends_with('=') && value != "==" && value != "===" && value != "!=" && value != "!==" && value != "<=" && value != ">=");
            emit({assignment ? Token::assignment : Token::other}, start, i); continue;
        }
        const auto start = i++;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && std::string_view("\"'`=:{}[](),;?!|&+*/%^<>").find(text[i]) == std::string_view::npos) ++i;
        emit({Token::text, decode_string(text.substr(start, i - start))}, start, i);
    }
    if (!stack.empty()) throw Error("unsafe_content", "Unbalanced configuration structure");
    return result;
}
std::size_t environment_reference_end(const std::vector<Token>& tokens, std::size_t start, std::size_t end) {
    if (start >= end || tokens[start].kind != Token::text || tokens[start].quoted) return start;
    const auto& value = tokens[start].value;
    if (value.starts_with("process.env.") && environment_name(std::string_view(value).substr(12))) return start + 1;
    if (end - start < 4 || tokens[start + 1].kind != Token::open || tokens[start + 2].kind != Token::text
        || !tokens[start + 2].quoted || !environment_name(tokens[start + 2].value) || tokens[start + 3].kind != Token::close) return start;
    if ((value == "System.getenv" || value == "os.getenv" || value == "getenv")
        && tokens[start + 1].delimiter == '(' && tokens[start + 3].delimiter == ')') return start + 4;
    if (value == "process.env" && tokens[start + 1].delimiter == '[' && tokens[start + 3].delimiter == ']') return start + 4;
    return start;
}
std::size_t expression_end(const std::vector<Token>& tokens, std::size_t start) {
    std::size_t depth = 0;
    auto end = start;
    for (; end < tokens.size(); ++end) {
        const auto& token = tokens[end];
        if (token.kind == Token::open) ++depth;
        else if (token.kind == Token::close) {
            if (!depth) break;
            --depth;
        } else if (!depth && token.kind == Token::end) break;
    }
    return end;
}
bool credential_tokens(const std::vector<Token>& tokens, std::size_t start, std::size_t end, bool script) {
    for (auto j = start; j < end; ++j) {
        if (script) {
            const auto after = environment_reference_end(tokens, j, end);
            if (after != j) { j = after - 1; continue; }
        }
        if (tokens[j].kind != Token::text) continue;
        if (j + 1 < end && (tokens[j + 1].kind == Token::field || tokens[j + 1].kind == Token::assignment)) continue;
        if ((tokens[j].quoted || !script || (!tokens[j].value.empty() && std::isdigit(static_cast<unsigned char>(tokens[j].value.front()))))
            && credential_value(tokens[j].value)) return true;
    }
    return false;
}
bool token_policy(std::string_view text, bool script, bool portable, bool lua = false) {
    const auto tokens = tokenize(text, script, lua);
    for (const auto& token : tokens) {
        if (token.kind == Token::text && (embedded_credentials(token.value)
            || (portable && (token.quoted || !script) && instances::contains_host_path(token.value)))) return false;
    }
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].kind != Token::text) continue;
        if (portable && (tokens[i].value == "pre_launch_command" || tokens[i].value == "preLaunchCommand")) return false;
        auto field = i + 1;
        if (tokens[field].kind == Token::close && tokens[field].delimiter == ']') {
            if (!script || !tokens[i].quoted || !i || tokens[i - 1].kind != Token::open || tokens[i - 1].delimiter != '['
                || field + 1 >= tokens.size() || tokens[field + 1].kind != Token::assignment) continue;
            ++field;
        }
        if ((tokens[field].kind != Token::field && tokens[field].kind != Token::assignment) || !credential_key(tokens[i].value)) continue;
        const auto start = field + 1;
        const auto end = expression_end(tokens, start);
        if (start == end) { if (script) return false; continue; }
        // Keep the entire unquoted scalar: a safe placeholder prefix must not hide a literal suffix.
        if (!script && tokens[start].kind != Token::open && !tokens[start].quoted) {
            if (credential_value(text.substr(tokens[start].begin, tokens[end - 1].finish - tokens[start].begin))) return false;
        } else if (credential_tokens(tokens, start, end, script)) return false;
    }
    return true;
}
// Depth and sensitivity are both structural: once a key looks credential-bearing, every
// nested scalar is treated as secret, and over-deep documents are rejected.
bool json_policy(const Json& value, bool sensitive, bool portable, std::size_t depth = 0) {
    if (depth > 64) return false;
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        return (!sensitive || !credential_value(text)) && !embedded_credentials(text)
            && (!portable || !instances::contains_host_path(text));
    }
    if (value.is_number()) return !sensitive;
    if (value.is_object()) {
        for (const auto& item : value.items()) {
            if (portable && (instances::contains_host_path(item.key()) || item.key() == "pre_launch_command" || item.key() == "preLaunchCommand")) return false;
            if (!json_policy(item.value(), sensitive || credential_key(item.key()), portable, depth + 1)) return false;
        }
    } else if (value.is_array()) for (const auto& child : value) if (!json_policy(child, sensitive, portable, depth + 1)) return false;
    return true;
}
bool yaml_policy(std::string_view text, bool portable) {
    struct Parser {
        yaml_parser_t parser{};
        Parser() { if (!yaml_parser_initialize(&parser)) throw Error("limit", "Cannot initialize YAML policy"); }
        ~Parser() { yaml_parser_delete(&parser); }
    } parser;
    yaml_parser_set_input_string(&parser.parser, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    struct Frame { bool mapping; bool sensitive; bool key = true; bool value_sensitive = false; std::set<std::string> keys; };
    std::vector<Frame> stack;
    const auto child_context = [&]() {
        if (stack.empty()) return false;
        auto& parent = stack.back();
        if (!parent.mapping) return parent.sensitive;
        if (parent.key) throw Error("unsafe_content", "Complex YAML mapping keys are not portable");
        parent.key = true;
        return parent.value_sensitive;
    };
    for (std::size_t events = 0; events < 200000; ++events) {
        yaml_event_t event{};
        if (!yaml_parser_parse(&parser.parser, &event)) return false;
        struct Event { yaml_event_t* value; ~Event() { yaml_event_delete(value); } } owner{&event};
        switch (event.type) {
        case YAML_SCALAR_EVENT: {
            if (event.data.scalar.anchor || event.data.scalar.tag) return false;
            const std::string scalar(reinterpret_cast<const char*>(event.data.scalar.value), event.data.scalar.length);
            if (embedded_credentials(scalar) || (portable && instances::contains_host_path(scalar))) return false;
            if (!stack.empty() && stack.back().mapping && stack.back().key) {
                auto& frame = stack.back();
                if (!frame.keys.insert(scalar).second) return false;
                if (portable && (scalar == "pre_launch_command" || scalar == "preLaunchCommand")) return false;
                frame.value_sensitive = frame.sensitive || credential_key(scalar);
                frame.key = false;
            } else if (child_context() && credential_value(scalar)) return false;
            break;
        }
        case YAML_SEQUENCE_START_EVENT:
        case YAML_MAPPING_START_EVENT: {
            const bool mapping = event.type == YAML_MAPPING_START_EVENT;
            if (stack.size() >= 64 || (mapping ? event.data.mapping_start.anchor || event.data.mapping_start.tag : event.data.sequence_start.anchor || event.data.sequence_start.tag)) return false;
            const bool sensitive = child_context();
            stack.push_back({mapping, sensitive, true, false, {}});
            break;
        }
        case YAML_SEQUENCE_END_EVENT:
        case YAML_MAPPING_END_EVENT:
            if (stack.empty() || stack.back().mapping != (event.type == YAML_MAPPING_END_EVENT) || (stack.back().mapping && !stack.back().key)) return false;
            stack.pop_back(); break;
        case YAML_ALIAS_EVENT: return false;
        case YAML_DOCUMENT_START_EVENT:
            if (!stack.empty() || event.data.document_start.tag_directives.start != event.data.document_start.tag_directives.end) return false;
            break;
        case YAML_DOCUMENT_END_EVENT: if (!stack.empty()) return false; break;
        case YAML_STREAM_END_EVENT: return stack.empty();
        case YAML_STREAM_START_EVENT: break;
        default: return false;
        }
    }
    return false;
}
bool toml_policy(const toml::node& node, bool sensitive, bool portable, std::size_t depth = 0) {
    if (depth > 64) return false;
    if (const auto* table = node.as_table()) {
        for (const auto& [key, value] : *table) {
            const auto name = key.str();
            if (portable && (instances::contains_host_path(name) || name == "pre_launch_command" || name == "preLaunchCommand")) return false;
            if (!toml_policy(value, sensitive || credential_key(name), portable, depth + 1)) return false;
        }
    } else if (const auto* array = node.as_array()) {
        for (const auto& value : *array) if (!toml_policy(value, sensitive, portable, depth + 1)) return false;
    } else if (const auto text = node.value_exact<std::string>()) {
        if ((sensitive && credential_value(*text)) || embedded_credentials(*text) || (portable && instances::contains_host_path(*text))) return false;
    } else if (sensitive && !node.is_boolean()) return false;
    return true;
}
bool policy_text(std::string_view text, std::string_view extension, bool portable) {
    if (text.size() > policy_max_bytes || !valid_utf8(text) || text.find('\0') != std::string_view::npos
        || embedded_credentials(text)) return false;
    try {
        if (extension == ".yaml" || extension == ".yml") return yaml_policy(text, portable);
        if (extension == ".toml") { const auto table = toml::parse(text); return toml_policy(table, false, portable); }
        if (extension == ".json" || extension == ".mcmeta") return json_policy(io::parse_strict_json(text, policy_max_bytes), false, portable);
        const auto trimmed = io::trim(text);
        // Strict JSON is authoritative; only formats that extend JSON may fall back to
        // token scanning when parsing fails.
        if (!trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '[')) {
            try { return json_policy(io::parse_strict_json(text, policy_max_bytes), false, portable); }
            catch (const Error&) { if (extension != ".json5" && extension != ".snbt" && extension != ".js" && extension != ".ts") return false; }
        }
        const bool script = extension == ".js" || extension == ".ts" || extension == ".lua" || extension == ".zs";
        return token_policy(text, script, portable, extension == ".lua");
    } catch (...) { return false; }
}
}
bool embedded_credentials(std::string_view text) {
    const auto lower = io::ascii_lower(text);
    if ((lower.find("-----begin ") != std::string::npos && lower.find("private key-----") != std::string::npos)
        || private_url_credentials(text)) return true;
    for (auto prefix : {std::string_view("bearer "), std::string_view("basic ")}) {
        const auto position = lower.find(prefix);
        if (position == std::string::npos) continue;
        const auto start = position + prefix.size();
        const auto end = text.find_first_of("\r\n\t \"'", start);
        const auto token = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        if (token.size() >= 8 && credential_value(token)) return true;
    }
    const auto jwt = text.find("eyJ");
    if (jwt != std::string_view::npos) {
        const auto first = text.find('.', jwt), second = first == std::string_view::npos ? first : text.find('.', first + 1);
        if (first != std::string_view::npos && second != std::string_view::npos && first - jwt >= 8 && second - first >= 9 && text.size() - second > 16) return true;
    }
    return false;
}
void validate_content_file(const fs::path& file, std::string_view relative, bool portable) {
    if (sensitive_content_path(relative)) throw Error("unsafe_content", "Credential-bearing file is excluded");
    const auto extension = io::ascii_lower(path_to_utf8(file.extension()));
    constexpr std::string_view formats[]{".json", ".json5", ".toml", ".yaml", ".yml", ".properties", ".cfg", ".conf", ".config", ".ini", ".txt", ".js", ".ts", ".zs", ".lua", ".mcmeta", ".snbt"};
    if (std::find(std::begin(formats), std::end(formats), extension) == std::end(formats)) return;
    const auto text = io::read_bounded(file, policy_max_bytes);
    if (!policy_text(text, extension, portable)) throw Error("unsafe_content", "Configuration cannot be safely packaged; remove secrets or deselect the file");
}
void validate_background(const fs::path& file) {
    NativeFile source(file, NativeFile::Mode::read);
    if (source.size() > background_max_bytes) throw Error("limit", "Portable background exceeds 32 MiB");
    std::array<char, 32> bytes{};
    const auto size = source.read_at(0, bytes.data(), bytes.size());
    const std::string_view header(bytes.data(), size);
    const auto extension = io::ascii_lower(path_to_utf8(file.extension()));
    const bool valid = (extension == ".png" && header.starts_with("\x89PNG\r\n\x1a\n"))
        || ((extension == ".jpg" || extension == ".jpeg") && header.starts_with("\xff\xd8\xff"))
        || (extension == ".gif" && (header.starts_with("GIF87a") || header.starts_with("GIF89a")))
        || (extension == ".bmp" && header.starts_with("BM"))
        || (extension == ".webp" && header.starts_with("RIFF") && header.size() >= 12 && header.substr(8, 4) == "WEBP")
        || (extension == ".avif" && header.size() >= 12 && header.substr(4, 4) == "ftyp" && (header.find("avif") != std::string_view::npos || header.find("avis") != std::string_view::npos));
    if (!valid) throw Error("unsafe_content", "Portable background is not a supported image");
    source.unchanged();
}
} // namespace nickel::packs::detail

namespace nickel::packs {
using namespace detail;
// Archive paths are validated for every host, not just Windows: reserved device names,
// trailing dots/spaces and meta characters would alias after extraction.
std::string validate_archive_path(std::string_view path, bool directory) {
    if (directory && path.ends_with('/')) path.remove_suffix(1);
    if (path.empty() || path.size() > 1024 || !valid_utf8(path) || path.starts_with('/') || path.find('\\') != std::string_view::npos)
        throw Error("unsafe_path", "Invalid archive path");
    for (std::size_t start = 0; start < path.size();) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == ".." || part.ends_with('.') || part.ends_with(' ')
            || std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32 || c == 127 || std::string_view(":*?<>|\"").find(static_cast<char>(c)) != std::string_view::npos; })) throw Error("unsafe_path", "Unsafe archive path component");
        const auto lower = io::ascii_lower(part);
        auto stem = lower.substr(0, lower.find('.'));
        while (!stem.empty() && stem.back() == ' ') stem.pop_back();
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "clock$" || stem == "conin$" || stem == "conout$"
            || ((stem.starts_with("com") || stem.starts_with("lpt")) && ((stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9') || stem.substr(3) == "\xc2\xb9" || stem.substr(3) == "\xc2\xb2" || stem.substr(3) == "\xc2\xb3"))) throw Error("unsafe_path", "Reserved archive device path");
        if (end == std::string_view::npos) break;
        start = end + 1;
        if (start == path.size()) throw Error("unsafe_path", "Empty archive path component");
    }
    return std::string(path);
}
void validate_archive_entries(std::span<const ArchiveEntry> entries, const ArchiveLimits& limits) {
    if (entries.size() > limits.entries) throw Error("limit", "Archive entry quota exceeded");
    std::unordered_map<std::string, bool> names;
    std::uint64_t total = 0;
    for (const auto& entry : entries) {
        const auto name = validate_archive_path(entry.name, entry.directory);
        if (entry.link) throw Error("unsafe_path", "Archive links or special files are not allowed");
        if (entry.size > limits.file_bytes || entry.size > limits.total_bytes - std::min(total, limits.total_bytes)) throw Error("limit", "Archive byte quota exceeded");
        if (entry.directory && entry.size) throw Error("archive", "Archive directory contains data");
        total += entry.size;
        auto key = io::ascii_lower(name);
#ifdef _WIN32
        key = instances::root_identity(key);
#endif
        if (!names.emplace(key, entry.directory).second) throw Error("conflict", "Archive duplicate or case collision");
    }
    for (const auto& [name, directory] : names) {
        (void)directory;
        for (auto end = name.find('/'); end != std::string::npos; end = name.find('/', end + 1)) {
            const auto parent = names.find(name.substr(0, end));
            if (parent != names.end() && !parent->second) throw Error("conflict", "Archive file and directory collision");
        }
    }
}
void validate_pack_path(std::string_view path) {
    validate_archive_path(path);
    const auto root = io::ascii_lower(path.substr(0, path.find('/')));
    constexpr std::string_view protected_roots[]{"gamedata.json", "launcher_profiles.json", "launcher_accounts.json", "versions", "libraries", "assets", "runtime", "natives", "logs", "crash-reports"};
    if (root.starts_with("nickel.") || root.starts_with(".nickel") || sensitive_content_path(path)
        || std::find(std::begin(protected_roots), std::end(protected_roots), root) != std::end(protected_roots)) throw Error("protected_path", "Pack path targets launcher or private data");
}
bool sensitive_content_path(std::string_view path) {
    const bool world = io::ascii_lower(path.substr(0, path.find('/'))) == "saves";
    std::size_t index = 0;
    for (std::size_t start = 0; start < path.size(); ++index) {
        const auto end = path.find('/', start);
        const auto part = io::ascii_lower(path.substr(start, end == std::string_view::npos ? path.size() - start : end - start));
        if (!(world && index == 1)) {
            const auto dot = part.rfind('.');
            const auto stem = dot == std::string::npos ? part : part.substr(0, dot);
            if (part.starts_with('.') || part.starts_with("nickel.") || credential_key(stem)
                || part == "accounts.json" || part == "launcher_accounts.json" || part == "launcher_profiles.json" || part == "usercache.json" || part == "usernamecache.json"
                || part.ends_with(".pem") || part.ends_with(".key") || part.ends_with(".p12") || part.ends_with(".pfx")) return true;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return false;
}
std::optional<std::pair<std::string, bool>> content_group(std::string_view path) {
    if (path.empty() || sensitive_content_path(path)) return {};
    const auto lower = io::ascii_lower(path);
    const auto root = lower.substr(0, lower.find('/'));
    if (root != "saves" && root != "resourcepacks" && root != "shaderpacks") {
        for (const auto* excluded : {"cache", "caches", "cached", "probe", "exported", "playerdata", "stats"}) {
            if (within(lower, excluded) || lower.find(std::string("/") + excluded + "/") != std::string::npos || lower.ends_with(std::string("/") + excluded)) return {};
        }
    }
    if (within(lower, "config/jei/world")) return {{"jeiPersonal", false}};
    if (lower == "emi.json" || lower == "config/emi.json") return {{"emiPersonal", false}};
    if (lower == "patchouli_data.json" || lower == "config/patchouli_data.json") return {{"patchouliPersonal", false}};
    if (within(lower, "config/tacz/custom")) return {{"tacz", true}};
    for (const auto* scope : {"config/openloader", "config/global_packs", "config/paxi/datapacks", "config/paxi/resourcepacks", "config/packmenu", "config/patchouli_books"}) if (within(lower, scope)) return {{"packData", true}};
    const auto shader = lower.starts_with("shaderpacks/") ? lower.substr(12) : std::string{};
    const auto dot = shader.rfind('.');
    const auto extension = dot == std::string::npos ? std::string{} : shader.substr(dot + 1);
    const bool shader_text = extension == "txt" || extension == "properties" || extension == "cfg" || extension == "ini" || extension == "json" || extension == "toml" || extension == "yaml" || extension == "yml";
    if (lower == "optionsshaders.txt" || lower == "config/iris.properties" || lower == "config/oculus.properties" || (!shader.empty() && shader.find('/') == std::string::npos && shader_text)) return {{"shaderOptions", true}};
    if (within(lower, "journeymap/config") || within(lower, "journeymap/server")) return {{"config", true}};
    if (root == "mods") return {{lower.ends_with(".disabled") || lower.ends_with(".disable") || lower.ends_with(".old") ? "disabledMods" : "mods", !(lower.ends_with(".disabled") || lower.ends_with(".disable") || lower.ends_with(".old"))}};
    if (root == "config" || root == "defaultconfigs") return {{"config", true}};
    for (const auto* group : {"kubejs", "scripts", "patchouli_books", "datapacks", "openloader", "global_packs", "resources", "structures", "packmenu", "paxi"}) if (root == group) return {{"packData", true}};
    if (root == "tacz") return {{"tacz", true}};
    if (root == "immersive_paintings") return {{"paintings", true}};
    if (root == "schematics") return {{"schematics", false}};
    if (root == "replay_recordings" || root == "replay_videos") return {{"replays", false}};
    if (root == "resourcepacks") return {{"resourcepacks", true}};
    if (root == "shaderpacks") return {{"shaderpacks", true}};
    if (root == "screenshots") return {{"screenshots", false}};
    if (root == "saves") return {{"saves", false}};
    for (const auto* map : {"xaero", "xaerowaypoints", "xaeroworldmap", "journeymap", "voxelmap"}) if (root == map) return {{"maps", false}};
    if (lower.find('/') == std::string::npos) {
        if (lower == "options.txt" || lower == "optionsof.txt") return {{"gameOptions", true}};
        if (lower == "servers.dat" || lower == "servers.dat_old") return {{"servers", false}};
        if (lower == "hotbar.nbt" || lower == "command_history.txt") return {{"personal", false}};
    }
    return {};
}
void validate_download_url(std::string_view url) {
    if (url.size() > 8192 || !url.starts_with("https://") || url.find('#') != std::string_view::npos || url.find('\\') != std::string_view::npos
        || std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c >= 127; })) throw Error("unsafe_url", "Pack downloads require credential-free HTTPS");
    const auto end = url.find_first_of("/?", 8);
    const auto authority = url.substr(8, end == std::string_view::npos ? url.size() - 8 : end - 8);
    if (authority.empty() || authority.find('@') != std::string_view::npos || authority.find('%') != std::string_view::npos || private_url_credentials(url)) throw Error("unsafe_url", "Pack URL contains credentials or invalid host");
    auto host = authority;
    std::string_view port;
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || authority.substr(1, close - 1).find(':') == std::string_view::npos) throw Error("unsafe_url", "Invalid HTTPS IPv6 host");
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) { if (authority[close + 1] != ':') throw Error("unsafe_url", "Invalid HTTPS authority"); port = authority.substr(close + 2); }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string_view::npos) { host = authority.substr(0, colon); port = authority.substr(colon + 1); }
        if (host.empty() || host.starts_with('.') || host.ends_with('.') || !std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '.'; })) throw Error("unsafe_url", "Invalid HTTPS host");
    }
    if (authority.ends_with(':')) throw Error("unsafe_url", "Invalid HTTPS port");
    if (!port.empty()) {
        unsigned int number = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || !number || number > 65535) throw Error("unsafe_url", "Invalid HTTPS port");
    }
    for (auto offset = url.find('%'); offset != std::string_view::npos; offset = url.find('%', offset + 3)) {
        if (offset + 2 >= url.size() || !std::isxdigit(static_cast<unsigned char>(url[offset + 1])) || !std::isxdigit(static_cast<unsigned char>(url[offset + 2]))) throw Error("unsafe_url", "Invalid HTTPS encoding");
    }
}
bool safe_content_text(std::string_view text) { return policy_text(text, ".txt", false); }
} // namespace nickel::packs
