// Editor support, internal: what names are in scope at the caret, worked out from the tokens of
// the script (so it works while the statement at the caret does not parse): the statement's query
// blocks and their table sources (aliases, derived tables, APPLY, CTEs), DML/DDL targets,
// variables and parameters declared earlier in the batch, temp tables created earlier in the
// script, the database selected by USE, and the catalog objects these names refer to.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Grammar.h"
#include "tsql/editor.hpp"

namespace tsql::editor::detail {

/// The parser-visible tokens of the script (up to the end of the caret's batch).
class ScriptTokens {
public:
    ScriptTokens(std::string_view sql, const std::vector<LexToken>& all);

    size_t size() const { return toks_.size(); }
    uint32_t Type(size_t i) const { return i < toks_.size() ? toks_[i].type : 0; }
    const LexToken& At(size_t i) const { return toks_[i]; }
    std::string_view Text(size_t i) const;
    /// Whether token i is the keyword or word `upper` (reserved or not; ASCII case-insensitive).
    bool Is(size_t i, std::string_view upper) const;
    /// Whether token i is a name (identifier, [quoted] or "quoted"), and the name it stands for.
    bool IsName(size_t i) const;
    std::string Name(size_t i) const;
    /// First token starting at or after byte `offset`.
    size_t IndexAt(size_t offset) const;
    /// Index of the token just past the GO that ends the batch containing token i's position.
    size_t BatchStart(size_t i) const;

private:
    std::string_view sql_;
    std::vector<LexToken> toks_;
};

struct ColumnInfo {
    std::string name;
    std::string type;
};

/// A table source visible to an expression (or a statement's target).
struct SourceInfo {
    enum class Kind { Table, View, TableFunction, Cte, TempTable, TableVariable, Derived, Unknown };
    Kind kind = Kind::Unknown;
    std::string alias;                // as written (empty: none)
    std::vector<std::string> parts;   // object name parts (empty for derived tables)
    std::string exposed;              // the name expressions qualify columns with
    std::vector<ColumnInfo> columns;
};

struct VariableInfo {
    std::string name;   // with @
    std::string type;
    bool isTable = false;
};

struct TempTableInfo {
    std::string name;   // with # / ##
    std::vector<ColumnInfo> columns;
};

struct CteInfo {
    std::string name;
    std::vector<ColumnInfo> columns;
};

class ScopeAnalyzer {
public:
    /// `caret`: token index of the caret; `statementStart`: token index where the caret's statement
    /// starts (SIZE_MAX: found from the tokens).
    ScopeAnalyzer(const ScriptTokens& tokens, const Catalog& catalog, size_t caret, size_t statementStart);
    ~ScopeAnalyzer();

    /// The database the caret is in (the last USE before it, else the catalog's current database).
    const std::string& CurrentDatabase() const;
    std::vector<VariableInfo> Variables() const;
    std::vector<TempTableInfo> TempTables() const;
    std::vector<CteInfo> Ctes() const;
    /// Sources an expression at the caret can reference.
    std::vector<SourceInfo> ExpressionSources() const;
    /// Output column names of the query block at the caret (ORDER BY can use them).
    std::vector<ColumnInfo> SelectListNames() const;
    /// The statement's INSERT/UPDATE/DELETE/MERGE/CREATE INDEX/STATISTICS target, resolved.
    bool Target(SourceInfo& out) const;
    /// UPDATE/DELETE: the sources of the statement's FROM clause (a target can be one of their aliases).
    std::vector<SourceInfo> TargetFromSources() const;
    /// Columns of the catalog object, CTE, temp table or table variable `parts` names.
    bool ResolveObject(const std::vector<std::string>& parts, SourceInfo& out) const;

    /// Catalog lookups in the caret's database.
    const CatalogObject* FindObject(const std::vector<std::string>& parts) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Index of the token where the statement containing token `at` starts, from the tokens alone.
size_t StatementStartFromTokens(const ScriptTokens& tokens, size_t at);

}  // namespace tsql::editor::detail
