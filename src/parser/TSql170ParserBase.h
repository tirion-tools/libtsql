// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql170ParserBaseInternal.cs
#pragma once

#include "TSql160ParserBase.h"

namespace tsql::parser {

class TSql170ParserBase : public TSql160ParserBase {
public:
    explicit TSql170ParserBase(antlr4::TokenStream* input) : TSql160ParserBase(input) {}

protected:
    bool ContainsVectorInLookahead();
    void ValidateFetchApproximate(ast::OffsetClause* offsetClause);
    ast::SecurityObjectKind ParseSecurityObjectKindTSql170(ast::Identifier* identifier1, ast::Identifier* identifier2);   // TSql170ParserBaseInternal.cs:56
};

}  // namespace tsql::parser
