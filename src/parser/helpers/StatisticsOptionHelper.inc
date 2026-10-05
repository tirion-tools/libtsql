// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/StatisticsOptionHelper.cs
// Hand-written member of StatisticsOptionHelper (included into the generated class body).
    TSqlParseErrorException GetMatchingException(antlr4::Token* token) const override {
        return TSqlParseErrorException(CreateParseError("SQL46020", token, TSqlParserResource::SQL46020Message, token->getText()));
    }
