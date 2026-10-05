// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql130ParserBaseInternal.cs
#pragma once

#include "TSql120ParserBase.h"

namespace tsql::parser {

class TSql130ParserBase : public TSql120ParserBase {
public:
    explicit TSql130ParserBase(antlr4::TokenStream* input) : TSql120ParserBase(input) {}

protected:
    void VerifyAllowedIndexOption130(IndexAffectingStatement statement, ast::IndexOption* option);   // TSql130ParserBaseInternal.cs:58
    bool NextIdentifierMatchesOneOf(const std::vector<std::string>& keywords);
    void CheckAndIncrementColumnCount(int& columnCount, antlr4::Token* token);   // TSql130ParserBaseInternal.cs:465
    void CheckCopyOptionDuplication(int& encountered, ast::CopyOptionKind newOption, antlr4::Token* token);   // TSql130ParserBaseInternal.cs:483
    void CheckCtasStatementHasDistributionOption(ast::CreateTableStatement* statement);   // TSql130ParserBaseInternal.cs:390
    void CheckExternalTableCtasStatementHasNotRejectedRowLocationOption(ast::CreateExternalTableStatement* statement);   // TSql130ParserBaseInternal.cs:407
    void CheckHekatonTableForInlineFilteredIndexes(ast::CreateTableStatement* statement);   // TSql130ParserBaseInternal.cs:301
    void CheckHekatonTableForNonClusteredColumnStoreIndexes(ast::CreateTableStatement* statement);   // TSql130ParserBaseInternal.cs:332
    void CheckRetentionPeriodDuration(ast::ScalarExpression* duration);   // TSql130ParserBaseInternal.cs:278
    void CheckTemporalGeneratedAlwaysColumns(ast::TableDefinition* definition, bool isInAlterStatement);   // TSql130ParserBaseInternal.cs:179
    void CheckTemporalPeriodInTableDefinition(ast::TableDefinition* definition, bool isInAlterStatement);   // TSql130ParserBaseInternal.cs:95
    void CheckValidWlmTimeLiteral(ast::StringLiteral* timeStringToken);   // TSql130ParserBaseInternal.cs:497
    ast::FunctionOptionKind ParseAlterCreateFunctionWithOption(antlr4::Token* token);   // TSql130ParserBaseInternal.cs:419 (`new`)
    bool IsMemoryOptimized(ast::CreateTableStatement* statement);   // TSql130ParserBaseInternal.cs:363
    void ThrowIfCompressionDelayValueOutOfRange(ast::Literal* value);   // TSql130ParserBaseInternal.cs:376
};

}  // namespace tsql::parser
