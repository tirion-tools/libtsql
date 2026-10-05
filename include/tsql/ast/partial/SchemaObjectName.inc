// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/SchemaObjectName.cs
// Included inside the generated struct SchemaObjectName (C# partial class members).
/// Name parts counted from the end: server.database.schema.base.
virtual ::tsql::ast::Identifier* ServerIdentifier() const { return ChooseIdentifier(4); }
virtual ::tsql::ast::Identifier* DatabaseIdentifier() const { return ChooseIdentifier(3); }
virtual ::tsql::ast::Identifier* SchemaIdentifier() const { return ChooseIdentifier(2); }
virtual ::tsql::ast::Identifier* BaseIdentifier() const { return ChooseIdentifier(1); }
/// Identifiers[Count - modifier], or null when that part was not specified.
::tsql::ast::Identifier* ChooseIdentifier(int modifier) const {
    const int index = static_cast<int>(Identifiers.size()) - modifier;
    return index < 0 ? nullptr : Identifiers[static_cast<std::size_t>(index)];
}
