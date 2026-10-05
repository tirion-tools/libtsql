// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/Identifier.cs, SqlScriptDom/Parser/TSql/IdentifierLiteral.cs
#include <stdexcept>

#include "tsql/ast/ast.hpp"

namespace tsql::ast {

namespace {

// C# String.Replace: left-to-right, non-overlapping.
std::string Replace(std::string text, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

}  // namespace

std::string Identifier::DecodeIdentifier(const std::string& identifier, ::tsql::ast::QuoteType& quote) {
    if (Utf16Length(identifier) <= 2) {
        quote = ::tsql::ast::QuoteType::NotQuoted;
        return identifier;
    }
    if (identifier[0] == '[' || identifier[0] == '"') {
        // C#: identifier.Substring(1, identifier.Length - 2). The opening quote is one byte; drop the
        // whole last code point (C# drops one UTF-16 unit, which only differs for an astral last char).
        std::size_t end = identifier.size() - 1;
        while (end > 1 && (static_cast<unsigned char>(identifier[end]) & 0xC0) == 0x80) --end;
        std::string value = identifier.substr(1, end - 1);
        if (identifier[0] == '[') {
            quote = ::tsql::ast::QuoteType::SquareBracket;
            return Replace(std::move(value), "]]", "]");
        }
        quote = ::tsql::ast::QuoteType::DoubleQuote;
        return Replace(std::move(value), "\"\"", "\"");
    }
    quote = ::tsql::ast::QuoteType::NotQuoted;
    return identifier;
}

std::string Identifier::EncodeIdentifier(const std::string& identifier) {
    return "[" + Replace(identifier, "]", "]]") + "]";
}

std::string Identifier::EncodeIdentifier(const std::string& identifier, ::tsql::ast::QuoteType quoteType) {
    switch (quoteType) {
        case ::tsql::ast::QuoteType::NotQuoted:
            return identifier;
        case ::tsql::ast::QuoteType::SquareBracket:
            return "[" + Replace(identifier, "]", "]]") + "]";
        case ::tsql::ast::QuoteType::DoubleQuote:
            return "\"" + Replace(identifier, "\"", "\"\"") + "\"";
    }
    throw std::out_of_range("quoteType");
}

void Identifier::SetUnquotedIdentifier(const std::string& text) {
    Value = text;
    QuoteType = ::tsql::ast::QuoteType::NotQuoted;
}

void Identifier::SetIdentifier(const std::string& text) {
    Value = DecodeIdentifier(text, QuoteType);
}

void IdentifierLiteral::SetUnquotedIdentifier(const std::string& text) {
    Value = text;
    QuoteType = ::tsql::ast::QuoteType::NotQuoted;
}

void IdentifierLiteral::SetIdentifier(const std::string& text) {
    Value = Identifier::DecodeIdentifier(text, QuoteType);
}

}  // namespace tsql::ast
