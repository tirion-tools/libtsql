// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TriggerOptionHelper.cs
// Hand-written member of TriggerOptionHelper (included into the generated class body).
    TSqlParseErrorException GetMatchingException(antlr4::Token* token) const override {
        return TSqlParseErrorException(CreateParseError("SQL46007", token, TSqlParserResource::SQL46007Message, token->getText()));
    }
