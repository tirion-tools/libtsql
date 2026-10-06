// Completion (tsql::editor::Complete, Document::Complete): the caret's word and replace range, the
// parser configuration at the caret (Grammar::ParseToCaret from where the script's parse starts the
// statement before it), the tokens that can follow it (Walk), what kind of name each candidate
// Identifier is (from the rules it is matched in), the names in scope (ScopeAnalyzer) and the
// catalog, ranked.
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>

#include "Builtins.h"
#include "Editor.h"
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

enum Tag : uint64_t {
    kWrapper = 1ull << 0,          // identifier, nonQuotedIdentifier, identifierList, identifierListElement
    kObjectName = 1ull << 1,       // schemaObject...PartName
    kTableReference = 1ull << 2,   // schemaObjectOrFunctionTableReference
    kDmlTarget = 1ull << 3,        // dmlTarget and its schemaObject... alternatives
    kProcedureRef = 1ull << 4,     // procedureReference, procObjectReference
    kObjectTarget = 1ull << 5,     // CREATE INDEX / STATISTICS ... ON, UPDATE STATISTICS, TRUNCATE, ALTER TABLE ...
    kDataType = 1ull << 6,         // scalarDataType, dataTypeSchemaObjectName
    kMultiPart = 1ull << 7,        // multiPartIdentifier
    kColumnExpression = 1ull << 8, // columnOrFunctionCall, selectStarExpression
    kTargetColumn = 1ull << 9,     // insertColumn, mergeInsertDmlColumn, setClause, identifierColumnReferenceExpression
    kBuiltinCall = 1ull << 10,     // builtInFunctionCall
    kGlobalTvf = 1ull << 11,       // globalFunctionTableReference
    kTableHint = 1ull << 12,
    kQueryHint = 1ull << 13,
    kExpressionPrimary = 1ull << 14,
    kOrderBy = 1ull << 15,
    kTableVariableRef = 1ull << 16,   // variableTableReference, variableDmlTarget
    kVariableDefinition = 1ull << 17, // DECLARE @x, parameters
    kNoNames = 1ull << 18,            // variableMethodCallTableReference, ...
    kFunctionTarget = 1ull << 19,     // schemaObjectFunctionDmlTarget: INSERT INTO f(arguments)
    kSetCommand = 1ull << 20,         // setCommand: SET DEADLOCK_PRIORITY value, ...
    kTableTypeAllowed = 1ull << 21,   // a variable's or parameter's type: a table type is allowed too
    kVariableRule = 1ull << 22,       // variable
    kSetParam = 1ull << 23,           // setParam: an EXEC argument ([@name =] value)
    kEndConversation = 1ull << 24,    // endConversationStatement: END is END CONVERSATION
    kQuerySpecification = 1ull << 25, // querySpecification (its ON <filegroup> needs INTO)
    kColumnRef = 1ull << 26,          // column: a column of the sources (FOR UPDATE OF, CONTAINS (...))
    kDropObject = 1ull << 27,         // dropObject, dropObjectList: the names of a DROP statement
    kDropTable = 1ull << 28,          // dropTableStatement, ... : what that DROP drops
    kDropView = 1ull << 29,
    kDropProcedure = 1ull << 30,
    kDropFunction = 1ull << 31,
    kDropSynonym = 1ull << 32,
    kComputedColumn = 1ull << 33,     // computedColumnBody: a column's AS expression
    kPeriodDefinition = 1ull << 34,   // tablePeriodDefinition: PERIOD FOR SYSTEM_TIME (start, end)
};

/// How the walk reached a keyword (Contexts::keywordPaths).
enum KeywordPath : uint8_t {
    kInStatement = 1,      // inside the statement the caret is in
    kAfterStatement = 2,   // as the start of a statement after it
    kColumnStart = 4,      // a column definition's data type or AS
    kPeriod = 8,           // PERIOD FOR SYSTEM_TIME (a column named PERIOD would need a data type)
};

bool Contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }
bool StartsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

uint64_t TagsOfRule(std::string_view n) {
    uint64_t tags = 0;
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
    if (n == "declareVariableElement" || n == "scalarProcedureParameter") tags |= kTableTypeAllowed;
    if (n == "variable") tags |= kVariableRule;
    if (n == "setParam") tags |= kSetParam;
    if (n == "endConversationStatement") tags |= kEndConversation;
    if (n == "querySpecification") tags |= kQuerySpecification;
    if (n == "column") tags |= kColumnRef;
    if (n == "dropObject" || n == "dropObjectList") tags |= kDropObject;
    if (n == "dropTableStatement") tags |= kDropTable;
    if (n == "dropViewStatement") tags |= kDropView;
    if (n == "dropProcedureStatement") tags |= kDropProcedure;
    if (n == "dropFunctionStatement") tags |= kDropFunction;
    if (n == "dropSynonymStatement") tags |= kDropSynonym;
    if (n == "computedColumnBody") tags |= kComputedColumn;
    if (n == "tablePeriodDefinition") tags |= kPeriodDefinition;
    return tags;
}

