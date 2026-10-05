// ('(' '*')=> becomes a predicate so that longest-match does not swallow "--(*" into a comment.
SingleLineComment
    : '--(*' { setType(OdbcInitiator); }
    | '--' { !(_input->LA(1) == '(' && _input->LA(2) == '*') }? ~[\r\n]* { _acceptableGoOffset = CurrentOffset(); }
    ;
