// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql80ParserBaseInternal.cs
// (CreateParseError, GetIncorrectSyntaxError, GetUnexpectedTokenError*).
#include "ParserErrors.h"

#include <cstdlib>

namespace tsql::parser {

const ::tsql::ast::ScriptTokenStream*& CurrentScriptTokens() {
    thread_local const ::tsql::ast::ScriptTokenStream* tokens = nullptr;
    return tokens;
}

TokenPosition PositionOf(const antlr4::Token* token) {
    TokenPosition p;
    const auto* tokens = CurrentScriptTokens();
    if (token == nullptr || tokens == nullptr) return p;   // C#: Debug.Assert(false); 0/1/1
    size_t i = token->getTokenIndex();
    if (i < tokens->size()) {
        const auto& t = (*tokens)[i];
        p.Offset = t.Offset;
        p.Line = t.Line;
        p.Column = t.Column;
    }
    return p;
}

ParseError MakeParseError(std::string_view identifier, int offset, int line, int column, std::string message) {
    ParseError e;
    e.Number = std::atoi(std::string(identifier.substr(3)).c_str());
    e.Offset = offset;
    e.Line = line;
    e.Column = column;
    e.Message = std::move(message);
    return e;
}

std::string TokenText(const antlr4::Token* token) {
    if (token == nullptr || token->getType() == antlr4::Token::EOF) return std::string();
    return token->getText();
}

ParseError GetIncorrectSyntaxError(const antlr4::Token* token) {
    return CreateParseError("SQL46010", token, TSqlParserResource::SQL46010Message, TokenText(token));
}

ParseError GetUnexpectedTokenError(const antlr4::Token* token) {
    if (token->getType() == antlr4::Token::EOF)
        return CreateParseError("SQL46029", token, TSqlParserResource::SQL46029Message);
    return GetIncorrectSyntaxError(token);
}

TSqlParseErrorException GetUnexpectedTokenErrorException(const antlr4::Token* token) {
    return TSqlParseErrorException(GetUnexpectedTokenError(token));
}

}  // namespace tsql::parser
