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
    /// The parser-visible tokens of `all` (copied).
    ScriptTokens(std::string_view sql, const std::vector<LexToken>& all);
    /// A view of `count` parser-visible tokens at `visible`.
    ScriptTokens(std::string_view sql, const LexToken* visible, size_t count) : sql_(sql), toks_(visible), n_(count) {}
    ScriptTokens(const ScriptTokens&) = delete;
    ScriptTokens& operator=(const ScriptTokens&) = delete;
    ScriptTokens(ScriptTokens&&) = default;

    size_t size() const { return n_; }
    uint32_t Type(size_t i) const { return i < n_ ? toks_[i].type : 0; }
    const LexToken& At(size_t i) const { return toks_[i]; }
    std::string_view Text(size_t i) const;
    /// Whether token i is the keyword or word `upper` (reserved or not; ASCII case-insensitive).
    bool Is(size_t i, std::string_view upper) const;
    /// Whether token i is a name (identifier, [quoted] or "quoted"), and the name it stands for.
    bool IsName(size_t i) const;
    std::string Name(size_t i) const;
    /// First token starting at or after byte `offset`.
    size_t IndexAt(size_t offset) const;
    /// Whether token i is the first parser-visible token on its line.
    bool LineStart(size_t i) const;
    /// Index of the token just past the GO that ends the batch containing token i's position.
    size_t BatchStart(size_t i) const;

private:
    std::string_view sql_;
    std::vector<LexToken> own_;
    const LexToken* toks_ = nullptr;
    size_t n_ = 0;
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
    const CatalogType* tableType = nullptr;   // declared with a user-defined table type
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

    /// Catalog lookups in the caret's database: `parts` is a written name (schema and database
    /// optional). Unqualified names look in the default schema, then dbo; unqualified sp_ and xp_
    /// procedures also in sys, as SQL Server does.
    const CatalogObject* FindObject(const std::vector<std::string>& parts) const;
    const CatalogType* FindType(const std::vector<std::string>& parts) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Index of the token where the statement containing token `at` starts, from the tokens alone,
/// scanning from token `from` (a statement start; SIZE_MAX: the start of the batch).
size_t StatementStartFromTokens(const ScriptTokens& tokens, size_t at, size_t from = SIZE_MAX);

// ------------------------------------------------------------------------------------ catalog

/// Whether an object or type of database `objectDb` exists in database `db` (an empty database
/// is a system object, present in every database).
bool InDatabase(std::string_view objectDb, std::string_view db);
/// The object `db.schema.name` (objects of the database first, then system objects).
const CatalogObject* FindCatalogObject(const Catalog& catalog, std::string_view db, std::string_view schema,
                                       std::string_view name);
/// The object a synonym stands for (its target names resolved against the synonym's database and
/// the catalog's default schema); nullptr when the catalog does not have it (e.g. a linked server).
const CatalogObject* SynonymTarget(const Catalog& catalog, const CatalogObject& synonym);
/// The type `db.schema.name` (types of the database first, then those of every database).
const CatalogType* FindCatalogType(const Catalog& catalog, std::string_view db, std::string_view schema,
                                   std::string_view name);
/// The parts of a multi-part name as written in a catalog field ("db.[my schema].t").
std::vector<std::string> SplitMultiPartName(std::string_view name);

}  // namespace tsql::editor::detail
