// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/GroupByOptionHelper.cs
// Hand-written member of GroupByOptionHelper (included into the generated class body).
    TSqlParseErrorException GetMatchingException(antlr4::Token* token) const override {
        return TSqlParseErrorException(CreateParseError("SQL46023", token, TSqlParserResource::SQL46023Message, token->getText()));
    }
