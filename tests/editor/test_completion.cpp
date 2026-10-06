// Contract tests for tsql::editor (include/tsql/editor.hpp): Complete() against completion_cases.json and
// Classify() against classify_cases.json, both with the fixture catalog catalog.json in the same directory
// (Document: test_document.cpp).
//
//   test_completion [--validate-only] [--suite completion|classify|all] [--filter TEXT] [--as-version NAME] [--verbose]
//                   CASES_DIR
//
// --validate-only checks the case files and the catalog without calling the engine. --filter runs only cases
// whose id contains TEXT. --as-version runs every completion case outside the "versions" area under grammar NAME
// instead of its own version (those cases use syntax every version accepts). --verbose prints the items/spans of
// every case run.
//
// Case format (also in the "_format" header of each case file):
// completion_cases.json: {"catalog": "catalog.json", "cases": [case...]}; case fields:
//   id (unique), area (report grouping), version (a GrammarName: TSql130..TSql180, TSqlFabricDW),
//   sql: the script with the caret marked by exactly one '|'; the '|' is removed and its UTF-8 byte offset is the
//        caret passed to Complete (so cases never use the '|' operator),
//   mustInclude / mustExclude: [{kind, label[, insertText]}]: kind is a CompletionKind name or an array of
//        acceptable kinds (omitted in mustExclude = any kind); label matches case-insensitively, and for Keyword,
//        TableHint and QueryHint also matches an item label that is a phrase starting with it ("ORDER" matches
//        "ORDER BY"); insertText, when given, must equal the item's insertText exactly; mustInclude items may also
//        give detailContains (strings that must occur in the item's detail in this order) and detailExcludes
//        (strings that must not occur), both case-insensitive whole words ("int" does not match "bigint"); any item
//        may give exact: true to turn off the phrase match ("END" then does not match "END CONVERSATION"),
//   expectedReplace (optional): {start, length}, UTF-8 byte offsets into the caret-less sql,
//   topN (optional): [{kind, label, n}]: a matching item must be among the first n items,
//   noItems (optional): true = no items at all (caret in a comment, string or number),
//   currentDatabase (optional): overrides the catalog's currentDatabase.
// classify_cases.json: {"cases": [{id, version, sql, expect: [{text, nth, class}]}]}: the nth (1-based, default 1)
//   occurrence of text in sql must be covered only by spans whose class is `class` (a TokenClass name or an array
//   of acceptable names).
// catalog.json: {currentDatabase, defaultSchema, databases: [...], objects: [{database, schema, name, type,
//   columns: [[name, type]...], parameters: [[name, type, "output"?, "default"?]...], target}], types: [{database,
//   schema, name, isTableType, columns}]}; an empty database is a system object/type (every database).
// Every result is also checked against the contract's invariants (see CheckCompletionInvariants and
// CheckClassifyInvariants).
#include "contract_support.h"

#include <map>

namespace {

using namespace editor_contract;

// ---------------------------------------------------------------------------------------------- case model

struct ItemSpec {
    std::vector<CompletionKind> kinds;  // empty: any kind (mustExclude only)
    std::string label;
    std::optional<std::string> insertText;
    bool exact = false;  // the label must equal the item's label (no phrase match: "END" does not match "END CONVERSATION")
    std::vector<std::string> detailContains;  // must occur in the item's detail, in this order (case-insensitive)
    std::vector<std::string> detailExcludes;  // must not occur in the item's detail (case-insensitive, whole word)
    size_t n = 0;  // topN only

