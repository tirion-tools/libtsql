#include "Scope.h"

#include <algorithm>
#include <functional>

#include "Names.h"
#include "tsql/ast/generated/token_types.hpp"

namespace tsql::editor::detail {

namespace {

using T = ast::TSqlTokenType;
constexpr uint32_t Ty(T t) { return static_cast<uint32_t>(t); }

bool OneOf(uint32_t type, std::initializer_list<T> types) {
    for (T t : types)
        if (type == Ty(t)) return true;
    return false;
}

}  // namespace

// ============================================================================================ tokens

ScriptTokens::ScriptTokens(std::string_view sql, const std::vector<LexToken>& all) : sql_(sql) {
    own_.reserve(all.size());
    for (const LexToken& t : all)
        if (!IsHiddenType(t.type)) own_.push_back(t);
    toks_ = own_.data();
    n_ = own_.size();
}

std::string_view ScriptTokens::Text(size_t i) const {
    if (i >= n_) return {};
    return sql_.substr(toks_[i].start, toks_[i].end - toks_[i].start);
}

bool ScriptTokens::Is(size_t i, std::string_view upper) const { return i < n_ && EqualsI(Text(i), upper); }

bool ScriptTokens::IsName(size_t i) const { return i < n_ && OneOf(toks_[i].type, {T::Identifier, T::QuotedIdentifier}); }

std::string ScriptTokens::Name(size_t i) const { return Unquote(Text(i)); }

size_t ScriptTokens::IndexAt(size_t offset) const {
    return static_cast<size_t>(
        std::lower_bound(toks_, toks_ + n_, offset, [](const LexToken& t, size_t o) { return t.start < o; }) - toks_);
}

bool ScriptTokens::LineStart(size_t i) const {
    if (i >= n_) return false;
    if (i == 0) return true;
    const size_t from = std::min<size_t>(toks_[i - 1].end, toks_[i].start);
    return sql_.substr(from, toks_[i].start - from).find('\n') != std::string_view::npos;
}

size_t ScriptTokens::BatchStart(size_t i) const {
    for (size_t k = std::min(i, n_); k > 0; --k)
        if (toks_[k - 1].type == Ty(T::Go)) return k;
    return 0;
}

// ======================================================================================= statements

namespace {

/// Splits a batch into statements (paren depth 0 of the batch; CASE ... END tracked) the way a
/// reader would: a ';', GO, or a keyword that cannot continue the current statement starts the next.
class StatementScanner {
public:
    explicit StatementScanner(const ScriptTokens& t) : t_(t) {}

