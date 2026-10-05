// ANTLR 2 picked the alternative by syntactic predicates in order: a money sign followed by a
// digit is Money even when an identifier would be longer ("£1abc" is Money "£1" + Identifier
// "abc"). ANTLR 4 takes the longest match; TSqlLexerBase::nextToken splits that case, since a
// predicate here would keep the lexer from caching its start state.
// Unterminated $( ... is reported like checkEOF(TokenKind.SqlCommandIdentifier).
Identifier
    : ( '$(' ( ~[)\n\r] | EndOfLine )+ ')' { setType(SqlCommandIdentifier); }
      | '$(' ( ~[)\n\r] | EndOfLine )* EOF { UnterminatedComplexToken(TokenKind::SqlCommandIdentifier); }
      | '$' ('@' | FirstLetter) (Letter | Digit)*
        {
            if (String_Equals(getText(), CodeGenerationSupporter::DollarPartition, StringComparison::OrdinalIgnoreCase))
                setType(DollarPartition);
            else
                setType(PseudoColumn);
        }
      | MoneySign ' '* (Minus | Plus)? Digit+ ( '.' Digit* Exponent? | Exponent )? { setType(Money); }
      | FirstLetter (Letter | Digit)* ( ':' { _input->LA(1) != ':' }? { setType(Label); } )?
      ) { TestLiterals(); }
    ;
