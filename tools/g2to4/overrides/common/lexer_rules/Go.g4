// (Colon ~':')=> Colon becomes a predicate on the character after ':'.
// GO is a batch separator only where the lexer allows it (_acceptableGoOffset); elsewhere the
// same text lexes as the Identifier rule would. A leading predicate would keep the lexer from
// caching its start state, so the offset is checked by a leading action instead.
Go
    : { _goAcceptable = (CurrentOffset() == _acceptableGoOffset); }
      'go'
      ( Letter { setType(Identifier); } | Digit { setType(Identifier); } )*
      ( ':' { _input->LA(1) != ':' }? { setType(Label); } )?
      { if (!_goAcceptable && getType() != Label) setType(Identifier); TestLiterals(); }
    ;
