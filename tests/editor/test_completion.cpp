// Contract tests for tsql::editor (include/tsql/editor.hpp): Complete() against completion_cases.json and
// Classify() against classify_cases.json, both with the fixture catalog catalog.json in the same directory.
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
//        "ORDER BY"); insertText, when given, must equal the item's insertText exactly,
//   expectedReplace (optional): {start, length}, UTF-8 byte offsets into the caret-less sql,
//   topN (optional): [{kind, label, n}]: a matching item must be among the first n items,
//   noItems (optional): true = no items at all (caret in a comment, string or number),
//   currentDatabase (optional): overrides the catalog's currentDatabase.
// classify_cases.json: {"cases": [{id, version, sql, expect: [{text, nth, class}]}]}: the nth (1-based, default 1)
//   occurrence of text in sql must be covered only by spans whose class is `class` (a TokenClass name or an array
//   of acceptable names).
// catalog.json: {currentDatabase, defaultSchema, databases: [...], objects: [{database, schema, name, type,
//   columns: [[name, type]...]}]}.
// Every result is also checked against the contract's invariants (see CheckCompletionInvariants and
// CheckClassifyInvariants).
#include "tsql/editor.hpp"
#include "tsql/parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tsql::SqlVersion;
using namespace tsql::editor;

// ---------------------------------------------------------------------------------------------- minimal JSON

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* get(std::string_view key) const {
        for (const auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s_(text) {}

    Json parseDocument() {
        Json v = parseValue();
        skipWs();
        if (i_ != s_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) const {
        size_t line = 1 + static_cast<size_t>(std::count(s_.begin(), s_.begin() + static_cast<long>(std::min(i_, s_.size())), '\n'));
        throw std::runtime_error("JSON line " + std::to_string(line) + ": " + msg);
    }
    void skipWs() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool consume(char c) {
        skipWs();
        if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; }
        return false;
    }
    void expect(char c) {
        if (!consume(c)) fail(std::string("expected '") + c + "'");
    }
    Json parseValue() {
        skipWs();
        if (i_ >= s_.size()) fail("unexpected end");
        Json v;
        char c = s_[i_];
        if (c == '{') {
            ++i_;
            v.type = Json::Type::Object;
            if (consume('}')) return v;
            do {
                skipWs();
                if (i_ >= s_.size() || s_[i_] != '"') fail("expected key");
                std::string key = parseString();
                for (const auto& kv : v.obj)
                    if (kv.first == key) fail("duplicate key " + key);
                expect(':');
                v.obj.emplace_back(std::move(key), parseValue());
            } while (consume(','));
            expect('}');
        } else if (c == '[') {
            ++i_;
            v.type = Json::Type::Array;
            if (consume(']')) return v;
            do v.arr.push_back(parseValue()); while (consume(','));
            expect(']');
        } else if (c == '"') {
            v.type = Json::Type::String;
            v.str = parseString();
        } else if (s_.substr(i_, 4) == "true") {
            i_ += 4; v.type = Json::Type::Bool; v.boolean = true;
        } else if (s_.substr(i_, 5) == "false") {
            i_ += 5; v.type = Json::Type::Bool;
        } else if (s_.substr(i_, 4) == "null") {
            i_ += 4;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = i_++;
            while (i_ < s_.size() && std::strchr("0123456789.eE+-", s_[i_])) ++i_;
            v.type = Json::Type::Number;
            v.number = std::strtod(std::string(s_.substr(start, i_ - start)).c_str(), nullptr);
        } else {
            fail("unexpected character");
        }
        return v;
    }
    unsigned hex4() {
        if (i_ + 4 > s_.size()) fail("bad \\u escape");
        unsigned cp = 0;
        for (int k = 0; k < 4; ++k) {
            char h = s_[i_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
            else fail("bad \\u escape");
        }
        return cp;
    }
    std::string parseString() {
        ++i_;  // opening quote
        std::string out;
        while (true) {
            if (i_ >= s_.size()) fail("unterminated string");
            char c = s_[i_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) fail("unterminated escape");
            char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        if (s_.substr(i_, 2) != "\\u") fail("lone surrogate");
                        i_ += 2;
                        unsigned lo = hex4();
                        if (lo < 0xDC00 || lo >= 0xE000) fail("bad surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    if (cp < 0x80) {
                        out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xF0 | (cp >> 18));
                        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: fail("bad escape");
            }
        }
    }

    std::string_view s_;
    size_t i_ = 0;
};

// ---------------------------------------------------------------------------------------------- names

const std::pair<CompletionKind, const char*> kKinds[] = {
    {CompletionKind::Keyword, "Keyword"},
    {CompletionKind::Database, "Database"},
    {CompletionKind::Schema, "Schema"},
    {CompletionKind::Table, "Table"},
    {CompletionKind::View, "View"},
    {CompletionKind::Column, "Column"},
    {CompletionKind::ScalarFunction, "ScalarFunction"},
    {CompletionKind::TableFunction, "TableFunction"},
    {CompletionKind::Procedure, "Procedure"},
    {CompletionKind::Alias, "Alias"},
    {CompletionKind::Cte, "Cte"},
    {CompletionKind::TempTable, "TempTable"},
    {CompletionKind::TableVariable, "TableVariable"},
    {CompletionKind::Variable, "Variable"},
    {CompletionKind::DataType, "DataType"},
    {CompletionKind::TableHint, "TableHint"},
    {CompletionKind::QueryHint, "QueryHint"},
    {CompletionKind::BuiltinFunction, "BuiltinFunction"},
};

const std::pair<TokenClass, const char*> kClasses[] = {
    {TokenClass::Keyword, "Keyword"},
    {TokenClass::Identifier, "Identifier"},
    {TokenClass::QuotedIdentifier, "QuotedIdentifier"},
    {TokenClass::Variable, "Variable"},
    {TokenClass::String, "String"},
    {TokenClass::Number, "Number"},
    {TokenClass::Comment, "Comment"},
    {TokenClass::Operator, "Operator"},
    {TokenClass::Punctuation, "Punctuation"},
    {TokenClass::BuiltinFunction, "BuiltinFunction"},
    {TokenClass::DataType, "DataType"},
    {TokenClass::Whitespace, "Whitespace"},
    {TokenClass::Error, "Error"},
};

const std::pair<CatalogObject::Type, const char*> kObjectTypes[] = {
    {CatalogObject::Type::Table, "Table"},
    {CatalogObject::Type::View, "View"},
    {CatalogObject::Type::ScalarFunction, "ScalarFunction"},
    {CatalogObject::Type::TableFunction, "TableFunction"},
    {CatalogObject::Type::Procedure, "Procedure"},
};

template <typename E, size_t N>
const char* NameOf(const std::pair<E, const char*> (&table)[N], E value) {
    for (const auto& [v, name] : table)
        if (v == value) return name;
    return "?";
}

template <typename E, size_t N>
std::optional<E> FromName(const std::pair<E, const char*> (&table)[N], std::string_view name) {
    for (const auto& [v, n] : table)
        if (name == n) return v;
    return std::nullopt;
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool StartsWithCi(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && Lower(s.substr(0, prefix.size())) == Lower(prefix);
}

bool ContainsCi(std::string_view hay, std::string_view needle) {
    return Lower(hay).find(Lower(needle)) != std::string::npos;
}

std::string Quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out + "\"";
}

// ---------------------------------------------------------------------------------------------- case model

struct ItemSpec {
    std::vector<CompletionKind> kinds;  // empty: any kind (mustExclude only)
    std::string label;
    std::optional<std::string> insertText;
    size_t n = 0;  // topN only

    std::string describe() const {
        std::string k;
        for (CompletionKind kind : kinds) k += (k.empty() ? "" : "/") + std::string(NameOf(kKinds, kind));
        std::string out = (k.empty() ? "*" : k) + ":" + label;
        if (insertText) out += " (insert " + Quote(*insertText) + ")";
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

class Errors {
public:
    void add(const std::string& where, const std::string& msg) { list_.push_back(where + ": " + msg); }
    bool empty() const { return list_.empty(); }
    void print() const {
        for (const auto& e : list_) std::fprintf(stderr, "invalid: %s\n", e.c_str());
    }

private:
    std::vector<std::string> list_;
};

std::optional<std::string> ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<Json> LoadJson(const std::string& path, Errors& errors) {
    auto text = ReadFile(path);
    if (!text) {
        errors.add(path, "cannot read");
        return std::nullopt;
    }
    try {
        return JsonParser(*text).parseDocument();
    } catch (const std::exception& e) {
        errors.add(path, e.what());
        return std::nullopt;
    }
}

const std::string* GetString(const Json& obj, std::string_view key) {
    const Json* v = obj.get(key);
    return v && v->type == Json::Type::String ? &v->str : nullptr;
}

void CheckKeys(const Json& obj, std::initializer_list<std::string_view> allowed, const std::string& where, Errors& errors) {
    for (const auto& kv : obj.obj)
        if (std::find(allowed.begin(), allowed.end(), kv.first) == allowed.end())
            errors.add(where, "unknown field '" + kv.first + "'");
}

// ---------------------------------------------------------------------------------------------- catalog

std::optional<Catalog> LoadCatalog(const std::string& path, Errors& errors) {
    auto doc = LoadJson(path, errors);
    if (!doc) return std::nullopt;
    if (doc->type != Json::Type::Object) {
        errors.add(path, "not an object");
        return std::nullopt;
    }
    CheckKeys(*doc, {"_comment", "currentDatabase", "defaultSchema", "databases", "objects"}, path, errors);
    Catalog catalog;
    const std::string* current = GetString(*doc, "currentDatabase");
    const std::string* schema = GetString(*doc, "defaultSchema");
    const Json* dbs = doc->get("databases");
    const Json* objects = doc->get("objects");
    if (!current || !schema || !dbs || dbs->type != Json::Type::Array || !objects || objects->type != Json::Type::Array) {
        errors.add(path, "needs currentDatabase, defaultSchema (strings), databases and objects (arrays)");
        return std::nullopt;
    }
    catalog.currentDatabase = *current;
    catalog.defaultSchema = *schema;
    for (const Json& d : dbs->arr) {
        if (d.type != Json::Type::String || d.str.empty()) errors.add(path, "databases: expected non-empty strings");
        else catalog.databases.push_back(d.str);
    }
    auto hasDb = [&](const std::string& name) {
        return std::any_of(catalog.databases.begin(), catalog.databases.end(),
                           [&](const std::string& d) { return Lower(d) == Lower(name); });
    };
    if (!hasDb(catalog.currentDatabase)) errors.add(path, "currentDatabase not in databases");
    std::set<std::string> seen;
    for (const Json& o : objects->arr) {
        std::string where = path + " object";
        const std::string *db = GetString(o, "database"), *sc = GetString(o, "schema"), *name = GetString(o, "name"),
                          *type = GetString(o, "type");
        if (!db || !sc || !name || !type) {
            errors.add(where, "needs database, schema, name, type strings");
            continue;
        }
        where += " " + *db + "." + *sc + "." + *name;
        CheckKeys(o, {"database", "schema", "name", "type", "columns"}, where, errors);
        CatalogObject obj;
        obj.database = *db;
        obj.schema = *sc;
        obj.name = *name;
        auto t = FromName(kObjectTypes, *type);
        if (!t) {
            errors.add(where, "unknown type " + *type);
            continue;
        }
        obj.type = *t;
        if (!hasDb(*db)) errors.add(where, "database not in databases");
        if (!seen.insert(Lower(*db + "." + *sc + "." + *name)).second) errors.add(where, "duplicate object");
        if (const Json* cols = o.get("columns")) {
            for (const Json& c : cols->arr) {
                if (c.type != Json::Type::Array || c.arr.size() != 2 || c.arr[0].type != Json::Type::String ||
                    c.arr[1].type != Json::Type::String || c.arr[0].str.empty()) {
                    errors.add(where, "columns: expected [name, type] string pairs");
                    continue;
                }
                obj.columns.push_back({c.arr[0].str, c.arr[1].str});
            }
        }
        bool hasColumns = obj.type == CatalogObject::Type::Table || obj.type == CatalogObject::Type::View ||
                          obj.type == CatalogObject::Type::TableFunction;
        if (hasColumns == obj.columns.empty())
            errors.add(where, hasColumns ? "tables, views and table functions need columns" : "only tables, views and table functions have columns");
        catalog.objects.push_back(std::move(obj));
    }
    return catalog;
}

// ---------------------------------------------------------------------------------------------- case loading

bool ParseVersion(const Json& c, const std::string& where, std::string& name, SqlVersion& version, Errors& errors) {
    const std::string* v = GetString(c, "version");
    if (!v) {
        errors.add(where, "missing version");
        return false;
    }
    name = *v;
    static const std::set<std::string> kEditorVersions = {"TSql130", "TSql140", "TSql150", "TSql160",
                                                          "TSql170", "TSql180", "TSqlFabricDW"};
    if (!kEditorVersions.count(*v) || !tsql::SqlVersionFromGrammarName(*v, version)) {
        errors.add(where, "version must be one of TSql130..TSql180, TSqlFabricDW");
        return false;
    }
    return true;
}

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
            return std::any_of(catalog.objects.begin(), catalog.objects.end(), [&](const CatalogObject& o) { return ieq(o.schema, spec.label); });
        case CompletionKind::Table: return objectOf(CatalogObject::Type::Table);
        case CompletionKind::View: return objectOf(CatalogObject::Type::View);
        case CompletionKind::ScalarFunction: return objectOf(CatalogObject::Type::ScalarFunction);
        case CompletionKind::TableFunction: return objectOf(CatalogObject::Type::TableFunction);
        case CompletionKind::Procedure: return objectOf(CatalogObject::Type::Procedure);
        case CompletionKind::Column:
            if (ContainsCi(sql, spec.label)) return true;
            for (const auto& o : catalog.objects)
                for (const auto& c : o.columns)
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
    if (isTop) CheckKeys(j, {"kind", "label", "n"}, where, errors);
    else CheckKeys(j, {"kind", "label", "insertText"}, where, errors);
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
    if (const Json* it = j.get("insertText")) {
        if (it->type != Json::Type::String || it->str.empty()) errors.add(where, "insertText must be a non-empty string");
        else spec.insertText = it->str;
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

bool Matches(const ItemSpec& spec, const CompletionItem& item, bool checkInsert) {
    if (!spec.kinds.empty() && std::find(spec.kinds.begin(), spec.kinds.end(), item.kind) == spec.kinds.end()) return false;
    bool labelOk = Lower(item.label) == Lower(spec.label) ||
                   (IsPhraseKind(item.kind) && StartsWithCi(item.label, spec.label + " "));
    if (!labelOk) return false;
    return !checkInsert || !spec.insertText || item.insertText == *spec.insertText;
}

std::string DescribeItem(const CompletionItem& item) {
    std::string out = std::string(NameOf(kKinds, item.kind)) + ":" + item.label;
    if (item.insertText != item.label) out += "=" + item.insertText;
    return out;
}

std::string ListItems(const std::vector<CompletionItem>& items, size_t limit) {
    std::string out;
    for (size_t i = 0; i < items.size() && i < limit; ++i) out += (i ? ", " : "") + DescribeItem(items[i]);
    if (items.size() > limit) out += ", ... (" + std::to_string(items.size()) + " items)";
    return out.empty() ? "(none)" : out;
}

// Contract invariants that hold for every Complete() result.
void CheckCompletionInvariants(const CompletionCase& tc, const CompletionResult& r, std::vector<std::string>& issues) {
    if (r.replaceStart > tc.caret || r.replaceStart + r.replaceLength < tc.caret || r.replaceStart + r.replaceLength > tc.sql.size()) {
        issues.push_back("replace range [" + std::to_string(r.replaceStart) + "," + std::to_string(r.replaceStart + r.replaceLength) +
                         ") does not contain the caret " + std::to_string(tc.caret) + " within the input");
        return;
    }
    std::string typed = tc.sql.substr(r.replaceStart, tc.caret - r.replaceStart);
    if (!typed.empty() && (typed[0] == '[' || typed[0] == '"')) typed.erase(0, 1);
    for (const auto& item : r.items) {
        if (item.label.empty() || item.insertText.empty()) {
            issues.push_back("item with empty label or insertText: " + DescribeItem(item));
            continue;
        }
        std::string_view insert = item.insertText;
        if (!insert.empty() && insert[0] == '[') insert.remove_prefix(1);
        if (!StartsWithCi(item.label, typed) && !StartsWithCi(insert, typed))
            issues.push_back("item " + DescribeItem(item) + " does not match the typed prefix " + Quote(typed));
    }
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
        CheckCompletionInvariants(tc, r, issues);
        if (tc.noItems && !r.items.empty()) issues.push_back("expected no items");
        if (tc.replace && (r.replaceStart != tc.replace->first || r.replaceLength != tc.replace->second))
            issues.push_back("replace range is {" + std::to_string(r.replaceStart) + ", " + std::to_string(r.replaceLength) +
                             "}, expected {" + std::to_string(tc.replace->first) + ", " + std::to_string(tc.replace->second) + "}");
        for (const auto& spec : tc.include) {
            auto exact = std::find_if(r.items.begin(), r.items.end(), [&](const CompletionItem& it) { return Matches(spec, it, true); });
            if (exact != r.items.end()) continue;
            auto loose = std::find_if(r.items.begin(), r.items.end(), [&](const CompletionItem& it) { return Matches(spec, it, false); });
            if (loose != r.items.end()) issues.push_back("wrong insertText for " + spec.describe() + ": got " + Quote(loose->insertText));
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

// Contract invariants that hold for every Classify() result.
void CheckClassifyInvariants(std::string_view sql, const std::vector<ColouredSpan>& spans, std::vector<std::string>& issues) {
    size_t expected = 0;
    for (const auto& s : spans) {
        if (s.length == 0) issues.push_back("empty span at " + std::to_string(s.start));
        if (s.start != expected) {
            issues.push_back("span at " + std::to_string(s.start) + " should start at " + std::to_string(expected) + " (gap or overlap)");
            return;
        }
        expected = s.start + s.length;
    }
    if (expected != sql.size())
        issues.push_back("spans cover " + std::to_string(expected) + " of " + std::to_string(sql.size()) + " bytes");
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
