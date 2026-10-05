#include "Names.h"

#include <algorithm>
#include <cstring>

#include "generated/support/Keywords.h"

namespace tsql::editor::detail {

namespace {
char UpperChar(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }
bool IsAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
}  // namespace

std::string Upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = UpperChar(c);
    return out;
}

bool EqualsI(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (UpperChar(a[i]) != UpperChar(b[i])) return false;
    return true;
}

bool StartsWithI(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && EqualsI(s.substr(0, prefix.size()), prefix);
}

std::string Unquote(std::string_view token) {
    if (token.size() >= 2 && (token.front() == '[' || token.front() == '"')) {
        const char close = token.front() == '[' ? ']' : '"';
        std::string_view body = token.substr(1);
        if (!body.empty() && body.back() == close) body.remove_suffix(1);
        std::string out;
        out.reserve(body.size());
        for (size_t i = 0; i < body.size(); ++i) {
            out += body[i];
            if (body[i] == close && i + 1 < body.size() && body[i + 1] == close) ++i;
        }
        return out;
    }
    if (token.size() == 1 && (token.front() == '[' || token.front() == '"')) return {};
    return std::string(token);
}

bool IsReservedKeyword(std::string_view upper) {
    std::string lower(upper);
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const auto* begin = std::begin(parser::kKeywords);
    const auto* end = std::end(parser::kKeywords);
    const auto* it = std::lower_bound(begin, end, lower, [](const parser::KeywordEntry& e, const std::string& s) {
        return std::strcmp(e.text, s.c_str()) < 0;
    });
    return it != end && lower == it->text;
}

bool NeedsQuoting(std::string_view name) {
    if (name.empty()) return true;
    const char first = name.front();
    if (!(IsAsciiLetter(first) || first == '_' || first == '@' || first == '#' ||
          static_cast<unsigned char>(first) >= 0x80))
        return true;
    for (char c : name.substr(1))
        if (!(IsAsciiLetter(c) || IsDigit(c) || c == '_' || c == '@' || c == '#' || c == '$' ||
              static_cast<unsigned char>(c) >= 0x80))
            return true;
    return IsReservedKeyword(Upper(name));
}

std::string QuoteName(std::string_view name) {
    if (!NeedsQuoting(name)) return std::string(name);
    std::string out = "[";
    for (char c : name) {
        out += c;
        if (c == ']') out += ']';
    }
    out += ']';
    return out;
}

}  // namespace tsql::editor::detail
