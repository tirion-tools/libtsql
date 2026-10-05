// tsql::editor::Complete: the caret's word and replace range, the parser configuration at the caret
// (Grammar::ParseToCaret), the tokens that can follow it (Walk), what kind of name each candidate
// Identifier is (from the rules it is matched in), the names in scope (ScopeAnalyzer) and the
// catalog, ranked.
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "Builtins.h"
#include "Grammar.h"
#include "Names.h"
#include "Scope.h"
#include "Walker.h"
#include "antlr4-runtime.h"
#include "tsql/ast/generated/token_types.hpp"
#include "tsql/editor.hpp"

namespace tsql::editor {

using namespace detail;

namespace {

using T = ast::TSqlTokenType;
constexpr uint32_t Ty(T t) { return static_cast<uint32_t>(t); }

bool OneOf(uint32_t type, std::initializer_list<T> types) {
    for (T t : types)
        if (type == Ty(t)) return true;
    return false;
}

// ------------------------------------------------------------------------------------- rule tags

enum Tag : uint32_t {
    kWrapper = 1u << 0,          // identifier, nonQuotedIdentifier, identifierList, identifierListElement
    kObjectName = 1u << 1,       // schemaObject...PartName
    kTableReference = 1u << 2,   // schemaObjectOrFunctionTableReference
    kDmlTarget = 1u << 3,        // dmlTarget and its schemaObject... alternatives
    kProcedureRef = 1u << 4,     // procedureReference, procObjectReference
    kObjectTarget = 1u << 5,     // CREATE INDEX / STATISTICS ... ON, UPDATE STATISTICS, TRUNCATE, ALTER TABLE ...
    kDataType = 1u << 6,         // scalarDataType, dataTypeSchemaObjectName
    kMultiPart = 1u << 7,        // multiPartIdentifier
    kColumnExpression = 1u << 8, // columnOrFunctionCall, selectStarExpression
    kTargetColumn = 1u << 9,     // insertColumn, mergeInsertDmlColumn, setClause, identifierColumnReferenceExpression
    kBuiltinCall = 1u << 10,     // builtInFunctionCall
    kGlobalTvf = 1u << 11,       // globalFunctionTableReference
    kTableHint = 1u << 12,
    kQueryHint = 1u << 13,
    kExpressionPrimary = 1u << 14,
    kOrderBy = 1u << 15,
    kTableVariableRef = 1u << 16,   // variableTableReference, variableDmlTarget
    kVariableDefinition = 1u << 17, // DECLARE @x, parameters
    kNoNames = 1u << 18,            // variableMethodCallTableReference, ...
    kFunctionTarget = 1u << 19,     // schemaObjectFunctionDmlTarget: INSERT INTO f(arguments)
    kSetCommand = 1u << 20,         // setCommand: SET DEADLOCK_PRIORITY value, ...
};

bool Contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }
bool StartsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

uint32_t TagsOfRule(std::string_view n) {
    uint32_t tags = 0;
    if (n == "identifier" || n == "nonQuotedIdentifier" || n == "identifierList" || n == "identifierListElement")
        tags |= kWrapper;
    if (StartsWith(n, "schemaObject") && Contains(n, "PartName")) tags |= kObjectName;
    if (n == "schemaObjectOrFunctionTableReference") tags |= kTableReference;
    if (n == "dmlTarget" || n == "schemaObjectDmlTarget" || n == "schemaObjectTableDmlTarget" ||
        n == "schemaObjectFunctionDmlTarget")
        tags |= kDmlTarget;
    if (n == "procedureReference" || n == "procObjectReference") tags |= kProcedureRef;
    if (StartsWith(n, "createRelationalIndex") || StartsWith(n, "createColumnStoreIndex") ||
        StartsWith(n, "createStatistics") || StartsWith(n, "updateStatistics") || StartsWith(n, "truncateTable") ||
        StartsWith(n, "alterTable") || StartsWith(n, "alterIndex") || StartsWith(n, "dropTable") ||
        StartsWith(n, "createXmlIndex") || StartsWith(n, "createSpatialIndex") || StartsWith(n, "createJsonIndex") ||
        StartsWith(n, "createVectorIndex"))
        tags |= kObjectTarget;
    if (n == "scalarDataType" || n == "dataTypeSchemaObjectName") tags |= kDataType;
    if (n == "multiPartIdentifier") tags |= kMultiPart;
    if (n == "columnOrFunctionCall" || n == "selectStarExpression") tags |= kColumnExpression;
    if (n == "insertColumn" || n == "mergeInsertDmlColumn" || n == "setClause" ||
        n == "identifierColumnReferenceExpression" || n == "filterColumn")
        tags |= kTargetColumn;
    if (n == "builtInFunctionCall") tags |= kBuiltinCall;
    if (n == "globalFunctionTableReference") tags |= kGlobalTvf;
    // the hints themselves, not the WITH (...) / OPTION (...) around them
    const bool plural = Contains(n, "Hints");
    if ((Contains(n, "TableHint") || n == "tableHint") && !plural) tags |= kTableHint;
    if ((Contains(n, "OptimizerHint") || n == "hint") && !plural) tags |= kQueryHint;
    if (n == "expressionPrimary") tags |= kExpressionPrimary;
    if (n == "orderByClause") tags |= kOrderBy;
    if (n == "variableTableReference" || n == "variableDmlTarget") tags |= kTableVariableRef;
    if ((Contains(n, "declare") || Contains(n, "Declare") || Contains(n, "Parameter")) && n != "setParam")
        tags |= kVariableDefinition;
    if (n == "variableMethodCallTableReference" || n == "stringOrIdentifier") tags |= kNoNames;
    if (n == "schemaObjectFunctionDmlTarget") tags |= kFunctionTarget;
    if (n == "setCommand") tags |= kSetCommand;
    return tags;
}

const std::vector<uint32_t>& RuleTags(const Grammar& g) {
    static std::mutex mutex;
    static std::unordered_map<const Grammar*, std::vector<uint32_t>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(&g);
    if (it != cache.end()) return it->second;
    std::vector<uint32_t> tags;
    tags.reserve(g.ruleNames->size());
    for (const std::string& n : *g.ruleNames) tags.push_back(TagsOfRule(n));
    return cache.emplace(&g, std::move(tags)).first->second;
}

// -------------------------------------------------------------------------------------- contexts

/// What the candidates at the caret ask for.
struct Contexts {
    bool expression = false;      // columns, aliases, scalar variables, built-in functions
    bool builtinFunctions = false;
    bool tableSource = false;     // FROM / JOIN / APPLY / USING
    bool dmlTarget = false;       // INSERT / UPDATE / DELETE / MERGE target
    bool objectTarget = false;    // CREATE INDEX ... ON, UPDATE STATISTICS, TRUNCATE TABLE, ...
    bool procedure = false;       // EXEC
    bool dataType = false;
    bool targetColumns = false;   // INSERT (cols), UPDATE SET, index key columns
    bool tableVariables = false;
    bool scalarVariables = false;
    bool orderBy = false;
    bool setCommandValue = false; // SET <command> |
    std::set<std::string> typeNames;
    std::vector<std::pair<std::string, CompletionKind>> keywords;
    std::set<std::string> keywordSeen;

