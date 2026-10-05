// Unit tests for the internals of the editor support (src/editor): name quoting, UTF-8 offsets,
// built-in tables, predicate evaluation, statement segmentation, scope analysis, the parser capture,
// the ATN walk, incremental lexing and parsing (a Document after edits equals the free functions on
// its text), and Warm running concurrently with Documents. The contract tests are in tests/editor.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <set>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Buffer.h"
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
    // a parenthesized query after one; not a function argument or an INSERT's source
    CHECK_EQ(StatementAt("SELECT a FROM t WHERE (a = 1)\n(((SELECT b FROM u)))", "b FROM"), std::string("("));
    CHECK_EQ(StatementAt("SELECT f((SELECT 1)), b FROM t", "b FROM"), std::string("SELECT"));
    CHECK_EQ(StatementAt("INSERT INTO t (a) (SELECT b FROM u)", "b FROM"), std::string("INSERT"));
    CHECK_EQ(StatementAt("ALTER RESOURCE GOVERNOR RECONFIGURE", "RECONFIGURE"), std::string("ALTER"));
    CHECK_EQ(StatementAt("ALTER RESOURCE GOVERNOR DISABLE\nRECONFIGURE", "RECONFIGURE"), std::string("RECONFIGURE"));
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
                               {{"OrderID", "int"}, {"TotalDue", "money"}}, {}, {}});
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

/// The parse of `text` from its start to its end (the caret).
CaretParse Prefix(const std::string& text) {
    Buffer b(G());
    b.SetText(text);
    return G().ParseToCaret(b.View(), ResumePoint{}, b.Tokens().size())->result;
}

void TestCapture() {
    const CaretParse ps = Prefix("SELECT FROM WHERE;\nSELECT * FROM t WITH (");
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

    const CaretParse broken = Prefix("SELECT a FROM t WHERE = = AND ");
    CHECK(broken.capture.captured);
    CHECK(broken.capture.errorInStatement);
    CHECK_EQ(broken.capture.statementIndex, size_t(0));

    const CaretParse empty = Prefix("");
    CHECK(empty.capture.captured);
    CHECK(!empty.syntaxErrors);

    // the parser stands inside identifier, called from tablePeriodDefinition with a Match(SYSTEM_TIME)
    // check: the walk starts past that call and must still apply the check
    const CaretParse period = Prefix("ALTER TABLE t ADD PERIOD FOR ");
    CHECK(period.capture.captured);
    CHECK(period.capture.pendingCall >= 0);
    WalkInput pin;
    pin.tokens = &period.tokens;
    pin.upper = &period.upper;
    pin.startState = period.capture.state;
    pin.startIndex = period.capture.index;
    pin.outerFollow = period.capture.follow;
    pin.startPending = period.capture.pendingCall;
    pin.caret = period.tokens.size();
    bool freeName = false;
    std::set<std::string> periodWords;
    Walk(G(), pin, [&](const WalkCandidate& c) {
        if (c.words != nullptr) periodWords.insert(c.words->begin(), c.words->end());
        else if (c.tokenType == static_cast<size_t>(tsql::ast::TSqlTokenType::Identifier)) freeName = true;
    });
    CHECK(!freeName);
    CHECK(periodWords.count("SYSTEM_TIME") == 1);
}

// ------------------------------------------------------------------------------- Document

bool SameSpans(const std::vector<tsql::editor::ColouredSpan>& a, const std::vector<tsql::editor::ColouredSpan>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].start != b[i].start || a[i].length != b[i].length || a[i].cls != b[i].cls) return false;
    return true;
}

bool SameCompletion(const tsql::editor::CompletionResult& a, const tsql::editor::CompletionResult& b) {
    if (a.replaceStart != b.replaceStart || a.replaceLength != b.replaceLength || a.items.size() != b.items.size())
        return false;
    for (size_t i = 0; i < a.items.size(); ++i)
        if (a.items[i].kind != b.items[i].kind || a.items[i].label != b.items[i].label ||
            a.items[i].insertText != b.items[i].insertText || a.items[i].detail != b.items[i].detail)
            return false;
    return true;
}

tsql::editor::Catalog SmallCatalog() {
    using OT = tsql::editor::CatalogObject::Type;
    tsql::editor::Catalog c;
    c.currentDatabase = "Sales";
    c.databases = {"Sales", "master"};
    c.objects.push_back({"Sales", "dbo", "Orders", OT::Table, {{"OrderID", "int"}, {"Status", "int"}}, {}, {}});
    c.objects.push_back({"Sales", "dbo", "usp_Get", OT::Procedure, {}, {{"@id", "int", false, false}}, {}});
    c.objects.push_back({"", "sys", "objects", OT::View, {{"object_id", "int"}, {"name", "sysname"}}, {}, {}});
    return c;
}

