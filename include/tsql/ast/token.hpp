// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlParserToken.cs
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tsql/ast/generated/token_types.hpp"

namespace tsql::ast {

/// A token of the script token stream. Offset/Line/Column follow SqlScriptDOM: Offset is in
/// UTF-16 code units from the start of the script; Line and Column are 1-based.
/// Text is UTF-8 (C# null text and "" are both represented as "").
struct TSqlParserToken {
    TSqlTokenType TokenType = TSqlTokenType::None;
    int Offset = 0;
    int Line = 1;
    int Column = 1;
    std::string Text;
    /// C# internal ConvertStringToIdentifier: AsciiStringOrQuotedIdentifier tokens are handed to
    /// the parser as QuotedIdentifier when set, AsciiStringLiteral otherwise.
    bool ConvertStringToIdentifier = false;

    /// Keywords are numbered between EndOfFile and Bang in TSqlTokenTypes.g.
    bool IsKeyword() const { return TokenType > TSqlTokenType::EndOfFile && TokenType < TSqlTokenType::Bang; }
};

using ScriptTokenStream = std::vector<TSqlParserToken>;

/// Length of UTF-8 text in UTF-16 code units (.NET string.Length).
int Utf16Length(std::string_view utf8);

}  // namespace tsql::ast