    bool Any() const {
        return expression || builtinFunctions || tableSource || dmlTarget || objectTarget || procedure || dataType ||
               targetColumns || tableVariables || scalarVariables || setCommandValue || !keywords.empty();
    }
    void AddKeyword(const std::string& word, CompletionKind kind) {
        if (keywordSeen.insert(word + '\x01' + std::to_string(static_cast<int>(kind))).second)
            keywords.emplace_back(word, kind);
    }
    void Merge(const Contexts& o) {
        expression |= o.expression;
        builtinFunctions |= o.builtinFunctions;
        tableSource |= o.tableSource;
        dmlTarget |= o.dmlTarget;
        objectTarget |= o.objectTarget;
        procedure |= o.procedure;
        dataType |= o.dataType;
        targetColumns |= o.targetColumns;
        tableVariables |= o.tableVariables;
        scalarVariables |= o.scalarVariables;
        orderBy |= o.orderBy;
        setCommandValue |= o.setCommandValue;
        typeNames.insert(o.typeNames.begin(), o.typeNames.end());
        for (const auto& [word, kind] : o.keywords) AddKeyword(word, kind);
    }
};

/// Sorts candidates into contexts. Candidates inside the arguments of a function DML target
/// (INSERT INTO f(...)) go to `functionArgs`: they only apply when the target is a function.
class CandidateClassifier {
public:
    CandidateClassifier(const Grammar& g, Contexts& ctx, Contexts& functionArgs)
        : tags_(RuleTags(g)), main_(ctx), functionArgs_(functionArgs) {}