    std::string describe() const {
        std::string k;
        for (CompletionKind kind : kinds) k += (k.empty() ? "" : "/") + std::string(NameOf(kKinds, kind));
        std::string out = (k.empty() ? "*" : k) + ":" + label;
        if (insertText) out += " (insert " + Quote(*insertText) + ")";
        if (exact) out += " (exact)";
        for (const auto& d : detailContains) out += " (detail has " + Quote(d) + ")";
        for (const auto& d : detailExcludes) out += " (detail lacks " + Quote(d) + ")";
        if (n) out += " in top " + std::to_string(n);
        return out;
    }
};

struct CompletionCase {
    std::string id, area, versionName, sql;  // sql without the caret marker
    SqlVersion version{};
    size_t caret = 0;
    std::vector<ItemSpec> include, exclude, top;
    std::optional<std::pair<size_t, size_t>> replace;
    bool noItems = false;
    std::optional<std::string> currentDatabase;
};

struct ClassifyExpect {
    std::string text;
    size_t nth = 1, start = 0;
    std::vector<TokenClass> classes;
};

struct ClassifyCase {
    std::string id, versionName, sql;
    SqlVersion version{};
    std::vector<ClassifyExpect> expects;
};

// ---------------------------------------------------------------------------------------------- case loading

// Whether a mustInclude/mustExclude/topN entry names something that exists: catalog objects in the catalog,
// script-defined names (aliases, CTEs, variables, temp tables, derived columns) in the case's sql. Catches typos
// that would make an exclusion vacuous or an inclusion unsatisfiable.
bool ReferenceExists(const ItemSpec& spec, const Catalog& catalog, const std::string& sql) {
    if (spec.kinds.size() != 1) return true;
    auto ieq = [](const std::string& a, const std::string& b) { return Lower(a) == Lower(b); };
    auto objectOf = [&](CatalogObject::Type t) {
        return std::any_of(catalog.objects.begin(), catalog.objects.end(),
                           [&](const CatalogObject& o) { return o.type == t && ieq(o.name, spec.label); });
    };
    switch (spec.kinds[0]) {
        case CompletionKind::Database:
            return std::any_of(catalog.databases.begin(), catalog.databases.end(), [&](const std::string& d) { return ieq(d, spec.label); });
        case CompletionKind::Schema:
            return std::any_of(catalog.objects.begin(), catalog.objects.end(), [&](const CatalogObject& o) { return ieq(o.schema, spec.label); }) ||
                   std::any_of(catalog.types.begin(), catalog.types.end(), [&](const CatalogType& t) { return ieq(t.schema, spec.label); });
        case CompletionKind::Table: return objectOf(CatalogObject::Type::Table);
        case CompletionKind::View: return objectOf(CatalogObject::Type::View);
        case CompletionKind::ScalarFunction: return objectOf(CatalogObject::Type::ScalarFunction);
        case CompletionKind::TableFunction: return objectOf(CatalogObject::Type::TableFunction);
        case CompletionKind::Procedure: return objectOf(CatalogObject::Type::Procedure);
        case CompletionKind::Synonym: return objectOf(CatalogObject::Type::Synonym);
        case CompletionKind::UserType:
            return std::any_of(catalog.types.begin(), catalog.types.end(), [&](const CatalogType& t) { return ieq(t.name, spec.label); });
        case CompletionKind::Parameter:
            for (const auto& o : catalog.objects)
                for (const auto& p : o.parameters)
                    if (ieq(p.name, spec.label)) return true;
            return false;
        case CompletionKind::Column:
            if (ContainsCi(sql, spec.label)) return true;
            for (const auto& o : catalog.objects)
                for (const auto& c : o.columns)
                    if (ieq(c.name, spec.label)) return true;
            for (const auto& t : catalog.types)
                for (const auto& c : t.columns)
                    if (ieq(c.name, spec.label)) return true;
            return false;
        case CompletionKind::Alias:
        case CompletionKind::Cte:
        case CompletionKind::TempTable:
        case CompletionKind::TableVariable:
        case CompletionKind::Variable:
            return ContainsCi(sql, spec.label);
        default:
            return true;
    }
}

std::optional<ItemSpec> ParseItem(const Json& j, bool kindRequired, bool isTop, const std::string& where, Errors& errors) {
    if (j.type != Json::Type::Object) {
        errors.add(where, "item is not an object");
        return std::nullopt;
    }
    bool isInclude = kindRequired && !isTop;
    if (isTop) CheckKeys(j, {"kind", "label", "n", "exact"}, where, errors);
    else if (isInclude) CheckKeys(j, {"kind", "label", "insertText", "detailContains", "detailExcludes", "exact"}, where, errors);
    else CheckKeys(j, {"kind", "label", "insertText", "exact"}, where, errors);
    ItemSpec spec;
    const Json* kind = j.get("kind");
    std::vector<const Json*> kindNames;
    if (kind && kind->type == Json::Type::Array)
        for (const Json& k : kind->arr) kindNames.push_back(&k);
    else if (kind)
        kindNames.push_back(kind);
    for (const Json* k : kindNames) {
        auto parsed = k->type == Json::Type::String ? FromName(kKinds, k->str) : std::nullopt;
        if (!parsed) {
            errors.add(where, "unknown kind " + (k->type == Json::Type::String ? k->str : std::string("(non-string)")));
            return std::nullopt;
        }
        spec.kinds.push_back(*parsed);
    }
    if (kindRequired && spec.kinds.empty()) {
        errors.add(where, "item needs a kind");
        return std::nullopt;
    }
    const std::string* label = GetString(j, "label");
    if (!label || label->empty()) {
        errors.add(where, "item needs a non-empty label");
        return std::nullopt;
    }
    spec.label = *label;
    if (const Json* e = j.get("exact")) {
        if (e->type != Json::Type::Bool) errors.add(where, "exact must be a boolean");
        else spec.exact = e->boolean;
    }
    if (const Json* it = j.get("insertText")) {
        if (it->type != Json::Type::String || it->str.empty()) errors.add(where, "insertText must be a non-empty string");
        else spec.insertText = it->str;
    }
    for (auto [key, out] : {std::pair{"detailContains", &spec.detailContains}, std::pair{"detailExcludes", &spec.detailExcludes}}) {
        const Json* list = j.get(key);
        if (!list) continue;
        if (list->type != Json::Type::Array || list->arr.empty()) {
            errors.add(where, std::string(key) + " must be a non-empty array of non-empty strings");
            continue;
        }
        for (const Json& s : list->arr) {
            if (s.type != Json::Type::String || s.str.empty()) errors.add(where, std::string(key) + " must hold non-empty strings");
            else out->push_back(s.str);
        }
    }
    if (isTop) {
        const Json* n = j.get("n");
        if (!n || n->type != Json::Type::Number || n->number < 1 || n->number != static_cast<double>(static_cast<size_t>(n->number))) {
            errors.add(where, "topN entry needs an integer n >= 1");
            return std::nullopt;
        }
        spec.n = static_cast<size_t>(n->number);
    }
    return spec;
}

bool KindsOverlap(const ItemSpec& a, const ItemSpec& b) {
    if (a.kinds.empty() || b.kinds.empty()) return true;
    for (CompletionKind k : a.kinds)
        if (std::find(b.kinds.begin(), b.kinds.end(), k) != b.kinds.end()) return true;
    return false;
}

std::vector<CompletionCase> LoadCompletionCases(const std::string& path, const Catalog& catalog, Errors& errors) {
    std::vector<CompletionCase> cases;
    auto doc = LoadJson(path, errors);
    if (!doc) return cases;
    CheckKeys(*doc, {"_format", "catalog", "cases"}, path, errors);
    const Json* list = doc->get("cases");
    if (!list || list->type != Json::Type::Array) {
        errors.add(path, "missing cases array");
        return cases;
    }
    std::set<std::string> ids;
    for (const Json& c : list->arr) {
        CompletionCase tc;
        const std::string* id = GetString(c, "id");
        const std::string* area = GetString(c, "area");
        const std::string* sql = GetString(c, "sql");
        std::string where = path + " case " + (id ? *id : std::string("(no id)"));
        if (!id || id->empty() || !area || area->empty() || !sql) {
            errors.add(where, "needs id, area, sql strings");
            continue;
        }
        CheckKeys(c, {"id", "area", "version", "sql", "mustInclude", "mustExclude", "expectedReplace", "topN", "noItems",
                      "currentDatabase"}, where, errors);
        if (!ids.insert(*id).second) errors.add(where, "duplicate id");
        tc.id = *id;
        tc.area = *area;
        bool ok = ParseVersion(c, where, tc.versionName, tc.version, errors);
        size_t bar = sql->find('|');
        if (bar == std::string::npos || sql->find('|', bar + 1) != std::string::npos) {
            errors.add(where, "sql must contain exactly one '|' caret marker");
            continue;
        }
        tc.sql = sql->substr(0, bar) + sql->substr(bar + 1);
        tc.caret = bar;
        auto items = [&](const char* key, bool kindRequired, bool isTop, std::vector<ItemSpec>& out) {
            const Json* arr = c.get(key);
            if (!arr) return;
            if (arr->type != Json::Type::Array) {
                errors.add(where, std::string(key) + " must be an array");
                ok = false;
                return;
            }
            for (const Json& j : arr->arr) {
                auto spec = ParseItem(j, kindRequired, isTop, where + " " + key, errors);
                if (!spec) {
                    ok = false;
                    continue;
                }
                if (!ReferenceExists(*spec, catalog, tc.sql))
                    errors.add(where, std::string(key) + " " + spec->describe() + " names nothing in the catalog or the sql");
                out.push_back(std::move(*spec));
            }
        };
        items("mustInclude", true, false, tc.include);
        items("mustExclude", false, false, tc.exclude);
        items("topN", true, true, tc.top);
        if (const Json* r = c.get("expectedReplace")) {
            const Json *s = r->get("start"), *l = r->get("length");
            if (r->type != Json::Type::Object || !s || !l || s->type != Json::Type::Number || l->type != Json::Type::Number ||
                s->number < 0 || l->number < 0) {
                errors.add(where, "expectedReplace needs numeric start and length");
                ok = false;
            } else {
                CheckKeys(*r, {"start", "length"}, where + " expectedReplace", errors);
                size_t start = static_cast<size_t>(s->number), length = static_cast<size_t>(l->number);
                if (start > tc.caret || start + length < tc.caret || start + length > tc.sql.size())
                    errors.add(where, "expectedReplace must contain the caret and lie within the sql");
                tc.replace = std::make_pair(start, length);
            }
        }
        if (const Json* n = c.get("noItems")) {
            if (n->type != Json::Type::Bool) errors.add(where, "noItems must be a boolean");
            tc.noItems = n->boolean;
        }
        if (const Json* db = c.get("currentDatabase")) {
            if (db->type != Json::Type::String ||
                std::none_of(catalog.databases.begin(), catalog.databases.end(), [&](const std::string& d) { return Lower(d) == Lower(db->str); }))
                errors.add(where, "currentDatabase must name a catalog database");
            else tc.currentDatabase = db->str;
        }
        if (tc.noItems && (!tc.include.empty() || !tc.top.empty() || tc.replace))
            errors.add(where, "noItems cases cannot have mustInclude, topN or expectedReplace");
        if (!tc.noItems && tc.include.empty() && tc.exclude.empty())
            errors.add(where, "case checks nothing (needs mustInclude, mustExclude or noItems)");
        for (const auto& in : tc.include)
            for (const auto& ex : tc.exclude)
                if (Lower(in.label) == Lower(ex.label) && KindsOverlap(in, ex))
                    errors.add(where, in.describe() + " is both included and excluded");
        if (tc.replace) {
            // The typed part of the word filters items by prefix, so every expected item must match it.
            std::string typed = tc.sql.substr(tc.replace->first, tc.caret - tc.replace->first);
            if (!typed.empty() && (typed[0] == '[' || typed[0] == '"')) typed.erase(0, 1);
            for (const auto* group : {&tc.include, &tc.top})
                for (const auto& spec : *group)
                    if (!StartsWithCi(spec.label, typed))
                        errors.add(where, spec.describe() + " does not start with the typed prefix " + Quote(typed));
        }
        if (ok) cases.push_back(std::move(tc));
    }
    return cases;
}

std::vector<ClassifyCase> LoadClassifyCases(const std::string& path, Errors& errors) {
    std::vector<ClassifyCase> cases;
    auto doc = LoadJson(path, errors);
    if (!doc) return cases;
    CheckKeys(*doc, {"_format", "cases"}, path, errors);
    const Json* list = doc->get("cases");
    if (!list || list->type != Json::Type::Array) {
        errors.add(path, "missing cases array");
        return cases;
    }
    std::set<std::string> ids;
    for (const Json& c : list->arr) {
        ClassifyCase tc;
        const std::string* id = GetString(c, "id");
        const std::string* sql = GetString(c, "sql");
        const Json* expect = c.get("expect");
        std::string where = path + " case " + (id ? *id : std::string("(no id)"));
        if (!id || id->empty() || !sql || !expect || expect->type != Json::Type::Array) {
            errors.add(where, "needs id, sql strings and an expect array");
            continue;
        }
        CheckKeys(c, {"id", "version", "sql", "expect"}, where, errors);
        if (!ids.insert(*id).second) errors.add(where, "duplicate id");
        tc.id = *id;
        tc.sql = *sql;
        bool ok = ParseVersion(c, where, tc.versionName, tc.version, errors);
        for (const Json& e : expect->arr) {
            ClassifyExpect ex;
            const std::string* text = GetString(e, "text");
            if (!text || text->empty()) {
                errors.add(where, "expect entry needs non-empty text");
                ok = false;
                continue;
            }
            CheckKeys(e, {"text", "nth", "class"}, where, errors);
            ex.text = *text;
            if (const Json* n = e.get("nth")) {
                if (n->type != Json::Type::Number || n->number < 1) errors.add(where, "nth must be >= 1");
                else ex.nth = static_cast<size_t>(n->number);
            }
            const Json* cls = e.get("class");
            std::vector<const Json*> names;
            if (cls && cls->type == Json::Type::Array)
                for (const Json& k : cls->arr) names.push_back(&k);
            else if (cls)
                names.push_back(cls);
            for (const Json* k : names) {
                auto parsed = k->type == Json::Type::String ? FromName(kClasses, k->str) : std::nullopt;
                if (!parsed) errors.add(where, "unknown class for " + Quote(ex.text));
                else ex.classes.push_back(*parsed);
            }
            if (ex.classes.empty()) {
                errors.add(where, "expect entry for " + Quote(ex.text) + " needs a class");
                ok = false;
                continue;
            }
            size_t pos = std::string::npos, from = 0;
            for (size_t k = 0; k < ex.nth; ++k) {
                pos = tc.sql.find(ex.text, from);
                if (pos == std::string::npos) break;
                from = pos + 1;
            }
            if (pos == std::string::npos) {
                errors.add(where, Quote(ex.text) + " does not occur " + std::to_string(ex.nth) + " time(s)");
                ok = false;
                continue;
            }
            ex.start = pos;
            tc.expects.push_back(std::move(ex));
        }
        if (ok) cases.push_back(std::move(tc));
    }
    return cases;
}

// ---------------------------------------------------------------------------------------------- running

bool IsPhraseKind(CompletionKind k) {
    return k == CompletionKind::Keyword || k == CompletionKind::TableHint || k == CompletionKind::QueryHint;
}

bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '@' || c == '#' || static_cast<unsigned char>(c) >= 0x80;
}

