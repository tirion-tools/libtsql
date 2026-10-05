// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql100ParserBaseInternal.cs
#pragma once

#include "TSql90ParserBase.h"

namespace tsql::parser {

class TSql100ParserBase : public TSql90ParserBase {
public:
    explicit TSql100ParserBase(antlr4::TokenStream* input) : TSql90ParserBase(input) {}

protected:
    static ast::SqlDataTypeOption ParseDataType100(CsStr token);
    void CheckBoundingBoxParameterDuplication(int current, ast::BoundingBoxParameterType newOption, antlr4::Token* token);   // TSql100ParserBaseInternal.cs:109
    void CheckBrokerPriorityParameterDuplication(int current, ast::BrokerPriorityParameterType newOption, antlr4::Token* token);   // TSql100ParserBaseInternal.cs:90
    void CheckComparisonOperandForIndexFilter(ast::ScalarExpression* rightOperand, bool convertAllowed);   // TSql100ParserBaseInternal.cs:177
    void CheckForCellsPerObjectValueRange(ast::Literal* value);   // TSql100ParserBaseInternal.cs:260
    void CheckIfValidSpatialIndexOptionValue(IndexAffectingStatement statement, ast::IndexOption* option);   // TSql100ParserBaseInternal.cs:121
    void CheckPartitionAllSpecifiedForIndexRebuild(ast::PartitionSpecifier* partitionSpecifier, std::vector<ast::IndexOption*>& indexOptions);   // TSql100ParserBaseInternal.cs:220
    ast::AutoCleanupChangeTrackingOptionDetail* CreateAutoCleanupDetail(antlr4::Token* firstToken, antlr4::Token* lastToken, bool& autoCleanupEncountered);   // TSql100ParserBaseInternal.cs:49
    void SetFileStreamStorageOption(ast::ColumnStorageOptions* storageOptions, antlr4::Token* fileStreamToken, ast::DataTypeReference* columnType, IndexAffectingStatement statementType);   // TSql100ParserBaseInternal.cs:134
    void SetSparseStorageOption(ast::ColumnStorageOptions* columnStorage, ast::SparseColumnOption option, antlr4::Token* token, IndexAffectingStatement statementType);   // TSql100ParserBaseInternal.cs:155
    void ThrowIfTooLargeAuditFileSize(ast::Literal* size, int shift);   // TSql100ParserBaseInternal.cs:250
    void ThrowIfWrongGuidFormat(ast::Literal* literal);   // TSql100ParserBaseInternal.cs:240
    void UpdateBoundingBoxParameterEncounteredOptions(int& encountered, ast::BoundingBoxParameter* vBoundingBoxParameter);   // TSql100ParserBaseInternal.cs:116
    void UpdateBrokerPriorityEncounteredOptions(int& encountered, ast::BrokerPriorityParameter* vBrokerPriorityParameter);   // TSql100ParserBaseInternal.cs:97
};

}  // namespace tsql::parser
