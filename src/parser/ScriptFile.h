// Script file decoding for the pilot tools (tsql_dump, tsql_bench): bytes -> UTF-8 text the way .NET's
// StreamReader decodes them for SqlScriptDOM: BOM detection (UTF-8, UTF-16 LE/BE), UTF-8 by default.
#pragma once

#include <initializer_list>
#include <string>

namespace tsql::pilot {

inline void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

inline std::string Utf16ToUtf8(const std::string& bytes, size_t start, bool bigEndian) {
    std::string out;
    auto unit = [&](size_t i) -> char32_t {
        unsigned char a = static_cast<unsigned char>(bytes[i]), b = static_cast<unsigned char>(bytes[i + 1]);
        return bigEndian ? (a << 8 | b) : (b << 8 | a);
    };
    for (size_t i = start; i + 1 < bytes.size(); i += 2) {
        char32_t u = unit(i);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < bytes.size()) {
            char32_t lo = unit(i + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                AppendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                i += 2;
                continue;
            }
        }
        if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;
        AppendUtf8(out, u);
    }
    if (bytes.size() > start && (bytes.size() - start) % 2) AppendUtf8(out, 0xFFFD);
    return out;
}

inline std::string DecodeScriptFile(const std::string& bytes) {
    auto has = [&](std::initializer_list<unsigned char> bom) {
        if (bytes.size() < bom.size()) return false;
        size_t i = 0;
        for (unsigned char c : bom)
            if (static_cast<unsigned char>(bytes[i++]) != c) return false;
        return true;
    };
    if (has({0xEF, 0xBB, 0xBF})) return bytes.substr(3);
    if (has({0xFF, 0xFE}) && !has({0xFF, 0xFE, 0x00, 0x00})) return Utf16ToUtf8(bytes, 2, false);
    if (has({0xFE, 0xFF})) return Utf16ToUtf8(bytes, 2, true);
    return bytes;   // UTF-8; invalid sequences become U+FFFD inside tsql::parse
}

}  // namespace tsql::pilot
