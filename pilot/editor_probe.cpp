// tsql_editor_probe: shows what the editor-support layer computes at a caret, and measures it.
//   tsql_editor_probe [--version TSql170] [mode] <sql file | -e 'sql with | at the caret'>
// The caret is the first '|' in the text (removed before use; none: the end of the text).
// Modes:
//   (none)                 Complete() items at the caret
//   --walk                 the parser capture and the raw ATN walk (candidate tokens and rule paths)
//   --classify             Classify() spans
//   --bench complete|classify ROUNDS
//                          latency of the free functions: the first call (cold: grammar set-up,
//                          empty ANTLR caches) and the median of ROUNDS further calls (warm)
//   --docbench ROUNDS [--warm]
//                          a Document of the text: SetText, the first Complete at the caret and
//                          Classify of the 100-line viewport around it (after Warm with --warm), then
//                          ROUNDS times a one-character edit at the caret followed by Complete, by a
//                          viewport Classify and by a whole-text Classify (medians)
//   --warmtime             Warm() of every grammar of the build, each in a fresh state (run once per
//                          process: the first Warm of a version is the cold one)
//   --sweep                Complete() at every byte offset: slowest call, failures
//   --overoffer STRIDE FILE... [--verbose | --all]
//                          at every STRIDE-th token start after whitespace whose batch prefix parses,
//                          the offered keywords that make `prefix KEYWORD` fail to parse (--verbose:
//                          list each; --all: list every offered keyword, marked valid or invalid)
//   --segments FILE...     statement starts of the token scanner (StatementStartFromTokens) against
//                          the parser's (Document resume points) in batches that parse
// Completion runs against a synthetic catalog of 5 databases x 400 objects x 12 columns plus a
// system catalog the size of SQL Server's (sys.all_objects / all_columns / all_parameters).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Buffer.h"
#include "Grammar.h"
#include "Scope.h"
#include "Walker.h"
#include "antlr4-runtime.h"
#include "tsql/ast/generated/token_types.hpp"
#include "tsql/editor.hpp"

using namespace tsql;
using Clock = std::chrono::steady_clock;