// Case-insensitive position of `needle` in `hay` at or after `from`, not inside a longer word ("int" is not found
// in "bigint"); npos if absent.
size_t FindWordCi(std::string_view hay, std::string_view needle, size_t from) {
    std::string h = Lower(hay), n = Lower(needle);
    for (size_t pos = h.find(n, from); pos != std::string::npos; pos = h.find(n, pos + 1)) {
        bool startOk = pos == 0 || !IsWordChar(n.front()) || !IsWordChar(h[pos - 1]);
        size_t end = pos + n.size();
        bool endOk = end == h.size() || !IsWordChar(n.back()) || !IsWordChar(h[end]);
        if (startOk && endOk) return pos;
    }
    return std::string::npos;
}

bool DetailMatches(const ItemSpec& spec, const CompletionItem& item) {
    size_t from = 0;
    for (const auto& d : spec.detailContains) {
        size_t pos = FindWordCi(item.detail, d, from);
        if (pos == std::string::npos) return false;
        from = pos + d.size();
    }
    for (const auto& d : spec.detailExcludes)
        if (FindWordCi(item.detail, d, 0) != std::string::npos) return false;
    return true;
}

// checkExtras: also insertText and detail (mustInclude only).
bool Matches(const ItemSpec& spec, const CompletionItem& item, bool checkExtras) {
    if (!spec.kinds.empty() && std::find(spec.kinds.begin(), spec.kinds.end(), item.kind) == spec.kinds.end()) return false;
    bool labelOk = Lower(item.label) == Lower(spec.label) ||
                   (!spec.exact && IsPhraseKind(item.kind) && StartsWithCi(item.label, spec.label + " "));
    if (!labelOk) return false;
    return !checkExtras || ((!spec.insertText || item.insertText == *spec.insertText) && DetailMatches(spec, item));
}

