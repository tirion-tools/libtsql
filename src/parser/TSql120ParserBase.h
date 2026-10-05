// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql120ParserBaseInternal.cs
#pragma once

#include "TSql110ParserBase.h"

namespace tsql::parser {

class TSql120ParserBase : public TSql110ParserBase {
public:
    explicit TSql120ParserBase(antlr4::TokenStream* input) : TSql110ParserBase(input) {}

protected:
    void CheckLowPriorityLockWaitValue(ast::IntegerLiteral* maxDuration, ast::AbortAfterWaitType abortAfterWait);   // TSql120ParserBaseInternal.cs:49
    void VerifyAllowedOnlineIndexOptionLowPriorityLockWait(IndexAffectingStatement statement, ast::IndexOption* option, SqlVersionFlags versionFlags);   // TSql120ParserBaseInternal.cs:69
};

}  // namespace tsql::parser
