// The ANTLR 2 rule-init local `resetAcceptable` becomes a lexer member set by a leading action
// (ANTLR 4 runs lexer actions after the match, positioned where they appear in the rule).
WhiteSpace
    : { _wsResetAcceptable = (CurrentOffset() == _acceptableGoOffset); }
      ( WS_CHAR_WO_NEWLINE { if (_wsResetAcceptable) _acceptableGoOffset = CurrentOffset(); } )+
    | EndOfLine { _acceptableGoOffset = CurrentOffset(); }
    ;
