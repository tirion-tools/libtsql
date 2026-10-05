// ANTLR 4 runtime adaptations shared by every converted parser (ANTLR 2 semantics).
#pragma once

#include "antlr4-runtime.h"

namespace tsql::parser {

/// ANTLR 2 semantics with defaultErrorHandler=false: no recovery inside rules; recognition
/// exceptions propagate to the `exception catch[...]` handlers SqlScriptDOM declares.
class TSqlBailErrorStrategy : public antlr4::DefaultErrorStrategy {
public:
    void recover(antlr4::Parser* recognizer, std::exception_ptr e) override;
    antlr4::Token* recoverInline(antlr4::Parser* recognizer) override;
    void sync(antlr4::Parser* recognizer) override;
    void reportError(antlr4::Parser* recognizer, const antlr4::RecognitionException& e) override;
};

/// ANTLR 4 defers a prediction failure to the caller when some alternative could leave the
/// decision's rule (getAltThatFinishedDecisionEntryRule); ANTLR 2 throws NoViableAlt at the
/// decision itself, which decides which rule's `exception` handler recovers. Never defer.
class TSqlParserATNSimulator : public antlr4::atn::ParserATNSimulator {
public:
    using antlr4::atn::ParserATNSimulator::ParserATNSimulator;

    /// Re-entrant: a speculative parse (TSql80ParserBase::Speculate) runs inside predicate
    /// evaluation of an outer prediction, so the simulator's per-prediction fields are restored.
    size_t adaptivePredict(antlr4::TokenStream* input, size_t decision, antlr4::ParserRuleContext* outerContext) override {
        auto* savedInput = _input;
        size_t savedStart = _startIndex;
        auto* savedOuter = _outerContext;
        auto* savedDfa = _dfa;
        struct Restore {
            TSqlParserATNSimulator* s;
            antlr4::TokenStream* i;
            size_t st;
            antlr4::ParserRuleContext* o;
            antlr4::dfa::DFA* d;
            ~Restore() { s->_input = i; s->_startIndex = st; s->_outerContext = o; s->_dfa = d; }
        } restore{this, savedInput, savedStart, savedOuter, savedDfa};
        return antlr4::atn::ParserATNSimulator::adaptivePredict(input, decision, outerContext);
    }

protected:
    size_t getAltThatFinishedDecisionEntryRule(antlr4::atn::ATNConfigSet*) override {
        return antlr4::atn::ATN::INVALID_ALT_NUMBER;
    }
    /// ANTLR 2 decided with two tokens of lookahead (plus predicates): when the first two tokens
    /// fit an alternative it committed to it and the error surfaced inside. ANTLR 4 looks further
    /// and reports no viable alternative at the decision instead. When no alternative survives
    /// but some survived the first two tokens, predict the first of those (whose predicates hold).
    size_t execATN(antlr4::dfa::DFA& dfa, antlr4::dfa::DFAState* s0, antlr4::TokenStream* input, size_t startIndex,
                   antlr4::ParserRuleContext* outerContext) override;
    /// Alternative number of the exit branch of a (...)* or (...)+ loop decision, or INVALID.
    static size_t LoopExitAlternative(const antlr4::atn::DecisionState* d);
    /// SLL prediction without collected predicates: the same closure as ANTLR's (the same
    /// configurations and closureBusy checks in the same order) without a configuration object per
    /// step, and a configuration that falls off a rule replays that rule's recorded follow walk.
    void closureCheckingStopState(const Ref<antlr4::atn::ATNConfig>& config, antlr4::atn::ATNConfigSet* configs,
                                  antlr4::atn::ATNConfig::Set& closureBusy, bool collectPredicates, bool fullCtx,
                                  int depth, bool treatEofAsEpsilon) override;

private:
    struct SecondTokens;
    struct FallOff;
    /// The tokens that can follow alternative `alt`'s first token in decision `dfa`, from its SLL
    /// start configurations; computed once per (decision, alternative).
    const SecondTokens& SecondTokensFor(const antlr4::dfa::DFA& dfa, const antlr4::atn::ATNConfigSet& start, size_t alt);
    /// The walk of a configuration that falls off rule stop state `stop`; recorded once per ATN.
    const FallOff& FallOffFor(antlr4::atn::ATNState* stop, bool treatEofAsEpsilon);
    /// Takes `fallOff`'s steps for `config` (at its stop state) as ANTLR's closure_ would.
    void Replay(const Ref<antlr4::atn::ATNConfig>& config, antlr4::atn::ATNConfigSet* configs,
                antlr4::atn::ATNConfig::Set& closureBusy, bool treatEofAsEpsilon, const FallOff& fallOff);
};

}  // namespace tsql::parser
