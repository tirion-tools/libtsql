// EOF alternative replaces checkEOF(TokenKind.String).
UnicodeStringLiteral
    : 'n\'' ( ~['\n\r] | EndOfLine | '\'\'' )* '\''
    | 'n\'' ( ~['\n\r] | EndOfLine | '\'\'' )* EOF { UnterminatedComplexToken(TokenKind::String); }
    ;