/// Edits that make and break statements, batches, comments, strings and brackets.
const char* const kFragments[] = {
    "SELECT ", "a", " FROM ", "dbo.Orders", " o", " WHERE ", "o.Status = 1", ";", "\n", "\nGO\n", "GO", "BEGIN ",
    " END", "(", ")", ",", "'", "'x'", "/*", "*/", "--", "[", "]", "\"", "UPDATE t SET a = 1 ", "SET @x = 2",
    "DECLARE @x int", "IF 1 = 1 ", "ELSE ", "WITH c AS (SELECT 1 AS n) ", "INSERT INTO t ", "EXEC dbo.usp_Get ",
    "CREATE VIEW v AS ", "SET QUOTED_IDENTIFIER OFF", "SET QUOTED_IDENTIFIER ON", "\"q\"", "CASE WHEN ", " THEN 1 END",
    "N'\xC3\xA4'", "\xE2\x82\xAC", "\xFF", "x", " ", "1e", "0x", "$1", "@", "#t", "OPTION (RECOMPILE)",
};

const char* const kScripts[] = {
    "SELECT o.OrderID, o.Status FROM dbo.Orders AS o WHERE o.Status = 1 ORDER BY o.OrderID;\n"
    "UPDATE dbo.Orders SET Status = 2 WHERE OrderID = 1\nSET @x = 1\n"
    "DECLARE c CURSOR FOR SELECT OrderID FROM dbo.Orders\nGO\n"
    "IF EXISTS (SELECT 1 FROM sys.objects) BEGIN SELECT 1; SELECT 2 END ELSE SELECT 3\n"
    "WITH x AS (SELECT 1 AS n) SELECT n FROM x;\nEXEC dbo.usp_Get @id = 1\nGO\nCREATE VIEW v AS SELECT 1 AS a\n",
    "SET QUOTED_IDENTIFIER OFF\nSELECT \"text\" FROM t\nGO\nSELECT \"col\" FROM t\n/* comment\n spanning */ SELECT 1 -- x\n",
    "BEGIN TRY\n  INSERT INTO t (a) VALUES (1)\nEND TRY\nBEGIN CATCH\n  THROW\nEND CATCH\nMERGE t USING s ON t.a = s.a "
    "WHEN MATCHED THEN UPDATE SET a = 1;\n",
    // an error the script rule does not recover from ends the first batch's parse
    "SELECT 1;\n)\nGO\nSET NOCOUNT ON\nSELECT o.Status FROM dbo.Orders o\nGO\nGO\nSELECT 2\n",
};

/// For random edit sequences, a Document's results equal the free functions' on its text.
void TestDocumentMatchesFreeFunctions() {
    using namespace tsql::editor;
    const Catalog catalog = SmallCatalog();
    uint32_t seed = 12345;
    auto next = [&](uint32_t n) {
        seed = seed * 1103515245u + 12345u;
        return n == 0 ? 0u : (seed >> 8) % n;
    };
    int mismatches = 0;
    for (SqlVersion version : {SqlVersion::Sql170, SqlVersion::Sql130}) {
        for (const char* script : kScripts) {
            Document d(version);
            d.SetText(script);
            for (int step = 0; step < 60 && mismatches < 5; ++step) {
                const std::string& text = d.Text();
                const size_t at = next(static_cast<uint32_t>(text.size() + 1));
                const size_t remove = next(4) == 0 ? next(static_cast<uint32_t>(std::min<size_t>(12, text.size() - at) + 1)) : 0;
                const char* insert = next(5) == 0 ? "" : kFragments[next(sizeof(kFragments) / sizeof(kFragments[0]))];
                d.Edit(at, remove, insert);
                const std::string now = d.Text();
                // query in an order that leaves parts unparsed: a viewport, a caret, everything
                const size_t vs = next(static_cast<uint32_t>(now.size() + 1));
                const size_t ve = std::min(now.size(), vs + next(80));
                const auto full = Classify(now, version);
                std::vector<ColouredSpan> expected;
                for (const auto& s : full)   // an empty range overlaps nothing
                    if (vs < ve && s.start < ve && s.start + s.length > vs) expected.push_back(s);
                std::string what;
                if (!SameSpans(d.Classify(vs, ve), expected)) what += " viewport Classify";
                const size_t caret = next(static_cast<uint32_t>(now.size() + 1));
                const CompletionResult got = d.Complete(caret, catalog), want = Complete(now, caret, version, catalog);
                if (!SameCompletion(got, want))
                    what += " Complete (" + std::to_string(got.items.size()) + " items, not " +
                            std::to_string(want.items.size()) + ")";
                if (step % 3 == 0 && !SameSpans(d.Classify(0, now.size()), full)) what += " full Classify";
                if (!what.empty()) {
                    ++mismatches;
                    std::printf("%s: Document differs from the free functions in%s after %d edits (last: %zu,%zu,'%s'), "
                                "viewport [%zu,%zu), caret %zu:\n%s\n",
                                GrammarName(version), what.c_str(), step + 1, at, remove, insert, vs, ve, caret, now.c_str());
                }
            }
        }
    }
    CHECK_EQ(mismatches, 0);
}

