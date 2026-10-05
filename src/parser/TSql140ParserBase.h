// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql140ParserBaseInternal.cs
#pragma once

#include "TSql130ParserBase.h"

namespace tsql::parser {

class TSql140ParserBase : public TSql130ParserBase {
public:
    explicit TSql140ParserBase(antlr4::TokenStream* input) : TSql130ParserBase(input) {}

protected:
    void VerifyAllowedIndexOption140(IndexAffectingStatement statement, ast::IndexOption* option);   // TSql140ParserBaseInternal.cs:65
    void CheckForDataFileFormatProhibitedOptionsInOpenRowsetBulk(int64_t encounteredOptions,
                                                                 ast::TSqlFragment* relatedFragment);
    void CheckForParquetFormatProhibitedOptionsInOpenRowsetBulk(int64_t encounteredOptions,
                                                                ast::BulkOpenRowset* bulkOpenRowset);
    ast::Identifier* CreateIdentifierFromToken(antlr4::Token* token);
    void CheckForDataFileFormatProhibitedOptionsBulkInsert(int64_t encounteredOptions, ast::BulkInsertStatement* statement);   // TSql140ParserBaseInternal.cs:103
    ast::IdentifierOrScalarExpression* CreateIdentifierOrScalarExpressionFromIdentifier(ast::Identifier* identifier);   // TSql140ParserBaseInternal.cs:211
};

}  // namespace tsql::parser