    void Add(const WalkCandidate& c) {
        const std::vector<size_t>& rules = *c.rules;
        auto tag = [&](size_t k) { return k < rules.size() ? tags_[rules[k]] : 0u; };
        auto within = [&](size_t n, uint32_t mask) {
            for (size_t k = 0; k < n && k < rules.size(); ++k)
                if (tags_[rules[k]] & mask) return true;
            return false;
        };
        Contexts& ctx = InFunctionArguments(rules) ? functionArgs_ : main_;
        const auto type = static_cast<uint32_t>(c.tokenType);
        if (type == Ty(T::Identifier) || type == Ty(T::QuotedIdentifier)) {
            if (c.words != nullptr) {
                for (const std::string& w : *c.words) Keyword(ctx, w, within);
                return;
            }
            size_t k = 0;
            while (tag(k) & kWrapper) ++k;
            const uint32_t first = tag(k);
            if (first & kNoNames) return;
            if (first & kObjectName) {
                while (tag(k) & kObjectName) ++k;
                const uint32_t next = tag(k);
                if (next & kTableReference) ctx.tableSource = true;
                else if (next & kDmlTarget) ctx.dmlTarget = true;
                else if (next & kProcedureRef) ctx.procedure = true;
                else if (next & kObjectTarget) ctx.objectTarget = true;
            } else if (first & kDataType) {
                ctx.dataType = true;
                if (c.hints != nullptr) ctx.typeNames.insert(c.hints->begin(), c.hints->end());
            } else if (first & kMultiPart) {
                const uint32_t next = tag(k + 1);
                if (next & kColumnExpression) {
                    ctx.expression = true;
                    if (within(rules.size(), kOrderBy)) ctx.orderBy = true;
                } else if (next & kTargetColumn) {
                    ctx.targetColumns = true;
                }
            } else if (first & kBuiltinCall) {
                ctx.builtinFunctions = true;
            } else if (within(3, kSetCommand)) {
                ctx.setCommandValue = true;
            }
            return;
        }
        if (type == Ty(T::Variable)) {
            if (within(4, kTableVariableRef)) ctx.tableVariables = true;
            else if (!within(4, kVariableDefinition | kNoNames)) ctx.scalarVariables = true;
            return;
        }
        if (const char* text = KeywordText(type)) Keyword(ctx, Upper(text), within);
    }

private:
    /// Inside schemaObjectFunctionDmlTarget other than its name.
    bool InFunctionArguments(const std::vector<size_t>& rules) const {
        for (size_t k = 0; k < rules.size(); ++k)
            if (tags_[rules[k]] & kFunctionTarget) return k == 0 || !(tags_[rules[k - 1]] & kObjectName);
        return false;
    }

    template <class Within>
    void Keyword(Contexts& ctx, const std::string& word, const Within& within) {
        CompletionKind kind = CompletionKind::Keyword;
        if (within(4, kTableHint)) kind = CompletionKind::TableHint;
        else if (within(3, kQueryHint)) kind = CompletionKind::QueryHint;
        else if (within(3, kExpressionPrimary) && FindBuiltin(word) != nullptr) kind = CompletionKind::BuiltinFunction;
        ctx.AddKeyword(word, kind);
    }

    const std::vector<uint32_t>& tags_;
    Contexts& main_;
    Contexts& functionArgs_;
};

// ------------------------------------------------------------------------------------- the caret

bool IsNumberType(uint32_t type) {
    return type == Ty(T::Integer) || type == Ty(T::Numeric) || type == Ty(T::Real) || type == Ty(T::Money) ||
           type == Ty(T::HexLiteral);
}
bool IsStringType(uint32_t type) {
    return type == Ty(T::AsciiStringLiteral) || type == Ty(T::UnicodeStringLiteral);
}
bool IsWordType(uint32_t type) {
    return type == Ty(T::Identifier) || type == Ty(T::Variable) || type == Ty(T::Label) || KeywordText(type) != nullptr;
}

struct CaretWord {
    bool none = false;          // inside a comment, string or number: no items
    size_t start = 0, end = 0;  // replace range
    std::string typed;          // [start, caret) without a leading [ or "
    bool quoted = false;        // the word starts with [ or "
};

CaretWord FindCaretWord(std::string_view sql, size_t caret, const std::vector<LexToken>& all) {
    CaretWord w;
    w.start = w.end = caret;
    auto it = std::upper_bound(all.begin(), all.end(), caret, [](size_t c, const LexToken& t) { return c < t.end; });
    // `it`: first token ending after the caret; the one before ends at or before it
    const LexToken* inside = it != all.end() && it->start < caret ? &*it : nullptr;
    const LexToken* before = it != all.begin() && (it - 1)->end == caret ? &*(it - 1) : nullptr;
    auto text = [&](const LexToken& t) { return sql.substr(t.start, t.end - t.start); };
    auto setWord = [&](const LexToken& t, size_t end) {
        w.start = t.start;
        w.end = end;
        std::string_view typed = sql.substr(t.start, caret - t.start);
        if (!typed.empty() && (typed.front() == '[' || typed.front() == '"')) {
            w.quoted = true;
            typed.remove_prefix(1);
        }
        w.typed = std::string(typed);
    };
    if (inside != nullptr) {
        const uint32_t type = inside->type;
        if (type == Ty(T::SingleLineComment) || type == Ty(T::MultilineComment) || IsStringType(type) ||
            IsNumberType(type)) {
            w.none = true;
        } else if (type == Ty(T::QuotedIdentifier)) {
            setWord(*inside, caret);
        } else if (IsWordType(type)) {
            setWord(*inside, inside->end);
        }
        return w;
    }
    if (before != nullptr) {
        const uint32_t type = before->type;
        const std::string_view t = text(*before);
        if (type == Ty(T::SingleLineComment) || IsNumberType(type)) {
            w.none = true;
        } else if (type == Ty(T::MultilineComment)) {
            w.none = t.size() < 4 || t.substr(t.size() - 2) != "*/";
        } else if (IsStringType(type)) {
            const size_t open = t.find('\'');
            w.none = open == std::string_view::npos || t.size() < open + 2 || t.back() != '\'';
            if (!w.none) {
                // 'It''s' ends with a quote too: count them
                size_t quotes = 0;
                for (char ch : t.substr(open)) quotes += ch == '\'';
                w.none = quotes % 2 != 0;
            }
        } else if (type == Ty(T::QuotedIdentifier)) {
            const char close = t.front() == '[' ? ']' : '"';
            bool closed = t.size() >= 2 && t.back() == close;
            if (closed) {
                size_t run = 0;   // a]]: the final ] is escaped when the run of ]s after the opener is odd... count
                for (size_t k = t.size() - 1; k > 0 && t[k] == close; --k) ++run;
                closed = run % 2 == 1;
            }
            if (!closed) setWord(*before, caret);
        } else if (IsWordType(type)) {
            setWord(*before, caret);
        }
        return w;
    }
    // a lone @ or # the lexer did not make a token of
    size_t s = caret;
    while (s > 0 && (sql[s - 1] == '@' || sql[s - 1] == '#')) --s;
    if (s < caret) {
        w.start = s;
        w.typed = std::string(sql.substr(s, caret - s));
    }
    return w;
}

/// The qualifier before the caret word: `a.b.` -> {a, b} (empty parts for `..`).
std::vector<std::string> Qualifier(const ScriptTokens& t, size_t caretTok) {
    std::vector<std::string> parts;
    size_t i = caretTok;
    while (i > 0 && t.Type(i - 1) == Ty(T::Dot)) {
        --i;
        if (i > 0 && t.IsName(i - 1)) {
            parts.insert(parts.begin(), t.Name(i - 1));
            --i;
        } else {
            parts.insert(parts.begin(), std::string());
        }
    }
    return parts;
}

// ----------------------------------------------------------------------------------------- items

int KindRank(CompletionKind k) {
    switch (k) {
        case CompletionKind::Column: return 0;
        case CompletionKind::Alias: return 1;
        case CompletionKind::Cte: return 2;
        case CompletionKind::TempTable: return 3;
        case CompletionKind::TableVariable: return 4;
        case CompletionKind::Table: return 5;
        case CompletionKind::View: return 6;
        case CompletionKind::TableFunction: return 7;
        case CompletionKind::Procedure: return 8;
        case CompletionKind::Variable: return 9;
        case CompletionKind::ScalarFunction: return 10;
        case CompletionKind::DataType: return 11;
        case CompletionKind::TableHint: return 12;
        case CompletionKind::QueryHint: return 13;
        case CompletionKind::BuiltinFunction: return 14;
        case CompletionKind::Schema: return 15;
        case CompletionKind::Database: return 16;
        case CompletionKind::Keyword: return 17;
    }
    return 18;
}

class ItemList {
public:
    explicit ItemList(const CaretWord& w) : w_(w) {}