const std::vector<uint64_t>& RuleTags(const Grammar& g) {
    static std::mutex mutex;
    static std::unordered_map<const Grammar*, std::vector<uint64_t>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(&g);
    if (it != cache.end()) return it->second;
    std::vector<uint64_t> tags;
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
    bool tableTypes = false;      // with dataType: user-defined table types too
    bool execParameters = false;  // EXEC procedure ... | where a parameter name can follow
    bool sourceColumns = false;   // columns of the query's sources only (FOR UPDATE OF ...)
    bool selectIntoOn = false;    // SELECT ... INTO t ON <filegroup>: ON only right after INTO t
    uint32_t dropTypes = 0;       // DROP <kind> names: bits 1 << CatalogObject::Type
    std::set<std::string> typeNames;
    std::vector<std::pair<std::string, CompletionKind>> keywords;
    std::set<std::string> keywordSeen;
    std::map<std::string, uint8_t> keywordPaths;   // KeywordPath bits of each keyword
    /// How sure the walk is of each keyword (WalkCandidate::doubtPath, doubtToken over its candidates).
    struct Doubt {
        bool certain = false;            // a candidate without doubt
        bool token = false;              // a candidate whose doubt depends on the keyword itself
        std::vector<uint64_t> paths;     // the path doubts of the others
    };
    std::map<std::string, Doubt> keywordDoubt;
    bool name = false;   // a free name (an Identifier of any text) can stand at the caret (perhaps in doubt)
    /// A keyword in doubt was reached past the end of the statement the walk started in
    /// (WalkCandidate::beyondStatement): its trials need the text from the enclosing statement on.
    bool doubtBeyond = false;

    bool Any() const {
        return expression || builtinFunctions || tableSource || dmlTarget || objectTarget || procedure || dataType ||
               targetColumns || tableVariables || scalarVariables || setCommandValue || execParameters ||
               sourceColumns || selectIntoOn || dropTypes != 0 || !keywords.empty();
    }
    void AddKeyword(const std::string& word, CompletionKind kind, uint8_t paths = kInStatement, uint64_t doubtPath = 0,
                    bool doubtToken = false) {
        keywordPaths[word] |= paths;
        Doubt& d = keywordDoubt[word];
        if (doubtToken) d.token = true;
        else if (doubtPath == 0) d.certain = true;
        else if (std::find(d.paths.begin(), d.paths.end(), doubtPath) == d.paths.end()) d.paths.push_back(doubtPath);
        if (keywordSeen.insert(word + '\x01' + std::to_string(static_cast<int>(kind))).second)
            keywords.emplace_back(word, kind);
    }
    /// Whether a keyword was reached by paths `only` and no other.
    bool AnyKeywordOnly(uint8_t only) const {
        for (const auto& [word, paths] : keywordPaths)
            if ((paths & ~only) == 0) return true;
        return false;
    }
    /// Drops the keywords `drop` says to (by their paths).
    template <class Drop>
    void DropKeywords(const Drop& drop) {
        keywords.erase(std::remove_if(keywords.begin(), keywords.end(),
                                      [&](const auto& k) { return drop(keywordPaths[k.first]); }),
                       keywords.end());
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
        tableTypes |= o.tableTypes;
        execParameters |= o.execParameters;
        sourceColumns |= o.sourceColumns;
        selectIntoOn |= o.selectIntoOn;
        dropTypes |= o.dropTypes;
        name |= o.name;
        doubtBeyond |= o.doubtBeyond;
        typeNames.insert(o.typeNames.begin(), o.typeNames.end());
        for (const auto& [word, kind] : o.keywords)
            if (keywordSeen.insert(word + '\x01' + std::to_string(static_cast<int>(kind))).second)
                keywords.emplace_back(word, kind);
        for (const auto& [word, paths] : o.keywordPaths) keywordPaths[word] |= paths;
        for (const auto& [word, d] : o.keywordDoubt) {
            Doubt& mine = keywordDoubt[word];
            mine.certain = mine.certain || d.certain;
            mine.token = mine.token || d.token;
            for (uint64_t p : d.paths)
                if (std::find(mine.paths.begin(), mine.paths.end(), p) == mine.paths.end()) mine.paths.push_back(p);
        }
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
        auto tag = [&](size_t k) { return k < rules.size() ? tags_[rules[k]] : uint64_t{0}; };
        auto within = [&](size_t n, uint64_t mask) {
            for (size_t k = 0; k < n && k < rules.size(); ++k)
                if (tags_[rules[k]] & mask) return true;
            return false;
        };
        Contexts& ctx = InFunctionArguments(rules) ? functionArgs_ : main_;
        candidate_ = &c;
        const uint8_t paths = static_cast<uint8_t>((c.nextStatement ? kAfterStatement : kInStatement) |
                                                   (within(rules.size(), kDataType | kComputedColumn) ? kColumnStart : 0) |
                                                   (within(rules.size(), kPeriodDefinition) ? kPeriod : 0));
        const auto type = static_cast<uint32_t>(c.tokenType);
        if (type == Ty(T::Identifier) || type == Ty(T::QuotedIdentifier)) {
            if (c.words != nullptr) {
                for (const std::string& w : *c.words) Keyword(ctx, w, within, paths);
                return;
            }
            if (type == Ty(T::Identifier)) ctx.name = true;
            size_t k = 0;
            while (tag(k) & kWrapper) ++k;
            const uint64_t first = tag(k);
            if (first & kNoNames) return;
            if (first & kObjectName) {
                while (tag(k) & kObjectName) ++k;
                const uint64_t next = tag(k);
                if (next & kTableReference) ctx.tableSource = true;
                else if (next & kDmlTarget) ctx.dmlTarget = true;
                else if (next & kProcedureRef) ctx.procedure = true;
                else if (next & kObjectTarget) ctx.objectTarget = true;
                else if (next & kDropObject) ctx.dropTypes |= DropTypes(rules, k);
            } else if (first & kDataType) {
                ctx.dataType = true;
                if (c.hints != nullptr) ctx.typeNames.insert(c.hints->begin(), c.hints->end());
                while (tag(k) & kDataType) ++k;
                if (tag(k) & kTableTypeAllowed) ctx.tableTypes = true;
            } else if (first & kMultiPart) {
                const uint64_t next = tag(k + 1);
                if (next & kColumnExpression) {
                    ctx.expression = true;
                    if (within(rules.size(), kOrderBy)) ctx.orderBy = true;
                } else if (next & kTargetColumn) {
                    ctx.targetColumns = true;
                } else if (next & kColumnRef) {
                    ctx.sourceColumns = true;
                }
            } else if (first & kBuiltinCall) {
                ctx.builtinFunctions = true;
            } else if (first & kPeriodDefinition) {
                ctx.targetColumns = true;   // PERIOD FOR SYSTEM_TIME (start column, end column)
            } else if (within(3, kSetCommand)) {
                ctx.setCommandValue = true;
            }
            return;
        }
        if (type == Ty(T::Variable)) {
            if ((tag(0) & kVariableRule) && (tag(1) & kSetParam)) ctx.execParameters = true;   // @name = value
            else if (within(4, kTableVariableRef)) ctx.tableVariables = true;
            else if (!within(4, kVariableDefinition | kNoNames)) ctx.scalarVariables = true;
            return;
        }
        if (const char* text = KeywordText(type)) {
            std::string word = Upper(text);
            // END at a statement start only begins END CONVERSATION (END of a block comes from the block)
            if (word == "END" && (tag(0) & kEndConversation)) word = "END CONVERSATION";
            // querySpecification's ON <filegroup> is rejected by its action unless INTO precedes it
            if (word == "ON" && (tag(0) & kQuerySpecification)) {
                ctx.selectIntoOn = true;
                return;
            }
            Keyword(ctx, word, within, paths);
        }
    }

private:
    /// Inside schemaObjectFunctionDmlTarget other than its name.
    bool InFunctionArguments(const std::vector<size_t>& rules) const {
        for (size_t k = 0; k < rules.size(); ++k)
            if (tags_[rules[k]] & kFunctionTarget) return k == 0 || !(tags_[rules[k - 1]] & kObjectName);
        return false;
    }

    /// What the DROP statement around the dropObject at rules[k] drops.
    uint32_t DropTypes(const std::vector<size_t>& rules, size_t k) const {
        auto bit = [](CatalogObject::Type t) { return 1u << static_cast<unsigned>(t); };
        for (; k < rules.size(); ++k) {
            const uint64_t t = tags_[rules[k]];
            if (t & kDropTable) return bit(CatalogObject::Type::Table);
            if (t & kDropView) return bit(CatalogObject::Type::View);
            if (t & kDropProcedure) return bit(CatalogObject::Type::Procedure);
            if (t & kDropFunction) return bit(CatalogObject::Type::ScalarFunction) | bit(CatalogObject::Type::TableFunction);
            if (t & kDropSynonym) return bit(CatalogObject::Type::Synonym);
        }
        return 0;
    }

    template <class Within>
    void Keyword(Contexts& ctx, const std::string& word, const Within& within, uint8_t paths) {
        CompletionKind kind = CompletionKind::Keyword;
        if (within(4, kTableHint)) kind = CompletionKind::TableHint;
        else if (within(3, kQueryHint)) kind = CompletionKind::QueryHint;
        else if (within(3, kExpressionPrimary) && FindBuiltin(word) != nullptr) kind = CompletionKind::BuiltinFunction;
        ctx.AddKeyword(word, kind, paths, candidate_->doubtPath, candidate_->doubtToken);
        if (candidate_->beyondStatement && (candidate_->doubtPath != 0 || candidate_->doubtToken)) ctx.doubtBeyond = true;
    }

    const std::vector<uint64_t>& tags_;
    const WalkCandidate* candidate_ = nullptr;   // the candidate being added
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
        case CompletionKind::Parameter: return 0;
        case CompletionKind::Column: return 1;
        case CompletionKind::Alias: return 2;
        case CompletionKind::Cte: return 3;
        case CompletionKind::TempTable: return 4;
        case CompletionKind::TableVariable: return 5;
        case CompletionKind::Table: return 6;
        case CompletionKind::View: return 7;
        case CompletionKind::TableFunction: return 8;
        case CompletionKind::Synonym: return 9;
        case CompletionKind::Procedure: return 10;
        case CompletionKind::Variable: return 11;
        case CompletionKind::ScalarFunction: return 12;
        case CompletionKind::DataType: return 13;
        case CompletionKind::UserType: return 14;
        case CompletionKind::TableHint: return 15;
        case CompletionKind::QueryHint: return 16;
        case CompletionKind::BuiltinFunction: return 17;
        case CompletionKind::Schema: return 18;
        case CompletionKind::Database: return 19;
        case CompletionKind::Keyword: return 20;
    }
    return 21;
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
            if (a.item.kind == CompletionKind::Column || a.item.kind == CompletionKind::Parameter)
                return a.order < b.order;   // definition order
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
        case CatalogObject::Type::Synonym: return CompletionKind::Synonym;
    }
    return CompletionKind::Table;
}

