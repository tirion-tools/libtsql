// Rule-init local -> member; nested comments go through a fragment without the Go bookkeeping
// (the inner rule's resetAcceptable was always false). EOF alternatives replace checkEOF.
MultilineComment
    : { _mlResetAcceptable = (CurrentOffset() == _acceptableGoOffset); }
      '/*' MultilineCommentBody* '*/'
      { if (_mlResetAcceptable) _acceptableGoOffset = CurrentOffset(); }
    | '/*' MultilineCommentBody* EOF { UnterminatedComplexToken(TokenKind::MultiLineComment); }
    ;

fragment MultilineCommentBody
    : EndOfLine
    | { _input->LA(2) != '/' }? '*'
    | { _input->LA(2) != '*' }? '/'
    | ~[*\n\r/]
    | NestedMultilineComment
    ;

fragment NestedMultilineComment
    : '/*' MultilineCommentBody* '*/'
    ;