    void Add(CompletionKind kind, const std::string& label, std::string insert, std::string detail = {}) {
        if (label.empty()) return;
        if (w_.quoted && (kind == CompletionKind::Keyword || kind == CompletionKind::TableHint ||
                          kind == CompletionKind::QueryHint || kind == CompletionKind::BuiltinFunction))
            return;
        std::string_view bare = insert;
        if (!bare.empty() && (bare.front() == '[' || bare.front() == '"')) bare.remove_prefix(1);
        if (!StartsWithI(label, w_.typed) && !StartsWithI(bare, w_.typed)) return;
        const std::string key = std::to_string(static_cast<int>(kind)) + '\x01' + Upper(label);
        if (!seen_.insert(key).second) return;
        items_.push_back({{kind, label, std::move(insert), std::move(detail)}, order_++});
    }
    void Name(CompletionKind kind, const std::string& name, std::string detail = {}) {
        Add(kind, name, QuoteName(name), std::move(detail));
    }

    std::vector<CompletionItem> Take() {
        std::stable_sort(items_.begin(), items_.end(), [](const Entry& a, const Entry& b) {
            const int ra = KindRank(a.item.kind), rb = KindRank(b.item.kind);
            if (ra != rb) return ra < rb;
            if (a.item.kind == CompletionKind::Column) return a.order < b.order;   // definition order
            const std::string la = Upper(a.item.label), lb = Upper(b.item.label);
            if (la != lb) return la < lb;
            return a.order < b.order;
        });
        std::vector<CompletionItem> out;
        out.reserve(items_.size());
        for (Entry& e : items_) out.push_back(std::move(e.item));
        return out;
    }

private:
    struct Entry {
        CompletionItem item;
        size_t order;
    };
    const CaretWord& w_;
    std::vector<Entry> items_;
    std::set<std::string> seen_;
    size_t order_ = 0;
};

CompletionKind KindOfSource(SourceInfo::Kind k) {
    switch (k) {
        case SourceInfo::Kind::Table: return CompletionKind::Table;
        case SourceInfo::Kind::View: return CompletionKind::View;
        case SourceInfo::Kind::TableFunction: return CompletionKind::TableFunction;
        case SourceInfo::Kind::Cte: return CompletionKind::Cte;
        case SourceInfo::Kind::TempTable: return CompletionKind::TempTable;
        case SourceInfo::Kind::TableVariable: return CompletionKind::TableVariable;
        default: return CompletionKind::Alias;
    }
}

CompletionKind KindOfObject(CatalogObject::Type t) {
    switch (t) {
        case CatalogObject::Type::Table: return CompletionKind::Table;
        case CatalogObject::Type::View: return CompletionKind::View;
        case CatalogObject::Type::ScalarFunction: return CompletionKind::ScalarFunction;
        case CatalogObject::Type::TableFunction: return CompletionKind::TableFunction;
        case CatalogObject::Type::Procedure: return CompletionKind::Procedure;
    }
    return CompletionKind::Table;
}

std::string ObjectDetail(const CatalogObject& o) {
    std::string d = o.database + "." + o.schema;
    switch (o.type) {
        case CatalogObject::Type::Table: return "table " + d;
        case CatalogObject::Type::View: return "view " + d;
        case CatalogObject::Type::ScalarFunction: return "scalar function " + d;
        case CatalogObject::Type::TableFunction: return "table-valued function " + d;
        case CatalogObject::Type::Procedure: return "procedure " + d;
    }
    return d;
}

class ItemBuilder {
public:
    ItemBuilder(const Catalog& catalog, SqlVersion version, const ScopeAnalyzer& scope, ItemList& items)
        : catalog_(catalog), version_(version), scope_(scope), items_(items) {}

