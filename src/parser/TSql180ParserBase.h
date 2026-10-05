// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql180ParserBaseInternal.cs
#pragma once

#include "TSql170ParserBase.h"

namespace tsql::parser {

class TSql180ParserBase : public TSql170ParserBase {
public:
    explicit TSql180ParserBase(antlr4::TokenStream* input) : TSql170ParserBase(input) {}

protected:
    void ValidateSemanticIndexExternalModel(ast::CreateSemanticIndexStatement* statement);   // TSql180ParserBaseInternal.cs:80
};

}  // namespace tsql::parser
