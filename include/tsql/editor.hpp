// libtsql editor support: completion and colouring for T-SQL editors, driven by the version's
// parser (tsql::parse's grammars). Built with the parser (TSQL_BUILD_PARSER_PILOT), CMake target
// tsql_editor.
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tsql/parser.hpp"

namespace tsql::editor {

enum class CompletionKind {
    Keyword, Database, Schema, Table, View, Column, ScalarFunction, TableFunction, Procedure,
    Alias, Cte, TempTable, TableVariable, Variable, DataType, TableHint, QueryHint, BuiltinFunction,
    Parameter, Synonym, UserType
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

/// A parameter of a procedure or function, in declaration order; `name` includes the '@'.
struct CatalogParameter {
    std::string name;
    std::string type;
    bool output = false;
    bool hasDefault = false;
};

/// An object with an empty `database` is a system object (sys.*, INFORMATION_SCHEMA.*) and exists
/// in every database, as on the server; fill these from sys.all_objects / sys.all_columns.
struct CatalogObject {
    enum class Type { Table, View, ScalarFunction, TableFunction, Procedure, Synonym };
    std::string database, schema, name;
    Type type;
    std::vector<CatalogColumn> columns;         // tables, views, TVFs
    std::vector<CatalogParameter> parameters;   // procedures and functions
    std::string target;                         // synonyms: the multi-part name it stands for
};

/// A user-defined type: an alias type, or a table type (with columns) for table variables and
/// READONLY parameters. Empty `database` = every database, as for CatalogObject.
struct CatalogType {
    std::string database, schema, name;
    bool isTableType = false;
    std::vector<CatalogColumn> columns;
};

struct Catalog {
    std::string currentDatabase;
    std::string defaultSchema = "dbo";
    std::vector<std::string> databases;
    std::vector<CatalogObject> objects;
    std::vector<CatalogType> types;
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

/// An editor buffer. Keeps lexer and parser results between calls so that after an edit only the
/// affected text is lexed and parsed again. For any sequence of edits, its results equal the free
/// functions' on Text(). Not thread-safe, but it may move between threads between calls (e.g. be
/// opened on a worker thread); distinct Documents may be used on different threads at once.
class Document {
public:
    explicit Document(SqlVersion version);   // throws std::invalid_argument like Complete
    ~Document();
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;

    void SetText(std::string_view sql);
    /// Replaces bytes [start, start+length) with `replacement`.
    void Edit(size_t start, size_t length, std::string_view replacement);
    const std::string& Text() const;

    CompletionResult Complete(size_t caret, const Catalog& catalog);
    /// The spans of the tokens overlapping [start, end), contiguous; the first may begin before
    /// `start` and the last end after `end`. Cost grows with the range, not the document.
    std::vector<ColouredSpan> Classify(size_t start, size_t end);

    /// Parses the text the queries have not needed yet, from the start, for about `budget`: it
    /// stops at the first statement boundary after the budget runs out, so one call can overrun by
    /// one statement's parse. Returns true once everything is parsed (until the next edit or
    /// SetText). Meant for the editor's idle time after opening or a large paste, so that a later
    /// Complete or Classify anywhere is fast; no other result depends on it.
    bool ParseAhead(std::chrono::steady_clock::duration budget);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Builds `version`'s grammar tables and primes the parser's prediction caches, so the first
/// Complete/Classify is not slow. Thread-safe: meant for a background thread at start-up, and may
/// run while Documents of any version are in use.
void Warm(SqlVersion version);

}  // namespace tsql::editor
