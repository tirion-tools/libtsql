// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/Identifier.cs
// Included inside the generated struct Identifier (C# partial class members).
static constexpr int MaxIdentifierLength = 128;
/// Strips [..] or ".." quoting and unescapes ]] / "". Text of at most 2 UTF-16 units is returned as-is.
static std::string DecodeIdentifier(const std::string& identifier, ::tsql::ast::QuoteType& quote);
/// Wraps in [..], escaping ].
static std::string EncodeIdentifier(const std::string& identifier);
/// Quotes per `quoteType`; throws std::out_of_range for undefined values.
static std::string EncodeIdentifier(const std::string& identifier, ::tsql::ast::QuoteType quoteType);
void SetUnquotedIdentifier(const std::string& text);
void SetIdentifier(const std::string& text);
