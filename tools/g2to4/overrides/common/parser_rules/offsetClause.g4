// g2to4: sha=928047f1
// Hand override: ANTLR 2 consumed a token inside an action (see tApproxNext).
offsetClause returns [ast::OffsetClause* vResult = nullptr]
@init {
    auto& vResult = _localctx->vResult;
    auto& tFetch = _localctx->tFetch;
    auto& tFetch2 = _localctx->tFetch2;
    auto& tFetchRowOrRows = _localctx->tFetchRowOrRows;
    auto& tFetchRowOrRows2 = _localctx->tFetchRowOrRows2;
    auto& tFirstOrNext = _localctx->tFirstOrNext;
    auto& tFirstOrNext2 = _localctx->tFirstOrNext2;
    auto& tApproxNext = _localctx->tApproxNext;
    auto& tApproxNext2 = _localctx->tApproxNext2;
    auto& tOffset = _localctx->tOffset;
    auto& tOffsetRowOrRows = _localctx->tOffsetRowOrRows;
    auto& tOnly = _localctx->tOnly;
    auto& tOnly2 = _localctx->tOnly2;
    vResult = CreateFragment<ast::OffsetClause>();
    ast::ScalarExpression* vExpression{};
}
    : (
          tOffset=Identifier rv1=expression {vExpression = $rv1.vResult;} {
                Match(tOffset, CodeGenerationSupporter::Offset);
                UpdateTokenInfo(vResult, tOffset);
                vResult->set_OffsetExpression(vExpression);
            } tOffsetRowOrRows=Identifier {
                Match(tOffsetRowOrRows, CodeGenerationSupporter::Row, CodeGenerationSupporter::Rows);
                UpdateTokenInfo(vResult, tOffsetRowOrRows);
            } (
              tFetch=Fetch tFirstOrNext=Identifier ( {TryMatch(LT(-1), CodeGenerationSupporter::Approximate) || TryMatch(LT(-1), CodeGenerationSupporter::Approx)}? tApproxNext=. )? {
                    // ANTLR 2 consumed the FIRST/NEXT after APPROX[IMATE] inside this action
                    // (tFirstOrNext = LT(1); consume();); ANTLR 4 needs it in the grammar so
                    // prediction sees it: the wildcard keeps Match() reporting a wrong token
                    if (tApproxNext != nullptr)
                    {
                        vResult->set_WithApproximate(true);
                        UpdateTokenInfo(vResult, tFirstOrNext);
                        tFirstOrNext = tApproxNext;
                    }
                    Match(tFirstOrNext, CodeGenerationSupporter::First, CodeGenerationSupporter::Next);
                    UpdateTokenInfo(vResult, tFirstOrNext);
                } rv2=expression {vExpression = $rv2.vResult;} {
                    vResult->set_FetchExpression(vExpression);
                } tFetchRowOrRows=Identifier tOnly=Identifier {
                    Match(tFetchRowOrRows, CodeGenerationSupporter::Row, CodeGenerationSupporter::Rows);
                    Match(tOnly, CodeGenerationSupporter::Only);
                    UpdateTokenInfo(vResult, tOnly);
                    
                    // Validate: OFFSET cannot be used with FETCH APPROXIMATE
                    ValidateFetchApproximate(vResult);
                }
            )?
        | tFetch2=Fetch {
                UpdateTokenInfo(vResult, tFetch2);
            } tFirstOrNext2=Identifier ( {TryMatch(LT(-1), CodeGenerationSupporter::Approximate) || TryMatch(LT(-1), CodeGenerationSupporter::Approx)}? tApproxNext2=. )? {
                    // ANTLR 2 consumed the FIRST/NEXT after APPROX[IMATE] inside this action
                    // (tFirstOrNext = LT(1); consume();); ANTLR 4 needs it in the grammar so
                    // prediction sees it: the wildcard keeps Match() reporting a wrong token
                    if (tApproxNext2 != nullptr)
                    {
                        vResult->set_WithApproximate(true);
                        UpdateTokenInfo(vResult, tFirstOrNext2);
                        tFirstOrNext2 = tApproxNext2;
                    }
                    Match(tFirstOrNext2, CodeGenerationSupporter::First, CodeGenerationSupporter::Next);
                    UpdateTokenInfo(vResult, tFirstOrNext2);
                } rv3=expression {vExpression = $rv3.vResult;} {
                vResult->set_FetchExpression(vExpression);
            } tFetchRowOrRows2=Identifier tOnly2=Identifier {
                Match(tFetchRowOrRows2, CodeGenerationSupporter::Row, CodeGenerationSupporter::Rows);
                Match(tOnly2, CodeGenerationSupporter::Only);
                UpdateTokenInfo(vResult, tOnly2);
                
                // No validation needed for standalone FETCH APPROXIMATE (no OFFSET present)
            }
        )
    ;