    /// Calls onStart(i) for every statement start in [begin, end); stops early when it returns false.
    void Run(size_t begin, size_t end, const std::function<bool(size_t)>& onStart) {
        size_t depth = 0, caseDepth = 0;
        bool inStatement = false;
        for (size_t i = begin; i < end && i < t_.size(); ++i) {
            const uint32_t type = t_.Type(i);
            if (type == Ty(T::Go)) {
                inStatement = false;
                depth = caseDepth = 0;
                continue;
            }
            const bool closesCase = type == Ty(T::End) && caseDepth > 0;
            if (depth == 0) {
                if (type == Ty(T::Semicolon)) {
                    inStatement = false;
                    continue;
                }
                if (!inStatement || (!closesCase && Starts(i))) {
                    if (!onStart(i)) return;
                    inStatement = true;
                    head_ = i;
                    main_ = 0;
                    insertSource_ = false;
                    principals_ = false;
                    caseDepth = 0;
                }
                if (main_ == 0 && OneOf(type, {T::Select, T::Insert, T::Update, T::Delete, T::Merge})) main_ = type;
                else if (main_ == Ty(T::Insert) && OneOf(type, {T::Select, T::Values, T::Exec, T::Execute}))
                    insertSource_ = true;
                if (OneOf(type, {T::To, T::From}) && OneOf(t_.Type(head_), {T::Grant, T::Deny, T::Revoke})) principals_ = true;
            }
            if (type == Ty(T::Case)) ++caseDepth;
            else if (closesCase) --caseDepth;
            // ( ) and the { } of ODBC escapes ({fn ...}, {d '...'})
            if (type == Ty(T::LeftParenthesis) || type == Ty(T::LeftCurly)) ++depth;
            else if ((type == Ty(T::RightParenthesis) || type == Ty(T::RightCurly)) && depth > 0) --depth;
        }
    }

private:
    bool Starts(size_t i) const {
        const uint32_t type = t_.Type(i);
        const uint32_t prev = i > 0 ? t_.Type(i - 1) : 0;
        const uint32_t head = t_.Type(head_);
        const bool afterCteList = prev == Ty(T::RightParenthesis) && head == Ty(T::With) && main_ == 0;
        // GRANT, DENY, REVOKE: permissions (SELECT, ALTER, CREATE ..., EXECUTE) and securables up to TO / FROM
        if (OneOf(head, {T::Grant, T::Deny, T::Revoke}) && !principals_) return false;
        switch (static_cast<T>(type)) {
            case T::Declare: case T::Print: case T::While: case T::Truncate: case T::Return: case T::Dbcc:
            case T::Checkpoint: case T::Raiserror: case T::WaitFor: case T::Close: case T::Open: case T::Deallocate:
            case T::GoTo: case T::Commit: case T::Save: case T::Kill: case T::Backup: case T::Restore:
            case T::Break: case T::Continue: case T::Begin:
            case T::UpdateText: case T::WriteText: case T::ReadText: case T::Label:
            case T::Shutdown: case T::SetUser:
                return true;
            case T::Reconfigure:
                return !t_.Is(i - 1, "GOVERNOR");   // ALTER RESOURCE GOVERNOR RECONFIGURE
            case T::LeftParenthesis: {
                // a parenthesized query after the closing parenthesis of one: (select ...) (((select ...)))
                size_t k = i;
                while (t_.Type(k) == Ty(T::LeftParenthesis)) ++k;
                if (t_.Type(k) != Ty(T::Select) || prev != Ty(T::RightParenthesis)) return false;
                if (main_ == Ty(T::Insert) && !insertSource_) return false;   // INSERT t (c1) (SELECT ...)
                return !afterCteList;
            }
            case T::Revert:
                return !t_.Is(i - 1, "NO");   // EXECUTE AS ... WITH NO REVERT
            case T::End:
                // GENERATED ALWAYS AS ROW END (a column of a system-versioned table)
                return !(t_.Is(i - 3, "ALWAYS") && t_.Type(i - 2) == Ty(T::As));
            case T::Rollback:
                return prev != Ty(T::With);   // ALTER DATABASE ... WITH ROLLBACK IMMEDIATE
            case T::Fetch:
                // ORDER BY ... [OFFSET n ROWS] FETCH { NEXT | FIRST } n ROWS ONLY / FETCH APPROXIMATE
                if (t_.Is(i + 1, "APPROX") || t_.Is(i + 1, "APPROXIMATE")) return false;
                return !((t_.Is(i + 1, "NEXT") || t_.Is(i + 1, "FIRST")) && t_.Type(i + 2) != Ty(T::From));
            case T::Bulk:
                return t_.Type(i + 1) == Ty(T::Insert);
            case T::Add:   // ADD [COUNTER] SIGNATURE
                return t_.Is(i + 1, "SIGNATURE") || (t_.Is(i + 1, "COUNTER") && t_.Is(i + 2, "SIGNATURE"));
            case T::Grant: case T::Deny: case T::Revoke:
                return prev != Ty(T::With);
            case T::Create:
                return true;
            case T::Alter: case T::Drop:
                // CREATE OR ALTER; lists of clauses (ALTER SECURITY POLICY p DROP ..., ALTER ...)
                if (prev == Ty(T::Or) || prev == Ty(T::Comma)) return false;
                // in an ALTER statement they are mostly clauses (ALTER COLUMN, DROP CONSTRAINT, ...): a
                // new statement after a ';', or at the start of a line unless a clause's word follows
                if (head != Ty(T::Alter) || prev == Ty(T::Semicolon)) return true;
                return t_.LineStart(i) && !AlterClause(i, i + 1);
            case T::Use:
                return prev != Ty(T::LeftParenthesis) && prev != Ty(T::Comma);
            case T::If:
                if (t_.Type(i + 1) == Ty(T::Exists) &&
                    (OneOf(prev, {T::Table, T::View, T::Index, T::Procedure, T::Proc, T::Function, T::Trigger, T::Schema,
                                  T::Database, T::Statistics, T::User, T::Default, T::Rule, T::Constraint, T::Column}) ||
                     prev == Ty(T::Identifier)))
                    return false;
                return true;
            case T::Select:
                if (OneOf(prev, {T::Union, T::All, T::Except, T::Intersect, T::LeftParenthesis, T::As, T::Grant, T::Deny,
                                 T::Revoke, T::Comma}) ||
                    t_.Is(i - 1, "OFFSETS"))   // SET OFFSETS SELECT, FROM, ... ON
                    return false;
                if (main_ == Ty(T::Insert) && !insertSource_) return false;
                return !afterCteList;
            case T::Insert: case T::Update: case T::Delete: case T::Merge:
                if (OneOf(prev, {T::Then, T::For, T::Comma, T::LeftParenthesis, T::On, T::Of, T::Grant, T::Deny, T::Revoke,
                                 T::As, T::Bulk}))
                    return false;
                if (t_.Is(i - 1, "AFTER") || t_.Is(i - 1, "BEFORE") || t_.Is(i - 1, "OF") || t_.Is(i - 1, "START"))
                    return false;   // trigger events, security policy predicates, START UPDATE POPULATION
                if (type == Ty(T::Update) && t_.Type(i + 1) == Ty(T::LeftParenthesis)) return false;   // IF UPDATE(c)
                if (type == Ty(T::Merge) && (t_.Is(i + 1, "RANGE") || t_.Type(i + 1) == Ty(T::Join) ||
                                             (head == Ty(T::Alter) && prev == Ty(T::RightParenthesis))))
                    return false;   // ALTER PARTITION FUNCTION f() MERGE RANGE, MERGE JOIN
                return !afterCteList;
            case T::Set:
                if (prev == Ty(T::Update) || t_.Type(i + 1) == Ty(T::LeftParenthesis)) return false;
                return !OneOf(head, {T::Update, T::Alter, T::Merge}) && main_ != Ty(T::Update) && main_ != Ty(T::Merge);
            case T::Exec: case T::Execute:
                if (main_ == Ty(T::Insert) && !insertSource_) return false;
                return !OneOf(prev, {T::LeftParenthesis, T::With, T::Comma});
            case T::With: {
                if (t_.Is(i + 1, "XMLNAMESPACES"))   // not a clause of CREATE / ALTER ... XML INDEX
                    return !(OneOf(head, {T::Create, T::Alter}) &&
                             (t_.Type(head_ + 1) == Ty(T::Index) || t_.Is(head_ + 1, "XML") || t_.Is(head_ + 2, "XML") ||
                              t_.Is(head_ + 3, "XML")));
                // WITH name AS ( or WITH name (columns) AS (: not WITH SCHEMABINDING AS, WITH option(...)
                if (!t_.IsName(i + 1) || prev == Ty(T::RightParenthesis)) return false;
                size_t k = i + 2;
                if (t_.Type(k) == Ty(T::LeftParenthesis)) {
                    for (size_t d = 0; k < t_.size(); ++k) {
                        if (t_.Type(k) == Ty(T::LeftParenthesis)) ++d;
                        else if (t_.Type(k) == Ty(T::RightParenthesis) && --d == 0) break;
                    }
                    ++k;
                }
                return t_.Type(k) == Ty(T::As) && t_.Type(k + 1) == Ty(T::LeftParenthesis);
            }
            case T::Identifier:
                // statements whose first word is not a keyword token
                if (t_.Is(i, "COPY")) return t_.Type(i + 1) == Ty(T::Into);
                if (t_.Is(i, "SEND")) return t_.Type(i + 1) == Ty(T::On);
                if (t_.Is(i, "GET")) return t_.Is(i + 1, "CONVERSATION");
                if (t_.Is(i, "RECEIVE"))
                    return OneOf(t_.Type(i + 1), {T::Star, T::Top, T::Variable});
                if (t_.Is(i, "THROW"))
                    return i + 1 >= t_.size() || t_.LineStart(i + 1) ||
                           OneOf(t_.Type(i + 1), {T::Integer, T::Variable, T::Semicolon, T::End});
                return false;
            default:
                return false;
        }
    }

    /// Whether the words after the ALTER or DROP at `at` continue an ALTER statement (ALTER COLUMN,
    /// DROP CONSTRAINT, DROP PERIOD FOR SYSTEM_TIME, DROP MEMBER, DROP FILE, ...).
    bool AlterClause(size_t at, size_t i) const {
        if (OneOf(t_.Type(i), {T::LeftParenthesis, T::AsciiStringLiteral, T::UnicodeStringLiteral, T::Integer})) return true;
        for (const char* w : {"COLUMN", "REPLICA", "LISTENER", "FILTER", "BLOCK"})
            if (t_.Is(i, w)) return true;
        if (t_.Type(at) == Ty(T::Alter)) return false;
        for (const char* w : {"CONSTRAINT", "PERIOD", "MEMBER", "FILE", "FILEGROUP", "EVENT", "TARGET", "VALUE", "CREDENTIAL",
                              "CONTRACT", "SIGNATURE", "COUNTER", "ROUTE", "PREDICATE", "SPECIFICATION"})
            if (t_.Is(i, w)) return true;
        return false;
    }

