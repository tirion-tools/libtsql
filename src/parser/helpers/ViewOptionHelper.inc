// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/ViewOptionHelper.cs
// Hand-written member of ViewOptionHelper (included into the generated class body).
    TSqlParseErrorException GetMatchingException(antlr4::Token* token) const override {
        return TSqlParseErrorException(CreateParseError("SQL46025", token, TSqlParserResource::SQL46025Message, token->getText()));
    }