/// An EXEC argument's parameter: `<type>[ OUTPUT][ = default]`.
std::string ParameterDetail(const CatalogParameter& p) {
    std::string d = p.type;
    if (p.output) d += " OUTPUT";
    if (p.hasDefault) d += " = default";
    return d;
}

/// `kind db.schema`; procedures and functions add their signature: `name(@p type, ...)`.
std::string ObjectDetail(const CatalogObject& o) {
    const std::string where = o.database.empty() ? o.schema : o.database + "." + o.schema;
    auto signature = [&]() {
        std::string s = " " + QuoteName(o.name) + "(";
        for (size_t i = 0; i < o.parameters.size(); ++i) {
            if (i > 0) s += ", ";
            s += o.parameters[i].name + " " + ParameterDetail(o.parameters[i]);
        }
        return s + ")";
    };
    switch (o.type) {
        case CatalogObject::Type::Table: return "table " + where;
        case CatalogObject::Type::View: return "view " + where;
        case CatalogObject::Type::ScalarFunction: return "scalar function " + where + signature();
        case CatalogObject::Type::TableFunction: return "table-valued function " + where + signature();
        case CatalogObject::Type::Procedure: return "procedure " + where + signature();
        case CatalogObject::Type::Synonym: return "synonym " + where + " for " + o.target;
    }
    return where;
}

