// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql150ParserBaseInternal.cs
#pragma once

#include "TSql140ParserBase.h"

namespace tsql::parser {

class TSql150ParserBase : public TSql140ParserBase {
public:
    explicit TSql150ParserBase(antlr4::TokenStream* input) : TSql140ParserBase(input) {}

protected:
    void VerifyAllowedIndexOption150(IndexAffectingStatement statement, ast::IndexOption* option);   // TSql150ParserBaseInternal.cs:49
};

}  // namespace tsql::parser