/// After edits, a Buffer's tokens are those of a fresh lex of its text.
void TestIncrementalLex() {
    Buffer b(G());
    std::string text = kScripts[0];
    b.SetText(text);
    uint32_t seed = 777;
    auto next = [&](uint32_t n) {
        seed = seed * 1103515245u + 12345u;
        return n == 0 ? 0u : (seed >> 8) % n;
    };
    int mismatches = 0;
    for (int step = 0; step < 400; ++step) {
        const size_t at = next(static_cast<uint32_t>(text.size() + 1));
        const size_t remove = next(3) == 0 ? std::min<size_t>(next(20), text.size() - at) : 0;
        const std::string insert = next(4) == 0 ? "" : kFragments[next(sizeof(kFragments) / sizeof(kFragments[0]))];
        b.Edit(at, remove, insert);
        text.replace(at, remove, insert);
        std::vector<LexToken> fresh;
        G().Lex(text, fresh);
        const auto& kept = b.Tokens();
        bool same = kept.size() == fresh.size() && b.Text() == text;
        for (size_t i = 0; same && i < kept.size(); ++i)
            same = kept[i].type == fresh[i].type && kept[i].start == fresh[i].start && kept[i].end == fresh[i].end;
        if (!same && ++mismatches <= 3) std::printf("tokens differ after edit %zu,%zu,'%s':\n%s\n", at, remove, insert.c_str(), text.c_str());
        // the parser-visible tokens (kept up to date by the edit once built) equal those of the fresh lex
        const std::vector<LexToken>& visible = b.Visible();
        const std::vector<uint32_t>& toToken = b.VisibleToToken();
        size_t v = 0;
        bool sameVisible = visible.size() == toToken.size();
        for (size_t i = 0; sameVisible && i < fresh.size(); ++i) {
            if (IsHiddenType(fresh[i].type)) continue;
            sameVisible = v < visible.size() && toToken[v] == i && visible[v].type == fresh[i].type &&
                          visible[v].start == fresh[i].start && visible[v].end == fresh[i].end;
            ++v;
        }
        if (!sameVisible || v != visible.size()) ++mismatches;
    }
    CHECK_EQ(mismatches, 0);
}

/// A long script: a viewport or a caret after an edit is parsed again near the edit only.
void TestDocumentReusesTheParse() {
    using namespace tsql::editor;
    std::string script;
    for (int i = 0; i < 400; ++i) script += "SELECT o.OrderID FROM dbo.Orders AS o WHERE o.Status = " + std::to_string(i) + ";\n";
    const Catalog catalog = SmallCatalog();
    Document d(SqlVersion::Sql170);
    d.SetText(script);
    CHECK(SameSpans(d.Classify(0, script.size()), Classify(script, SqlVersion::Sql170)));
    const size_t at = script.find("= 200;") + 2;
    d.Edit(at, 3, "x.");
    const std::string now = d.Text();
    CHECK(SameCompletion(d.Complete(at + 2, catalog), Complete(now, at + 2, SqlVersion::Sql170, catalog)));
    CHECK(SameSpans(d.Classify(0, now.size()), Classify(now, SqlVersion::Sql170)));
}

