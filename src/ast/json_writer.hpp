// libtsql AST: JSON writer used by the generated DumpJson member walkers.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tsql/ast/ast.hpp"

namespace tsql::ast::detail {

class JsonWriter {
public:
    std::string out;

    /// NODE or null.
    void Node(const TSqlFragment* node);
    void String(std::string_view text);

    // Members always follow "$pos", so every key is comma-prefixed.
    void Key(const char* name) {
        out += ",\"";
        out += name;
        out += "\":";
    }
    void Fragment(const char* name, const TSqlFragment* value) {
        Key(name);
        Node(value);
    }
    template <class T>
    void Collection(const char* name, const std::vector<T*>& values) {
        Key(name);
        out += '[';
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) out += ',';
            Node(values[i]);
        }
        out += ']';
    }
    void Value(const char* name, bool value) {
        Key(name);
        out += value ? "true" : "false";
    }
    void Value(const char* name, std::int32_t value) {
        Key(name);
        out += std::to_string(value);
    }
    void Value(const char* name, const std::optional<bool>& value) {
        if (value) {
            Value(name, *value);
        } else {
            Key(name);
            out += "null";
        }
    }
    void Value(const char* name, const std::optional<std::string>& value) {
        Key(name);
        if (value) {
            String(*value);
        } else {
            out += "null";
        }
    }
    template <class E>
    void Enum(const char* name, E value) {
        Key(name);
        String(ToString(value));
    }
    template <class E>
    void Enum(const char* name, const std::optional<E>& value) {
        Key(name);
        if (value) {
            String(ToString(*value));
        } else {
            out += "null";
        }
    }
};

/// Generated: writes the Ast.xml members of `node` (base chain first) as ,"Name":value pairs.
void DumpMembers(JsonWriter& w, const TSqlFragment& node);

}  // namespace tsql::ast::detail
