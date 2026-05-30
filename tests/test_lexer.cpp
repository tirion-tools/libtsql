// SPDX-License-Identifier: MIT
// libtsql v0.1 lexer + anonymize_identifiers tests.
#include "tsql/tsql.hpp"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failed = 0;

void expect_eq(std::string_view got, std::string_view want, const char* label) {
    if (got != want) {
        ++g_failed;
        std::cerr << "FAIL " << label << "\n"
                  << "  got:  [" << got  << "]\n"
                  << "  want: [" << want << "]\n";
    }
}

void expect_true(bool b, const char* label) {
    if (!b) { ++g_failed; std::cerr << "FAIL " << label << "\n"; }
}

const char* kind_name(tsql::TokenKind k) {
    switch (k) {
        case tsql::TokenKind::EndOfFile: return "EOF";
        case tsql::TokenKind::Whitespace: return "WS";
        case tsql::TokenKind::LineComment: return "LC";
        case tsql::TokenKind::BlockComment: return "BC";
        case tsql::TokenKind::Identifier: return "ID";
        case tsql::TokenKind::QuotedIdentifier: return "QID";
        case tsql::TokenKind::BracketedIdentifier: return "BID";
        case tsql::TokenKind::Keyword: return "KW";
        case tsql::TokenKind::StringLiteral: return "STR";
        case tsql::TokenKind::NumericLiteral: return "NUM";
        case tsql::TokenKind::Variable: return "VAR";
        case tsql::TokenKind::Punctuation: return "PUNC";
        case tsql::TokenKind::Operator: return "OP";
        case tsql::TokenKind::Unknown: return "UNK";
    }
    return "?";
}

std::string sketch(std::string_view sql) {
    auto toks = tsql::tokenize(sql);
    std::string out;
    for (const auto& t : toks) {
        if (t.kind == tsql::TokenKind::EndOfFile) continue;
        if (t.kind == tsql::TokenKind::Whitespace) continue;
        out += kind_name(t.kind);
        out += ':';
        out += std::string(sql.substr(t.start, t.length));
        out += ' ';
    }
    if (!out.empty()) out.pop_back();
    return out;
}

}  // namespace

int main() {
    // keywords (case-insensitive)
    expect_true( tsql::is_keyword("SELECT"),  "SELECT is keyword");
    expect_true( tsql::is_keyword("select"),  "select is keyword");
    expect_true( tsql::is_keyword("From"),    "From is keyword");
    expect_true(!tsql::is_keyword("Customer"),"Customer is not");
    expect_true(!tsql::is_keyword(""),        "empty is not");

    // basic SELECT
    expect_eq(sketch("SELECT a FROM t"),
              "KW:SELECT ID:a KW:FROM ID:t",
              "basic select");

    // bracketed + quoted identifiers
    expect_eq(sketch("SELECT [Customer].name FROM [dbo].[Order]"),
              "KW:SELECT BID:[Customer] PUNC:. ID:name KW:FROM "
              "BID:[dbo] PUNC:. BID:[Order]",
              "bracketed");
    expect_eq(sketch("SELECT \"a b\".c"),
              "KW:SELECT QID:\"a b\" PUNC:. ID:c",
              "quoted ident");

    // doubled-escape inside delimited identifier
    expect_eq(sketch("SELECT [a]]b]"),
              "KW:SELECT BID:[a]]b]",
              "bracket escape");

    // string literals + N'unicode'
    expect_eq(sketch("WHERE n = 'O''Brien'"),
              "KW:WHERE ID:n OP:= STR:'O''Brien'",
              "string escape");
    expect_eq(sketch("SET @x = N'unicode'"),
              "KW:SET VAR:@x OP:= STR:N'unicode'",
              "nstring");

    // numeric literals
    expect_eq(sketch("VALUES (1, 2.5, 0xFF, 1e10)"),
              "KW:VALUES PUNC:( NUM:1 PUNC:, NUM:2.5 PUNC:, "
              "NUM:0xFF PUNC:, NUM:1e10 PUNC:)",
              "numeric variants");

    // line + block comments
    expect_eq(sketch("SELECT 1 -- trailing\nFROM t"),
              "KW:SELECT NUM:1 LC:-- trailing KW:FROM ID:t",
              "line comment");
    expect_eq(sketch("/* outer /* inner */ still */ SELECT 1"),
              "BC:/* outer /* inner */ still */ KW:SELECT NUM:1",
              "nested block comment");

    // variables
    expect_eq(sketch("SET @@trancount = @rows"),
              "KW:SET VAR:@@trancount OP:= VAR:@rows",
              "@@ variable");

    // multi-char operators
    expect_eq(sketch("WHERE a >= 1 AND b <> 2"),
              "KW:WHERE ID:a OP:>= NUM:1 KW:AND ID:b OP:<> NUM:2",
              "operators");

    // byte offsets land on the right bytes
    {
        std::string_view s = "SELECT [x]";
        auto toks = tsql::tokenize(s);
        // Token 2 should be the bracketed identifier "[x]" starting at 7.
        bool found = false;
        for (const auto& t : toks) {
            if (t.kind == tsql::TokenKind::BracketedIdentifier) {
                found = (t.start == 7 && t.length == 3);
                break;
            }
        }
        expect_true(found, "byte offset of [x]");
    }

    // anonymize_identifiers preserves quoting form + non-id text
    std::unordered_map<std::string, std::string> m = {
        {"customer", "Tbl_1"},
        {"name",     "Col_1"},
        {"dbo",      "Schema_1"},
    };
    expect_eq(tsql::anonymize_identifiers(
                  "SELECT [Customer].name FROM dbo.Customer WHERE name = 'X'",
                  m),
              "SELECT [Tbl_1].Col_1 FROM Schema_1.Tbl_1 WHERE Col_1 = 'X'",
              "anonymize: mixed quoting");

    // String literal content is left untouched even when it spells an
    // identifier in the mapping.
    expect_eq(tsql::anonymize_identifiers("SELECT 'customer'", m),
              "SELECT 'customer'",
              "anonymize: don't touch string literals");

    // Unknown identifiers pass through verbatim.
    expect_eq(tsql::anonymize_identifiers("SELECT foo FROM bar", m),
              "SELECT foo FROM bar",
              "anonymize: unmapped passes through");

    if (g_failed) {
        std::cerr << g_failed << " failure(s)\n";
        return 1;
    }
    std::cout << "tsql lexer ok\n";
    return 0;
}
