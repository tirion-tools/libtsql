// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql80ParserBaseInternal.cs
// ("Error reporting utilities" region: the static CreateParseError/GetUnexpectedTokenError family).
#pragma once

#include <string>
#include <string_view>

#include "antlr4-runtime.h"
#include "tsql/ast/ast.hpp"
#include "CsCompat.h"
#include "ParseError.h"
#include "generated/support/TSqlParserResource.h"

namespace tsql::parser {

/// Token stream of the parse running on this thread. C# reads Offset/Line/Column off the token
/// object; ANTLR 4 tokens carry code-point positions, so the UTF-16 positions are looked up here.
const ::tsql::ast::ScriptTokenStream*& CurrentScriptTokens();

struct TokenPosition {
    int Offset = 0;
    int Line = 1;
    int Column = 1;
};
TokenPosition PositionOf(const antlr4::Token* token);

ParseError MakeParseError(std::string_view identifier, int offset, int line, int column, std::string message);

template <class... A>
ParseError CreateParseError(std::string_view identifier, int offset, int line, int column, std::string_view messageTemplate,
                            const A&... args) {
    return MakeParseError(identifier, offset, line, column, FormatMessage(messageTemplate, args...));
}

template <class... A>
ParseError CreateParseError(std::string_view identifier, const antlr4::Token* token, std::string_view messageTemplate,
                            const A&... args) {
    TokenPosition p = PositionOf(token);
    return MakeParseError(identifier, p.Offset, p.Line, p.Column, FormatMessage(messageTemplate, args...));
}

/// Token text as SqlScriptDOM sees it (EOF has no text).
std::string TokenText(const antlr4::Token* token);

ParseError GetIncorrectSyntaxError(const antlr4::Token* token);
ParseError GetUnexpectedTokenError(const antlr4::Token* token);
TSqlParseErrorException GetUnexpectedTokenErrorException(const antlr4::Token* token);

}  // namespace tsql::parser
