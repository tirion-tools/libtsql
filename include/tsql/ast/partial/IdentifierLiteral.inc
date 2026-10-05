// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/Literal.cs, SqlScriptDom/Parser/TSql/IdentifierLiteral.cs
// Included inside the generated struct IdentifierLiteral (C# partial class members).
::tsql::ast::LiteralType LiteralType() const override { return ::tsql::ast::LiteralType::Identifier; }
/// Sets Value to `text` as-is and QuoteType to NotQuoted.
void SetUnquotedIdentifier(const std::string& text);
/// Sets Value to the decoded identifier text and QuoteType to its quoting.
void SetIdentifier(const std::string& text);