    void Columns(const std::vector<ColumnInfo>& columns, const std::string& of) {
        for (const ColumnInfo& c : columns)
            items_.Name(CompletionKind::Column, c.name, c.type.empty() ? of : c.type + " (" + of + ")");
    }

    void Sources(const std::vector<SourceInfo>& sources, bool withColumns) {
        for (const SourceInfo& s : sources) {
            if (s.exposed.empty()) continue;
            const CompletionKind kind = s.alias.empty() ? KindOfSource(s.kind) : CompletionKind::Alias;
            items_.Name(kind, s.exposed, s.parts.empty() ? std::string("derived table") : Joined(s.parts));
        }
        if (withColumns)
            for (const SourceInfo& s : sources) Columns(s.columns, s.exposed);
    }

    /// Catalog objects of `types` in database/schema.
    void Objects(const std::string& db, const std::string& schema, std::initializer_list<CatalogObject::Type> types) {
        for (const CatalogObject& o : catalog_.objects) {
            if (!EqualsI(o.database, db) || !EqualsI(o.schema, schema)) continue;
            if (std::find(types.begin(), types.end(), o.type) == types.end()) continue;
            items_.Name(KindOfObject(o.type), o.name, ObjectDetail(o));
        }
    }

    bool IsDatabase(const std::string& name) const {
        for (const std::string& d : catalog_.databases)
            if (EqualsI(d, name)) return true;
        for (const CatalogObject& o : catalog_.objects)
            if (EqualsI(o.database, name)) return true;
        return false;
    }

    void Schemas(const std::string& db) {
        for (const CatalogObject& o : catalog_.objects)
            if (EqualsI(o.database, db)) items_.Name(CompletionKind::Schema, o.schema, "schema in " + o.database);
    }

    void Databases() {
        for (const std::string& d : catalog_.databases) items_.Name(CompletionKind::Database, d, "database");
    }

    /// FROM / target / EXEC names: `parts` is the qualifier typed before the caret word.
    void ObjectNames(const std::vector<std::string>& parts, std::initializer_list<CatalogObject::Type> types,
                     bool scriptObjects) {
        const std::string& db = scope_.CurrentDatabase();
        if (parts.empty()) {
            Objects(db, catalog_.defaultSchema, types);
            if (scriptObjects) {
                for (const CteInfo& c : scope_.Ctes()) items_.Name(CompletionKind::Cte, c.name, "common table expression");
                for (const TempTableInfo& t : scope_.TempTables()) items_.Name(CompletionKind::TempTable, t.name, "temp table");
            }
            Schemas(db);
            Databases();
            return;
        }
        if (parts.size() == 1) {
            Objects(db, parts[0], types);
            if (IsDatabase(parts[0])) Schemas(parts[0]);
            return;
        }
        // db.schema. / db.. / server.db.schema.
        const std::string& d = parts[parts.size() - 2];
        Objects(d.empty() ? db : d, parts.back().empty() ? catalog_.defaultSchema : parts.back(), types);
    }