/// Which synonyms an object list includes: those whose target is of the listed types, and with
/// `Unresolved` also those whose target the catalog does not have.
enum class Synonyms { None, Resolved, Unresolved };

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

    /// Catalog objects of `types` in database/schema (system objects are in every database). Listing
    /// Synonym in `types` lists every synonym; otherwise `synonyms` selects them by their targets.
    void Objects(const std::string& db, const std::string& schema, const std::vector<CatalogObject::Type>& types,
                 Synonyms synonyms) {
        auto listed = [&](CatalogObject::Type t) { return std::find(types.begin(), types.end(), t) != types.end(); };
        for (const CatalogObject& o : catalog_.objects) {
            if (!InDatabase(o.database, db) || !EqualsI(o.schema, schema)) continue;
            if (o.type == CatalogObject::Type::Synonym && !listed(o.type)) {
                if (synonyms == Synonyms::None) continue;
                const CatalogObject* target = SynonymTarget(catalog_, o);
                if (target != nullptr ? !listed(target->type) : synonyms != Synonyms::Unresolved) continue;
            } else if (!listed(o.type)) {
                continue;
            }
            items_.Name(KindOfObject(o.type), o.name, ObjectDetail(o));
        }
    }

    /// User-defined types named by `parts` (the qualifier) in a data type; table types with `tables`.
    void UserTypes(const std::vector<std::string>& parts, bool tables) {
        if (parts.size() > 2) return;
        const std::string& db = parts.size() == 2 && !parts[0].empty() ? parts[0] : scope_.CurrentDatabase();
        std::vector<std::string> schemas;
        if (!parts.empty()) {
            schemas.push_back(parts.back().empty() ? catalog_.defaultSchema : parts.back());
        } else {
            schemas = {catalog_.defaultSchema, "dbo", "sys"};   // where unqualified type names resolve
        }
        for (const CatalogType& ty : catalog_.types) {
            if (!InDatabase(ty.database, db) || (ty.isTableType && !tables)) continue;
            if (std::none_of(schemas.begin(), schemas.end(), [&](const std::string& s) { return EqualsI(s, ty.schema); }))
                continue;
            const std::string where = ty.database.empty() ? ty.schema : ty.database + "." + ty.schema;
            items_.Name(CompletionKind::UserType, ty.name, (ty.isTableType ? "table type " : "user type ") + where);
        }
    }

    /// Parameters of the procedure or function `parts` names that an EXEC argument can still name:
    /// those after the first `positional` and not in `named`.
    void Parameters(const std::vector<std::string>& parts, size_t positional, const std::vector<std::string>& named) {
        const CatalogObject* o = scope_.FindObject(parts);
        if (o != nullptr && o->type == CatalogObject::Type::Synonym) o = SynonymTarget(catalog_, *o);
        if (o == nullptr) return;
        for (size_t i = positional; i < o->parameters.size(); ++i) {
            const CatalogParameter& p = o->parameters[i];
            if (std::any_of(named.begin(), named.end(), [&](const std::string& n) { return EqualsI(n, p.name); }))
                continue;
            items_.Add(CompletionKind::Parameter, p.name, p.name + " = ", ParameterDetail(p));
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
            if (InDatabase(o.database, db))
                items_.Name(CompletionKind::Schema, o.schema, o.database.empty() ? "system schema" : "schema in " + o.database);
    }

    void Databases() {
        for (const std::string& d : catalog_.databases) items_.Name(CompletionKind::Database, d, "database");
    }

    /// FROM / target / EXEC names: `parts` is the qualifier typed before the caret word.
    void ObjectNames(const std::vector<std::string>& parts, const std::vector<CatalogObject::Type>& types,
                     Synonyms synonyms, bool scriptObjects) {
        const std::string& db = scope_.CurrentDatabase();
        if (parts.empty()) {
            Objects(db, catalog_.defaultSchema, types, synonyms);
            if (scriptObjects) {
                for (const CteInfo& c : scope_.Ctes()) items_.Name(CompletionKind::Cte, c.name, "common table expression");
                for (const TempTableInfo& t : scope_.TempTables()) items_.Name(CompletionKind::TempTable, t.name, "temp table");
            }
            Schemas(db);
            Databases();
            return;
        }
        if (parts.size() == 1) {
            Objects(db, parts[0], types, synonyms);
            if (IsDatabase(parts[0])) Schemas(parts[0]);
            return;
        }
        // db.schema. / db.. / server.db.schema.
        const std::string& d = parts[parts.size() - 2];
        Objects(d.empty() ? db : d, parts.back().empty() ? catalog_.defaultSchema : parts.back(), types, synonyms);
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
                Objects(scope_.CurrentDatabase(), parts[0], {CatalogObject::Type::ScalarFunction}, Synonyms::Resolved);
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
                    parts[1].empty() ? catalog_.defaultSchema : parts[1], {CatalogObject::Type::ScalarFunction},
                    Synonyms::Resolved);
    }

    void Variables(bool tables, bool scalars, bool globals) {
        for (const VariableInfo& v : scope_.Variables()) {
            if (v.isTable && tables)
                items_.Add(CompletionKind::TableVariable, v.name, v.name, v.tableType != nullptr ? v.type : "table variable");
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
    bool atCaret = false;               // the parser stood at the caret (the walk crossed no tokens)
    bool emitted = false;               // the walk found candidates (classified or not)
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

WalkResult WalkCaret(const Grammar& g, CaretSession& session, Contexts& ctx, Contexts& functionArgs) {
    WalkResult r;
    const CaretParse& ps = session.result;
    const Capture& cap = ps.capture;
    if (!cap.captured) return r;
    CandidateClassifier classifier(g, ctx, functionArgs);
    // `locals`: the capture the walk starts at (its rule locals), null for a statement-start walk
    auto run = [&](int state, size_t index, const std::vector<int>& follow, size_t budget, const Capture* locals,
                   int pending = -1) {
        WalkInput in;
        in.tokens = &ps.tokens;
        in.upper = &ps.upper;
        in.startState = state;
        in.startIndex = index;
        in.outerFollow = follow;
        in.startPending = pending;
        in.caret = ps.tokens.size();
        in.budget = budget;
        in.evaluate = [&session, locals](size_t rule, size_t pred, size_t at, bool live, const ProbeToken* probe) {
            return session.EvaluatePredicate(rule, pred, at, live ? locals : nullptr, probe);
        };
        in.simulator = &session.Simulator();
        const bool any = Walk(g, in, [&](const WalkCandidate& c) { classifier.Add(c); }).emitted > 0;
        r.emitted = r.emitted || any;
        return any;
    };
    // a decision whose opaque predicate looked at the caret: its alternatives as well (the parse
    // went on after it, so its context's locals are not those of the decision any more)
    if (ps.early.captured && !ps.early.errorInStatement)
        run(ps.early.state, ps.early.index, ps.early.follow, 200'000, nullptr, ps.early.pendingCall);
    if (!cap.errorInStatement) {
        run(cap.state, cap.index, cap.follow, 400'000, &cap, cap.pendingCall);
        r.statementIndex = cap.statementState >= 0 ? cap.statementIndex : SIZE_MAX;
        r.atCaret = cap.index == ps.tokens.size();
        return r;
    }
    // a syntax error earlier in the caret's statement: walk the statement from its start, else from
    // the first later points where a statement can start
    if (run(cap.statementState, cap.statementIndex, cap.statementFollow, 200'000, nullptr)) {
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
        if (run(cap.statementState, p, cap.statementFollow, 50'000, nullptr)) {
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

/// The caret right after the name of a column being defined, with no data type yet (a table's
/// column list: TABLE name ( ..., or ALTER TABLE ... ADD): only a data type or AS (a computed
/// column) follows; anything else fails VerifyColumnDataType unless the column is named timestamp.
/// (PERIOD there may also begin PERIOD FOR SYSTEM_TIME: kPeriod keywords stay.)
bool ColumnWithoutType(const ScriptTokens& t, size_t caretTok) {
    if (caretTok < 2 || !t.IsName(caretTok - 1)) return false;
    if (EqualsI(t.Name(caretTok - 1), "TIMESTAMP")) return false;
    const size_t k = caretTok - 2;
    if (t.Is(k, "ADD")) return true;
    if (t.Type(k) != Ty(T::LeftParenthesis) && t.Type(k) != Ty(T::Comma)) return false;
    // the parenthesis the column is in, or the ADD of ALTER TABLE ... ADD c1 int, c2
    size_t depth = 0;
    for (size_t i = k + 1; i-- > 0;) {
        const uint32_t type = t.Type(i);
        if (type == Ty(T::RightParenthesis)) {
            ++depth;
        } else if (type == Ty(T::LeftParenthesis)) {
            if (depth-- > 0) continue;
            // TABLE ( (a table variable or type, RETURNS @t TABLE), TABLE name (
            size_t j = i;
            while (j > 0 && (t.IsName(j - 1) || t.Type(j - 1) == Ty(T::Dot))) --j;
            return j > 0 && t.Type(j - 1) == Ty(T::Table);
        } else if (depth == 0 && (type == Ty(T::Semicolon) || type == Ty(T::Go))) {
            return false;
        } else if (depth == 0 && t.Is(i, "ADD")) {
            return true;
        }
    }
    return false;
}

/// A trial parse of `text` (Grammar::TryParse).
Trial TryParse(const Grammar& g, const std::string& text) {
    Buffer scratch(g);
    scratch.SetText(text);
    scratch.Tokens();
    return g.TryParse(scratch.View());
}

/// Whether a trial parse reached the end of its text.
bool Parsed(Trial t) { return t == Trial::Parses || t == Trial::Prefix; }

/// Whether the statement before byte `end` can end there: the text from `start` (where the caret's
/// statement starts) to `end`, followed by a statement, parses. (The grammar lets many statements
/// end where an action then requires more, e.g. ALTER MESSAGE TYPE ... VALIDATION = VALID_XML
/// needs WITH SCHEMA COLLECTION.)
bool StatementCanEnd(const Grammar& g, std::string_view sql, size_t start, size_t end) {
    return Parsed(TryParse(g, std::string(sql.substr(start, end - start)) + "\nDECLARE "));
}

/// Trial parses of a text followed by different words: the text is lexed once, each word's text
/// from the token it touches on (Buffer::Edit).
class Trials {
public:
    Trials(const Grammar& g, std::string_view text) : g_(g), buffer_(g), base_(text.size()) {
        buffer_.SetText(text);
        buffer_.Tokens();
    }
    /// The text followed by `tail`.
    Trial With(std::string_view tail) {
        buffer_.Edit(base_, buffer_.Text().size() - base_, tail);
        buffer_.Tokens();
        return g_.TryParse(buffer_.View());
    }

private:
    const Grammar& g_;
    Buffer buffer_;
    size_t base_;
};

/// The keywords at the caret that the walk is unsure of (Contexts::Doubt) and that trial parses
/// reject: the caret's statement text from `start` to `end` (`tokens` parser tokens), which parses
/// on its own, followed by the keyword does not parse.
///  - A path doubt (an action that can reject the input on the way to the keyword) is the same for
///    every keyword behind it: a trial with one of them that the parser takes settles it (one only
///    that doubt reaches where there is one; a few tries). A keyword is rejected when all its path
///    doubts failed, unless it may also stand there as a name (then it takes a trial of its own).
///  - A doubt about the keyword itself (an action right after it) takes a trial of each such
///    keyword, when there are at most kKeywordTrials of them (when there are more, the action does
///    not tell words apart: it counts options or checks the statement).
/// The trials parse at most kTrialBudget tokens in all; what they leave undecided stays.
std::vector<std::string> DoubtfulKeywordsRejected(const Grammar& g, std::string_view sql, size_t start, size_t end,
                                                  size_t tokens, const Contexts& ctx) {
    constexpr size_t kTrialBudget = 1024, kKeywordTrials = 16, kTriesPerPath = 3;
    std::vector<std::string> rejected;
    const std::string_view text = sql.substr(start, end - start);
    size_t spent = 0;
    std::optional<Trials> trials;   // made by the first trial
    bool textParses = false;
    std::map<std::string, Trial> result;   // keyword -> its trial
    // nullopt: not run (over budget; the text alone does not parse: a statement cut out of a block, say)
    auto trial = [&](const std::string& word) -> std::optional<Trial> {
        auto it = result.find(word);
        if (it != result.end()) return it->second;
        if (!trials) {
            trials.emplace(g, text);
            spent += tokens;
            textParses = Parsed(trials->With(""));
        }
        if (!textParses || (spent += tokens + 1) > kTrialBudget) return std::nullopt;
        const Trial t = trials->With(word + " ");
        result.emplace(word, t);
        return t;
    };
    auto doubt = [&](const std::string& word) -> const Contexts::Doubt& { return ctx.keywordDoubt.at(word); };
    std::map<uint64_t, bool> pathOk;    // settled path doubts
    std::map<uint64_t, size_t> tries;   // trials made to settle each
    auto anyPathOk = [&](const Contexts::Doubt& d) {
        return std::any_of(d.paths.begin(), d.paths.end(), [&](uint64_t p) {
            auto it = pathOk.find(p);
            return it != pathOk.end() && it->second;
        });
    };
    bool over = false;
    for (int round = 0; round < 2 && !over; ++round) {
        // first the keywords with a single path doubt, then those with several
        for (const auto& [word, kind] : ctx.keywords) {
            const Contexts::Doubt& d = doubt(word);
            if (d.certain || d.token || d.paths.empty() || (round == 0) != (d.paths.size() == 1)) continue;
            std::vector<uint64_t> open;
            for (uint64_t p : d.paths)
                if (pathOk.count(p) == 0 && tries[p] < kTriesPerPath) open.push_back(p);
            if (open.empty() || result.count(word) != 0) continue;
            std::optional<Trial> r = trial(word);
            if (!r) {
                over = true;
                break;
            }
            for (uint64_t p : open) ++tries[p];
            // failed: all its paths fail; taken: at least one passes (taken to be every open one)
            if (*r == Trial::Fails)
                for (uint64_t p : d.paths) pathOk.emplace(p, false);
            else if (*r == Trial::Parses)
                for (uint64_t p : open) pathOk.emplace(p, true);
        }
    }
    auto pathRejected = [&](const Contexts::Doubt& d) {
        return !d.token && !d.paths.empty() && std::all_of(d.paths.begin(), d.paths.end(), [&](uint64_t p) {
            auto it = pathOk.find(p);
            return it != pathOk.end() && !it->second;
        });
    };
    // a word the parser may take as a name there (its own trial decides)
    auto nameWord = [&](const std::string& word) { return ctx.name && !IsReservedKeyword(word); };
    // keywords whose own trial decides: doubts about themselves, and names a failed path rejects
    std::vector<std::string> own;
    for (const auto& [word, kind] : ctx.keywords) {
        const Contexts::Doubt& d = doubt(word);
        if (d.certain || result.count(word) != 0 || anyPathOk(d) ||
            std::find(own.begin(), own.end(), word) != own.end())
            continue;
        if (d.token || (pathRejected(d) && nameWord(word))) own.push_back(word);
    }
    if (!over && own.size() <= kKeywordTrials)
        for (const std::string& word : own)
            if (!trial(word)) break;
    for (const auto& [word, kind] : ctx.keywords) {
        const Contexts::Doubt& d = doubt(word);
        if (d.certain || std::find(rejected.begin(), rejected.end(), word) != rejected.end()) continue;
        auto it = result.find(word);
        const bool reject = it != result.end() ? it->second == Trial::Fails
                                               : !anyPathOk(d) && pathRejected(d) && !nameWord(word);
        if (reject) rejected.push_back(word);
    }
    return rejected;
}

/// An EXEC call whose argument list the caret starts an argument of.
struct ExecCall {
    std::vector<std::string> procedure;   // the name as written
    size_t positional = 0;                // arguments before the caret without `@name =`
    std::vector<std::string> named;       // the @names of those with it
    bool afterComma = false;              // not the first argument
};

bool FindExecCall(const ScriptTokens& t, size_t caretTok, ExecCall& out) {
    if (caretTok == 0) return false;
    size_t exec = SIZE_MAX, depth = 0;
    for (size_t i = StatementStartFromTokens(t, caretTok - 1); i < caretTok; ++i) {
        const uint32_t type = t.Type(i);
        if (type == Ty(T::LeftParenthesis)) ++depth;
        else if (type == Ty(T::RightParenthesis) && depth > 0) --depth;
        else if (depth == 0 && OneOf(type, {T::Exec, T::Execute})) exec = i;
    }
    if (exec == SIZE_MAX) return false;
    size_t i = exec + 1;
    if (t.Type(i) == Ty(T::Variable) && t.Type(i + 1) == Ty(T::EqualsSign)) i += 2;   // EXEC @status = name
    out.procedure.clear();
    while (i < caretTok) {
        if (t.IsName(i)) out.procedure.push_back(t.Name(i++));
        else if (t.Type(i) == Ty(T::Dot)) out.procedure.emplace_back();
        else break;
        if (t.Type(i) != Ty(T::Dot)) break;
        ++i;
    }
    if (out.procedure.empty() || i > caretTok || t.Type(i - 1) == Ty(T::Dot)) return false;
    if (t.Type(i) == Ty(T::Semicolon) && t.Type(i + 1) == Ty(T::Integer) && i + 2 <= caretTok) i += 2;   // ;number
    const size_t first = i;
    size_t a = i;
    depth = 0;
    for (size_t k = i; k < caretTok; ++k) {
        const uint32_t type = t.Type(k);
        if (type == Ty(T::LeftParenthesis)) ++depth;
        else if (type == Ty(T::RightParenthesis) && depth > 0) --depth;
        else if (depth == 0 && type == Ty(T::Semicolon)) return false;
        else if (depth == 0 && type == Ty(T::Comma)) {
            if (t.Type(a) == Ty(T::Variable) && t.Type(a + 1) == Ty(T::EqualsSign)) out.named.emplace_back(t.Text(a));
            else ++out.positional;
            a = k + 1;
        }
    }
    out.afterComma = a > first;
    return a == caretTok;
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

namespace detail {

CompletionResult CompleteAt(Buffer& buffer, size_t caret, const Catalog& catalog) {
    const Grammar& g = buffer.grammar();
    const SqlVersion version = g.version;
    const std::string& sql = buffer.Text();
    CompletionResult result;
    caret = std::min(caret, sql.size());
    result.replaceStart = caret;

    // the tokens through the caret's batch (the text after its GO is lexed only if a parse reads it)
    buffer.LexBatchOf(caret);
    const std::vector<LexToken>& all = buffer.Lexed();
    const CaretWord word = FindCaretWord(sql, caret, all);
    if (word.none) return result;
    result.replaceStart = word.start;
    result.replaceLength = word.end - word.start;

    // the parser-visible tokens up to the end of the caret's batch (for the scope analysis); the
    // buffer's grow when the parse below lexes more, so they are read by index until it is done
    const std::vector<LexToken>* visible = &buffer.Visible();
    std::vector<LexToken> relexed;
    if (word.quoted) {
        // an unterminated [name at the caret swallowed the text after it: lex that text again
        auto keep = std::find_if(all.begin(), all.end(), [&](const LexToken& t) { return t.end > word.start; });
        if (keep != all.end() && keep->end > caret) {
            for (auto it = all.begin(); it != keep; ++it)
                if (!IsHiddenType(it->type)) relexed.push_back(*it);
            std::vector<LexToken> rest;
            g.Lex(std::string_view(sql).substr(caret), rest);
            for (LexToken t : rest) {
                if (IsHiddenType(t.type)) continue;
                t.start += static_cast<uint32_t>(caret);
                t.end += static_cast<uint32_t>(caret);
                relexed.push_back(t);
            }
            visible = &relexed;
        }
    }
    const size_t caretTok = static_cast<size_t>(
        std::lower_bound(visible->begin(), visible->end(), word.start,
                         [](const LexToken& t, size_t o) { return t.start < o; }) -
        visible->begin());
    size_t batchEnd = caretTok;
    while (batchEnd < visible->size() &&
           !((*visible)[batchEnd].type == Ty(T::Go) && (*visible)[batchEnd].start >= caret))
        ++batchEnd;
    const size_t tokenCount = std::min(visible->size(), batchEnd + 1);

    // Parse from where the parse of the whole text starts the statement that holds the token before
    // the caret (a statement or batch boundary of it), so that the parse also offers what continues
    // that statement; it reads the tokens up to the caret only.
    const std::vector<uint32_t>& visToTok = buffer.VisibleToToken();
    auto visibleIndexOf = [&](size_t token) {
        return static_cast<size_t>(std::lower_bound(visToTok.begin(), visToTok.end(), token) - visToTok.begin());
    };
    const size_t limit = buffer.TokenAt(word.start);
    // the token before the caret (right after a GO: the caret's own, where its batch starts)
    size_t before = 0;
    ResumePoint from;
    if (caretTok > 0) {
        // (no visible token at or after the caret: everything is lexed)
        const size_t own = caretTok < visToTok.size() ? visToTok[caretTok] : all.size();
        before = (*visible)[caretTok - 1].type == Ty(T::Go) ? own : visToTok[caretTok - 1];
        buffer.EnsureParsed(before, before + 1);
        // a point the text's parse reached only by looking at the caret or past it (a look-ahead
        // decision that read on) may not be one of the text up to the caret
        from = buffer.ResumeAtOrBefore(before, limit);
    }
    const ScriptTokens tokens(sql, visible->data(), tokenCount);
    auto parseFrom = [&](const ResumePoint& at) { return g.ParseToCaret(buffer.View(), at, limit); };
    size_t parseTok = visibleIndexOf(from.token);
    std::unique_ptr<CaretSession> session = parseFrom(from);
    {
        // errors before the caret: past them, start at the statement the tokens suggest
        const CaretParse& ps = session->result;
        if (ps.syntaxErrors && ps.firstError < ps.tokens.size() && caretTok > parseTok) {
            const size_t tail = StatementStartFromTokens(tokens, caretTok - 1, parseTok);
            if (tail > parseTok + ps.firstError && tail < caretTok) {
                ResumePoint at;
                at.kind = ResumePoint::Kind::InBatch;
                at.firstBatch = from.kind == ResumePoint::Kind::ScriptStart ||
                                (from.kind == ResumePoint::Kind::InBatch && from.firstBatch);
                at.quotedIdentifier = from.kind == ResumePoint::Kind::BatchStart || from.quotedIdentifier;
                at.token = visToTok[tail];
                auto retry = parseFrom(at);
                if (retry->result.capture.captured && !(retry->result.syntaxErrors && retry->result.firstError < 3)) {
                    session = std::move(retry);
                    parseTok = tail;
                }
            }
        }
    }
    if (from.kind == ResumePoint::Kind::InBatch && caretTok > 0) {
        // the parse fails right at the statement start: parse the whole batch
        const CaretParse& ps = session->result;
        if (!ps.capture.captured || (ps.syntaxErrors && ps.firstError < 3)) {
            const ResumePoint batch = buffer.BatchResumeAtOrBefore(before);
            auto retry = parseFrom(batch);
            if (retry->result.capture.captured) {
                session = std::move(retry);
                parseTok = visibleIndexOf(batch.token);
            }
        }
    }
    const CaretParse& ps = session->result;

    Contexts ctx, functionArgs;
    const WalkResult walk = WalkCaret(g, *session, ctx, functionArgs);
    const bool aligned = caretTok - parseTok == ps.tokens.size();
    if (!walk.emitted && !ctx.Any() && !functionArgs.Any()) FallbackContexts(tokens, caretTok, ctx);
    if (ColumnWithoutType(tokens, caretTok))
        ctx.DropKeywords([](uint8_t paths) { return !(paths & (kColumnStart | kPeriod)); });
    // trial parses of the caret's statement start where the parser's innermost statement does (inside a
    // block, IF, a module body: the inner statement, which parses on its own)
    const size_t parserStatement =
        aligned && walk.statementIndex != SIZE_MAX ? parseTok + walk.statementIndex : SIZE_MAX;
    const size_t trialTok = parserStatement != SIZE_MAX && parserStatement < caretTok ? parserStatement : parseTok;
    size_t statementStart = parserStatement;
    // the parser stood before the caret, in a compound statement (a look-ahead decision of BEGIN,
    // IF, ... that read up to the caret): the statement in it that holds the caret, from the tokens
    if (statementStart != SIZE_MAX && !walk.atCaret && caretTok > statementStart &&
        OneOf(tokens.Type(statementStart), {T::Begin, T::If, T::While, T::Else, T::Create, T::Alter}))
        statementStart = StatementStartFromTokens(tokens, caretTok - 1, statementStart);
    // statements after the caret's (in its batch or its block): only when that statement can end at the caret
    if (trialTok < caretTok && ctx.AnyKeywordOnly(kAfterStatement | kColumnStart) &&
        !StatementCanEnd(g, sql, tokens.At(trialTok).start, word.start))
        ctx.DropKeywords([](uint8_t paths) { return !(paths & kInStatement); });
    // keywords only paths the walk cannot decide reached (an option given twice, a value an action
    // rejects, ...): a trial parse each, if the text up to the caret parses (after a syntax error
    // every trial would fail); from the statement the parse resumed at when a path in doubt left
    // the inner statement (its text alone does not parse with what follows its end)
    const size_t doubtTok = ctx.doubtBeyond ? parseTok : trialTok;
    if (doubtTok <= caretTok && !ps.syntaxErrors) {
        const std::vector<std::string> rejected = DoubtfulKeywordsRejected(
            g, sql, doubtTok < caretTok ? tokens.At(doubtTok).start : word.start, word.start, caretTok - doubtTok, ctx);
        if (!rejected.empty())
            ctx.keywords.erase(std::remove_if(ctx.keywords.begin(), ctx.keywords.end(),
                                              [&](const auto& k) {
                                                  return std::find(rejected.begin(), rejected.end(), k.first) !=
                                                         rejected.end();
                                              }),
                               ctx.keywords.end());
    }
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

    ExecCall call;
    if (ctx.execParameters && qualifier.empty() && FindExecCall(tokens, caretTok, call)) {
        build.Parameters(call.procedure, call.positional, call.named);
        if (call.afterComma && !call.named.empty()) {
            // after an argument with @name = every argument needs it (SQL46089): no bare values
            ctx.expression = ctx.builtinFunctions = ctx.scalarVariables = false;
            ctx.keywords.clear();
        }
    }
    if (ctx.expression || ctx.targetColumns || ctx.sourceColumns) {
        const std::vector<SourceInfo> sources = scope.ExpressionSources();
        if (ctx.expression || ctx.sourceColumns) build.ExpressionNames(qualifier, false, sources);
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
    if (ctx.dataType) build.UserTypes(qualifier, ctx.tableTypes);
    if (ctx.tableSource) {
        build.ObjectNames(qualifier, {OT::Table, OT::View, OT::TableFunction}, Synonyms::Unresolved, true);
        if (qualifier.empty()) build.TableFunctions();
    }
    if (ctx.dmlTarget) {
        build.ObjectNames(qualifier, {OT::Table, OT::View}, Synonyms::Resolved, true);
        if (qualifier.empty()) build.Sources(scope.TargetFromSources(), false);
    }
    if (ctx.objectTarget) build.ObjectNames(qualifier, {OT::Table, OT::View}, Synonyms::None, true);
    if (ctx.procedure) {
        build.ObjectNames(qualifier, {OT::Procedure}, Synonyms::Unresolved, false);
        // unqualified sp_ and xp_ names resolve to the system procedures as well
        if (qualifier.empty() && (StartsWithI(word.typed, "sp_") || StartsWithI(word.typed, "xp_")))
            build.Objects(scope.CurrentDatabase(), "sys", {OT::Procedure}, Synonyms::None);
    }
    if (ctx.dropTypes != 0) {
        std::vector<OT> types;
        for (OT t : {OT::Table, OT::View, OT::ScalarFunction, OT::TableFunction, OT::Procedure, OT::Synonym})
            if (ctx.dropTypes & (1u << static_cast<unsigned>(t))) types.push_back(t);
        build.ObjectNames(qualifier, types, Synonyms::None, (ctx.dropTypes & (1u << static_cast<unsigned>(OT::Table))) != 0);
    }
    if (ctx.selectIntoOn && qualifier.empty()) {
        // SELECT ... INTO name | ON filegroup
        size_t i = caretTok;
        while (i > 0 && (tokens.IsName(i - 1) || tokens.Type(i - 1) == Ty(T::Dot))) --i;
        if (i < caretTok && i > 0 && tokens.Type(i - 1) == Ty(T::Into)) ctx.AddKeyword("ON", CompletionKind::Keyword);
    }
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

}  // namespace detail

}  // namespace tsql::editor
