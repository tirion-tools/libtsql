// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/ProcedureOptionHelper.cs
// Hand-written member of ProcedureOptionHelper (included into the generated class body).
    TSqlParseErrorException GetMatchingException(antlr4::Token* token) const override {
        return TSqlParseErrorException(CreateParseError("SQL46006", token, TSqlParserResource::SQL46006Message, token->getText()));
    }