    /// Expression names; `parts` is the qualifier.
    void ExpressionNames(const std::vector<std::string>& parts, bool targetOnly, const std::vector<SourceInfo>& sources) {
        if (parts.empty()) {
            if (targetOnly) {
                SourceInfo target;
                if (scope_.Target(target)) Columns(target.columns, target.exposed);
                return;
            }
            Sources(sources, true);
            return;
        }
        bool found = false;
        if (parts.size() == 1) {
            for (const SourceInfo& s : sources)
                if (EqualsI(s.exposed, parts[0])) {
                    Columns(s.columns, s.exposed);
                    found = true;
                }
            if (!found && (EqualsI(parts[0], "inserted") || EqualsI(parts[0], "deleted"))) {
                SourceInfo target;
                if (scope_.Target(target)) {
                    Columns(target.columns, parts[0]);
                    found = true;
                }
            }
            if (!found) {
                Objects(scope_.CurrentDatabase(), parts[0], {CatalogObject::Type::ScalarFunction});
                if (IsDatabase(parts[0])) Schemas(parts[0]);
            }
            return;
        }
        // schema.table. (an unaliased source written with its schema, or matching by its last part)
        for (const SourceInfo& s : sources) {
            if (!s.alias.empty() || s.parts.empty()) continue;
            bool match = EqualsI(s.parts.back(), parts.back());
            if (match && s.parts.size() >= 2) match = EqualsI(s.parts[s.parts.size() - 2], parts[parts.size() - 2]);
            if (match) {
                Columns(s.columns, s.exposed);
                found = true;
            }
        }
        if (!found && parts.size() == 2)
            Objects(parts[0].empty() ? scope_.CurrentDatabase() : parts[0],
                    parts[1].empty() ? catalog_.defaultSchema : parts[1], {CatalogObject::Type::ScalarFunction});
    }

    void Variables(bool tables, bool scalars, bool globals) {
        for (const VariableInfo& v : scope_.Variables()) {
            if (v.isTable && tables) items_.Add(CompletionKind::TableVariable, v.name, v.name, "table variable");
            if (!v.isTable && scalars) items_.Add(CompletionKind::Variable, v.name, v.name, v.type);
        }
        if (globals)
            for (const Builtin* b : BuiltinsFor(version_))
                if (b->kind == Builtin::Kind::GlobalVariable) items_.Add(CompletionKind::BuiltinFunction, b->name, b->name);
    }

    void Functions() {
        for (const Builtin* b : BuiltinsFor(version_))
            if (b->kind == Builtin::Kind::Scalar || b->kind == Builtin::Kind::Aggregate || b->kind == Builtin::Kind::Window)
                items_.Add(CompletionKind::BuiltinFunction, b->name, b->name,
                           b->kind == Builtin::Kind::Aggregate ? "aggregate function"
                           : b->kind == Builtin::Kind::Window ? "window function"
                                                              : "function");
    }

    void TableFunctions() {
        for (const Builtin* b : BuiltinsFor(version_))
            if (b->kind == Builtin::Kind::TableValued)
                items_.Add(CompletionKind::BuiltinFunction, b->name, b->name, "table-valued function");
    }

private:
    static std::string Joined(const std::vector<std::string>& parts) {
        std::string s;
        for (const std::string& p : parts) {
            if (!s.empty()) s += '.';
            s += p;
        }
        return s;
    }