    const ScriptTokens& t_;
    size_t head_ = 0;
    uint32_t main_ = 0;
    bool insertSource_ = false;
    bool principals_ = false;   // GRANT / DENY / REVOKE: TO or FROM seen
};

/// The first statement boundary after token `from` (a ';', GO, or the start of the next statement),
/// scanning the batch from the statement start `start`.
size_t StatementEnd(const ScriptTokens& t, size_t start, size_t from) {
    size_t end = t.size();
    StatementScanner scanner(t);
    bool first = true;
    scanner.Run(start, t.size(), [&](size_t i) {
        if (first) {
            first = false;
            return true;
        }
        if (i > from) {
            end = i;
            return false;
        }
        return true;
    });
    // a ';' or GO at the statement's depth ends it too
    size_t depth = 0;
    for (size_t i = start; i < end; ++i) {
        const uint32_t type = t.Type(i);
        if (type == Ty(T::LeftParenthesis)) ++depth;
        else if (type == Ty(T::RightParenthesis) && depth > 0) --depth;
        if (i > from && ((depth == 0 && type == Ty(T::Semicolon)) || type == Ty(T::Go))) return i;
    }
    return end;
}

}  // namespace

size_t StatementStartFromTokens(const ScriptTokens& tokens, size_t at, size_t from) {
    const size_t batch = from == SIZE_MAX ? tokens.BatchStart(at) : from;
    size_t last = batch;
    StatementScanner(tokens).Run(batch, tokens.size(), [&](size_t i) {
        if (i > at) return false;
        last = i;
        return true;
    });
    return last;
}

// ========================================================================================= analyzer

namespace {
/// The ')' matching the '(' at `open` (or `limit` when it is not closed before it).
size_t MatchParen(const ScriptTokens& t, size_t open, size_t limit) {
    size_t d = 0;
    for (size_t i = open; i < limit && i < t.size(); ++i) {
        if (t.Type(i) == Ty(T::LeftParenthesis)) ++d;
        else if (t.Type(i) == Ty(T::RightParenthesis) && --d == 0) return i;
        else if (t.Type(i) == Ty(T::Go)) return i;
    }
    return std::min(limit, t.size());
}

/// Dotted name at i (empty parts for `..`); returns the index after it.
size_t ReadNameAny(const ScriptTokens& t, size_t i, std::vector<std::string>& parts) {
    parts.clear();
    while (i < t.size()) {
        if (t.IsName(i)) parts.push_back(t.Name(i++));
        else if (t.Type(i) == Ty(T::Dot)) parts.emplace_back();
        else break;
        if (t.Type(i) != Ty(T::Dot)) break;
        ++i;
    }
    return i;
}

/// Type text of a parameter starting at k: up to ',', '=', ')' at its depth or a keyword ending it.
size_t SkipParam(const ScriptTokens& t, size_t k) {
    size_t d = 0;
    for (; k < t.size(); ++k) {
        const uint32_t type = t.Type(k);
        if (type == Ty(T::LeftParenthesis)) ++d;
        else if (type == Ty(T::RightParenthesis)) {
            if (d == 0) return k;
            --d;
        } else if (d == 0 && (OneOf(type, {T::Comma, T::EqualsSign, T::As, T::With, T::For, T::Go, T::Semicolon}) ||
                              t.Is(k, "OUTPUT") || t.Is(k, "OUT") || t.Is(k, "READONLY") || t.Is(k, "RETURNS")))
            return k;
    }
    return k;
}

/// Column definitions of the table element list at `open`: name type ... per element.
void ColumnDefinitions(const ScriptTokens& t, size_t open, std::vector<ColumnInfo>& out) {
    const size_t close = MatchParen(t, open, t.size());
    size_t a = open + 1;
    size_t d = 0;
    for (size_t i = open + 1; i <= close && i < t.size() + 1; ++i) {
        const uint32_t type = i < close ? t.Type(i) : Ty(T::Comma);
        if (type == Ty(T::LeftParenthesis)) ++d;
        else if (type == Ty(T::RightParenthesis) && d > 0) --d;
        if (!(d == 0 && type == Ty(T::Comma))) continue;
        // element [a, i)
        if (i > a && t.IsName(a) &&
            !(t.Is(a, "PERIOD") || t.Is(a, "INDEX")) &&
            !OneOf(t.Type(a), {T::Constraint, T::Primary, T::Unique, T::Check, T::Foreign, T::Index})) {
            std::string type;
            if (a + 1 < i && t.Type(a + 1) == Ty(T::As)) {
                type = "computed";
            } else {
                size_t e = a + 1;
                if (e < i && t.IsName(e)) {
                    type = std::string(t.Text(e++));
                    if (e < i && t.Type(e) == Ty(T::Dot) && t.IsName(e + 1)) {
                        type += '.';
                        type += t.Text(e + 1);
                        e += 2;
                    }
                    if (e < i && t.Type(e) == Ty(T::LeftParenthesis)) {
                        const size_t c = MatchParen(t, e, i);
                        for (size_t k = e; k <= c && k < i; ++k) type += t.Text(k);
                    }
                }
            }
            out.push_back({t.Name(a), type});
        }
        a = i + 1;
    }
}


struct RawSource {
    enum class Kind { Object, Variable, Derived, Function, Opaque } kind = Kind::Opaque;
    std::vector<std::string> parts;
    std::string alias;
    size_t declTok = 0;
    size_t parenOpen = SIZE_MAX;              // derived tables: their '('
    std::vector<std::string> columnAliases;   // derived tables / functions: AS x (c1, c2)
    bool apply = false;
};

struct Spec {
    enum class Role { Top, Subquery, Derived, ApplyDerived, CteBody };
    enum class Kind { Select, Update, Delete, Insert, Merge, IndexTarget };
    Kind kind = Kind::Select;
    Role role = Role::Top;
    size_t start = 0, end = 0, depth = 0;
    size_t parenOpen = SIZE_MAX;
    int parent = -1;
    size_t fromStart = SIZE_MAX, fromEnd = SIZE_MAX;   // where the sources are declared
    size_t listStart = 0, listEnd = 0;                 // SELECT list
    std::vector<RawSource> sources;
    bool hasTarget = false;
    RawSource target;
};

struct Cte {
    std::string name;
    std::vector<std::string> columns;
    size_t bodyOpen = 0, bodyClose = 0;
};

}  // namespace

struct ScopeAnalyzer::Impl {
    const ScriptTokens& t;
    const Catalog& catalog;
    size_t caret;
    size_t stmtStart = 0, stmtEnd = 0;
    std::string currentDb;

    std::vector<size_t> depth, match, encl;
    std::vector<Spec> specs;
    std::vector<Cte> ctes;
    size_t mainStart = 0;   // after the CTE list
    mutable int resolveDepth = 0;

    Impl(const ScriptTokens& tokens, const Catalog& cat, size_t c, size_t start) : t(tokens), catalog(cat), caret(c) {
        stmtStart = start == SIZE_MAX ? StatementStartFromTokens(t, caret) : start;
        if (stmtStart > caret) stmtStart = StatementStartFromTokens(t, caret);
        stmtEnd = StatementEnd(t, stmtStart, caret == 0 ? 0 : caret - 1);
        if (stmtEnd < caret) stmtEnd = std::min(t.size(), caret);
        currentDb = catalog.currentDatabase;
        for (size_t i = 0; i < caret && i < t.size(); ++i)
            if (t.Type(i) == Ty(T::Use) && t.IsName(i + 1) && i + 1 < caret &&
                (i == 0 || !OneOf(t.Type(i - 1), {T::LeftParenthesis, T::Comma})))
                currentDb = t.Name(i + 1);
        Structure(stmtStart, stmtEnd);
    }

    // ------------------------------------------------------------------------------- structure

    void Parens(size_t s, size_t e) {
        const size_t n = t.size() + 1;
        depth.assign(n, 0);
        match.assign(n, SIZE_MAX);
        encl.assign(n, SIZE_MAX);
        std::vector<size_t> stack;
        for (size_t i = s; i < e; ++i) {
            depth[i] = stack.size();
            encl[i] = stack.empty() ? SIZE_MAX : stack.back();
            if (t.Type(i) == Ty(T::LeftParenthesis)) {
                stack.push_back(i);
            } else if (t.Type(i) == Ty(T::RightParenthesis) && !stack.empty()) {
                match[stack.back()] = i;
                match[i] = stack.back();
                stack.pop_back();
                depth[i] = stack.size();
                encl[i] = stack.empty() ? SIZE_MAX : stack.back();
            }
        }
        for (size_t open : stack) match[open] = e;   // unclosed: to the end of the statement
    }

    size_t Close(size_t open) const { return match[open] == SIZE_MAX ? stmtEnd : match[open]; }

