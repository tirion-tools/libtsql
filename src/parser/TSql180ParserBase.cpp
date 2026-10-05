// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql180ParserBaseInternal.cs
#include "TSql180ParserBase.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <stack>

namespace tsql::parser {

namespace {
[[maybe_unused]] constexpr size_t TT(ast::TSqlTokenType t) { return static_cast<size_t>(t); }
using TK = ast::TSqlTokenType;
[[maybe_unused]] constexpr size_t kEOF = antlr4::Token::EOF;
}  // namespace

// TSql180ParserBaseInternal.cs:80
void TSql180ParserBase::ValidateSemanticIndexExternalModel(ast::CreateSemanticIndexStatement* statement) {
    // EXTERNAL_MODEL is NOT required only when ALL columns are explicitly FULLTEXT
    // If any column is Vector, Hybrid, or NotSpecified (defaults to Vector), EXTERNAL_MODEL is required
    bool allColumnsAreFulltext = std::all_of(statement->Columns.begin(), statement->Columns.end(), [](auto* col) {
        return col->SearchType == ast::SemanticIndexSearchType::Fulltext;
    });
    // EXTERNAL_MODEL is required unless all columns are explicitly fulltext
    if (!allColumnsAreFulltext && statement->ExternalModelName == nullptr)
        ThrowParseErrorException("SQL46144", statement, TSqlParserResource::SQL46144Message, "EXTERNAL_MODEL");
}

}  // namespace tsql::parser