std::string ListItems(const std::vector<CompletionItem>& items, size_t limit) {
    std::string out;
    for (size_t i = 0; i < items.size() && i < limit; ++i) out += (i ? ", " : "") + DescribeItem(items[i]);
    if (items.size() > limit) out += ", ... (" + std::to_string(items.size()) + " items)";
    return out.empty() ? "(none)" : out;
}

bool RunCompletionCase(const CompletionCase& tc, const Catalog& baseCatalog, bool verbose) {
    Catalog catalog = baseCatalog;
    if (tc.currentDatabase) catalog.currentDatabase = *tc.currentDatabase;
    std::vector<std::string> issues;
    CompletionResult r;
    try {
        r = Complete(tc.sql, tc.caret, tc.version, catalog);
    } catch (const std::exception& e) {
        issues.push_back(std::string("Complete threw: ") + e.what());
    }
    if (issues.empty()) {
        CheckCompletionInvariants(tc.sql, tc.caret, r, issues);
        if (tc.noItems && !r.items.empty()) issues.push_back("expected no items");
        if (tc.replace && (r.replaceStart != tc.replace->first || r.replaceLength != tc.replace->second))
            issues.push_back("replace range is {" + std::to_string(r.replaceStart) + ", " + std::to_string(r.replaceLength) +
                             "}, expected {" + std::to_string(tc.replace->first) + ", " + std::to_string(tc.replace->second) + "}");
        for (const auto& spec : tc.include) {
            auto exact = std::find_if(r.items.begin(), r.items.end(), [&](const CompletionItem& it) { return Matches(spec, it, true); });
            if (exact != r.items.end()) continue;
            auto loose = std::find_if(r.items.begin(), r.items.end(), [&](const CompletionItem& it) { return Matches(spec, it, false); });
            if (loose != r.items.end())
                issues.push_back("wrong insertText/detail for " + spec.describe() + ": got " + Quote(loose->insertText) + ", detail " + Quote(loose->detail));
            else issues.push_back("missing " + spec.describe());
        }
        for (const auto& spec : tc.exclude)
            for (const auto& it : r.items)
                if (Matches(spec, it, false)) issues.push_back("unexpected " + DescribeItem(it) + " (excluded " + spec.describe() + ")");
        for (const auto& spec : tc.top) {
            auto it = std::find_if(r.items.begin(), r.items.end(), [&](const CompletionItem& i) { return Matches(spec, i, false); });
            size_t rank = static_cast<size_t>(it - r.items.begin());
            if (it == r.items.end()) issues.push_back("missing " + spec.describe());
            else if (rank >= spec.n) issues.push_back(spec.describe() + " is at position " + std::to_string(rank + 1));
        }
    }
    if (!issues.empty() || verbose) {
        std::printf("%s %s [%s] %s\n  sql %s caret %zu\n", issues.empty() ? "ok  " : "FAIL", tc.id.c_str(), tc.area.c_str(),
                    tc.versionName.c_str(), Quote(tc.sql).c_str(), tc.caret);
        for (const auto& i : issues) std::printf("  - %s\n", i.c_str());
        std::printf("  got replace {%zu, %zu}: %s\n", r.replaceStart, r.replaceLength, ListItems(r.items, verbose ? 1000 : 25).c_str());
    }
    return issues.empty();
}

