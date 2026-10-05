// libtsql editor support: completion and colouring for T-SQL editors, driven by the version's
// parser (tsql::parse's grammars). Built with the parser (TSQL_BUILD_PARSER_PILOT), CMake target
// tsql_editor.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "tsql/parser.hpp"

namespace tsql::editor {

enum class CompletionKind {
    Keyword, Database, Schema, Table, View, Column, ScalarFunction, TableFunction, Procedure,
    Alias, Cte, TempTable, TableVariable, Variable, DataType, TableHint, QueryHint, BuiltinFunction
};

struct CompletionItem {
    CompletionKind kind;
    std::string label;
    std::string insertText;
    std::string detail;
};

/// Offsets are UTF-8 byte offsets into the input. [replaceStart, replaceStart+replaceLength) is the
/// partial word the items replace (empty at a word boundary). Items are ranked best first.
struct CompletionResult {
    size_t replaceStart = 0;
    size_t replaceLength = 0;
    std::vector<CompletionItem> items;
};

struct CatalogColumn {
    std::string name;
    std::string type;
};

struct CatalogObject {
    enum class Type { Table, View, ScalarFunction, TableFunction, Procedure };
    std::string database, schema, name;
    Type type;
    std::vector<CatalogColumn> columns;   // columns of tables, views, TVFs
};

struct Catalog {
    std::string currentDatabase;
    std::string defaultSchema = "dbo";
    std::vector<std::string> databases;
    std::vector<CatalogObject> objects;
};

/// Completion items at byte offset `caret` of `sql` for `version` (names match case-insensitively;
/// the partial word before the caret filters by prefix). Throws std::invalid_argument when
/// tsql::IsParserAvailable(version) is false.
CompletionResult Complete(std::string_view sql, size_t caret, SqlVersion version, const Catalog& catalog);

enum class TokenClass {
    Keyword, Identifier, QuotedIdentifier, Variable, String, Number, Comment, Operator,
    Punctuation, BuiltinFunction, DataType, Whitespace, Error
};

/// UTF-8 byte offsets; the spans cover the whole input, in order.
struct ColouredSpan {
    size_t start;
    size_t length;
    TokenClass cls;
};

/// Token classes for colouring: the lexer's class of every token, except that identifiers the
/// version's parser takes as keywords (it checks their text), as built-in data type names or,
/// followed by '(', as built-in function names get that class. Bytes the lexer cannot read are
/// Error spans. Throws std::invalid_argument like Complete.
std::vector<ColouredSpan> Classify(std::string_view sql, SqlVersion version);

}  // namespace tsql::editor