namespace {

const char* KindName(editor::CompletionKind k) {
    static const char* names[] = {"Keyword",      "Database",      "Schema",        "Table",     "View",
                                  "Column",       "ScalarFunction", "TableFunction", "Procedure", "Alias",
                                  "Cte",          "TempTable",     "TableVariable", "Variable",  "DataType",
                                  "TableHint",    "QueryHint",     "BuiltinFunction", "Parameter", "Synonym",
                                  "UserType"};
    return names[static_cast<int>(k)];
}

const char* ClassName(editor::TokenClass c) {
    static const char* names[] = {"Keyword", "Identifier", "QuotedIdentifier", "Variable", "String", "Number", "Comment",
                                  "Operator", "Punctuation", "BuiltinFunction", "DataType", "Whitespace", "Error"};
    return names[static_cast<int>(c)];
}

double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

double Median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

editor::Catalog SyntheticCatalog() {
    using OT = editor::CatalogObject::Type;
    editor::Catalog c;
    c.currentDatabase = "Db0";
    for (int d = 0; d < 5; ++d) {
        const std::string db = "Db" + std::to_string(d);
        c.databases.push_back(db);
        for (int o = 0; o < 400; ++o) {
            editor::CatalogObject obj;
            obj.database = db;
            obj.schema = o % 4 == 0 ? "sales" : "dbo";
            obj.name = (o % 10 == 9 ? "usp_Proc" : "Table") + std::to_string(o);
            obj.type = o % 10 == 9 ? OT::Procedure : OT::Table;
            if (obj.type == OT::Table)
                for (int k = 0; k < 12; ++k) obj.columns.push_back({"Column" + std::to_string(k), "int"});
            else
                for (int k = 0; k < 4; ++k) obj.parameters.push_back({"@p" + std::to_string(k), "int", k == 3, k == 2});
            c.objects.push_back(std::move(obj));
        }
        for (int s = 0; s < 20; ++s)
            c.objects.push_back({db, "dbo", "syn" + std::to_string(s), OT::Synonym, {}, {}, "dbo.Table" + std::to_string(s)});
        for (int t = 0; t < 20; ++t) {
            editor::CatalogType type{db, "dbo", "Type" + std::to_string(t), t % 2 == 0, {}};
            if (type.isTableType) type.columns = {{"Id", "int"}, {"Value", "nvarchar(100)"}};
            c.types.push_back(std::move(type));
        }
    }
    // the objects the bench scripts use
    for (const char* name : {"Orders", "Customers"}) {
        editor::CatalogObject obj{"Db0", "dbo", name, OT::Table, {}, {}, {}};
        for (const char* col : {"OrderID", "CustomerID", "CustomerName", "Region", "OrderDate", "Status", "TotalDue"})
            obj.columns.push_back({col, "int"});
        c.objects.push_back(std::move(obj));
    }
    // a system catalog the size of SQL Server 2022's: ~600 views and table-valued functions of
    // ~20 columns, ~1,500 procedures, ~300 scalar functions, INFORMATION_SCHEMA
    for (const char* name : {"dm_exec_query_stats", "dm_exec_requests", "dm_exec_sessions", "objects", "indexes",
                             "columns", "tables", "dm_db_index_usage_stats", "dm_os_wait_stats", "partitions"}) {
        editor::CatalogObject obj{"", "sys", name, OT::View, {}, {}, {}};
        for (int k = 0; k < 40; ++k) obj.columns.push_back({"col_" + std::to_string(k), "bigint"});
        c.objects.push_back(std::move(obj));
    }
    for (int v = 0; v < 600; ++v) {
        editor::CatalogObject obj{"", "sys", (v % 3 == 0 ? "dm_view_" : "catalog_view_") + std::to_string(v),
                                  v % 7 == 0 ? OT::TableFunction : OT::View, {}, {}, {}};
        for (int k = 0; k < 20; ++k) obj.columns.push_back({"column_" + std::to_string(k), "int"});
        if (obj.type == OT::TableFunction) obj.parameters = {{"@a", "int", false, false}, {"@b", "int", false, true}};
        c.objects.push_back(std::move(obj));
    }
    for (int p = 0; p < 1500; ++p) {
        editor::CatalogObject obj{"", "sys", "sp_proc_" + std::to_string(p), OT::Procedure, {}, {}, {}};
        for (int k = 0; k < 5; ++k) obj.parameters.push_back({"@param" + std::to_string(k), "nvarchar(128)", false, k > 1});
        c.objects.push_back(std::move(obj));
    }
    for (int f = 0; f < 300; ++f)
        c.objects.push_back({"", "sys", "fn_func_" + std::to_string(f), OT::ScalarFunction, {}, {{"@x", "int", false, false}}, {}});
    for (int v = 0; v < 25; ++v) {
        editor::CatalogObject obj{"", "INFORMATION_SCHEMA", "VIEW_" + std::to_string(v), OT::View, {}, {}, {}};
        for (int k = 0; k < 10; ++k) obj.columns.push_back({"COLUMN_" + std::to_string(k), "nvarchar(128)"});
        c.objects.push_back(std::move(obj));
    }
    return c;
}

std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// The parse of `text` from its start up to its end (Grammar::ParseToCaret on a Buffer of it).
std::unique_ptr<editor::detail::CaretSession> ParsePrefix(const editor::detail::Grammar& g, editor::detail::Buffer& b,
                                                          std::string_view text) {
    b.SetText(text);
    return g.ParseToCaret(b.View(), editor::detail::ResumePoint{}, b.Tokens().size());
}

int Walk(SqlVersion version, const std::string& sql, size_t caret) {
    const auto& g = editor::detail::GrammarFor(version);
    editor::detail::Buffer buffer(g);
    auto t0 = Clock::now();
    auto session = ParsePrefix(g, buffer, std::string_view(sql).substr(0, caret));
    const auto& ps = session->result;
    auto t1 = Clock::now();
    const auto& cap = ps.capture;
    std::printf("parse %.2f ms, %zu tokens, captured %d state %d index %zu follow %zu; statement state %d index %zu "
                "error %d at %zu\n",
                Ms(t0, t1), ps.tokens.size(), cap.captured, cap.state, cap.index, cap.follow.size(), cap.statementState,
                cap.statementIndex, cap.errorInStatement, cap.errorIndex);
    if (!cap.captured) return 0;
    auto ruleOf = [&](int s) { return (*g.ruleNames)[g.atn->states[static_cast<size_t>(s)]->ruleIndex]; };
    std::printf("at %s", ruleOf(cap.state).c_str());
    for (int f : cap.follow) std::printf(" < %s", ruleOf(f).c_str());
    std::printf("\n");
    editor::detail::WalkInput in;
    in.tokens = &ps.tokens;
    in.upper = &ps.upper;
    in.startState = cap.errorInStatement ? cap.statementState : cap.state;
    in.startIndex = cap.errorInStatement ? cap.statementIndex : cap.index;
    in.outerFollow = cap.errorInStatement ? cap.statementFollow : cap.follow;
    in.startPending = cap.errorInStatement ? -1 : cap.pendingCall;
    in.caret = ps.tokens.size();
    const editor::detail::Capture* locals = cap.errorInStatement ? nullptr : &cap;
    in.evaluate = [&](size_t rule, size_t pred, size_t at, bool live) {
        const int v = session->EvaluatePredicate(rule, pred, at, live ? locals : nullptr);
        std::printf("  predicate %zu (%s) at %zu%s: %d\n", pred, (*g.ruleNames)[rule].c_str(), at, live ? " live" : "", v);
        return v;
    };
    std::map<std::string, std::set<std::string>> seen;
    auto stats = editor::detail::Walk(g, in, [&](const editor::detail::WalkCandidate& c) {
        std::string tok(g.vocabulary->getSymbolicName(c.tokenType));
        if (c.words) {
            tok += "{";
            for (auto& w : *c.words) tok += w + " ";
            tok += "}";
        }
        if (c.hints) tok += "+hints" + std::to_string(c.hints->size());
        if (c.nextStatement) tok += " (next statement)";
        std::string path;
        for (size_t k = 0; k < c.rules->size() && k < 8; ++k) path += (*g.ruleNames)[(*c.rules)[k]] + "<";
        seen[tok].insert(path);
    });
    std::printf("walk %.2f ms, visited %zu truncated %d, %zu token kinds\n", Ms(t1, Clock::now()), stats.visited,
                stats.truncated, seen.size());
    for (auto& [tok, paths] : seen) {
        std::printf("%s\n", tok.c_str());
        size_t n = 0;
        for (auto& p : paths) {
            if (++n > 6) {
                std::printf("    ... %zu paths\n", paths.size());
                break;
            }
            std::printf("    %s\n", p.c_str());
        }
    }
    return 0;
}

int Bench(SqlVersion version, const std::string& sql, size_t caret, const std::string& op, int rounds) {
    const editor::Catalog catalog = SyntheticCatalog();
    auto once = [&]() -> size_t {
        if (op == "classify") return editor::Classify(sql, version).size();
        return editor::Complete(sql, caret, version, catalog).items.size();
    };
    auto t0 = Clock::now();
    const size_t n = once();
    const double cold = Ms(t0, Clock::now());
    std::vector<double> warm;
    for (int r = 0; r < rounds; ++r) {
        auto a = Clock::now();
        once();
        warm.push_back(Ms(a, Clock::now()));
    }
    std::printf("%s: %zu bytes, %zu %s, cold %.2f ms, warm median %.2f ms (min %.2f, max %.2f, %d rounds)\n", op.c_str(),
                sql.size(), n, op == "classify" ? "spans" : "items", cold, Median(warm),
                *std::min_element(warm.begin(), warm.end()), *std::max_element(warm.begin(), warm.end()), rounds);
    return 0;
}

/// The 100 lines around byte `at`.
std::pair<size_t, size_t> Viewport(const std::string& sql, size_t at) {
    size_t start = at, end = at;
    for (int lines = 0; lines < 50 && start > 0; start = start > 0 ? start - 1 : 0)
        if (sql[start - 1] == '\n' && ++lines == 50) break;
    for (int lines = 0; lines < 50 && end < sql.size(); ++end)
        if (sql[end] == '\n') ++lines;
    return {start, end};
}

int DocBench(SqlVersion version, const std::string& sql, size_t caret, int rounds, bool warm) {
    const editor::Catalog catalog = SyntheticCatalog();
    if (warm) {
        auto w = Clock::now();
        editor::Warm(version);
        std::printf("warm: %.2f ms\n", Ms(w, Clock::now()));
    }
    editor::Document d(version);
    auto t0 = Clock::now();
    d.SetText(sql);
    auto t1 = Clock::now();
    const size_t items = d.Complete(caret, catalog).items.size();
    auto t2 = Clock::now();
    auto [vs, ve] = Viewport(sql, caret);
    const size_t spans = d.Classify(vs, ve).size();
    auto t3 = Clock::now();
    std::printf("%zu bytes: SetText %.2f ms; first Complete %.2f ms (%zu items); first viewport Classify [%zu,%zu) %.2f ms "
                "(%zu spans)\n",
                sql.size(), Ms(t0, t1), Ms(t1, t2), items, vs, ve, Ms(t2, t3), spans);
    std::vector<double> complete, viewport, full, edit;
    for (int r = 0; r < rounds; ++r) {
        // type a character at the caret, then take it back
        const bool insert = r % 2 == 0;
        auto a = Clock::now();
        if (insert) d.Edit(caret, 0, "x");
        else d.Edit(caret, 1, "");
        auto b = Clock::now();
        d.Complete(caret + (insert ? 1 : 0), catalog);
        auto c = Clock::now();
        edit.push_back(Ms(a, b));
        complete.push_back(Ms(a, c));
        if (insert) d.Edit(caret, 1, "");
        else d.Edit(caret, 0, "x");
        a = Clock::now();
        d.Classify(vs, ve);
        viewport.push_back(Ms(a, Clock::now()));
        if (insert) d.Edit(caret, 0, "x");
        else d.Edit(caret, 1, "");
        a = Clock::now();
        d.Classify(0, d.Text().size());
        full.push_back(Ms(a, Clock::now()));
    }
    std::printf("edit %.3f ms; edit + Complete %.2f ms (max %.2f); edit + viewport Classify %.2f ms (max %.2f); edit + "
                "full Classify %.2f ms (max %.2f); %d rounds\n",
                Median(edit), Median(complete), *std::max_element(complete.begin(), complete.end()), Median(viewport),
                *std::max_element(viewport.begin(), viewport.end()), Median(full),
                *std::max_element(full.begin(), full.end()), rounds);
    return 0;
}

int WarmTime() {
    for (SqlVersion v : {SqlVersion::Sql130, SqlVersion::Sql140, SqlVersion::Sql150, SqlVersion::Sql160, SqlVersion::Sql170,
                         SqlVersion::Sql180, SqlVersion::SqlFabricDW}) {
        if (!IsParserAvailable(v)) continue;
        auto t = Clock::now();
        editor::Warm(v);
        const double first = Ms(t, Clock::now());
        t = Clock::now();
        editor::Warm(v);
        std::printf("%s: Warm %.1f ms (again %.1f ms)\n", GrammarName(v), first, Ms(t, Clock::now()));
    }
    return 0;
}

int Sweep(SqlVersion version, const std::string& sql) {
    const editor::Catalog catalog = SyntheticCatalog();
    double worst = 0;
    size_t worstAt = 0, failures = 0, items = 0;
    editor::Document d(version);
    d.SetText(sql);
    auto t0 = Clock::now();
    for (size_t caret = 0; caret <= sql.size(); ++caret) {
        auto a = Clock::now();
        try {
            const auto r = d.Complete(caret, catalog);
            items += r.items.size();
            if (r.replaceStart > caret || r.replaceStart + r.replaceLength > sql.size() ||
                caret > r.replaceStart + r.replaceLength) {
                ++failures;
                std::printf("bad replace range at %zu: {%zu, %zu}\n", caret, r.replaceStart, r.replaceLength);
            }
        } catch (const std::exception& e) {
            ++failures;
            std::printf("exception at %zu: %s\n", caret, e.what());
        }
        const double ms = Ms(a, Clock::now());
        if (ms > worst) {
            worst = ms;
            worstAt = caret;
        }
    }
    std::printf("sweep: %zu carets, %.1f ms total, slowest %.2f ms at %zu, %zu items, %zu failures\n", sql.size() + 1,
                Ms(t0, Clock::now()), worst, worstAt, items, failures);
    return failures == 0 ? 0 : 1;
}

bool PrefixHasError(const editor::detail::Grammar& g, editor::detail::Buffer& b, std::string_view text) {
    const auto session = ParsePrefix(g, b, text);
    return session->result.syntaxErrors || !session->result.capture.captured;
}

int OverOffer(SqlVersion version, const std::vector<std::string>& files, size_t stride, bool verbose, bool listAll) {
    using editor::CompletionKind;
    const auto& g = editor::detail::GrammarFor(version);
    editor::detail::Buffer scratch(g);
    const editor::Catalog catalog = SyntheticCatalog();
    size_t carets = 0, offered = 0, invalid = 0, seen = 0;
    std::map<std::string, size_t> offenders;
    for (const std::string& path : files) {
        const std::string sql = ReadFile(path);
        std::vector<editor::detail::LexToken> all;
        g.Lex(sql, all);
        editor::Document d(version);
        d.SetText(sql);
        size_t batchStart = 0;
        for (size_t i = 1; i < all.size(); ++i) {
            if (all[i - 1].type == static_cast<uint32_t>(ast::TSqlTokenType::Go)) batchStart = all[i].start;
            if (all[i - 1].type != static_cast<uint32_t>(ast::TSqlTokenType::WhiteSpace)) continue;
            if (editor::detail::IsHiddenType(all[i].type)) continue;
            if (seen++ % stride != 0) continue;
            const size_t caret = all[i].start;
            const std::string prefix = sql.substr(batchStart, caret - batchStart);
            if (PrefixHasError(g, scratch, prefix)) continue;
            ++carets;
            const auto r = d.Complete(caret, catalog);
            for (const auto& item : r.items) {
                if (item.kind != CompletionKind::Keyword && item.kind != CompletionKind::TableHint &&
                    item.kind != CompletionKind::QueryHint)
                    continue;
                ++offered;
                const bool bad = PrefixHasError(g, scratch, prefix + item.insertText + " ");
                if (bad) ++invalid, ++offenders[item.insertText];
                if (verbose && (bad || listAll)) {
                    const size_t from = prefix.size() > 80 ? prefix.size() - 80 : 0;
                    std::printf("  %s @%zu: ...%s| %s%s\n", path.c_str(), caret, prefix.substr(from).c_str(),
                                item.insertText.c_str(), listAll ? (bad ? "  [invalid]" : "  [valid]") : "");
                }
            }
        }
    }
    std::printf("over-offer: %zu files, %zu carets, %zu keywords offered, %zu invalid (%.2f%%)\n", files.size(), carets,
                offered, invalid, offered ? 100.0 * static_cast<double>(invalid) / static_cast<double>(offered) : 0.0);
    std::vector<std::pair<size_t, std::string>> top;
    for (auto& [w, n] : offenders) top.emplace_back(n, w);
    std::sort(top.rbegin(), top.rend());
    for (size_t k = 0; k < top.size() && k < 25; ++k) std::printf("  %6zu %s\n", top[k].first, top[k].second.c_str());
    return 0;
}

/// Whether the statement at token `p` holds statements of its own (a block, IF, WHILE, a module
/// body, CREATE SCHEMA's elements, a cursor's query): the scanner finds those, the batch's resume
/// points do not.
bool Compound(const editor::detail::ScriptTokens& t, size_t p) {
    using T = ast::TSqlTokenType;
    auto is = [&](size_t i, T type) { return t.Type(i) == static_cast<uint32_t>(type); };
    if (is(p, T::Begin) || is(p, T::If) || is(p, T::While)) return true;
    if (is(p, T::Declare)) return t.Is(p + 2, "CURSOR") || t.Is(p + 3, "CURSOR") || t.Is(p + 4, "CURSOR");
    // SET @c = CURSOR ... FOR <query>: the cursor's query, as in DECLARE CURSOR
    if (is(p, T::Set)) return is(p + 1, T::Variable) && is(p + 2, T::EqualsSign) && t.Is(p + 3, "CURSOR");
    if (!is(p, T::Create) && !is(p, T::Alter)) return false;
    size_t k = p + 1;
    if (is(k, T::Or)) k += 2;   // CREATE OR ALTER
    return is(k, T::Procedure) || is(k, T::Proc) || is(k, T::Function) || is(k, T::Trigger) || is(k, T::Schema);
}

/// Statement starts: the token scanner's against the parser's, in the batches that parse.
int Segments(SqlVersion version, const std::vector<std::string>& files, bool verbose) {
    const auto& g = editor::detail::GrammarFor(version);
    editor::detail::Buffer scratch(g);
    size_t batches = 0, statements = 0, missed = 0, extra = 0, nested = 0;
    for (const std::string& path : files) {
        const std::string sql = ReadFile(path);
        editor::detail::Buffer b(g);
        b.SetText(sql);
        b.EnsureParsed(0, b.Tokens().size());
        const auto& visible = b.Visible();
        const auto& toTok = b.VisibleToToken();
        const editor::detail::ScriptTokens tokens(sql, visible.data(), visible.size());
        size_t from = 0;
        for (size_t end = 0; end <= visible.size(); ++end) {
            if (end < visible.size() && visible[end].type != static_cast<uint32_t>(ast::TSqlTokenType::Go)) continue;
            // batch [from, end)
            if (end > from) {
                const size_t a = visible[from].start, z = visible[end - 1].end;
                if (!PrefixHasError(g, scratch, std::string_view(sql).substr(a, z - a))) {
                    ++batches;
                    std::set<size_t> parser, scanner;
                    for (size_t i = from; i < end; ++i) {
                        const auto p = b.ResumeAtOrBefore(toTok[i]);
                        if (p.token == toTok[i] && p.kind == editor::detail::ResumePoint::Kind::InBatch) parser.insert(i);
                        if (editor::detail::StatementStartFromTokens(tokens, i) == i) scanner.insert(i);
                    }
                    parser.insert(from);
                    statements += parser.size();
                    for (size_t i : parser)
                        if (!scanner.count(i)) {
                            ++missed;
                            if (verbose)
                                std::printf("%s: scanner misses the start at '%s'\n", path.c_str(),
                                            sql.substr(visible[i].start, std::min<size_t>(60, z - visible[i].start)).c_str());
                        }
                    for (size_t i : scanner) {
                        if (parser.count(i)) continue;
                        // inside a statement that holds statements: a start the batch's points lack
                        if (Compound(tokens, *std::prev(parser.upper_bound(i)))) {
                            ++nested;
                            continue;
                        }
                        ++extra;
                        if (verbose)
                            std::printf("%s: scanner splits at '%s'\n", path.c_str(),
                                        sql.substr(visible[i].start, std::min<size_t>(60, z - visible[i].start)).c_str());
                    }
                }
            }
            from = end + 1;
        }
    }
    std::printf("segments: %zu files, %zu batches that parse, %zu statements; the scanner misses %zu starts and adds %zu "
                "(and %zu inside blocks, IF, WHILE, module bodies, CREATE SCHEMA, cursors)\n",
                files.size(), batches, statements, missed, extra, nested);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SqlVersion version = SqlVersion::Sql170;
    std::string mode, op, sql;
    std::vector<std::string> files;
    int rounds = 20;
    size_t stride = 1;
    bool warm = false, verbose = false, listAll = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--version" && i + 1 < argc) {
            if (!SqlVersionFromGrammarName(argv[++i], version)) return 2;
        } else if (a == "--walk" || a == "--classify" || a == "--sweep" || a == "--segments" || a == "--warmtime") {
            mode = a;
        } else if (a == "--verbose") {
            verbose = true;
        } else if (a == "--all") {
            verbose = listAll = true;
        } else if (a == "--warm") {
            warm = true;
        } else if (a == "--overoffer" && i + 1 < argc) {
            mode = a;
            stride = static_cast<size_t>(std::atoi(argv[++i]));
        } else if (a == "--bench" && i + 2 < argc) {
            mode = a;
            op = argv[++i];
            rounds = std::atoi(argv[++i]);
        } else if (a == "--docbench" && i + 1 < argc) {
            mode = a;
            rounds = std::atoi(argv[++i]);
        } else if (a == "-e" && i + 1 < argc) {
            sql = argv[++i];
        } else {
            files.push_back(a);
            sql = ReadFile(a);
        }
    }
    if (mode == "--overoffer") return OverOffer(version, files, stride == 0 ? 1 : stride, verbose, listAll);
    if (mode == "--segments") return Segments(version, files, verbose);
    if (mode == "--warmtime") return WarmTime();
    size_t caret = sql.find('|');
    if (caret == std::string::npos) caret = sql.size();
    else sql.erase(caret, 1);

    if (mode == "--walk") return Walk(version, sql, caret);
    if (mode == "--bench") return Bench(version, sql, caret, op, rounds);
    if (mode == "--docbench") return DocBench(version, sql, caret, rounds, warm);
    if (mode == "--sweep") return Sweep(version, sql);
    if (mode == "--classify") {
        for (const auto& s : editor::Classify(sql, version))
            std::printf("%-16s %s\n", ClassName(s.cls), sql.substr(s.start, s.length).c_str());
        return 0;
    }
    const editor::Catalog catalog = SyntheticCatalog();
    auto t0 = Clock::now();
    auto r = editor::Complete(sql, caret, version, catalog);
    std::printf("%.2f ms, replace [%zu,+%zu), %zu items\n", Ms(t0, Clock::now()), r.replaceStart, r.replaceLength,
                r.items.size());
    for (auto& it : r.items)
        std::printf("  %-15s %-30s %-30s %s\n", KindName(it.kind), it.label.c_str(), it.insertText.c_str(), it.detail.c_str());
    return 0;
}
