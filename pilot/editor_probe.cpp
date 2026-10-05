// tsql_editor_probe: shows what the editor-support layer computes at a caret, and measures it.
//   tsql_editor_probe [--version TSql170] [mode] <sql file | -e 'sql with | at the caret'>
// The caret is the first '|' in the text (removed before use; none: the end of the text).
// Modes:
//   (none)                 Complete() items at the caret
//   --walk                 the parser capture and the raw ATN walk (candidate tokens and rule paths)
//   --classify             Classify() spans
//   --bench complete|classify ROUNDS
//                          latency: the first call (cold: grammar set-up, empty ANTLR caches) and
//                          the median of ROUNDS further calls (warm), against a synthetic catalog of
//                          5 databases x 400 objects x 12 columns
//   --sweep                Complete() at every byte offset: slowest call, failures
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

#include "Grammar.h"
#include "Walker.h"
#include "antlr4-runtime.h"
#include "tsql/editor.hpp"

using namespace tsql;
using Clock = std::chrono::steady_clock;

namespace {

const char* KindName(editor::CompletionKind k) {
    static const char* names[] = {"Keyword", "Database", "Schema", "Table", "View", "Column", "ScalarFunction",
                                  "TableFunction", "Procedure", "Alias", "Cte", "TempTable", "TableVariable",
                                  "Variable", "DataType", "TableHint", "QueryHint", "BuiltinFunction"};
    return names[static_cast<int>(k)];
}

const char* ClassName(editor::TokenClass c) {
    static const char* names[] = {"Keyword", "Identifier", "QuotedIdentifier", "Variable", "String", "Number", "Comment",
                                  "Operator", "Punctuation", "BuiltinFunction", "DataType", "Whitespace", "Error"};
    return names[static_cast<int>(c)];
}

double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

editor::Catalog SyntheticCatalog() {
    editor::Catalog c;
    c.currentDatabase = "Db0";
    for (int d = 0; d < 5; ++d) {
        c.databases.push_back("Db" + std::to_string(d));
        for (int o = 0; o < 400; ++o) {
            editor::CatalogObject obj;
            obj.database = "Db" + std::to_string(d);
            obj.schema = o % 4 == 0 ? "sales" : "dbo";
            obj.name = (o % 10 == 9 ? "usp_Proc" : "Table") + std::to_string(o);
            obj.type = o % 10 == 9 ? editor::CatalogObject::Type::Procedure : editor::CatalogObject::Type::Table;
            if (obj.type == editor::CatalogObject::Type::Table)
                for (int k = 0; k < 12; ++k) obj.columns.push_back({"Column" + std::to_string(k), "int"});
            c.objects.push_back(std::move(obj));
        }
    }
    // the objects the bench scripts use
    for (const char* name : {"Orders", "Customers"}) {
        editor::CatalogObject obj{"Db0", "dbo", name, editor::CatalogObject::Type::Table, {}};
        for (const char* col : {"OrderID", "CustomerID", "CustomerName", "Region", "OrderDate", "Status", "TotalDue"})
            obj.columns.push_back({col, "int"});
        c.objects.push_back(std::move(obj));
    }
    return c;
}

int Walk(SqlVersion version, const std::string& sql, size_t caret) {
    const auto& g = editor::detail::GrammarFor(version);
    auto t0 = Clock::now();
    auto ps = g.ParseToCaret(std::string_view(sql).substr(0, caret));
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
    in.caret = ps.tokens.size();
    std::map<std::string, std::set<std::string>> seen;
    auto stats = editor::detail::Walk(g, in, [&](const editor::detail::WalkCandidate& c) {
        std::string tok(g.vocabulary->getSymbolicName(c.tokenType));
        if (c.words) {
            tok += "{";
            for (auto& w : *c.words) tok += w + " ";
            tok += "}";
        }
        if (c.hints) tok += "+hints" + std::to_string(c.hints->size());
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
    std::sort(warm.begin(), warm.end());
    std::printf("%s: %zu bytes, %zu %s, cold %.2f ms, warm median %.2f ms (min %.2f, max %.2f, %d rounds)\n", op.c_str(),
                sql.size(), n, op == "classify" ? "spans" : "items", cold, warm.empty() ? 0.0 : warm[warm.size() / 2],
                warm.empty() ? 0.0 : warm.front(), warm.empty() ? 0.0 : warm.back(), rounds);
    return 0;
}

int Sweep(SqlVersion version, const std::string& sql) {
    const editor::Catalog catalog = SyntheticCatalog();
    double worst = 0;
    size_t worstAt = 0, failures = 0, items = 0;
    auto t0 = Clock::now();
    for (size_t caret = 0; caret <= sql.size(); ++caret) {
        auto a = Clock::now();
        try {
            const auto r = editor::Complete(sql, caret, version, catalog);
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

}  // namespace

int main(int argc, char** argv) {
    SqlVersion version = SqlVersion::Sql170;
    std::string mode, op, sql;
    int rounds = 20;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--version" && i + 1 < argc) {
            if (!SqlVersionFromGrammarName(argv[++i], version)) return 2;
        } else if (a == "--walk" || a == "--classify" || a == "--sweep") {
            mode = a;
        } else if (a == "--bench" && i + 2 < argc) {
            mode = a;
            op = argv[++i];
            rounds = std::atoi(argv[++i]);
        } else if (a == "-e" && i + 1 < argc) {
            sql = argv[++i];
        } else {
            std::ifstream f(a, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            sql = ss.str();
        }
    }
    size_t caret = sql.find('|');
    if (caret == std::string::npos) caret = sql.size();
    else sql.erase(caret, 1);

    if (mode == "--walk") return Walk(version, sql, caret);
    if (mode == "--bench") return Bench(version, sql, caret, op, rounds);
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
