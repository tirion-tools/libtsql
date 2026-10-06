// ANTLR 4 runtime adaptations shared by every converted parser (ANTLR 2 semantics).
#pragma once

#include "antlr4-runtime.h"

namespace tsql::parser {

class TSqlParserATNSimulator;
class Antlr2Tests;

/// ANTLR 2 semantics with defaultErrorHandler=false: no recovery inside rules; recognition
/// exceptions propagate to the `exception catch[...]` handlers SqlScriptDOM declares.
class TSqlBailErrorStrategy : public antlr4::DefaultErrorStrategy {
public:
    void recover(antlr4::Parser* recognizer, std::exception_ptr e) override;
    antlr4::Token* recoverInline(antlr4::Parser* recognizer) override;
    /// Called before every decision: ANTLR 2's test of a (...)? block that ANTLR 4 decides with
    /// LA(1) inline (TSqlParserATNSimulator::Antlr2OptionalBlock).
    void sync(antlr4::Parser* recognizer) override;
    void reportError(antlr4::Parser* recognizer, const antlr4::RecognitionException& e) override;

    TSqlParserATNSimulator* simulator = nullptr;   // the parser's (TSql80ParserBase::InitializeForNewInput)
};

/// ANTLR 4 defers a prediction failure to the caller when some alternative could leave the
/// decision's rule (getAltThatFinishedDecisionEntryRule); ANTLR 2 throws NoViableAlt at the
/// decision itself, which decides which rule's `exception` handler recovers. Never defer.
class TSqlParserATNSimulator : public antlr4::atn::ParserATNSimulator {
public:
    /// ANTLR clears its merge cache after every prediction, a large part of the cost of one that its
    /// DFA answers; this simulator clears it after those that may have filled it (mergeCacheUsed_),
    /// which leaves it empty at the same points.
    TSqlParserATNSimulator(antlr4::Parser* parser, const antlr4::atn::ATN& atn,
                           std::vector<antlr4::dfa::DFA>& decisionToDFA,
                           antlr4::atn::PredictionContextCache& sharedContextCache);

