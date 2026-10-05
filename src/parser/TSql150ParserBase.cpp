// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql150ParserBaseInternal.cs
#include "TSql150ParserBase.h"

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

// TSql150ParserBaseInternal.cs:49
void TSql150ParserBase::VerifyAllowedIndexOption150(IndexAffectingStatement statement, ast::IndexOption* option) {
    VerifyAllowedIndexOption(statement, option, SqlVersionFlags::TSql150);
    VerifyAllowedOnlineIndexOptionLowPriorityLockWait(statement, option, SqlVersionFlags::TSql150);
}

}  // namespace tsql::parser
