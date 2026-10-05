// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TableHintOptionsHelper.cs
// Hand-written member of TableHintOptionsHelper (included into the generated class body).
    TSqlParseErrorException GetMatchingException(antlr4::Token* token) const override {
        return TSqlParseErrorException(CreateParseError("SQL46022", token, TSqlParserResource::SQL46022Message, token->getText()));
    }
