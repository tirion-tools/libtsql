// ANTLR 2: (Semicolon (WS_CHAR_WO_NEWLINE)* Number)=> Semicolon | Semicolon {$setType(Semicolon);}
// A lexer rule cannot look past its own text in ANTLR 4, so the lookahead moves into an action.
ProcNameSemicolon
    : ';' { if (!NextIsWhitespaceThenNumber()) setType(Semicolon); }
    ;
