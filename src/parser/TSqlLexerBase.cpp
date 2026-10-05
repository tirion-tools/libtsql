// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlLexerBaseInternal.cs
#include "TSqlLexerBase.h"

#include <algorithm>
#include <cstring>

#include "generated/support/Keywords.h"
#include "tsql/ast/generated/token_types.hpp"

namespace tsql::parser {

int TSqlLexerBase::CurrentOffset() {
    size_t i = _input->index();
    return (_utf16 && i < _utf16->size()) ? (*_utf16)[i] : static_cast<int>(i);
}

void TSqlLexerBase::TestLiterals() {
    std::string text = getText();
    for (char c : text)
        if (static_cast<unsigned char>(c) >= 0x80) return;   // every literal is ASCII
    std::string lower = AsciiLower(text);
    auto* begin = std::begin(kKeywords);
    auto* end = std::end(kKeywords);
    auto* it = std::lower_bound(begin, end, lower, [](const KeywordEntry& e, const std::string& s) { return std::strcmp(e.text, s.c_str()) < 0; });
    if (it != end && lower == it->text) setType(static_cast<size_t>(it->type));
}

bool TSqlLexerBase::IsMoneySign(size_t c) {
    switch (c) {
        case 0x0024: case 0x00A3: case 0x00A4: case 0x00A5: case 0x09F2: case 0x09F3: case 0x0E3F: case 0x20AC:
        case 0x20A1: case 0x20A2: case 0x20A3: case 0x20A4: case 0x20A6: case 0x20A7: case 0x20A8: case 0x20A9:
        case 0x20AA: case 0x20AB:
            return true;
        default:
            return false;
    }
}

namespace {
// The code point at code-point index `i` of the input.
size_t CodePointAt(antlr4::CharStream* input, size_t i) {
    const std::string utf8 = input->getText(antlr4::misc::Interval(i, i));
    if (utf8.empty()) return antlr4::Token::EOF;
    const auto* p = reinterpret_cast<const unsigned char*>(utf8.data());
    if (p[0] < 0x80) return p[0];
    const int extra = p[0] >= 0xF0 ? 3 : p[0] >= 0xE0 ? 2 : 1;
    size_t c = p[0] & (0x3F >> extra);
    for (int k = 1; k <= extra && k < static_cast<int>(utf8.size()); ++k) c = (c << 6) | (p[k] & 0x3F);
    return c;
}
bool IsDigit(size_t c) { return c >= '0' && c <= '9'; }
}  // namespace

std::unique_ptr<antlr4::Token> TSqlLexerBase::nextToken() {
    auto token = antlr4::Lexer::nextToken();
    const auto type = token->getType();
    if (type != static_cast<size_t>(ast::TSqlTokenType::Identifier) &&
        type != static_cast<size_t>(ast::TSqlTokenType::Label))
        return token;
    // Only a non-ASCII money sign can start an identifier ('$' is not a first letter).
    if (static_cast<unsigned char>(token->getText()[0]) < 0x80) return token;
    const size_t start = token->getStartIndex();
    const size_t stop = token->getStopIndex();
    if (!IsMoneySign(CodePointAt(_input, start)) || !IsDigit(CodePointAt(_input, start + 1))) return token;
    // Money: MoneySign Digit+ Exponent?, where an identifier can only hold 'e' Digit* of the exponent.
    size_t end = start + 1;
    while (end <= stop && IsDigit(CodePointAt(_input, end))) ++end;
    if (end <= stop && (CodePointAt(_input, end) | 0x20) == 'e') {
        ++end;
        while (end <= stop && IsDigit(CodePointAt(_input, end))) ++end;
    }
    if (end > stop) return token;   // the whole identifier is money-shaped: the money alternative already won the tie
    _input->seek(end);
    hitEOF = false;   // the identifier may have run to EOF; the rest of it is still to be lexed
    return _factory->create({this, _input}, static_cast<size_t>(ast::TSqlTokenType::Money),
                            _input->getText(antlr4::misc::Interval(start, end - 1)), token->getChannel(), start,
                            end - 1, token->getLine(), token->getCharPositionInLine());
}

namespace {
bool IsWsCharWoNewline(size_t c) {
    return c <= 0x0008 || c == 0x0009 || c == 0x000B || c == 0x000C || (c >= 0x000E && c <= 0x001F) || c == 0x0020 ||
           c == 0x0085 || c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x200B || c == 0x2028 ||
           c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
}  // namespace

bool TSqlLexerBase::NextIsWhitespaceThenNumber() {
    // (Semicolon (WS_CHAR_WO_NEWLINE)* Number)=> ; Number starts with a digit or '.' (Dot alone is a Number alt)
    for (ssize_t k = 1;; ++k) {
        size_t c = _input->LA(k);
        if (c == antlr4::Token::EOF) return false;
        if (IsWsCharWoNewline(c)) continue;
        return (c >= '0' && c <= '9') || c == '.';
    }
}

void TSqlLexerBase::UnterminatedComplexToken(TokenKind kind) {
    if (_error) return;
    LexerError e;
    switch (kind) {
        case TokenKind::String: e.kind = LexerError::Kind::UnterminatedString; break;
        case TokenKind::QuotedIdentifier: e.kind = LexerError::Kind::UnterminatedQuotedIdentifier; break;
        case TokenKind::SqlCommandIdentifier: e.kind = LexerError::Kind::UnterminatedSqlCommandIdentifier; break;
        default: e.kind = LexerError::Kind::UnterminatedMultiLineComment; break;
    }
    e.startIndex = tokenStartCharIndex;
    e.endIndex = _input->index();
    e.text = getText();
    _error = e;
}

void TSqlLexerBase::notifyListeners(const antlr4::LexerNoViableAltException&) {
    if (_error) return;
    LexerError e;
    e.kind = LexerError::Kind::UnexpectedChar;
    e.startIndex = tokenStartCharIndex;
    e.endIndex = _input->index();
    e.text = _input->getText(antlr4::misc::Interval(tokenStartCharIndex, tokenStartCharIndex));
    _error = e;
}

bool TSqlLexerBase::IsValueTooLargeForTokenInteger(const std::string& source) {
    const size_t MaxIntegerLength = 10;
    const size_t MaxIntegerLengthWithLeadingZero = MaxIntegerLength + 1;
    size_t tokenLength = source.size();
    if (tokenLength > MaxIntegerLengthWithLeadingZero) return true;
    if (tokenLength >= MaxIntegerLength) return std::stoll(source) > 2147483647LL;
    return false;
}

}  // namespace tsql::parser
