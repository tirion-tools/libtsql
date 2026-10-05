// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlParserToken.cs
#include "tsql/ast/token.hpp"

namespace tsql::ast {

int Utf16Length(std::string_view utf8) {
    int length = 0;
    for (unsigned char c : utf8) {
        if ((c & 0xC0) != 0x80) {
            // Lead byte: one UTF-16 unit, two for code points above U+FFFF (4-byte sequences).
            length += c >= 0xF0 ? 2 : 1;
        }
    }
    return length;
}

}  // namespace tsql::ast
