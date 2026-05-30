// SPDX-License-Identifier: MIT
// libtsql lexer. State machine + keyword table only; no parser or AST.
// Follows the documented T-SQL lexical conventions: case-insensitive
// keywords, nested block comments, [bracketed] and "quoted" identifiers
// with doubled-quote escapes.

#include "tsql/tsql.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace tsql {

namespace {

// ASCII lower-case folder. T-SQL identifiers are not Unicode-cased in
// practice for the keyword set (every keyword is ASCII), so this is the
// right tool for keyword lookup.
char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(ascii_lower(c));
    return out;
}

const std::unordered_set<std::string>& keyword_set() {
    static const std::unordered_set<std::string> kws = {
#define TSQL_KEYWORD(s) std::string{s},
#include "keywords.inc"
#undef TSQL_KEYWORD
    };
    return kws;
}

// T-SQL identifier start: ASCII letter, _, @, # (also '$' historically).
// Unicode letters are technically legal as identifier starts in modern
// T-SQL; we accept any byte >= 0x80 so multibyte UTF-8 identifiers
// survive the round-trip (they won't match keywords, which are all
// ASCII).
bool is_id_start(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '_' || c == '#' || c == '$' || c >= 0x80;
}

bool is_id_cont(unsigned char c) {
    return is_id_start(c) || (c >= '0' && c <= '9') || c == '@';
}

bool is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

bool is_hex_digit(unsigned char c) {
    return is_digit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

// Scan a 'string literal' or N'unicode literal' starting at p[i]. Caller
// guarantees the opening quote is at p[i]. Doubled quotes ('') escape.
std::size_t scan_string(std::string_view p, std::size_t i) {
    std::size_t j = i + 1;
    while (j < p.size()) {
        if (p[j] == '\'') {
            if (j + 1 < p.size() && p[j + 1] == '\'') { j += 2; continue; }
            return j + 1;
        }
        ++j;
    }
    return p.size();  // unterminated; consume to end
}

// Same shape for [bracketed] and "quoted" identifiers, parameterised by
// the closing character + the doubled-escape rule.
std::size_t scan_delimited(std::string_view p, std::size_t i, char close) {
    std::size_t j = i + 1;
    while (j < p.size()) {
        if (p[j] == close) {
            if (j + 1 < p.size() && p[j + 1] == close) { j += 2; continue; }
            return j + 1;
        }
        ++j;
    }
    return p.size();
}

// /* nested block comment */. T-SQL allows nesting, unlike ANSI SQL.
std::size_t scan_block_comment(std::string_view p, std::size_t i) {
    std::size_t j = i + 2;  // past the opening /*
    int depth = 1;
    while (j + 1 < p.size() && depth > 0) {
        if (p[j] == '/' && p[j + 1] == '*') { depth++; j += 2; }
        else if (p[j] == '*' && p[j + 1] == '/') { depth--; j += 2; }
        else { ++j; }
    }
    return j;
}

std::size_t scan_line_comment(std::string_view p, std::size_t i) {
    std::size_t j = i + 2;
    while (j < p.size() && p[j] != '\n') ++j;
    return j;  // newline stays as Whitespace next iteration
}

// Numeric: integer, decimal, hex (0x...), float with e/E exponent. Loose
// enough for T-SQL; this is a lexer, not a validator.
std::size_t scan_number(std::string_view p, std::size_t i) {
    std::size_t j = i;
    if (p[j] == '0' && j + 1 < p.size() && (p[j + 1] == 'x' || p[j + 1] == 'X')) {
        j += 2;
        while (j < p.size() && is_hex_digit(static_cast<unsigned char>(p[j]))) ++j;
        return j;
    }
    while (j < p.size() && is_digit(static_cast<unsigned char>(p[j]))) ++j;
    if (j < p.size() && p[j] == '.') {
        ++j;
        while (j < p.size() && is_digit(static_cast<unsigned char>(p[j]))) ++j;
    }
    if (j < p.size() && (p[j] == 'e' || p[j] == 'E')) {
        ++j;
        if (j < p.size() && (p[j] == '+' || p[j] == '-')) ++j;
        while (j < p.size() && is_digit(static_cast<unsigned char>(p[j]))) ++j;
    }
    return j;
}

}  // namespace

bool is_keyword(std::string_view name) {
    return keyword_set().count(to_lower(name)) > 0;
}

