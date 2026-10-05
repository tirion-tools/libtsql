// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/BulkInsertStringOptionHelper.cs
// Hand-written member of BulkInsertStringOptionsHelper (included into the generated class body).
    // Regex.Replace(option.Value.ToUpperInvariant(), @"\s*", "")
    static std::string Trim(const ::tsql::ast::Literal* option) {
        return RemoveWhitespace(Str_ToUpperInvariant(option->Value));
    }
