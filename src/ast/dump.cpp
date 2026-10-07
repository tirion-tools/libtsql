// libtsql AST: structural JSON dump of an AST (format shared with the .NET oracle).
#include "tsql/ast/dump.hpp"

#include "json_writer.hpp"

namespace tsql::ast {

namespace detail {

void JsonWriter::Node(const TSqlFragment* node) {
    if (node == nullptr) {
        out += "null";
        return;
    }
    out += "{\"$type\":";
    String(node->TypeName());
    out += ",\"$pos\":[";
    out += std::to_string(node->FirstTokenIndex);
    out += ',';
    out += std::to_string(node->LastTokenIndex);
    out += ',';
    out += std::to_string(node->StartOffset());
    out += ',';
    out += std::to_string(node->FragmentLength());
    out += ']';
    DumpMembers(*this, *node);
    out += '}';
}

void JsonWriter::String(std::string_view text) {
    static const char kHex[] = "0123456789abcdef";
    out += '"';
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c < 0x20) {
            out += "\\u00";
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        } else {
            out += ch;  // non-ASCII stays raw UTF-8
        }
    }
    out += '"';
}

}  // namespace detail

std::string DumpNodeJson(const TSqlFragment* node) {
    detail::JsonWriter w;
    w.Node(node);
    return std::move(w.out);
}

std::string DumpJson(const TSqlFragment* root, const std::vector<DumpError>& errors) {
    detail::JsonWriter w;
    w.out += "{\"errors\":[";
    for (std::size_t i = 0; i < errors.size(); ++i) {
        const DumpError& e = errors[i];
        if (i != 0) w.out += ',';
        w.out += "{\"Number\":" + std::to_string(e.Number);
        w.out += ",\"Offset\":" + std::to_string(e.Offset);
        w.out += ",\"Line\":" + std::to_string(e.Line);
        w.out += ",\"Column\":" + std::to_string(e.Column);
        w.out += ",\"Message\":";
        w.String(e.Message);
        w.out += '}';
    }
    w.out += "],\"tree\":";
    w.Node(root);
    w.out += '}';
    return std::move(w.out);
}

}  // namespace tsql::ast