    /// Dotted name at i: parts (empty for `..`); returns the index after it.
    size_t ReadName(size_t i, std::vector<std::string>& parts) const {
        parts.clear();
        if (!t.IsName(i) && t.Type(i) != Ty(T::Dot)) return i;
        while (i < stmtEnd) {
            if (t.IsName(i)) {
                parts.push_back(t.Name(i));
                ++i;
            } else {
                parts.emplace_back();
            }
            if (t.Type(i) != Ty(T::Dot)) break;
            ++i;
        }
        return i;
    }

    bool IsClauseWord(size_t i) const {
        // non-reserved words that end a table source rather than alias it
        return t.Is(i, "WINDOW") || t.Is(i, "APPLY");
    }

    size_t ReadAlias(size_t j, RawSource& src) const {
        if (t.Type(j) == Ty(T::As)) {
            ++j;
            if (t.IsName(j)) src.alias = t.Name(j++);
        } else if (t.IsName(j) && !IsClauseWord(j)) {
            src.alias = t.Name(j++);
        }
        if (!src.alias.empty() && t.Type(j) == Ty(T::LeftParenthesis) &&
            (src.kind == RawSource::Kind::Derived || src.kind == RawSource::Kind::Function)) {
            const size_t close = Close(j);
            for (size_t k = j + 1; k < close; ++k)
                if (t.IsName(k)) src.columnAliases.push_back(t.Name(k));
            j = close + 1;
        }
        return j;
    }

    /// One table source at i (depth d); appends it (or a parenthesised join's sources).
    size_t ReadSource(size_t i, size_t d, bool apply, std::vector<RawSource>& out) const {
        RawSource src;
        src.declTok = i;
        src.apply = apply;
        const uint32_t type = t.Type(i);
        size_t j = i + 1;
        if (type == Ty(T::LeftParenthesis)) {
            const size_t close = Close(i);
            const uint32_t inner = t.Type(i + 1);
            if (OneOf(inner, {T::Select, T::With, T::Values, T::LeftParenthesis}) &&
                !(inner == Ty(T::LeftParenthesis) && !StartsQuery(i + 1))) {
                src.kind = RawSource::Kind::Derived;
                src.parenOpen = i;
            } else {
                ReadSources(i + 1, close, d + 1, out);   // FROM (a JOIN b ON ...)
                return close + 1;
            }
            j = close + 1;
        } else if (type == Ty(T::Variable)) {
            src.kind = RawSource::Kind::Variable;
            src.parts.push_back(std::string(t.Text(i)));
        } else if (t.IsName(i)) {
            j = ReadName(i, src.parts);
            src.kind = RawSource::Kind::Object;
            if (t.Type(j) == Ty(T::LeftParenthesis)) {
                src.kind = RawSource::Kind::Function;
                j = Close(j) + 1;
            }
        } else if (OneOf(type, {T::OpenRowSet, T::OpenQuery, T::OpenXml, T::OpenDataSource, T::ContainsTable,
                                T::FreeTextTable})) {
            src.kind = RawSource::Kind::Opaque;
            if (t.Type(j) == Ty(T::LeftParenthesis)) j = Close(j) + 1;
        } else {
            return i + 1;
        }
        j = ReadAlias(j, src);
        out.push_back(std::move(src));
        return j;
    }

    bool StartsQuery(size_t i) const {
        while (t.Type(i) == Ty(T::LeftParenthesis)) ++i;
        return OneOf(t.Type(i), {T::Select, T::With, T::Values});
    }

    void ReadSources(size_t a, size_t b, size_t d, std::vector<RawSource>& out) const {
        bool expect = true, apply = false;
        for (size_t i = a; i < b && i < stmtEnd;) {
            if (depth[i] != d) {
                ++i;
                continue;
            }
            if (expect) {
                const size_t next = ReadSource(i, d, apply, out);
                expect = false;
                apply = false;
                i = std::max(next, i + 1);
                continue;
            }
            const uint32_t type = t.Type(i);
            if (type == Ty(T::Comma) || type == Ty(T::Join)) {
                expect = true;
            } else if (t.Is(i, "APPLY")) {
                expect = true;
                apply = true;
            }
            ++i;
        }
    }

    /// First token at depth d in [a, b) that is one of `types` (or SIZE_MAX).
    size_t FindAt(size_t a, size_t b, size_t d, std::initializer_list<T> types) const {
        for (size_t i = a; i < b; ++i)
            if (depth[i] == d && OneOf(t.Type(i), types)) return i;
        return SIZE_MAX;
    }

    size_t SkipTop(size_t j) const {
        if (t.Type(j) != Ty(T::Top)) return j;
        ++j;
        if (t.Type(j) == Ty(T::LeftParenthesis)) j = Close(j) + 1;
        else ++j;
        if (t.Type(j) == Ty(T::Percent)) ++j;
        if (t.Type(j) == Ty(T::With) && t.Is(j + 1, "TIES")) j += 2;
        return j;
    }

    size_t ReadTarget(size_t j, RawSource& target) const {
        target.declTok = j;
        if (t.Type(j) == Ty(T::Variable)) {
            target.kind = RawSource::Kind::Variable;
            target.parts.push_back(std::string(t.Text(j)));
            return j + 1;
        }
        j = ReadName(j, target.parts);
        target.kind = target.parts.empty() ? RawSource::Kind::Opaque : RawSource::Kind::Object;
        return j;
    }

