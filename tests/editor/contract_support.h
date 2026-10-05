// Shared by the tsql::editor contract tests (test_completion.cpp, test_document.cpp): a minimal JSON reader, the
// enum names used in the case files, the fixture catalog loader (catalog.json; format in test_completion.cpp) and
// the invariants every Complete()/Classify() result satisfies.
#pragma once

#include "tsql/editor.hpp"
#include "tsql/parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace editor_contract {

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

inline const std::pair<CompletionKind, const char*> kKinds[] = {
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
    {CompletionKind::Parameter, "Parameter"},
    {CompletionKind::Synonym, "Synonym"},
    {CompletionKind::UserType, "UserType"},
};

inline const std::pair<TokenClass, const char*> kClasses[] = {
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

inline const std::pair<CatalogObject::Type, const char*> kObjectTypes[] = {
    {CatalogObject::Type::Table, "Table"},
    {CatalogObject::Type::View, "View"},
    {CatalogObject::Type::ScalarFunction, "ScalarFunction"},
    {CatalogObject::Type::TableFunction, "TableFunction"},
    {CatalogObject::Type::Procedure, "Procedure"},
    {CatalogObject::Type::Synonym, "Synonym"},
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

inline std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline bool StartsWithCi(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && Lower(s.substr(0, prefix.size())) == Lower(prefix);
}

inline bool ContainsCi(std::string_view hay, std::string_view needle) {
    return Lower(hay).find(Lower(needle)) != std::string::npos;
}

inline std::string Quote(std::string_view s) {
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

inline std::optional<std::string> ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline std::optional<Json> LoadJson(const std::string& path, Errors& errors) {
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

inline const std::string* GetString(const Json& obj, std::string_view key) {
    const Json* v = obj.get(key);
    return v && v->type == Json::Type::String ? &v->str : nullptr;
}

inline void CheckKeys(const Json& obj, std::initializer_list<std::string_view> allowed, const std::string& where, Errors& errors) {
    for (const auto& kv : obj.obj)
        if (std::find(allowed.begin(), allowed.end(), kv.first) == allowed.end())
            errors.add(where, "unknown field '" + kv.first + "'");
}

// ---------------------------------------------------------------------------------------------- catalog

// [[name, type]...] into `out`.
inline void LoadColumns(const Json* cols, std::vector<CatalogColumn>& out, const std::string& where, Errors& errors) {
    if (!cols) return;
    if (cols->type != Json::Type::Array) {
        errors.add(where, "columns must be an array");
        return;
    }
    for (const Json& c : cols->arr) {
        if (c.type != Json::Type::Array || c.arr.size() != 2 || c.arr[0].type != Json::Type::String ||
            c.arr[1].type != Json::Type::String || c.arr[0].str.empty()) {
            errors.add(where, "columns: expected [name, type] string pairs");
            continue;
        }
        out.push_back({c.arr[0].str, c.arr[1].str});
    }
}

inline std::optional<Catalog> LoadCatalog(const std::string& path, Errors& errors) {
    auto doc = LoadJson(path, errors);
    if (!doc) return std::nullopt;
    if (doc->type != Json::Type::Object) {
        errors.add(path, "not an object");
        return std::nullopt;
    }
    CheckKeys(*doc, {"_comment", "currentDatabase", "defaultSchema", "databases", "objects", "types"}, path, errors);
    Catalog catalog;
    const std::string* current = GetString(*doc, "currentDatabase");
    const std::string* schema = GetString(*doc, "defaultSchema");
    const Json* dbs = doc->get("databases");
    const Json* objects = doc->get("objects");
    const Json* types = doc->get("types");
    if (!current || !schema || !dbs || dbs->type != Json::Type::Array || !objects || objects->type != Json::Type::Array ||
        (types && types->type != Json::Type::Array)) {
        errors.add(path, "needs currentDatabase, defaultSchema (strings), databases and objects (arrays), optional types (array)");
        return std::nullopt;
    }
    catalog.currentDatabase = *current;
    catalog.defaultSchema = *schema;
    for (const Json& d : dbs->arr) {
        if (d.type != Json::Type::String || d.str.empty()) errors.add(path, "databases: expected non-empty strings");
        else catalog.databases.push_back(d.str);
    }
    // An empty database is a system object or type, present in every database.
    auto hasDb = [&](const std::string& name) {
        return name.empty() || std::any_of(catalog.databases.begin(), catalog.databases.end(),
                                           [&](const std::string& d) { return Lower(d) == Lower(name); });
    };
    if (catalog.currentDatabase.empty() || !hasDb(catalog.currentDatabase)) errors.add(path, "currentDatabase not in databases");
    std::set<std::string> seen;
    for (const Json& o : objects->arr) {
        std::string where = path + " object";
        const std::string *db = GetString(o, "database"), *sc = GetString(o, "schema"), *name = GetString(o, "name"),
                          *type = GetString(o, "type");
        if (!db || !sc || !name || !type || sc->empty() || name->empty()) {
            errors.add(where, "needs database, schema, name, type strings (schema and name non-empty)");
            continue;
        }
        where += " " + *db + "." + *sc + "." + *name;
        CheckKeys(o, {"database", "schema", "name", "type", "columns", "parameters", "target"}, where, errors);
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
        LoadColumns(o.get("columns"), obj.columns, where, errors);
        bool hasColumns = obj.type == CatalogObject::Type::Table || obj.type == CatalogObject::Type::View ||
                          obj.type == CatalogObject::Type::TableFunction;
        if (hasColumns == obj.columns.empty())
            errors.add(where, hasColumns ? "tables, views and table functions need columns" : "only tables, views and table functions have columns");
        if (const Json* params = o.get("parameters")) {
            bool takesParameters = obj.type == CatalogObject::Type::Procedure || obj.type == CatalogObject::Type::ScalarFunction ||
                                   obj.type == CatalogObject::Type::TableFunction;
            if (!takesParameters || params->type != Json::Type::Array) errors.add(where, "only procedures and functions have parameters (an array)");
            std::set<std::string> names;
            for (const Json& p : params->arr) {
                if (p.type != Json::Type::Array || p.arr.size() < 2 || p.arr[0].type != Json::Type::String ||
                    p.arr[1].type != Json::Type::String || p.arr[0].str.size() < 2 || p.arr[0].str[0] != '@' || p.arr[1].str.empty()) {
                    errors.add(where, "parameters: expected [\"@name\", type, flag...]");
                    continue;
                }
                CatalogParameter param;
                param.name = p.arr[0].str;
                param.type = p.arr[1].str;
                for (size_t k = 2; k < p.arr.size(); ++k) {
                    const Json& f = p.arr[k];
                    if (f.type == Json::Type::String && f.str == "output") param.output = true;
                    else if (f.type == Json::Type::String && f.str == "default") param.hasDefault = true;
                    else errors.add(where, "parameter " + param.name + ": flags are \"output\" and \"default\"");
                }
                if (!names.insert(Lower(param.name)).second) errors.add(where, "duplicate parameter " + param.name);
                obj.parameters.push_back(std::move(param));
            }
        }
        if (const std::string* target = GetString(o, "target")) obj.target = *target;
        if ((obj.type == CatalogObject::Type::Synonym) == obj.target.empty())
            errors.add(where, obj.target.empty() ? "synonyms need a target" : "only synonyms have a target");
        catalog.objects.push_back(std::move(obj));
    }
    if (types) {
        for (const Json& t : types->arr) {
            std::string where = path + " type";
            const std::string *db = GetString(t, "database"), *sc = GetString(t, "schema"), *name = GetString(t, "name");
            const Json* isTable = t.get("isTableType");
            if (!db || !sc || !name || sc->empty() || name->empty() || !isTable || isTable->type != Json::Type::Bool) {
                errors.add(where, "needs database, schema, name strings and isTableType (boolean)");
                continue;
            }
            where += " " + *db + "." + *sc + "." + *name;
            CheckKeys(t, {"database", "schema", "name", "isTableType", "columns"}, where, errors);
            CatalogType type;
            type.database = *db;
            type.schema = *sc;
            type.name = *name;
            type.isTableType = isTable->boolean;
            LoadColumns(t.get("columns"), type.columns, where, errors);
            if (type.isTableType == type.columns.empty())
                errors.add(where, type.isTableType ? "table types need columns" : "alias types have no columns");
            if (!hasDb(*db)) errors.add(where, "database not in databases");
            if (!seen.insert("type:" + Lower(*db + "." + *sc + "." + *name)).second) errors.add(where, "duplicate type");
            catalog.types.push_back(std::move(type));
        }
    }
    return catalog;
}

inline bool ParseVersion(const Json& c, const std::string& where, std::string& name, SqlVersion& version, Errors& errors) {
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

inline std::string DescribeItem(const CompletionItem& item) {
    std::string out = std::string(NameOf(kKinds, item.kind)) + ":" + item.label;
    if (item.insertText != item.label) out += "=" + item.insertText;
    return out;
}

// Contract invariants that hold for every Complete() result.
inline void CheckCompletionInvariants(std::string_view sql, size_t caret, const CompletionResult& r, std::vector<std::string>& issues) {
    if (r.replaceStart > caret || r.replaceStart + r.replaceLength < caret || r.replaceStart + r.replaceLength > sql.size()) {
        issues.push_back("replace range [" + std::to_string(r.replaceStart) + "," + std::to_string(r.replaceStart + r.replaceLength) +
                         ") does not contain the caret " + std::to_string(caret) + " within the input");
        return;
    }
    std::string typed(sql.substr(r.replaceStart, caret - r.replaceStart));
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
    // A list showing the same entry twice is a visible defect (e.g. a system type also listed as a built-in of the
    // same kind, or a synonym listed once per resolution).
    std::set<std::tuple<CompletionKind, std::string, std::string>> seen;
    for (const auto& item : r.items)
        if (!seen.emplace(item.kind, item.label, item.insertText).second)
            issues.push_back("duplicate item " + DescribeItem(item));
}

// Contract invariants that hold for every Classify() result.
inline void CheckClassifyInvariants(std::string_view sql, const std::vector<ColouredSpan>& spans, std::vector<std::string>& issues) {
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

}  // namespace editor_contract
