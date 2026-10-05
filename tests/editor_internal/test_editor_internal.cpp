// Unit tests for the internals of the editor support (src/editor): name quoting, UTF-8 offsets,
// built-in tables, predicate evaluation, statement segmentation, scope analysis, the parser capture
// and the ATN walk. The contract tests are in tests/editor.
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <stdexcept>
#include <vector>

#include "Builtins.h"
#include "Grammar.h"
#include "Names.h"
#include "Scope.h"
#include "Walker.h"
#include "tsql/editor.hpp"

using namespace tsql;
using namespace tsql::editor::detail;

namespace {

int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            ++failures;                                                                \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const auto va = (a);                                                                        \
        const auto vb = (b);                                                                        \
        if (!(va == vb)) {                                                                          \
            ++failures;                                                                             \
            std::printf("%s:%d: CHECK_EQ failed: %s != %s\n", __FILE__, __LINE__, #a, #b);          \
        }                                                                                           \
    } while (0)

const Grammar& G() { return GrammarFor(SqlVersion::Sql170); }

ScriptTokens TokensOf(const std::string& sql, std::vector<LexToken>& all) {
    all.clear();
    G().Lex(sql, all);
    return ScriptTokens(sql, all);
}

void TestNames() {
    CHECK_EQ(QuoteName("Orders"), std::string("Orders"));
    CHECK_EQ(QuoteName("Order"), std::string("[Order]"));             // reserved
    CHECK_EQ(QuoteName("Order Details"), std::string("[Order Details]"));
    CHECK_EQ(QuoteName("Raw]Value"), std::string("[Raw]]Value]"));
    CHECK_EQ(QuoteName("2024Targets"), std::string("[2024Targets]"));
    CHECK_EQ(QuoteName("Ums\xC3\xA4tze"), std::string("Ums\xC3\xA4tze"));   // non-ASCII letters are regular
    CHECK_EQ(QuoteName("#work"), std::string("#work"));
    CHECK_EQ(QuoteName("@v"), std::string("@v"));
    CHECK_EQ(QuoteName("a$b"), std::string("a$b"));
    CHECK_EQ(QuoteName("$ab"), std::string("[$ab]"));
    CHECK_EQ(QuoteName(""), std::string("[]"));
    CHECK_EQ(Unquote("[a]]b]"), std::string("a]b"));
    CHECK_EQ(Unquote("\"x\"\"y\""), std::string("x\"y"));
    CHECK_EQ(Unquote("[abc"), std::string("abc"));   // unterminated
    CHECK_EQ(Unquote("["), std::string());
    CHECK_EQ(Unquote("plain"), std::string("plain"));
    CHECK(StartsWithI("NOLOCK", "nol"));
    CHECK(!StartsWithI("NO", "nol"));
    CHECK(EqualsI("select", "SELECT"));
    CHECK(IsReservedKeyword("SELECT"));
    CHECK(!IsReservedKeyword("NOLOCK"));
}

void TestUtf8() {
    CHECK(CodePointByteOffsets("abc").empty());
    const std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" "b";   // a é € 😀 b
    const std::vector<uint32_t> expect = {0, 1, 3, 6, 10, 11};
    CHECK(CodePointByteOffsets(s) == expect);
    const std::string bad = "a\xFF" "b\xC3";   // an invalid byte, a truncated sequence
    const std::vector<uint32_t> expectBad = {0, 1, 2, 3, 4};
    CHECK(CodePointByteOffsets(bad) == expectBad);
    std::string out;
    CHECK(!SanitizeUtf8(s, out));
    CHECK(SanitizeUtf8(bad, out));
    CHECK_EQ(out, std::string("a\xEF\xBF\xBD" "b\xEF\xBF\xBD"));
    // an overlong encoding and a surrogate are invalid too
    CHECK(SanitizeUtf8("\xC0\xAF", out));
    CHECK(SanitizeUtf8("\xED\xA0\x80", out));
}

void TestBuiltins() {
    // every built-in is found by its name (the table must stay sorted)
    for (const Builtin* b : BuiltinsFor(SqlVersion::Sql180)) CHECK(FindBuiltin(b->name) == b);
    CHECK(FindBuiltin("STRING_AGG") != nullptr);
    CHECK(FindBuiltin("NOPE") == nullptr);
    auto has = [](SqlVersion v, const char* name) {
        for (const Builtin* b : BuiltinsFor(v))
            if (std::string(b->name) == name) return true;
        return false;
    };
    CHECK(!has(SqlVersion::Sql130, "STRING_AGG"));
    CHECK(has(SqlVersion::Sql140, "STRING_AGG"));
    CHECK(has(SqlVersion::SqlFabricDW, "GREATEST"));
    CHECK(!has(SqlVersion::SqlFabricDW, "REGEXP_LIKE"));
    CHECK(has(SqlVersion::Sql170, "REGEXP_LIKE"));
    CHECK(VersionAtLeast(SqlVersion::Sql180, SqlVersion::Sql170));
    CHECK(!VersionAtLeast(SqlVersion::Sql150, SqlVersion::Sql160));
}

void TestPredicates() {
    const std::vector<LexToken> tokens = {{static_cast<uint32_t>(ast::TSqlTokenType::Identifier), 0, 4},
                                          {static_cast<uint32_t>(ast::TSqlTokenType::Identifier), 5, 9}};
    const std::vector<std::string> upper = {"WITH", "TIES"};
    WalkInput in;
    in.tokens = &tokens;
    in.upper = &upper;
    in.caret = 2;
    using K = PredicateValue;
    CHECK(EvaluatePredicate(G(), "m1:WITH;", in, 0).kind == K::True);
    CHECK(EvaluatePredicate(G(), "m1:TOP;", in, 0).kind == K::False);
    CHECK(EvaluatePredicate(G(), "m2:TIES;", in, 0).kind == K::True);
    CHECK(EvaluatePredicate(G(), "t1:Identifier;", in, 1).kind == K::True);
    CHECK(EvaluatePredicate(G(), "t1:Comma;", in, 1).kind == K::False);
    // conditions on the caret token become word sets
    const PredicateValue req = EvaluatePredicate(G(), "m1:NOLOCK;", in, 2);
    CHECK(req.kind == K::Require && req.words == std::vector<std::string>{"NOLOCK"});
    const PredicateValue excl = EvaluatePredicate(G(), "&2!m1:DISABLE;!m1:ENABLE;", in, 2);
    CHECK(excl.kind == K::Exclude && excl.words == (std::vector<std::string>{"DISABLE", "ENABLE"}));
    const PredicateValue any = EvaluatePredicate(G(), "|2m1:A;m1:B;", in, 2);
    CHECK(any.kind == K::Require && any.words == (std::vector<std::string>{"A", "B"}));
    CHECK(EvaluatePredicate(G(), "&2m1:A;m1:B;", in, 2).kind == K::False);
    CHECK(EvaluatePredicate(G(), "&2m1:WITH;m1:B;", in, 0).kind == K::False);
    CHECK(EvaluatePredicate(G(), "?", in, 0).kind == K::Unknown);
    CHECK(EvaluatePredicate(G(), "|2?m1:WITH;", in, 0).kind == K::True);
    // past the caret: unknown
    CHECK(EvaluatePredicate(G(), "m4:X;", in, 0).kind == K::Unknown);
}

/// Index of the statement start of the token at byte `at` of `sql`.
std::string StatementAt(const std::string& sql, const std::string& marker) {
    std::vector<LexToken> all;
    const ScriptTokens t = TokensOf(sql, all);
    const size_t at = t.IndexAt(sql.find(marker));
    const size_t start = StatementStartFromTokens(t, at);
    return std::string(t.Text(start));
}

void TestStatements() {
    CHECK_EQ(StatementAt("SELECT 1 SELECT b FROM t", "b FROM"), std::string("SELECT"));
    CHECK_EQ(StatementAt("SELECT 1; SELECT b FROM t", "b FROM"), std::string("SELECT"));
    CHECK_EQ(StatementAt("SELECT 1\nUPDATE t SET a = 1 WHERE b = 2", "b = 2"), std::string("UPDATE"));
    CHECK_EQ(StatementAt("INSERT INTO t (a) SELECT b FROM u", "b FROM"), std::string("INSERT"));
    CHECK_EQ(StatementAt("INSERT INTO t EXEC p", "p"), std::string("INSERT"));
    CHECK_EQ(StatementAt("WITH x AS (SELECT 1 AS a) SELECT b FROM x", "b FROM"), std::string("WITH"));
    CHECK_EQ(StatementAt("SELECT a FROM t UNION ALL SELECT b FROM u", "b FROM"), std::string("SELECT"));
    CHECK_EQ(StatementAt("SELECT CASE WHEN a = 1 THEN 1 END, b FROM t", "b FROM"), std::string("SELECT"));
    CHECK_EQ(StatementAt("DROP TABLE IF EXISTS t1\nSELECT b FROM t", "b FROM"), std::string("SELECT"));
    CHECK_EQ(StatementAt("IF EXISTS (SELECT 1) DELETE FROM t WHERE b = 1", "b = 1"), std::string("DELETE"));
    CHECK_EQ(StatementAt("MERGE t USING s ON 1 = 1 WHEN MATCHED THEN UPDATE SET b = 1", "b = 1"), std::string("MERGE"));
    CHECK_EQ(StatementAt("SELECT * FROM t WITH (NOLOCK) WHERE b = 1", "b = 1"), std::string("SELECT"));
    CHECK_EQ(StatementAt("BEGIN TRY SELECT b FROM t END TRY", "b FROM"), std::string("SELECT"));
    CHECK_EQ(StatementAt("SELECT 1\nGO\nSELECT b FROM t", "b FROM"), std::string("SELECT"));
    CHECK_EQ(StatementAt("GRANT SELECT, INSERT ON t TO u", "u"), std::string("GRANT"));
}

void TestScope() {
    const std::string sql =
        "USE Warehouse;\nGO\n"
        "CREATE TABLE #work (Id int NOT NULL, Amount decimal(9, 2), CONSTRAINT pk PRIMARY KEY (Id));\nGO\n"
        "DECLARE @a int = 1, @t TABLE (X int, Y nvarchar(10)), @b date;\n"
        "SELECT OrderID, TotalDue INTO #snap FROM dbo.Orders;\n"
        "WITH c1 (P, Q) AS (SELECT 1, 2), c2 AS (SELECT P AS R FROM c1)\n"
        "SELECT d.| FROM c2 JOIN (SELECT Id, Amount AS Amt FROM #work) AS d ON 1 = 1 CROSS APPLY (SELECT 1 AS z) ap;\n"
        "DECLARE @late int;";
    const size_t caret = sql.find('|');
    std::string text = sql;
    text.erase(caret, 1);
    std::vector<LexToken> all;
    const ScriptTokens t = TokensOf(text, all);
    tsql::editor::Catalog catalog;
    catalog.currentDatabase = "SalesDb";
    catalog.objects.push_back({"Warehouse", "dbo", "Orders", tsql::editor::CatalogObject::Type::Table,
                               {{"OrderID", "int"}, {"TotalDue", "money"}}});
    const ScopeAnalyzer scope(t, catalog, t.IndexAt(caret), SIZE_MAX);
    CHECK_EQ(scope.CurrentDatabase(), std::string("Warehouse"));

    std::set<std::string> vars;
    for (const VariableInfo& v : scope.Variables()) vars.insert(v.name + (v.isTable ? ":table" : ":" + v.type));
    CHECK(vars == (std::set<std::string>{"@a:int", "@t:table", "@b:date"}));

    std::vector<TempTableInfo> temps = scope.TempTables();
    CHECK_EQ(temps.size(), size_t(2));
    if (temps.size() == 2) {
        CHECK_EQ(temps[0].name, std::string("#work"));
        CHECK_EQ(temps[0].columns.size(), size_t(2));
        if (temps[0].columns.size() == 2) CHECK_EQ(temps[0].columns[1].type, std::string("decimal(9,2)"));
        CHECK_EQ(temps[1].name, std::string("#snap"));
        CHECK_EQ(temps[1].columns.size(), size_t(2));
    }

    std::vector<CteInfo> ctes = scope.Ctes();
    CHECK_EQ(ctes.size(), size_t(2));
    if (ctes.size() == 2) {
        CHECK_EQ(ctes[0].columns.size(), size_t(2));
        CHECK_EQ(ctes[1].columns.size(), size_t(1));
        if (ctes[1].columns.size() == 1) CHECK_EQ(ctes[1].columns[0].name, std::string("R"));
    }

    std::set<std::string> exposed;
    std::vector<std::string> dColumns;
    for (const SourceInfo& s : scope.ExpressionSources()) {
        exposed.insert(s.exposed);
        if (s.exposed == "d")
            for (const ColumnInfo& c : s.columns) dColumns.push_back(c.name);
    }
    CHECK(exposed == (std::set<std::string>{"c2", "d", "ap"}));
    CHECK(dColumns == (std::vector<std::string>{"Id", "Amt"}));
}

void TestScopeQueries() {
    // ON sees the sources joined so far; a derived table does not see its siblings; APPLY does
    const std::string sql =
        "SELECT * FROM a x JOIN b y ON y.k = | JOIN c z ON 1 = 1 "
        "JOIN (SELECT 1 AS one FROM d w WHERE w.k = 1) q ON 1 = 1 "
        "CROSS APPLY (SELECT 1 AS two FROM e v WHERE v.k = 1) r";
    auto sourcesAt = [&](const std::string& marker) {
        std::string text = sql;
        const size_t bar = text.find('|');
        text.erase(bar, 1);
        const size_t at = marker.empty() ? bar : text.find(marker);
        std::vector<LexToken> all;
        const ScriptTokens t = TokensOf(text, all);
        tsql::editor::Catalog catalog;
        const ScopeAnalyzer scope(t, catalog, t.IndexAt(at), SIZE_MAX);
        std::set<std::string> names;
        for (const SourceInfo& s : scope.ExpressionSources()) names.insert(s.exposed);
        return names;
    };
    CHECK(sourcesAt("") == (std::set<std::string>{"x", "y"}));
    CHECK(sourcesAt("w.k") == (std::set<std::string>{"w"}));
    CHECK(sourcesAt("v.k") == (std::set<std::string>{"v", "x", "y", "z", "q"}));
}

void TestCapture() {
    const CaretParse ps = G().ParseToCaret("SELECT FROM WHERE;\nSELECT * FROM t WITH (");
    CHECK(ps.capture.captured);
    CHECK(!ps.capture.errorInStatement);   // the error was in the previous statement
    CHECK(ps.syntaxErrors);
    std::set<std::string> words;
    WalkInput in;
    in.tokens = &ps.tokens;
    in.upper = &ps.upper;
    in.startState = ps.capture.state;
    in.startIndex = ps.capture.index;
    in.outerFollow = ps.capture.follow;
    in.caret = ps.tokens.size();
    const WalkStats stats = Walk(G(), in, [&](const WalkCandidate& c) {
        if (c.words != nullptr) words.insert(c.words->begin(), c.words->end());
    });
    CHECK(!stats.truncated);
    CHECK(words.count("NOLOCK") == 1);
    CHECK(words.count("RECOMPILE") == 0);

    const CaretParse broken = G().ParseToCaret("SELECT a FROM t WHERE = = AND ");
    CHECK(broken.capture.captured);
    CHECK(broken.capture.errorInStatement);
    CHECK_EQ(broken.capture.statementIndex, size_t(0));

    const CaretParse empty = G().ParseToCaret("");
    CHECK(empty.capture.captured);
    CHECK(!empty.syntaxErrors);
}

void TestEdges() {
    tsql::editor::Catalog catalog;
    // carets past the end, inside a multi-byte character, in invalid UTF-8: no exception, a valid range
    const std::string bad = "SELECT \xFF\xC3 FROM t WHERE \xE2\x82";
    for (size_t caret = 0; caret <= bad.size() + 2; ++caret) {
        const auto r = tsql::editor::Complete(bad, caret, SqlVersion::Sql170, catalog);
        CHECK(r.replaceStart <= std::min(caret, bad.size()));
        CHECK(r.replaceStart + r.replaceLength <= bad.size());
    }
    for (const std::string& s : {std::string(""), bad, std::string("SELECT 'a"), std::string("/* x"), std::string("`~\x01"),
                                 std::string("SELECT [a"), std::string("\0\0", 2)}) {
        size_t pos = 0;
        for (const auto& span : tsql::editor::Classify(s, SqlVersion::Sql170)) {
            CHECK_EQ(span.start, pos);
            CHECK(span.length > 0);
            pos += span.length;
        }
        CHECK_EQ(pos, s.size());
    }
    bool threw = false;
    try {
        tsql::editor::Complete("SELECT 1", 0, SqlVersion::Sql90, catalog);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    TestNames();
    TestUtf8();
    TestBuiltins();
    TestPredicates();
    TestStatements();
    TestScope();
    TestScopeQueries();
    TestCapture();
    TestEdges();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
