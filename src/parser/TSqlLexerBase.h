// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlLexerBaseInternal.cs
// Base of every generated TSql<ver>Lexer: GO-offset tracking, keyword (literals table) lookup, and
// the premature-EOF / unexpected-character errors SqlScriptDOM's lexer reports.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "antlr4-runtime.h"
#include "CsCompat.h"
#include "generated/support/CodeGenerationSupporter.h"

namespace tsql::parser {

/// First lexer error (SqlScriptDOM stops at the first one and returns an empty script).
struct LexerError {
    enum class Kind { UnexpectedChar, UnterminatedString, UnterminatedQuotedIdentifier, UnterminatedSqlCommandIdentifier,
                      UnterminatedMultiLineComment };
    Kind kind;
    size_t startIndex;   // code-point index of the token start (or the offending char)
    size_t endIndex;     // code-point index where lexing stopped
    std::string text;    // token text so far
};

class TSqlLexerBase : public antlr4::Lexer {
public:
    explicit TSqlLexerBase(antlr4::CharStream* input) : antlr4::Lexer(input) {}

    /// UTF-16 offset of each code point index (size = code points + 1).
    void SetUtf16Offsets(const std::vector<int>* offsets) { _utf16 = offsets; }
    const std::optional<LexerError>& Error() const { return _error; }

    void notifyListeners(const antlr4::LexerNoViableAltException& e) override;
    /// ANTLR 2 preferred Money over an identifier when a money sign is directly followed by a digit
    /// ("£1abc" is Money "£1" + Identifier "abc"). ANTLR 4 takes the longest match; a predicate on
    /// the identifier alternative would keep the lexer from caching its start state, so the
    /// rare case is split here instead.
    std::unique_ptr<antlr4::Token> nextToken() override;

protected:
    enum class TokenKind { Common = 0, String = 1, SqlCommandIdentifier = 2, QuotedIdentifier = 3, MultiLineComment = 4 };

    /// TSqlLexerBaseInternal.CurrentOffset: UTF-16 offset of the current input position.
    int CurrentOffset();
    /// ANTLR 2 testLiterals: an identifier whose text is in the literals table becomes that keyword.
    void TestLiterals();
    static bool IsMoneySign(size_t c);
    /// ProcNameSemicolon lookahead: (WS_CHAR_WO_NEWLINE)* Number after the ';'.
    bool NextIsWhitespaceThenNumber();
    /// checkEOF(kind) for a token that reached EOF before its terminator.
    void UnterminatedComplexToken(TokenKind kind);
    static bool IsValueTooLargeForTokenInteger(const std::string& source);

    int _acceptableGoOffset = 0;
    bool _wsResetAcceptable = false;
    bool _mlResetAcceptable = false;
    bool _goAcceptable = false;

private:
    const std::vector<int>* _utf16 = nullptr;
    std::optional<LexerError> _error;
};

}  // namespace tsql::parser