    void Structure(size_t s, size_t e) {
        Parens(s, e);
        // CTE list
        mainStart = s;
        if (t.Type(s) == Ty(T::With) && !t.Is(s + 1, "XMLNAMESPACES")) {
            size_t j = s + 1;
            while (j < e && t.IsName(j)) {
                Cte cte;
                cte.name = t.Name(j++);
                if (t.Type(j) == Ty(T::LeftParenthesis)) {
                    const size_t close = Close(j);
                    for (size_t k = j + 1; k < close; ++k)
                        if (t.IsName(k)) cte.columns.push_back(t.Name(k));
                    j = close + 1;
                }
                if (t.Type(j) != Ty(T::As)) break;
                ++j;
                if (t.Type(j) != Ty(T::LeftParenthesis)) break;
                cte.bodyOpen = j;
                cte.bodyClose = Close(j);
                ctes.push_back(std::move(cte));
                j = ctes.back().bodyClose + 1;
                if (t.Type(j) != Ty(T::Comma)) break;
                ++j;
            }
            mainStart = j;
        }

        // query specifications
        for (size_t p = s; p < e; ++p) {
            if (t.Type(p) != Ty(T::Select)) continue;
            Spec spec;
            spec.start = p;
            spec.depth = depth[p];
            spec.end = e;
            for (size_t q = p + 1; q < e; ++q) {
                if (depth[q] < depth[p] ||
                    (depth[q] == depth[p] && OneOf(t.Type(q), {T::Union, T::Except, T::Intersect, T::Semicolon}))) {
                    spec.end = q;
                    break;
                }
            }
            size_t j = p + 1;
            if (OneOf(t.Type(j), {T::All, T::Distinct})) ++j;
            j = SkipTop(j);
            spec.listStart = j;
            const size_t listEnd = FindAt(j, spec.end, spec.depth,
                                          {T::Into, T::From, T::Where, T::Group, T::Having, T::Order, T::Option, T::For});
            spec.listEnd = listEnd == SIZE_MAX ? spec.end : listEnd;
            const size_t from = FindAt(j, spec.end, spec.depth, {T::From});
            if (from != SIZE_MAX) {
                spec.fromStart = from + 1;
                const size_t fe = FindAt(from + 1, spec.end, spec.depth, {T::Where, T::Group, T::Having, T::Order, T::Option, T::For});
                spec.fromEnd = fe == SIZE_MAX ? spec.end : fe;
                for (size_t k = spec.fromStart; k < spec.fromEnd; ++k)
                    if (depth[k] == spec.depth && t.Is(k, "WINDOW") && t.IsName(k + 1) && t.Type(k + 2) == Ty(T::As)) {
                        spec.fromEnd = k;
                        break;
                    }
                ReadSources(spec.fromStart, spec.fromEnd, spec.depth, spec.sources);
            }
            spec.parenOpen = encl[p];
            specs.push_back(std::move(spec));
        }

        // the DML / DDL statement around them
        DmlSpec(e);

        // parents and roles
        for (size_t k = 0; k < specs.size(); ++k) {
            Spec& sp = specs[k];
            if (sp.kind != Spec::Kind::Select) continue;
            const size_t open = sp.parenOpen;
            for (const Cte& cte : ctes)
                if (open != SIZE_MAX && open >= cte.bodyOpen && open <= cte.bodyClose && depth[sp.start] == depth[cte.bodyOpen] + 1)
                    sp.role = Spec::Role::CteBody;
            if (sp.role == Spec::Role::CteBody) continue;
            const size_t anchor = open == SIZE_MAX ? sp.start : open;
            int best = -1;
            for (size_t m = 0; m < specs.size(); ++m) {
                if (m == k) continue;
                const Spec& o = specs[m];
                const bool contains = o.start < anchor && anchor < o.end && o.depth <= depth[anchor];
                if (!contains) continue;
                if (best < 0 || o.start > specs[best].start ||
                    (o.start == specs[best].start && o.kind == Spec::Kind::Select))
                    best = static_cast<int>(m);
            }
            sp.parent = best;
            if (best < 0) continue;
            const Spec& par = specs[best];
            if (open == SIZE_MAX) {
                sp.role = Spec::Role::Subquery;   // INSERT ... SELECT
                continue;
            }
            const bool inFrom = par.fromStart != SIZE_MAX && open >= par.fromStart && open < par.fromEnd;
            bool derived = false, apply = false;
            for (const RawSource& rs : par.sources)
                if (rs.parenOpen == open) {
                    derived = true;
                    apply = rs.apply;
                }
            sp.role = derived && inFrom ? (apply ? Spec::Role::ApplyDerived : Spec::Role::Derived) : Spec::Role::Subquery;
        }
    }

    void DmlSpec(size_t e) {
        const size_t d = depth[mainStart];
        size_t m = mainStart;
        while (m < e && depth[m] != d) ++m;
        if (m >= e) return;
        Spec spec;
        spec.start = m;
        spec.end = e;
        spec.depth = d;
        const uint32_t head = t.Type(m);
        size_t j = m + 1;
        if (head == Ty(T::Update) && t.Type(j) != Ty(T::Statistics)) {
            spec.kind = Spec::Kind::Update;
            j = ReadTarget(SkipTop(j), spec.target);
            const size_t set = FindAt(j, e, d, {T::Set});
            const size_t from = FindAt(set == SIZE_MAX ? j : set, e, d, {T::From});
            if (from != SIZE_MAX) {
                spec.fromStart = from + 1;
                const size_t fe = FindAt(from + 1, e, d, {T::Where, T::Option});
                spec.fromEnd = fe == SIZE_MAX ? e : fe;
                ReadSources(spec.fromStart, spec.fromEnd, d, spec.sources);
            }
        } else if (head == Ty(T::Delete)) {
            spec.kind = Spec::Kind::Delete;
            j = SkipTop(j);
            // DELETE | FROM t x ...: the target is being typed and the FROM is the source clause
            if (j != caret) {
                if (t.Type(j) == Ty(T::From)) ++j;
                j = ReadTarget(j, spec.target);
            }
            const size_t from = FindAt(j, e, d, {T::From});
            if (from != SIZE_MAX) {
                spec.fromStart = from + 1;
                const size_t fe = FindAt(from + 1, e, d, {T::Where, T::Option});
                spec.fromEnd = fe == SIZE_MAX ? e : fe;
                ReadSources(spec.fromStart, spec.fromEnd, d, spec.sources);
            }
        } else if (head == Ty(T::Insert)) {
            spec.kind = Spec::Kind::Insert;
            j = SkipTop(j);
            if (t.Type(j) == Ty(T::Into)) ++j;
            ReadTarget(j, spec.target);
        } else if (head == Ty(T::Merge)) {
            spec.kind = Spec::Kind::Merge;
            j = SkipTop(j);
            if (t.Type(j) == Ty(T::Into)) ++j;
            j = ReadTarget(j, spec.target);
            if (t.Type(j) == Ty(T::With) && t.Type(j + 1) == Ty(T::LeftParenthesis)) j = Close(j + 1) + 1;
            j = ReadAlias(j, spec.target);
            const size_t on = FindAt(j, e, d, {T::On});
            if (t.Is(j, "USING")) {
                spec.fromStart = j + 1;
                spec.fromEnd = on == SIZE_MAX ? e : on;
                ReadSources(spec.fromStart, spec.fromEnd, d, spec.sources);
            }
        } else if (head == Ty(T::Alter) && t.Type(j) == Ty(T::Table)) {
            // ALTER TABLE t ...: its columns (PERIOD FOR SYSTEM_TIME (start, end), ...)
            spec.kind = Spec::Kind::IndexTarget;
            ReadTarget(j + 1, spec.target);
        } else if (head == Ty(T::Create) || head == Ty(T::Update) || head == Ty(T::Alter)) {
            // CREATE [UNIQUE] [CLUSTERED|NONCLUSTERED] [COLUMNSTORE] INDEX x ON t, CREATE STATISTICS s ON t,
            // UPDATE STATISTICS t, ALTER INDEX x ON t
            size_t k = j;
            while (k < e && depth[k] == d && t.Type(k) != Ty(T::Index) && t.Type(k) != Ty(T::Statistics)) ++k;
            if (k >= e) return;
            spec.kind = Spec::Kind::IndexTarget;
            if (head == Ty(T::Update)) {
                ReadTarget(k + 1, spec.target);
            } else {
                const size_t on = FindAt(k, e, d, {T::On});
                if (on == SIZE_MAX) return;
                ReadTarget(on + 1, spec.target);
            }
        } else {
            return;
        }
        spec.hasTarget = !spec.target.parts.empty();
        specs.push_back(std::move(spec));
    }

    // ------------------------------------------------------------------------------- resolution

    const Spec* Innermost(size_t at) const {
        const Spec* best = nullptr;
        for (const Spec& s : specs) {
            if (!(s.start < at && at <= s.end)) continue;
            if (best == nullptr || s.start > best->start || (s.start == best->start && s.kind == Spec::Kind::Select))
                best = &s;
        }
        return best;
    }

    const Cte* FindCte(std::string_view name) const {
        for (const Cte& c : ctes)
            if (EqualsI(c.name, name)) return &c;
        return nullptr;
    }

    std::vector<ColumnInfo> CteColumns(const Cte& cte) const {
        std::vector<ColumnInfo> out;
        if (!cte.columns.empty()) {
            for (const std::string& n : cte.columns) out.push_back({n, ""});
            return out;
        }
        for (const Spec& s : specs)
            if (s.kind == Spec::Kind::Select && s.start > cte.bodyOpen && s.start < cte.bodyClose &&
                depth[s.start] == depth[cte.bodyOpen] + 1)
                return OutputNames(s);   // the anchor (first) query of the body
        return out;
    }