bool RunClassifyCase(const ClassifyCase& tc, bool verbose) {
    std::vector<std::string> issues;
    std::vector<ColouredSpan> spans;
    try {
        spans = Classify(tc.sql, tc.version);
    } catch (const std::exception& e) {
        issues.push_back(std::string("Classify threw: ") + e.what());
    }
    if (issues.empty()) {
        CheckClassifyInvariants(tc.sql, spans, issues);
        for (const auto& ex : tc.expects) {
            std::string got;
            bool ok = true;
            for (const auto& s : spans) {
                if (s.start + s.length <= ex.start || s.start >= ex.start + ex.text.size()) continue;
                if (std::find(ex.classes.begin(), ex.classes.end(), s.cls) == ex.classes.end()) ok = false;
                got += (got.empty() ? "" : "+") + std::string(NameOf(kClasses, s.cls));
            }
            if (!ok || got.empty()) {
                std::string want;
                for (TokenClass c : ex.classes) want += (want.empty() ? "" : "/") + std::string(NameOf(kClasses, c));
                issues.push_back(Quote(ex.text) + " #" + std::to_string(ex.nth) + " at " + std::to_string(ex.start) + " is " +
                                 (got.empty() ? "uncovered" : got) + ", expected " + want);
            }
        }
    }
    if (!issues.empty() || verbose) {
        std::printf("%s classify %s %s\n  sql %s\n", issues.empty() ? "ok  " : "FAIL", tc.id.c_str(), tc.versionName.c_str(),
                    Quote(tc.sql).c_str());
        for (const auto& i : issues) std::printf("  - %s\n", i.c_str());
        if (verbose || !issues.empty()) {
            std::string list;
            for (const auto& s : spans)
                list += " " + Quote(tc.sql.substr(std::min(s.start, tc.sql.size()), s.length)) + ":" + NameOf(kClasses, s.cls);
            std::printf("  spans%s\n", list.c_str());
        }
    }
    return issues.empty();
}

