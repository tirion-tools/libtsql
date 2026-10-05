// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlFabricDWParserBaseInternal.cs
#pragma once

#include "TSql150ParserBase.h"

namespace tsql::parser {

class TSqlFabricDWParserBase : public TSql150ParserBase {
public:
    explicit TSqlFabricDWParserBase(antlr4::TokenStream* input) : TSql150ParserBase(input) {}

protected:
    using TSql150ParserBase::CheckTemporalGeneratedAlwaysColumns;
    using TSql150ParserBase::CheckTemporalPeriodInTableDefinition;

    void CheckForConflictingOptionsInOpenRowsetBulkCosmos(ast::OpenRowsetCosmos* openRowsetCosmos);   // TSqlFabricDWParserBaseInternal.cs:325
    static ast::SqlDataTypeOption ParseDataTypeFabricDW(CsStr token);   // TSqlFabricDWParserBaseInternal.cs:59
    void CheckTemporalGeneratedAlwaysColumns(ast::TableDefinition* definition, bool isInAlterStatement, bool isLedgerSupported);   // TSqlFabricDWParserBaseInternal.cs:164
    void CheckTemporalPeriodInTableDefinition(ast::TableDefinition* definition, bool isInAlterStatement, bool isLedgerSupported);   // TSqlFabricDWParserBaseInternal.cs:79
    void VerifyAllowedIndexOptionFabricDW(IndexAffectingStatement statement, ast::IndexOption* option);   // TSqlFabricDWParserBaseInternal.cs:53
};

}  // namespace tsql::parser