    const Catalog& catalog_;
    SqlVersion version_;
    const ScopeAnalyzer& scope_;
    ItemList& items_;
};

// ---------------------------------------------------------------------------------------- walking

struct WalkResult {
    size_t statementIndex = SIZE_MAX;   // parser-token index where the caret's statement starts
};

/// FIRST set of a rule (cached: the analysis of `statement` visits much of the grammar).
const antlr4::misc::IntervalSet& FirstSet(const Grammar& g, size_t rule) {
    static std::mutex mutex;
    static std::map<std::pair<const Grammar*, size_t>, antlr4::misc::IntervalSet> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find({&g, rule});
    if (it == cache.end()) it = cache.emplace(std::make_pair(&g, rule), g.atn->nextTokens(g.atn->ruleToStartState[rule])).first;
    return it->second;
}

WalkResult WalkCaret(const Grammar& g, const CaretParse& ps, Contexts& ctx, Contexts& functionArgs) {
    WalkResult r;
    const Capture& cap = ps.capture;
    if (!cap.captured) return r;
    CandidateClassifier classifier(g, ctx, functionArgs);
    auto run = [&](int state, size_t index, const std::vector<int>& follow, size_t budget) {
        WalkInput in;
        in.tokens = &ps.tokens;
        in.upper = &ps.upper;
        in.startState = state;
        in.startIndex = index;
        in.outerFollow = follow;
        in.caret = ps.tokens.size();
        in.budget = budget;
        return Walk(g, in, [&](const WalkCandidate& c) { classifier.Add(c); }).emitted > 0;
    };
    // a decision whose opaque predicate looked at the caret: its alternatives as well
    if (ps.early.captured && !ps.early.errorInStatement) run(ps.early.state, ps.early.index, ps.early.follow, 200'000);
    if (!cap.errorInStatement) {
        run(cap.state, cap.index, cap.follow, 400'000);
        r.statementIndex = cap.statementState >= 0 ? cap.statementIndex : SIZE_MAX;
        return r;
    }
    // a syntax error earlier in the caret's statement: walk the statement from its start, else from
    // the first later points where a statement can start
    if (run(cap.statementState, cap.statementIndex, cap.statementFollow, 200'000)) {
        r.statementIndex = cap.statementIndex;
        return r;
    }
    const size_t statementRule = g.atn->states[static_cast<size_t>(cap.statementState)]->ruleIndex;
    const antlr4::misc::IntervalSet& first = FirstSet(g, statementRule);
    size_t tries = 0;
    for (size_t p = cap.errorIndex + 1; p <= ps.tokens.size() && tries < 6; ++p) {
        const bool afterSemicolon = ps.tokens[p - 1].type == Ty(T::Semicolon);
        const bool keywordStart = p < ps.tokens.size() && ps.tokens[p].type != Ty(T::Identifier) &&
                                  first.contains(static_cast<ssize_t>(ps.tokens[p].type));
        if (!afterSemicolon && !keywordStart && p != ps.tokens.size()) continue;
        ++tries;
        if (run(cap.statementState, p, cap.statementFollow, 50'000)) {
            r.statementIndex = p;
            return r;
        }
    }
    return r;
}

/// Contexts from the tokens before the caret when the walk found nothing (no capture, or no
/// alternative of the statement matches the tokens before the caret).
void FallbackContexts(const ScriptTokens& t, size_t caretTok, Contexts& ctx) {
    size_t i = caretTok;
    while (i > 0 && (t.Type(i - 1) == Ty(T::Dot) || t.IsName(i - 1))) --i;
    const uint32_t prev = i > 0 ? t.Type(i - 1) : 0;
    if (OneOf(prev, {T::From, T::Join, T::Into, T::Update, T::Table}) || t.Is(i - 1, "APPLY") || t.Is(i - 1, "USING")) {
        ctx.tableSource = true;
        ctx.tableVariables = true;
    } else if (OneOf(prev, {T::Exec, T::Execute})) {
        ctx.procedure = true;
    } else {
        ctx.expression = ctx.builtinFunctions = ctx.scalarVariables = true;
    }
}

/// SqlScriptDOM's merge-action checks (SQL46040, SQL46041 in insertMergeAction / updateMergeAction /
/// deleteMergeAction): WHEN MATCHED and WHEN NOT MATCHED BY SOURCE take UPDATE or DELETE, WHEN NOT
/// MATCHED [BY TARGET] takes INSERT. Returns the action keywords to drop after `... THEN`.
std::vector<std::string> ExcludedMergeActions(const ScriptTokens& t, size_t caretTok) {
    if (caretTok == 0 || t.Type(caretTok - 1) != Ty(T::Then)) return {};
    size_t depth = 0;
    for (size_t i = caretTok - 1; i-- > 0;) {
        const uint32_t type = t.Type(i);
        if (type == Ty(T::RightParenthesis)) ++depth;
        else if (type == Ty(T::LeftParenthesis) && depth > 0) --depth;
        else if (depth == 0 && type == Ty(T::When)) {
            if (!t.Is(i + 1, "MATCHED") && !(t.Type(i + 1) == Ty(T::Not) && t.Is(i + 2, "MATCHED"))) return {};
            const bool notMatched = t.Type(i + 1) == Ty(T::Not);
            const bool bySource = notMatched && t.Type(i + 3) == Ty(T::By) && t.Is(i + 4, "SOURCE");
            if (notMatched && !bySource) return {"UPDATE", "DELETE"};
            return {"INSERT"};
        } else if (depth == 0 && (type == Ty(T::Semicolon) || type == Ty(T::Go))) {
            return {};
        }
    }
    return {};
}

/// Values of SET options the parser takes as any identifier (SQL Server documentation of SET
/// DEADLOCK_PRIORITY and SET DATEFORMAT).
const std::vector<std::string>* SetCommandValues(std::string_view command) {
    static const std::vector<std::string> deadlock = {"HIGH", "LOW", "NORMAL"};
    static const std::vector<std::string> dateformat = {"DMY", "DYM", "MDY", "MYD", "YDM", "YMD"};
    if (EqualsI(command, "DEADLOCK_PRIORITY")) return &deadlock;
    if (EqualsI(command, "DATEFORMAT")) return &dateformat;
    return nullptr;
}

}  // namespace

// ============================================================================================= API

CompletionResult Complete(std::string_view sql, size_t caret, SqlVersion version, const Catalog& catalog) {
    const Grammar& g = GrammarFor(version);
    CompletionResult result;
    caret = std::min(caret, sql.size());
    result.replaceStart = caret;

    std::vector<LexToken> all;
    g.Lex(sql, all, caret);
    const CaretWord word = FindCaretWord(sql, caret, all);
    if (word.none) return result;
    result.replaceStart = word.start;
    result.replaceLength = word.end - word.start;
    if (word.quoted) {
        // an unterminated [name at the caret swallowed the text after it: lex that text again
        auto keep = std::find_if(all.begin(), all.end(), [&](const LexToken& t) { return t.end > word.start; });
        if (keep != all.end() && keep->end > caret) {
            all.erase(keep, all.end());
            std::vector<LexToken> rest;
            g.Lex(sql.substr(caret), rest, 0);
            for (LexToken t : rest) {
                t.start += static_cast<uint32_t>(caret);
                t.end += static_cast<uint32_t>(caret);
                all.push_back(t);
            }
        }
    }

    const ScriptTokens tokens(sql, all);
    const size_t caretTok = tokens.IndexAt(word.start);
    const size_t batchTok = tokens.BatchStart(caretTok);

    // Parse from the start of the statement before the caret (found from the tokens; the parse then
    // also offers what continues that statement; after a ';' nothing does). When the parse fails
    // right at that start, the tokens misjudged where the statement starts: parse the whole batch.
    size_t parseTok = batchTok;
    if (caretTok > batchTok)
        parseTok = tokens.Type(caretTok - 1) == Ty(T::Semicolon) ? caretTok
                                                                  : StatementStartFromTokens(tokens, caretTok - 1);
    if (parseTok < batchTok || parseTok > caretTok) parseTok = batchTok;
    auto parseFrom = [&](size_t tok) {
        const size_t from = tok < caretTok ? tokens.At(tok).start : word.start;
        return g.ParseToCaret(sql.substr(from, word.start - from));
    };
    CaretParse ps = parseFrom(parseTok);
    if (parseTok != batchTok && (!ps.capture.captured || (ps.syntaxErrors && ps.firstError < 3))) {
        parseTok = batchTok;
        ps = parseFrom(parseTok);
    }

    Contexts ctx, functionArgs;
    const WalkResult walk = WalkCaret(g, ps, ctx, functionArgs);
    const bool aligned = caretTok - parseTok == ps.tokens.size();
    if (!ctx.Any() && !functionArgs.Any()) FallbackContexts(tokens, caretTok, ctx);
    const size_t statementStart =
        aligned && walk.statementIndex != SIZE_MAX ? parseTok + walk.statementIndex : SIZE_MAX;
    const ScopeAnalyzer scope(tokens, catalog, caretTok, statementStart);
    const std::vector<std::string> qualifier = Qualifier(tokens, caretTok);
    if (functionArgs.Any()) {
        SourceInfo target;
        const bool notFunction = scope.Target(target) && target.kind != SourceInfo::Kind::Unknown &&
                                 target.kind != SourceInfo::Kind::TableFunction;
        if (!notFunction) ctx.Merge(functionArgs);
    }

    ItemList items(word);
    ItemBuilder build(catalog, version, scope, items);
    using OT = CatalogObject::Type;

    if (ctx.expression || ctx.targetColumns) {
        const std::vector<SourceInfo> sources = scope.ExpressionSources();
        if (ctx.expression) build.ExpressionNames(qualifier, false, sources);
        if (ctx.targetColumns) build.ExpressionNames(qualifier, qualifier.empty(), sources);
        if (ctx.orderBy && qualifier.empty()) build.Columns(scope.SelectListNames(), "select list");
    }
    if (qualifier.empty()) {
        if (ctx.scalarVariables || ctx.tableVariables)
            build.Variables(ctx.tableVariables, ctx.scalarVariables,
                            ctx.scalarVariables && !word.typed.empty() && word.typed[0] == '@');
        if (ctx.expression || ctx.builtinFunctions) build.Functions();
        if (ctx.dataType) {
            for (const std::string& w : ctx.typeNames) {
                if (IsReservedKeyword(w)) continue;
                std::string lower = w;
                for (char& c : lower)
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                items.Add(CompletionKind::DataType, lower, lower, "data type");
            }
        }
    }
    if (ctx.tableSource) {
        build.ObjectNames(qualifier, {OT::Table, OT::View, OT::TableFunction}, true);
        if (qualifier.empty()) build.TableFunctions();
    }
    if (ctx.dmlTarget) {
        build.ObjectNames(qualifier, {OT::Table, OT::View}, true);
        if (qualifier.empty()) build.Sources(scope.TargetFromSources(), false);
    }
    if (ctx.objectTarget) build.ObjectNames(qualifier, {OT::Table, OT::View}, true);
    if (ctx.procedure) build.ObjectNames(qualifier, {OT::Procedure}, false);
    if (ctx.setCommandValue && qualifier.empty() && caretTok > 0)
        if (const auto* values = SetCommandValues(tokens.Text(caretTok - 1)))
            for (const std::string& v : *values) items.Add(CompletionKind::Keyword, v, v);
    if (qualifier.empty()) {
        const std::vector<std::string> excluded = ExcludedMergeActions(tokens, caretTok);
        for (const auto& [keyword, kind] : ctx.keywords)
            if (std::find(excluded.begin(), excluded.end(), keyword) == excluded.end()) items.Add(kind, keyword, keyword);
    }

    result.items = items.Take();
    return result;
}

}  // namespace tsql::editor