    /// Schemas the name `parts` is looked up in: its own, else the default schema and dbo (and sys
    /// for `systemNames`).
    std::vector<std::string> SearchSchemas(const std::vector<std::string>& parts, bool systemNames) const {
        std::vector<std::string> schemas;
        if (parts.size() >= 2 && !parts[parts.size() - 2].empty()) {
            schemas.push_back(parts[parts.size() - 2]);
            return schemas;
        }
        schemas.push_back(catalog.defaultSchema);
        if (!EqualsI(catalog.defaultSchema, "dbo")) schemas.push_back("dbo");
        if (systemNames) schemas.push_back("sys");
        return schemas;
    }

    std::string DatabaseOf(const std::vector<std::string>& parts) const {
        return parts.size() >= 3 && !parts[parts.size() - 3].empty() ? parts[parts.size() - 3] : currentDb;
    }

    const CatalogObject* FindObject(const std::vector<std::string>& partsIn) const {
        if (partsIn.empty() || partsIn.size() > 4) return nullptr;
        if (partsIn.size() == 4 && !partsIn[0].empty()) return nullptr;   // a linked server
        const std::string& name = partsIn.back();
        const bool systemProcedure = StartsWithI(name, "sp_") || StartsWithI(name, "xp_");
        const std::string db = DatabaseOf(partsIn);
        for (const std::string& schema : SearchSchemas(partsIn, systemProcedure))
            if (const CatalogObject* o = FindCatalogObject(catalog, db, schema, name)) return o;
        return nullptr;
    }

    const CatalogType* FindType(const std::vector<std::string>& parts) const {
        if (parts.empty() || parts.size() > 3) return nullptr;
        const std::string db = DatabaseOf(parts);
        for (const std::string& schema : SearchSchemas(parts, true))
            if (const CatalogType* ty = FindCatalogType(catalog, db, schema, parts.back())) return ty;
        return nullptr;
    }

    std::vector<VariableInfo> Variables() const;
    std::vector<TempTableInfo> TempTables() const;

    static SourceInfo::Kind KindOf(CatalogObject::Type type) {
        switch (type) {
            case CatalogObject::Type::Table: return SourceInfo::Kind::Table;
            case CatalogObject::Type::View: return SourceInfo::Kind::View;
            case CatalogObject::Type::TableFunction: return SourceInfo::Kind::TableFunction;
            default: return SourceInfo::Kind::Unknown;
        }
    }

    bool ResolveObject(const std::vector<std::string>& parts, SourceInfo& out) const {
        out.parts = parts;
        if (parts.empty()) return false;
        out.exposed = parts.back();
        if (parts.size() == 1 && !parts[0].empty() && parts[0][0] == '@') {
            for (const VariableInfo& v : Variables())
                if (v.isTable && EqualsI(v.name, parts[0])) {
                    out.kind = SourceInfo::Kind::TableVariable;
                    out.columns = TableVariableColumns(v.name);
                    return true;
                }
            return false;
        }
        if (parts.size() == 1 && !parts[0].empty() && parts[0][0] == '#') {
            for (const TempTableInfo& tt : TempTables())
                if (EqualsI(tt.name, parts[0])) {
                    out.kind = SourceInfo::Kind::TempTable;
                    out.columns = tt.columns;
                    return true;
                }
            return false;
        }
        if (parts.size() == 1)
            if (const Cte* cte = FindCte(parts[0])) {
                out.kind = SourceInfo::Kind::Cte;
                if (resolveDepth < 8) {
                    ++resolveDepth;
                    out.columns = CteColumns(*cte);
                    --resolveDepth;
                }
                return true;
            }
        const CatalogObject* o = FindObject(parts);
        if (o != nullptr && o->type == CatalogObject::Type::Synonym) {
            o = SynonymTarget(catalog, *o);
            if (o == nullptr) return true;   // a synonym of an object the catalog does not have
        }
        if (o == nullptr) return false;
        out.kind = KindOf(o->type);
        for (const CatalogColumn& c : o->columns) out.columns.push_back({c.name, c.type});
        return true;
    }

    std::vector<ColumnInfo> TableVariableColumns(const std::string& name) const;

    SourceInfo Resolve(const RawSource& rs) const {
        SourceInfo out;
        out.alias = rs.alias;
        switch (rs.kind) {
            case RawSource::Kind::Object:
            case RawSource::Kind::Variable:
            case RawSource::Kind::Function:
                ResolveObject(rs.parts, out);
                break;
            case RawSource::Kind::Derived:
                out.kind = SourceInfo::Kind::Derived;
                if (!rs.columnAliases.empty()) {
                    for (const std::string& c : rs.columnAliases) out.columns.push_back({c, ""});
                } else if (resolveDepth < 8) {
                    ++resolveDepth;
                    for (const Spec& s : specs)
                        if (s.kind == Spec::Kind::Select && s.parenOpen == rs.parenOpen) {
                            out.columns = OutputNames(s);
                            break;
                        }
                    --resolveDepth;
                }
                break;
            case RawSource::Kind::Opaque:
                break;
        }
        if (!rs.columnAliases.empty() && rs.kind == RawSource::Kind::Function) {
            out.columns.clear();
            for (const std::string& c : rs.columnAliases) out.columns.push_back({c, ""});
        }
        if (!rs.alias.empty()) out.exposed = rs.alias;
        return out;
    }

    /// The own sources of `s` an expression at `at` sees (inside its FROM clause: those declared before).
    std::vector<SourceInfo> Own(const Spec& s, size_t at) const {
        std::vector<SourceInfo> out;
        const bool inFrom = s.fromStart != SIZE_MAX && at >= s.fromStart && at < s.fromEnd;
        if (s.hasTarget && s.kind != Spec::Kind::Insert) {
            bool aliasOfSource = false;
            if (s.target.parts.size() == 1)
                for (const RawSource& rs : s.sources)
                    if (!rs.alias.empty() && EqualsI(rs.alias, s.target.parts[0])) aliasOfSource = true;
            if (!aliasOfSource) out.push_back(Resolve(s.target));
        }
        for (const RawSource& rs : s.sources) {
            if (inFrom && rs.declTok >= at) continue;
            out.push_back(Resolve(rs));
        }
        return out;
    }

    std::vector<SourceInfo> ExpressionSources() const {
        std::vector<SourceInfo> out;
        const Spec* s = Innermost(caret);
        if (s == nullptr) return out;
        auto add = [&](std::vector<SourceInfo> v) {
            for (SourceInfo& x : v) out.push_back(std::move(x));
        };
        add(Own(*s, caret));
        const Spec* cur = s;
        while (cur->parent >= 0 && cur->role != Spec::Role::CteBody) {
            const Spec& par = specs[static_cast<size_t>(cur->parent)];
            if (cur->role == Spec::Role::Derived) {
                // a derived table does not see its siblings; it does see the queries around them
            } else {
                add(Own(par, cur->parenOpen == SIZE_MAX ? caret : cur->parenOpen));
            }
            cur = &par;
        }
        return out;
    }

