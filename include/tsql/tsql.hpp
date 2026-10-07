// SPDX-License-Identifier: MIT
// libtsql - C++ T-SQL lexer.
//
// Tokenize a T-SQL fragment into a token stream and run a thin identifier-rewriter on top of it.
// The parser and the editor support are in tsql/parser.hpp and tsql/editor.hpp (TSQL_BUILD_PARSER).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tsql {

enum class TokenKind : std::uint8_t {
    EndOfFile,
    Whitespace,
    LineComment,          // -- to end of line
    BlockComment,         // /* ... */, may nest
    Identifier,           // bare: Customer
    QuotedIdentifier,     // "Customer" (when QUOTED_IDENTIFIER ON)
    BracketedIdentifier,  // [Customer]
    Keyword,              // reserved T-SQL word, case-insensitive
    StringLiteral,        // 'text', N'text'
    NumericLiteral,       // 1, 1.5, 0x1A, 1e10
    Variable,             // @var, @@cur
    Punctuation,          // . , ; ( )
    Operator,             // + - * / % = < > <= >= <> != etc.
    Unknown,              // anything the lexer doesn't recognise
};

struct Token {
    TokenKind   kind   = TokenKind::EndOfFile;
    std::size_t start  = 0;   // byte offset into the source view
    std::size_t length = 0;   // byte length
};

// Tokenize a T-SQL fragment. Whitespace and comments are surfaced as their
// own tokens so callers can losslessly stitch the source back together.
std::vector<Token> tokenize(std::string_view sql);

// True iff `name` matches a T-SQL reserved keyword (case-insensitive).
// 169 entries.
bool is_keyword(std::string_view name);

// Rewrite `sql`, replacing identifier tokens whose unquoted body has a
// (case-insensitive) match in `mapping`. The original quoting form is
// preserved: a [bracketed] identifier stays bracketed, "quoted" stays
// quoted, bare stays bare. Keywords, literals, comments, whitespace and
// operators are emitted verbatim from the source.
std::string anonymize_identifiers(
    std::string_view sql,
    const std::unordered_map<std::string, std::string>& mapping);

}  // namespace tsql
