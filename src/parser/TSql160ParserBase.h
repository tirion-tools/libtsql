// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql160ParserBaseInternal.cs
#pragma once

#include "TSql150ParserBase.h"

namespace tsql::parser {

class TSql160ParserBase : public TSql150ParserBase {
public:
    explicit TSql160ParserBase(antlr4::TokenStream* input) : TSql150ParserBase(input) {}

protected:
    using TSql150ParserBase::CheckTemporalGeneratedAlwaysColumns;
    using TSql150ParserBase::CheckTemporalPeriodInTableDefinition;

    void CheckForConflictingOptionsInOpenRowsetBulkCosmos(ast::OpenRowsetCosmos* openRowsetCosmos);
    static ast::SqlDataTypeOption ParseDataType160(CsStr token);
    void CheckTemporalGeneratedAlwaysColumns(ast::TableDefinition* definition, bool isInAlterStatement, bool isLedgerSupported);   // TSql160ParserBaseInternal.cs:164
    void CheckTemporalPeriodInTableDefinition(ast::TableDefinition* definition, bool isInAlterStatement, bool isLedgerSupported);   // TSql160ParserBaseInternal.cs:79
    void VerifyAllowedIndexOption160(IndexAffectingStatement statement, ast::IndexOption* option);   // TSql160ParserBaseInternal.cs:53
};

}  // namespace tsql::parser
