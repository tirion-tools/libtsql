// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/MultiPartIdentifier.cs
// Included inside the generated struct MultiPartIdentifier (C# partial class members).
/// C# indexer this[int]: Identifiers[index] (unchecked, like std::vector::operator[]).
::tsql::ast::Identifier*& operator[](int index) { return Identifiers[static_cast<std::size_t>(index)]; }
::tsql::ast::Identifier* operator[](int index) const { return Identifiers[static_cast<std::size_t>(index)]; }
int Count() const { return static_cast<int>(Identifiers.size()); }