    /// ANTLR 4's prediction checked against ANTLR 2's: SqlScriptDOM's parser decided with two
    /// tokens of linear approximate lookahead (Antlr2Decision), so on input that does not parse
    /// it entered an alternative, exited a loop or threw NoViableAlt where ANTLR 4's unbounded
    /// lookahead does something else, and the syntax error surfaced elsewhere. Where ANTLR 2's
    /// tests reject ANTLR 4's alternative, or ANTLR 4 finds none, this returns ANTLR 2's choice
    /// or throws NoViableAlt at LT(1). LA(2) is read only where ANTLR 2 read it.
    /// Re-entrant: a speculative parse (TSql80ParserBase::Speculate) runs inside predicate
    /// evaluation of an outer prediction, so the simulator's per-prediction fields are restored.
    size_t adaptivePredict(antlr4::TokenStream* input, size_t decision, antlr4::ParserRuleContext* outerContext) override;
    /// Whether some path through rule `rule`, predicates ignored, matches the input's tokens ahead
    /// up to the rule's end: a necessary condition for a speculative parse of a syntactic
    /// predicate's rule (the converter's R_synpredN) to match (SynPredDfa).
    bool SynPredMayMatch(size_t rule, antlr4::TokenStream* input);
    /// At the start of a (...)? block, ANTLR 2 threw NoViableAlt at LT(1) when LA(1) starts none of
    /// its alternatives nor what follows it; ANTLR 4's inline LL(1) code for one just skips it. A
    /// no-op at any other `state` (one bit test: it runs before every decision).
    void Antlr2OptionalBlock(size_t state) {
        if ((optionalBlocks_[state / 64] >> (state % 64) & 1) != 0) Antlr2OptionalBlockAt(state);
    }
    /// ANTLR 2's tests at decision `decision`, for walks of the ATN (computed once per ATN and
    /// decision, as for a prediction).
    Antlr2Tests Antlr2TestsFor(size_t decision);

protected:
    size_t getAltThatFinishedDecisionEntryRule(antlr4::atn::ATNConfigSet*) override {
        return antlr4::atn::ATN::INVALID_ALT_NUMBER;
    }
    /// Alternative number of the exit branch of a (...)* or (...)+ loop decision, or INVALID.
    static size_t LoopExitAlternative(const antlr4::atn::DecisionState* d);
    /// SLL prediction without collected predicates: the same closure as ANTLR's (the same
    /// configurations and closureBusy checks in the same order) without a configuration object per
    /// step, and a configuration that falls off a rule replays that rule's recorded follow walk.
    void closureCheckingStopState(const Ref<antlr4::atn::ATNConfig>& config, antlr4::atn::ATNConfigSet* configs,
                                  antlr4::atn::ATNConfig::Set& closureBusy, bool collectPredicates, bool fullCtx,
                                  int depth, bool treatEofAsEpsilon) override;
    /// ANTLR's, noting that the merge cache may have entries (mergeCacheUsed_): every merge of a
    /// prediction happens inside one of these two.
    std::unique_ptr<antlr4::atn::ATNConfigSet> computeReachSet(antlr4::atn::ATNConfigSet* closure, size_t t,
                                                              bool fullCtx) override;
    std::unique_ptr<antlr4::atn::ATNConfigSet> computeStartState(antlr4::atn::ATNState* p, antlr4::RuleContext* ctx,
                                                                bool fullCtx) override;

private:
    struct Antlr2Decision;
    struct Antlr2Decisions;
    struct SynPredDfa;
    struct FallOff;
    friend class Antlr2Tests;
    Antlr2Decisions* antlr2_ = nullptr;   // this ATN's, shared by every simulator on it (Tables)
    const uint64_t* optionalBlocks_ = nullptr;   // antlr2_->optionalBlocks
    bool mergeCacheUsed_ = false;   // mergeCache may have entries (cleared after the prediction)
    /// Sets antlr2_ and optionalBlocks_, making this ATN's tables the first time (the constructor).
    void Tables();
    /// Antlr2OptionalBlock at the start state of a (...)? block
    void Antlr2OptionalBlockAt(size_t state);
    /// ANTLR 2's tests at decision `decision`; computed once per ATN and decision.
    const Antlr2Decision& Antlr2DecisionFor(size_t decision);
    /// Whether the tests of alternative i (0-based) of `d` beyond its lookahead pass at the input's
    /// position: its syntactic predicate and, unless ANTLR 4 `chosen` it, its semantic predicates.
    bool Antlr2PredicatesMatch(const Antlr2Decision& d, size_t i, antlr4::TokenStream* input,
                               antlr4::ParserRuleContext* outerContext, bool chosen);
    /// The walk of a configuration that falls off rule stop state `stop`; recorded once per ATN.
    const FallOff& FallOffFor(antlr4::atn::ATNState* stop, bool treatEofAsEpsilon);
    /// Takes `fallOff`'s steps for `config` (at its stop state) as ANTLR's closure_ would.
    void Replay(const Ref<antlr4::atn::ATNConfig>& config, antlr4::atn::ATNConfigSet* configs,
                antlr4::atn::ATNConfig::Set& closureBusy, bool treatEofAsEpsilon, const FallOff& fallOff);
};

/// ANTLR 2's tests at one decision as TSqlParserATNSimulator applies them, for a walk of the ATN
/// that follows the parser's choices (the editor's). Alternatives are 0-based and ANTLR 2's: the
/// decision's own, or for the enter/exit decision of a loop the loop block's.
class Antlr2Tests {
public:
    size_t Alternatives() const;
    /// The enter/exit decision of a (...)* or (...)+ loop: ANTLR 4's alternative number of the exit
    /// (1-based), else 0.
    size_t LoopExit() const;
    /// Whether ANTLR 2 tests LA(2) for alternative `i` (its LA(1) set meets another's).
    bool Depth2(size_t i) const;
    /// Whether `token` is in alternative i's LA(1) / LA(2) set.
    bool First(size_t i, size_t token) const;
    bool Second(size_t i, size_t token) const;
    /// Whether ANTLR 2 takes it only when its syntactic predicate (a speculative parse) matches.
    bool SyntacticPredicate(size_t i) const;
    /// The semantic predicates ANTLR 2 tests together with its lookahead (those it starts with).
    const std::vector<const antlr4::atn::PredicateTransition*>& Predicates(size_t i) const;

private:
    friend class TSqlParserATNSimulator;
    explicit Antlr2Tests(const TSqlParserATNSimulator::Antlr2Decision* d) : d_(d) {}
    const TSqlParserATNSimulator::Antlr2Decision* d_;
};

}  // namespace tsql::parser