    std::vector<ColumnInfo> OutputNames(const Spec& s) const {
        std::vector<ColumnInfo> out;
        std::vector<SourceInfo> own;
        bool ownResolved = false;
        auto sources = [&]() -> const std::vector<SourceInfo>& {
            if (!ownResolved) {
                ++resolveDepth;
                if (resolveDepth < 8) own = Own(s, s.end);
                --resolveDepth;
                ownResolved = true;
            }
            return own;
        };
        size_t a = s.listStart;
        for (size_t i = s.listStart; i <= s.listEnd; ++i) {
            if (i < s.listEnd && !(depth[i] == s.depth && t.Type(i) == Ty(T::Comma))) continue;
            const size_t b = i;
            if (b > a) {
                if (t.Type(b - 1) == Ty(T::Star)) {
                    if (b - a == 1) {
                        for (const SourceInfo& src : sources()) out.insert(out.end(), src.columns.begin(), src.columns.end());
                    } else if (b - a >= 3 && t.Type(b - 2) == Ty(T::Dot) && t.IsName(b - 3)) {
                        const std::string q = t.Name(b - 3);
                        for (const SourceInfo& src : sources())
                            if (EqualsI(src.exposed, q)) out.insert(out.end(), src.columns.begin(), src.columns.end());
                    }
                } else if (t.Type(a) == Ty(T::Variable) && t.Type(a + 1) == Ty(T::EqualsSign)) {
                    // @v = expression assigns a variable
                } else if (t.IsName(a) && t.Type(a + 1) == Ty(T::EqualsSign) && b - a > 2) {
                    out.push_back({t.Name(a), ""});
                } else if (t.IsName(b - 1)) {
                    out.push_back({t.Name(b - 1), ""});
                }
            }
            a = i + 1;
        }
        return out;
    }
};

// --------------------------------------------------------------------------------- variables

std::vector<VariableInfo> ScopeAnalyzer::Impl::Variables() const {
    std::vector<VariableInfo> out;
    const size_t batch = t.BatchStart(caret);
    auto typeText = [&](size_t a, size_t b) {
        std::string s;
        for (size_t k = a; k < b; ++k) {
            if (!s.empty() && t.Type(k) != Ty(T::LeftParenthesis) && t.Type(k) != Ty(T::RightParenthesis) &&
                t.Type(k) != Ty(T::Comma) && t.Type(k - 1) != Ty(T::LeftParenthesis))
                s += ' ';
            s += t.Text(k);
        }
        return s;
    };
    auto add = [&](size_t at, bool isTable, std::string type, const CatalogType* tableType = nullptr) {
        if (at >= caret) return;
        const std::string name(t.Text(at));
        for (const VariableInfo& v : out)
            if (EqualsI(v.name, name)) return;
        out.push_back({name, std::move(type), isTable, tableType});
    };
    // a variable or parameter of the type written in [a, e): a table variable when it names a
    // user-defined table type
    auto addTyped = [&](size_t at, size_t a, size_t e) {
        std::vector<std::string> parts;
        const CatalogType* ty = ReadNameAny(t, a, parts) == e ? FindType(parts) : nullptr;
        if (ty != nullptr && ty->isTableType) add(at, true, typeText(a, e), ty);
        else add(at, false, typeText(a, e));
    };
    // end of a declaration's type or initializer: ',' or ')' at its depth, '=' (type only), a ';'
    // or a token that starts a statement
    auto skipTo = [&](size_t j, bool stopAtEquals) {
        size_t d = 0;
        for (; j < t.size() && t.Type(j) != Ty(T::Go); ++j) {
            const uint32_t type = t.Type(j);
            if (type == Ty(T::LeftParenthesis)) ++d;
            else if (type == Ty(T::RightParenthesis)) {
                if (d == 0) return j;
                --d;
            } else if (d == 0 && (type == Ty(T::Comma) || type == Ty(T::Semicolon) ||
                                  (stopAtEquals && type == Ty(T::EqualsSign)) ||
                                  OneOf(type, {T::Select, T::Insert, T::Update, T::Delete, T::Declare, T::Set, T::If,
                                               T::While, T::Begin, T::End, T::Exec, T::Execute, T::Print, T::Return,
                                               T::As, T::Merge, T::With})))
                return j;
        }
        return j;
    };
    for (size_t i = batch; i < caret && i < t.size(); ++i) {
        const uint32_t type = t.Type(i);
        if (type == Ty(T::Declare)) {
            size_t j = i + 1;
            while (t.Type(j) == Ty(T::Variable)) {
                const size_t v = j++;
                if (t.Type(j) == Ty(T::As)) ++j;
                if (t.Type(j) == Ty(T::Table)) {
                    add(v, true, "table");
                    if (t.Type(j + 1) != Ty(T::LeftParenthesis)) break;
                    j = MatchParen(t, j + 1, t.size()) + 1;
                } else {
                    const size_t e = skipTo(j, true);
                    addTyped(v, j, e);
                    j = e;
                    if (t.Type(j) == Ty(T::EqualsSign)) j = skipTo(j + 1, false);
                }
                if (t.Type(j) != Ty(T::Comma)) break;
                ++j;
            }
        } else if (OneOf(type, {T::Create, T::Alter})) {
            size_t j = i + 1;
            if (t.Type(j) == Ty(T::Or) && t.Type(j + 1) == Ty(T::Alter)) j += 2;
            const uint32_t kind = t.Type(j);
            const bool proc = OneOf(kind, {T::Procedure, T::Proc});
            const bool func = kind == Ty(T::Function);
            if (!proc && !func) continue;
            std::vector<std::string> parts;
            j = ReadNameAny(t, j + 1, parts);
            if (t.Type(j) == Ty(T::Semicolon) && t.Type(j + 1) == Ty(T::Integer)) j += 2;   // ;number
            size_t d = 0;
            for (; j < t.size() && j < caret; ++j) {
                const uint32_t ty = t.Type(j);
                if (ty == Ty(T::LeftParenthesis)) ++d;
                else if (ty == Ty(T::RightParenthesis) && d > 0) --d;
                else if (d == 0 && (ty == Ty(T::As) || ty == Ty(T::With) || t.Is(j, "RETURNS") || ty == Ty(T::For)))
                    break;
                else if (ty == Ty(T::Variable) && d <= 1) {
                    const uint32_t prev = t.Type(j - 1);
                    if (prev == Ty(T::Comma) || prev == Ty(T::LeftParenthesis) || t.IsName(j - 1)) {
                        size_t k = j + 1;
                        if (t.Type(k) == Ty(T::As)) ++k;
                        const size_t e = SkipParam(t, k);
                        addTyped(j, k, e);
                    }
                }
            }
            // RETURNS @t TABLE (...) of a multi-statement table-valued function
            if (func && t.Is(j, "RETURNS") && t.Type(j + 1) == Ty(T::Variable) && t.Type(j + 2) == Ty(T::Table))
                add(j + 1, true, "table");
        }
    }
    return out;
}

std::vector<ColumnInfo> ScopeAnalyzer::Impl::TableVariableColumns(const std::string& name) const {
    std::vector<ColumnInfo> out;
    for (const VariableInfo& v : Variables())
        if (v.tableType != nullptr && EqualsI(v.name, name)) {
            for (const CatalogColumn& c : v.tableType->columns) out.push_back({c.name, c.type});
            return out;
        }
    const size_t batch = t.BatchStart(caret);
    for (size_t i = batch; i + 2 < caret && i + 2 < t.size(); ++i) {
        if (t.Type(i) != Ty(T::Variable) || !EqualsI(t.Text(i), name)) continue;
        size_t j = i + 1;
        if (t.Type(j) == Ty(T::As)) ++j;
        if (t.Type(j) != Ty(T::Table) || t.Type(j + 1) != Ty(T::LeftParenthesis)) continue;
        ColumnDefinitions(t, j + 1, out);
        return out;
    }
    return out;
}

std::vector<TempTableInfo> ScopeAnalyzer::Impl::TempTables() const {
    std::vector<TempTableInfo> out;
    auto find = [&](const std::string& name) -> TempTableInfo* {
        for (TempTableInfo& tt : out)
            if (EqualsI(tt.name, name)) return &tt;
        return nullptr;
    };
    for (size_t i = 0; i + 1 < caret && i + 1 < t.size(); ++i) {
        // CREATE TABLE #name (...)
        if (t.Type(i) == Ty(T::Create) && t.Type(i + 1) == Ty(T::Table) && t.IsName(i + 2) &&
            t.Name(i + 2).rfind('#', 0) == 0 && i + 2 < caret) {
            const std::string name = t.Name(i + 2);
            if (find(name) != nullptr) continue;
            TempTableInfo tt{name, {}};
            if (t.Type(i + 3) == Ty(T::LeftParenthesis)) ColumnDefinitions(t, i + 3, tt.columns);
            out.push_back(std::move(tt));
            continue;
        }
        // SELECT ... INTO #name FROM ...
        if (t.Type(i) == Ty(T::Into) && t.IsName(i + 1) && t.Name(i + 1).rfind('#', 0) == 0 && i + 1 < caret) {
            const std::string name = t.Name(i + 1);
            if (find(name) != nullptr) continue;
            size_t d = 0, select = SIZE_MAX;
            for (size_t k = i; k-- > 0;) {
                const uint32_t type = t.Type(k);
                if (type == Ty(T::RightParenthesis)) ++d;
                else if (type == Ty(T::LeftParenthesis)) {
                    if (d == 0) break;
                    --d;
                } else if (d == 0 && type == Ty(T::Select)) {
                    select = k;
                    break;
                } else if (d == 0 && (type == Ty(T::Semicolon) || type == Ty(T::Go))) {
                    break;
                }
            }
            if (select == SIZE_MAX) continue;   // INSERT INTO #name: no definition here
            ScopeAnalyzer inner(t, catalog, i + 1, StatementStartFromTokens(t, select));
            TempTableInfo tt{name, {}};
            tt.columns = inner.SelectListNames();
            out.push_back(std::move(tt));
        }
    }
    return out;
}

// ========================================================================================== catalog

bool InDatabase(std::string_view objectDb, std::string_view db) { return objectDb.empty() || EqualsI(objectDb, db); }

const CatalogObject* FindCatalogObject(const Catalog& catalog, std::string_view db, std::string_view schema,
                                       std::string_view name) {
    const CatalogObject* system = nullptr;
    for (const CatalogObject& o : catalog.objects) {
        if (!EqualsI(o.name, name) || !EqualsI(o.schema, schema)) continue;
        if (EqualsI(o.database, db)) return &o;
        if (o.database.empty() && system == nullptr) system = &o;
    }
    return system;
}

const CatalogType* FindCatalogType(const Catalog& catalog, std::string_view db, std::string_view schema,
                                   std::string_view name) {
    const CatalogType* system = nullptr;
    for (const CatalogType& ty : catalog.types) {
        if (!EqualsI(ty.name, name) || !EqualsI(ty.schema, schema)) continue;
        if (EqualsI(ty.database, db)) return &ty;
        if (ty.database.empty() && system == nullptr) system = &ty;
    }
    return system;
}

std::vector<std::string> SplitMultiPartName(std::string_view name) {
    std::vector<std::string> parts(1);
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '.') {
            parts.emplace_back();
        } else if ((c == '[' || c == '"') && parts.back().empty()) {
            const char close = c == '[' ? ']' : '"';
            for (++i; i < name.size(); ++i) {
                if (name[i] == close) {
                    if (i + 1 < name.size() && name[i + 1] == close) ++i;   // ]] or "" inside the name
                    else break;
                }
                parts.back() += name[i];
            }
        } else if (c != ' ') {
            parts.back() += c;
        }
    }
    return parts;
}