/// Each batch after a GO is parsed on its own: also after an error that ends the script rule's
/// parse of an earlier batch, and a query in a later batch does not parse the batches before it.
void TestBatchesParsedIndependently() {
    using namespace tsql::editor;
    const std::string sql = "SELECT 1;\n)\nGO\nSET NOCOUNT ON";
    bool keyword = false;
    for (const ColouredSpan& s : Classify(sql, SqlVersion::Sql170))
        if (sql.substr(s.start, s.length) == "NOCOUNT") keyword = s.cls == TokenClass::Keyword;
    CHECK(keyword);
    // the GO taken out: the merged batch's parse ends at ')' again and NOCOUNT stays untaken
    Document d(SqlVersion::Sql170);
    d.SetText(sql);
    d.Classify(0, sql.size());
    d.Edit(sql.find("GO"), 3, "");
    CHECK(SameSpans(d.Classify(0, d.Text().size()), Classify(d.Text(), SqlVersion::Sql170)));

    std::string script;
    for (int i = 0; i < 50; ++i) script += "SELECT o.Status FROM dbo.Orders AS o\nGO\n";
    Buffer b(G());
    b.SetText(script);
    const size_t o = b.Tokens().size() - 4;   // the last batch's o (then newline, GO, newline)
    b.EnsureParsed(o, o + 1);
    CHECK(b.Roles()[o].state >= 0);
    CHECK_EQ(b.Roles()[0].state, -1);         // the first batch's SELECT: not parsed
    CHECK(b.ResumeAtOrBefore(o).kind == ResumePoint::Kind::InBatch);
}

/// Warm on some threads while Documents of the same and other versions are used on others.
void TestWarmConcurrently() {
    using namespace tsql::editor;
    const Catalog catalog = SmallCatalog();
    std::vector<SqlVersion> versions;
    for (SqlVersion v : {SqlVersion::Sql130, SqlVersion::Sql140, SqlVersion::Sql150, SqlVersion::Sql160,
                         SqlVersion::Sql170, SqlVersion::Sql180, SqlVersion::SqlFabricDW})
        if (IsParserAvailable(v)) versions.push_back(v);
    // what each Document must produce, computed up front on this thread
    struct Expected {
        SqlVersion version;
        std::string text;
        std::vector<ColouredSpan> spans;
        CompletionResult completion;
    };
    std::vector<Expected> expected;
    for (SqlVersion v : versions)
        for (const char* s : kScripts) {
            const std::string text = s;
            expected.push_back({v, text, Classify(text, v), Complete(text, text.size() / 2, v, catalog)});
        }
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (size_t k = t; k < versions.size() * 2; k += 4) Warm(versions[k % versions.size()]);
        });
    for (size_t t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (int round = 0; round < 3; ++round)
                for (size_t k = t; k < expected.size(); k += 4) {
                    const Expected& e = expected[k];
                    Document d(e.version);
                    d.SetText(e.text);
                    if (!SameCompletion(d.Complete(e.text.size() / 2, catalog), e.completion)) ++wrong;
                    d.Edit(0, 0, " ");
                    d.Edit(0, 1, "");
                    if (!SameSpans(d.Classify(0, e.text.size()), e.spans)) ++wrong;
                }
        });
    for (std::thread& t : threads) t.join();
    CHECK_EQ(wrong.load(), 0);
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

/// A syntax error inside a CREATE OR ALTER PROCEDURE body made the parser dereference the null
/// statement it builds for it (createOrAlterStatements): Complete, Classify and a Document over
/// such text must work and still complete the columns of the statement at the caret.
void TestErrorInCreateOrAlterBody() {
    using namespace tsql::editor;
    const Catalog catalog = SmallCatalog();
    const std::string sql = "CREATE OR ALTER PROCEDURE dbo.p AS SELECT 1 FROM dbo.Orders o WHERE o.| GROUP BY c";
    const size_t caret = sql.find('|');
    std::string text = sql;
    text.erase(caret, 1);
    for (SqlVersion v : {SqlVersion::Sql130, SqlVersion::Sql140, SqlVersion::Sql150, SqlVersion::Sql160,
                         SqlVersion::Sql170, SqlVersion::Sql180, SqlVersion::SqlFabricDW}) {
        if (!IsParserAvailable(v)) continue;
        const CompletionResult r = Complete(text, caret, v, catalog);
        CHECK(std::any_of(r.items.begin(), r.items.end(),
                          [](const CompletionItem& i) { return i.kind == CompletionKind::Column && i.label == "Status"; }));
        CHECK(!Classify(text, v).empty());
        Document doc(v);
        doc.SetText(text);
        CHECK_EQ(doc.Complete(caret, catalog).items.size(), r.items.size());
        CHECK(!doc.Classify(0, text.size()).empty());
    }
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
    TestErrorInCreateOrAlterBody();
    TestIncrementalLex();
    TestDocumentMatchesFreeFunctions();
    TestDocumentReusesTheParse();
    TestBatchesParsedIndependently();
    TestWarmConcurrently();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
