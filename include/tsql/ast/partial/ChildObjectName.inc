// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/ChildObjectName.cs
// Included inside the generated struct ChildObjectName (C# partial class members).
// Name parts are shifted by one compared with SchemaObjectName: server.database.schema.base.child.
::tsql::ast::Identifier* ServerIdentifier() const override { return ChooseIdentifier(5); }
::tsql::ast::Identifier* DatabaseIdentifier() const override { return ChooseIdentifier(4); }
::tsql::ast::Identifier* SchemaIdentifier() const override { return ChooseIdentifier(3); }
::tsql::ast::Identifier* BaseIdentifier() const override { return ChooseIdentifier(2); }
virtual ::tsql::ast::Identifier* ChildIdentifier() const { return ChooseIdentifier(1); }
