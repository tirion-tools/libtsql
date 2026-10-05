// EOF alternative replaces checkEOF(TokenKind.String).
AsciiStringLiteral
    : '\'' ( ~['\n\r] | EndOfLine | '\'\'' )* '\''
    | '\'' ( ~['\n\r] | EndOfLine | '\'\'' )* EOF { UnterminatedComplexToken(TokenKind::String); }
    ;