int Usage() {
    std::fprintf(stderr, "usage: test_completion [--validate-only] [--suite completion|classify|all] [--filter TEXT] "
                         "[--as-version NAME] [--verbose] CASES_DIR\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered, to keep the reports of earlier cases if one crashes. (Line buffering with size 0 is
    // an invalid parameter to MSVC's CRT, which has no line buffering and fails fast on it.)
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool validateOnly = false, verbose = false;
    std::string suite = "all", filter, dir, asVersionName;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--validate-only") validateOnly = true;
        else if (a == "--verbose" || a == "-v") verbose = true;
        else if (a == "--suite" && i + 1 < argc) suite = argv[++i];
        else if (a == "--filter" && i + 1 < argc) filter = argv[++i];
        else if (a == "--as-version" && i + 1 < argc) asVersionName = argv[++i];
        else if (!a.empty() && a[0] != '-' && dir.empty()) dir = a;
        else return Usage();
    }
    if (dir.empty() || (suite != "all" && suite != "completion" && suite != "classify")) return Usage();
    bool doCompletion = suite != "classify", doClassify = suite != "completion";

    Errors errors;
    auto catalog = LoadCatalog(dir + "/catalog.json", errors);
    std::vector<CompletionCase> completionCases;
    std::vector<ClassifyCase> classifyCases;
    if (catalog) completionCases = LoadCompletionCases(dir + "/completion_cases.json", *catalog, errors);
    classifyCases = LoadClassifyCases(dir + "/classify_cases.json", errors);
    if (!asVersionName.empty()) {
        Json spec;
        spec.type = Json::Type::Object;
        Json name;
        name.type = Json::Type::String;
        name.str = asVersionName;
        spec.obj.emplace_back("version", std::move(name));
        SqlVersion asVersion{};
        std::string ignored;
        if (ParseVersion(spec, "--as-version", ignored, asVersion, errors)) {
            for (auto& c : completionCases) {
                if (c.area == "versions") continue;
                c.version = asVersion;
                c.versionName = asVersionName;
            }
        }
    }
    if (!errors.empty()) {
        errors.print();
        return 1;
    }
    std::map<std::string, size_t> areaTotals;
    for (const auto& c : completionCases) ++areaTotals[c.area];
    std::printf("cases valid: %zu completion, %zu classify\n", completionCases.size(), classifyCases.size());
    if (validateOnly) {
        for (const auto& [area, n] : areaTotals) std::printf("  %-20s %zu\n", area.c_str(), n);
        return 0;
    }

    struct Tally { size_t pass = 0, fail = 0, skip = 0; };
    std::map<std::string, Tally> byArea;
    Tally total;
    auto selected = [&](const std::string& id) { return filter.empty() || id.find(filter) != std::string::npos; };
    if (doCompletion) {
        for (const auto& tc : completionCases) {
            if (!selected(tc.id)) continue;
            Tally& t = byArea[tc.area];
            if (!tsql::IsParserAvailable(tc.version)) ++t.skip;
            else if (RunCompletionCase(tc, *catalog, verbose)) ++t.pass;
            else ++t.fail;
        }
    }
    if (doClassify) {
        for (const auto& tc : classifyCases) {
            if (!selected(tc.id)) continue;
            Tally& t = byArea["(classify)"];
            if (!tsql::IsParserAvailable(tc.version)) ++t.skip;
            else if (RunClassifyCase(tc, verbose)) ++t.pass;
            else ++t.fail;
        }
    }
    std::printf("\n%-20s %6s %6s %6s\n", "area", "pass", "fail", "skip");
    for (const auto& [area, t] : byArea) {
        std::printf("%-20s %6zu %6zu %6zu\n", area.c_str(), t.pass, t.fail, t.skip);
        total.pass += t.pass;
        total.fail += t.fail;
        total.skip += t.skip;
    }
    std::printf("%-20s %6zu %6zu %6zu\n", "total", total.pass, total.fail, total.skip);
    return total.fail == 0 ? 0 : 1;
}
