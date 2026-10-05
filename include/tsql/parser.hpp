// libtsql parser: SqlScriptDOM's TSql<ver>Parser.Parse ported to C++ on ANTLR 4 (grammars
// converted by tools/g2to4). Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e:
// SqlScriptDom/Parser/TSql/TSqlParser.cs, TSql*Parser.cs, ParseError.cs,
// SqlScriptDom/ScriptDom/SqlServer/SqlVersion.cs
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tsql/ast/ast.hpp"

namespace tsql {

/// SqlScriptDOM's SqlVersion (same names and values). parse() supports Sql130..Sql180 and
/// SqlFabricDW, each when its grammar was built (CMake TSQL_PARSER_GRAMMARS).
enum class SqlVersion {
    Sql90 = 0,
    Sql80 = 1,
    Sql100 = 2,
    Sql110 = 3,
    Sql120 = 4,
    Sql130 = 5,
    Sql140 = 6,
    Sql150 = 7,
    Sql160 = 8,
    Sql170 = 9,
    SqlFabricDW = 10,
    Sql180 = 11,
};

/// "TSql160" etc. (the ScriptDom parser class name without "Parser"); nullptr if no such parser.
const char* GrammarName(SqlVersion version);
/// Inverse of GrammarName; false if `name` names no supported grammar.
bool SqlVersionFromGrammarName(std::string_view name, SqlVersion& version);
/// Whether this build contains the parser for `version`.
bool IsParserAvailable(SqlVersion version);

/// SqlScriptDOM's ParseError (Number e.g. 46010; Offset is in UTF-16 code units; Line/Column 1-based).
struct ParseError {
    int Number = 0;
    int Offset = 0;
    int Line = 0;
    int Column = 0;
    std::string Message;
};

struct ParseResult {
    /// Every token of the script incl. whitespace/comments and the final EndOfFile token;
    /// fragments' FirstTokenIndex/LastTokenIndex index into it.
    std::unique_ptr<ast::ScriptTokenStream> tokens;
    /// Owns every AST node.
    std::unique_ptr<ast::FragmentFactory> factory;
    /// Like SqlScriptDOM: lexer errors yield an empty TSqlScript without a token stream; null when
    /// a parse error escaped every recovering rule.
    ast::TSqlScript* script = nullptr;
    std::vector<ParseError> errors;
};

/// TSql<ver>Parser(initialQuotedIdentifiers).Parse(new StringReader(sql), out errors).
/// `sql` is UTF-8 (invalid sequences decode to U+FFFD like .NET's StreamReader).
/// Throws std::invalid_argument when IsParserAvailable(version) is false.
ParseResult parse(std::string_view sql, SqlVersion version = SqlVersion::Sql170, bool initialQuotedIdentifiers = true);

}  // namespace tsql
