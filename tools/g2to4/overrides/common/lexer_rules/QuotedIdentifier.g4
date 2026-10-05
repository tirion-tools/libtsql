// EOF alternatives replace checkEOF(TokenKind.QuotedIdentifier). The ANTLR 2 action ran before the
// closing '"' (text.Length > 1 there); it moves after it (> 2) because the ANTLR 4.13.2 C++
// runtime leaves the input at a mid-rule action's position when that action executes last
// (LexerActionExecutor::execute captures requiresSeek by value), which would truncate the token.
QuotedIdentifier
    : '[' ( ~[\]\n\r] | EndOfLine | ']]' )+ ']'
    | '[' ( ~[\]\n\r] | EndOfLine | ']]' )* EOF { UnterminatedComplexToken(TokenKind::QuotedIdentifier); }
    | '"' ( ~["\n\r] | EndOfLine | '""' )* '"' { if (Utf16Len(getText()) > 2) setType(AsciiStringOrQuotedIdentifier); else setType(AsciiStringLiteral); }
    | '"' ( ~["\n\r] | EndOfLine | '""' )* EOF { UnterminatedComplexToken(TokenKind::QuotedIdentifier); }
    ;