const CatalogObject* SynonymTarget(const Catalog& catalog, const CatalogObject& synonym) {
    const std::vector<std::string> parts = SplitMultiPartName(synonym.target);
    if (parts.size() > 3 || parts.back().empty()) return nullptr;   // a linked server's object
    const std::string& db = parts.size() == 3 && !parts[0].empty() ? parts[0] : synonym.database;
    const std::string& schema = parts.size() >= 2 && !parts[parts.size() - 2].empty() ? parts[parts.size() - 2]
                                                                                      : catalog.defaultSchema;
    const CatalogObject* o = FindCatalogObject(catalog, db, schema, parts.back());
    return o != nullptr && o->type != CatalogObject::Type::Synonym ? o : nullptr;
}

// ============================================================================================= API

ScopeAnalyzer::ScopeAnalyzer(const ScriptTokens& tokens, const Catalog& catalog, size_t caret, size_t statementStart)
    : impl_(std::make_unique<Impl>(tokens, catalog, caret, statementStart)) {}

ScopeAnalyzer::~ScopeAnalyzer() = default;

const std::string& ScopeAnalyzer::CurrentDatabase() const { return impl_->currentDb; }

std::vector<VariableInfo> ScopeAnalyzer::Variables() const { return impl_->Variables(); }

std::vector<TempTableInfo> ScopeAnalyzer::TempTables() const { return impl_->TempTables(); }

std::vector<CteInfo> ScopeAnalyzer::Ctes() const {
    std::vector<CteInfo> out;
    const size_t caret = impl_->caret;
    for (const Cte& cte : impl_->ctes) {
        const bool inBody = caret > cte.bodyOpen && caret <= cte.bodyClose;
        if (caret <= cte.bodyOpen && caret < impl_->mainStart) break;   // later CTEs are not visible yet
        out.push_back({cte.name, impl_->CteColumns(cte)});
        if (inBody) break;
    }
    if (caret < impl_->stmtStart || caret > impl_->stmtEnd) out.clear();
    return out;
}

std::vector<SourceInfo> ScopeAnalyzer::ExpressionSources() const { return impl_->ExpressionSources(); }

std::vector<ColumnInfo> ScopeAnalyzer::SelectListNames() const {
    const Spec* s = impl_->Innermost(impl_->caret);
    if (s == nullptr || s->kind != Spec::Kind::Select) return {};
    return impl_->OutputNames(*s);
}

bool ScopeAnalyzer::Target(SourceInfo& out) const {
    for (const Spec& s : impl_->specs) {
        if (s.kind == Spec::Kind::Select || !s.hasTarget) continue;
        if (s.target.parts.size() == 1)
            for (const RawSource& rs : s.sources)
                if (!rs.alias.empty() && EqualsI(rs.alias, s.target.parts[0])) {
                    out = impl_->Resolve(rs);
                    return true;
                }
        out = impl_->Resolve(s.target);
        return true;
    }
    return false;
}

std::vector<SourceInfo> ScopeAnalyzer::TargetFromSources() const {
    std::vector<SourceInfo> out;
    for (const Spec& s : impl_->specs)
        if (s.kind == Spec::Kind::Update || s.kind == Spec::Kind::Delete)
            for (const RawSource& rs : s.sources) out.push_back(impl_->Resolve(rs));
    return out;
}

bool ScopeAnalyzer::ResolveObject(const std::vector<std::string>& parts, SourceInfo& out) const {
    return impl_->ResolveObject(parts, out);
}

const CatalogObject* ScopeAnalyzer::FindObject(const std::vector<std::string>& parts) const {
    return impl_->FindObject(parts);
}

const CatalogType* ScopeAnalyzer::FindType(const std::vector<std::string>& parts) const { return impl_->FindType(parts); }

}  // namespace tsql::editor::detail