std::vector<Token> tokenize(std::string_view sql) {
    std::vector<Token> out;
    out.reserve(sql.size() / 8 + 4);
    std::size_t i = 0;
    const std::size_t n = sql.size();
    while (i < n) {
        const std::size_t start = i;
        const unsigned char c = static_cast<unsigned char>(sql[i]);

        if (is_ws(c)) {
            while (i < n && is_ws(static_cast<unsigned char>(sql[i]))) ++i;
            out.push_back({TokenKind::Whitespace, start, i - start});
            continue;
        }
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
            i = scan_line_comment(sql, i);
            out.push_back({TokenKind::LineComment, start, i - start});
            continue;
        }
        if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
            i = scan_block_comment(sql, i);
            out.push_back({TokenKind::BlockComment, start, i - start});
            continue;
        }
        if (c == '[') {
            i = scan_delimited(sql, i, ']');
            out.push_back({TokenKind::BracketedIdentifier, start, i - start});
            continue;
        }
        if (c == '"') {
            i = scan_delimited(sql, i, '"');
            out.push_back({TokenKind::QuotedIdentifier, start, i - start});
            continue;
        }
        if (c == '\'') {
            i = scan_string(sql, i);
            out.push_back({TokenKind::StringLiteral, start, i - start});
            continue;
        }
        // N'unicode literal' - N as identifier start would also match
        // below, so this prefix check has to come first.
        if ((c == 'N' || c == 'n') && i + 1 < n && sql[i + 1] == '\'') {
            ++i;  // consume the N
            i = scan_string(sql, i);
            out.push_back({TokenKind::StringLiteral, start, i - start});
            continue;
        }
        if (c == '@') {
            ++i;
            if (i < n && sql[i] == '@') ++i;  // @@CURSOR etc.
            while (i < n && is_id_cont(static_cast<unsigned char>(sql[i]))) ++i;
            out.push_back({TokenKind::Variable, start, i - start});
            continue;
        }
        if (is_digit(c) ||
            (c == '.' && i + 1 < n &&
             is_digit(static_cast<unsigned char>(sql[i + 1])))) {
            i = scan_number(sql, i);
            out.push_back({TokenKind::NumericLiteral, start, i - start});
            continue;
        }
        if (is_id_start(c)) {
            ++i;
            while (i < n && is_id_cont(static_cast<unsigned char>(sql[i]))) ++i;
            std::string_view word = sql.substr(start, i - start);
            const bool kw = is_keyword(word);
            out.push_back({kw ? TokenKind::Keyword : TokenKind::Identifier,
                           start, i - start});
            continue;
        }
        // Punctuation vs operator. Punctuation tokens carry no
        // semantics for the rewriter; operators may need joining
        // (>=, <=, <>, !=, +=, ...) but for the anonymizer all we
        // care about is "not an identifier".
        if (c == '(' || c == ')' || c == ',' || c == ';' || c == '.') {
            ++i;
            out.push_back({TokenKind::Punctuation, start, 1});
            continue;
        }
        // Multi-char operators first.
        if (i + 1 < n) {
            char a = sql[i], b = sql[i + 1];
            bool two = false;
            if ((a == '>' || a == '<' || a == '!' || a == '+' || a == '-' ||
                 a == '*' || a == '/' || a == '%' || a == '&' || a == '|' ||
                 a == '^' || a == ':') && (b == '=' || b == '>' || b == '<')) {
                two = true;
            } else if (a == '<' && b == '>') {
                two = true;
            } else if (a == '|' && b == '|') {
                two = true;
            }
            if (two) {
                i += 2;
                out.push_back({TokenKind::Operator, start, 2});
                continue;
            }
        }
        // Single-char operator (or unknown byte).
        ++i;
        out.push_back({TokenKind::Operator, start, 1});
    }
    out.push_back({TokenKind::EndOfFile, n, 0});
    return out;
}

namespace {

// Strip a leading/trailing delimiter pair and undo doubled-escape, e.g.
// [foo]]bar] -> foo]bar. For bare identifiers returns the view unchanged.
std::string unwrap_identifier(std::string_view raw, TokenKind kind) {
    if (kind == TokenKind::BracketedIdentifier && raw.size() >= 2 &&
        raw.front() == '[' && raw.back() == ']') {
        std::string out;
        out.reserve(raw.size() - 2);
        for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
            if (raw[i] == ']' && i + 2 < raw.size() && raw[i + 1] == ']') {
                out.push_back(']');
                ++i;
            } else {
                out.push_back(raw[i]);
            }
        }
        return out;
    }
    if (kind == TokenKind::QuotedIdentifier && raw.size() >= 2 &&
        raw.front() == '"' && raw.back() == '"') {
        std::string out;
        out.reserve(raw.size() - 2);
        for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
            if (raw[i] == '"' && i + 2 < raw.size() && raw[i + 1] == '"') {
                out.push_back('"');
                ++i;
            } else {
                out.push_back(raw[i]);
            }
        }
        return out;
    }
    return std::string{raw};
}

// Wrap a replacement back into the original quoting form. We trust the
// caller's mapping not to contain the delimiter byte; that's a fair ask
// for an anonymizer that generates Tbl_1 / Col_1 style names.
std::string rewrap(const std::string& replacement, TokenKind kind) {
    if (kind == TokenKind::BracketedIdentifier)
        return "[" + replacement + "]";
    if (kind == TokenKind::QuotedIdentifier)
        return "\"" + replacement + "\"";
    return replacement;
}

}  // namespace

std::string anonymize_identifiers(
        std::string_view sql,
        const std::unordered_map<std::string, std::string>& mapping) {
    auto tokens = tokenize(sql);
    std::string out;
    out.reserve(sql.size());
    for (const auto& t : tokens) {
        if (t.kind == TokenKind::EndOfFile) continue;
        std::string_view raw = sql.substr(t.start, t.length);
        if (t.kind == TokenKind::Identifier ||
            t.kind == TokenKind::BracketedIdentifier ||
            t.kind == TokenKind::QuotedIdentifier) {
            std::string body = unwrap_identifier(raw, t.kind);
            auto it = mapping.find(to_lower(body));
            if (it != mapping.end()) {
                out.append(rewrap(it->second, t.kind));
                continue;
            }
        }
        out.append(raw);
    }
    return out;
}

}  // namespace tsql
