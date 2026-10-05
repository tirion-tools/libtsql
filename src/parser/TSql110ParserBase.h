// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql110ParserBaseInternal.cs
#pragma once

#include "TSql100ParserBase.h"

namespace tsql::parser {

class TSql110ParserBase : public TSql100ParserBase {
public:
    explicit TSql110ParserBase(antlr4::TokenStream* input) : TSql100ParserBase(input) {}

protected:
    void CheckWindowFrame(ast::WindowFrameClause* windowFrameClause);
    std::vector<std::string> OptionValidForCreateDatabase();   // TSql110ParserBaseInternal.cs:128
};

}  // namespace tsql::parser
