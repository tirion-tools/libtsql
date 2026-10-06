// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql170ParserBaseInternal.cs
#include "TSql170ParserBase.h"

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

bool TSql170ParserBase::ContainsVectorInLookahead() {
    const int LookaheadLimit = 100;
    for (int i = 1; i <= LookaheadLimit; i++) {
        antlr4::Token* token = LT(i);
        if (token == nullptr || token->getType() == kEOF) break;
        if (token->getType() == TT(TK::Identifier) && TextMatches(token, CodeGenerationSupporter::Vector))
            return true;
    }
    return false;
}

void TSql170ParserBase::ValidateFetchApproximate(ast::OffsetClause* offsetClause) {
    if (offsetClause != nullptr && offsetClause->WithApproximate && offsetClause->OffsetExpression != nullptr)
        ThrowParseErrorException("SQL46145", offsetClause, TSqlParserResource::SQL46145Message);
}

// TSql170ParserBaseInternal.cs:56 (15 C# lines) (hand-fixed)
ast::SecurityObjectKind TSql170ParserBase::ParseSecurityObjectKindTSql170(ast::Identifier* identifier1, ast::Identifier* identifier2) {
    if (identifier1 == nullptr) throw std::invalid_argument("identifier1");   // ArgumentNullException
    if (Str_ToUpperInvariant(identifier1->Value) == CodeGenerationSupporter::External) {
        Match(identifier2, CodeGenerationSupporter::Model);
        return ast::SecurityObjectKind::ExternalModel;
    }
    // Fall back to the base class implementation for all other cases
    return ParseSecurityObjectKind(identifier1, identifier2);
}

}  // namespace tsql::parser
