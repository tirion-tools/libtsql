// ANTLR 4 runtime adaptations shared by every converted parser (ANTLR 2 semantics).
#include "ParserRuntime.h"

#include <algorithm>
#include <cstdint>

namespace tsql::parser {

// ============================================================================ error strategy

void TSqlBailErrorStrategy::recover(antlr4::Parser*, std::exception_ptr e) { std::rethrow_exception(e); }

antlr4::Token* TSqlBailErrorStrategy::recoverInline(antlr4::Parser* recognizer) {
    throw antlr4::InputMismatchException(recognizer);
}

void TSqlBailErrorStrategy::sync(antlr4::Parser*) {}

void TSqlBailErrorStrategy::reportError(antlr4::Parser*, const antlr4::RecognitionException&) {}

// ============================================================================ prediction

size_t TSqlParserATNSimulator::execATN(antlr4::dfa::DFA& dfa, antlr4::dfa::DFAState* s0, antlr4::TokenStream* input,
                                       size_t startIndex, antlr4::ParserRuleContext* outerContext) {
    try {
        return antlr4::atn::ParserATNSimulator::execATN(dfa, s0, input, startIndex, outerContext);
    } catch (antlr4::NoViableAltException&) {
        // ANTLR 2 (k=2) entered the first alternative whose two tokens of lookahead and left-edge
        // predicates matched, so the syntax error surfaced inside it rather than at the decision.
        // Only runs where ANTLR 4 finds no viable alternative (never on bench-big.sql). Measured on the
        // 1,914 broken SELECTs: exact two-token match 1,563 -> + loop exits 1,598 -> + linear
        // approximate match 1,614 identical to ScriptDom; deciding with one token where one
        // suffices (as ANTLR 2's code generator did) made it worse (1,518).
        input->seek(startIndex);
        std::unique_ptr<antlr4::atn::ATNConfigSet> configs =
            computeStartState(dfa.atnStartState, &antlr4::ParserRuleContext::EMPTY, false);
        for (int k = 0; k < 2 && configs != nullptr; ++k) {
            const size_t t = input->LA(1);
            configs = computeReachSet(configs.get(), t, false);
            if (t == antlr4::Token::EOF) break;
            input->consume();
        }
        input->seek(startIndex);
        size_t best = SIZE_MAX;
        static const std::vector<Ref<antlr4::atn::ATNConfig>> kNone;
        for (const auto& c : configs ? configs->configs : kNone) {
            if (c->alt >= best) continue;
            if (c->semanticContext != antlr4::atn::SemanticContext::Empty::Instance &&
                !evalSemanticContext(c->semanticContext, outerContext, c->alt, false))
                continue;
            best = c->alt;
        }
        if (best != SIZE_MAX) return best;
        {
            // ANTLR 2's k=2 lookahead is linear approximate: an alternative also matches when LA(1)
            // can start it and LA(2) can be its second token after *any* first token.
            const size_t la1 = input->LA(1);
            const size_t la2 = input->LA(2);
            if (la1 != antlr4::Token::EOF) {
                auto start = computeStartState(dfa.atnStartState, &antlr4::ParserRuleContext::EMPTY, false);
                auto first = computeReachSet(start.get(), la1, false);
                std::vector<size_t> depth1;
                if (first)
                    for (const auto& c : first->configs) depth1.push_back(c->alt);
                std::vector<size_t> depth2;
                for (size_t t = 1; t <= atn.maxTokenType && !depth1.empty(); ++t) {
                    auto r1 = computeReachSet(start.get(), t, false);
                    if (!r1) continue;
                    auto r2 = computeReachSet(r1.get(), la2, false);
                    if (r2)
                        for (const auto& c : r2->configs) depth2.push_back(c->alt);
                }
                for (const auto& c : first ? first->configs : kNone) {
                    if (c->alt >= best || std::find(depth2.begin(), depth2.end(), c->alt) == depth2.end()) continue;
                    if (c->semanticContext != antlr4::atn::SemanticContext::Empty::Instance &&
                        !evalSemanticContext(c->semanticContext, outerContext, c->alt, false))
                        continue;
                    best = c->alt;
                }
                if (best != SIZE_MAX) return best;
            }
        }
        // ANTLR 2's (...)* and (...)+ loops exit whenever the lookahead does not start another
        // iteration, without checking what follows (its (...)? blocks do check the follow set):
        // the error surfaces after the loop.
        const size_t exit = LoopExitAlternative(dfa.atnStartState);
        if (exit != antlr4::atn::ATN::INVALID_ALT_NUMBER) return exit;
        throw;
    }
}

size_t TSqlParserATNSimulator::LoopExitAlternative(const antlr4::atn::DecisionState* d) {
    using antlr4::atn::ATNStateType;
    if (d->getStateType() == ATNStateType::STAR_LOOP_ENTRY || d->getStateType() == ATNStateType::PLUS_LOOP_BACK)
        return d->nonGreedy ? 1 : 2;
    return antlr4::atn::ATN::INVALID_ALT_NUMBER;
}

}  // namespace tsql::parser
