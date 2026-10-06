// ANTLR 4 runtime adaptations shared by every converted parser (ANTLR 2 semantics).
#include "ParserRuntime.h"

#include "TSql80ParserBase.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace tsql::parser {

// ============================================================================ error strategy

void TSqlBailErrorStrategy::recover(antlr4::Parser*, std::exception_ptr e) { std::rethrow_exception(e); }

antlr4::Token* TSqlBailErrorStrategy::recoverInline(antlr4::Parser* recognizer) {
    throw antlr4::InputMismatchException(recognizer);
}

void TSqlBailErrorStrategy::sync(antlr4::Parser* recognizer) {
    if (simulator != nullptr) simulator->Antlr2OptionalBlock(recognizer->getState());
}

void TSqlBailErrorStrategy::reportError(antlr4::Parser*, const antlr4::RecognitionException&) {}

// ============================================================================ prediction

namespace {

/// Guards the per-ATN caches below (FallOffFor, Antlr2Decisions); their nodes never move.
std::mutex cacheMutex;

/// Token types 1..maxTokenType and EOF, while a decision's tests are computed (ByToken tests them).
struct TokenSet {
    std::vector<uint64_t> bits;
    bool eof = false;

    explicit TokenSet(size_t maxToken) : bits(maxToken / 64 + 1) {}
    void Add(size_t t) {
        if (t == antlr4::Token::EOF) eof = true;
        else bits[t / 64] |= uint64_t{1} << (t % 64);
    }
    void All() {
        std::fill(bits.begin(), bits.end(), ~uint64_t{0});
        eof = true;
    }
    bool Intersects(const TokenSet& other) const {
        if (eof && other.eof) return true;
        for (size_t i = 0; i < bits.size(); ++i)
            if ((bits[i] & other.bits[i]) != 0) return true;
        return false;
    }
    void Union(const TokenSet& other) {
        eof |= other.eof;
        for (size_t i = 0; i < bits.size(); ++i) bits[i] |= other.bits[i];
    }
};

/// A decision's tests by token (Antlr2Decision::tests), `Count` entries: for each token the first
/// alternative ANTLR 2 tries (1-based; 0: none, kAlt: kAlt or a later one) with its flags, and the
/// decision's kind in every entry, so that a prediction tests LA(1) with a single load. Then for
/// each alternative its look2 and, after all of those, its look1, as bit arrays (`Words` long).
struct ByToken {
    static constexpr uint16_t kAlt = 0x0FFF;
    static constexpr uint16_t kNoSynpred = 0x1000;   // the alternative has no syntactic predicate
    static constexpr uint16_t kDepth2 = 0x2000;      // ANTLR 2 tests LA(2) for it
    static constexpr uint16_t kLoop = 0x4000;        // the enter/exit decision of a loop ...
    static constexpr uint16_t kExit1 = 0x8000;       // ... whose exit is ANTLR 4's alternative 1

    /// a token's index: EOF 0, token type t at t + 1
    static size_t Index(size_t t) {
        static_assert(antlr4::Token::EOF == SIZE_MAX);
        return t + 1;
    }
    static size_t Count(size_t maxToken) { return maxToken + 2; }
    static size_t Words(size_t maxToken) { return (Count(maxToken) + 15) / 16; }
    static size_t Size(size_t maxToken, size_t n) { return Count(maxToken) + 2 * n * Words(maxToken); }
    /// alternative i's look2 / look1 (0-based) of a decision with n alternatives
    template <typename T>
    static T* Look2(T* tests, size_t maxToken, size_t i) {
        return tests + Count(maxToken) + i * Words(maxToken);
    }
    template <typename T>
    static T* Look1(T* tests, size_t maxToken, size_t n, size_t i) {
        return Look2(tests, maxToken, n + i);
    }
    static bool Test(const uint16_t* bits, size_t index) { return (bits[index / 16] >> (index % 16) & 1) != 0; }
};

/// __builtin_popcountll without the library call it compiles to for baseline x86-64
inline size_t Popcount(uint64_t x) {
    x -= x >> 1 & 0x5555555555555555;
    x = (x & 0x3333333333333333) + (x >> 2 & 0x3333333333333333);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0F;
    return static_cast<size_t>(x * 0x0101010101010101 >> 56);
}

/// Calls f(t) for each token type 1..maxToken that `tr` consumes (as Transition::matches decides).
template <typename F>
void ForEachToken(const antlr4::atn::Transition& tr, size_t maxToken, F f) {
    using namespace antlr4::atn;
    auto range = [&](size_t from, size_t to) {
        for (size_t t = std::max<size_t>(from, 1); t <= std::min(to, maxToken); ++t) f(t);
    };
    switch (tr.getTransitionType()) {
        case TransitionType::ATOM:
            range(static_cast<const AtomTransition&>(tr)._label, static_cast<const AtomTransition&>(tr)._label);
            return;
        case TransitionType::RANGE:
            range(static_cast<const RangeTransition&>(tr).from, static_cast<const RangeTransition&>(tr).to);
            return;
        case TransitionType::SET:
            for (const auto& i : static_cast<const SetTransition&>(tr).set.getIntervals())
                if (i.b >= 1) range(static_cast<size_t>(std::max<ssize_t>(i.a, 1)), static_cast<size_t>(i.b));
            return;
        case TransitionType::NOT_SET:
        case TransitionType::WILDCARD:
            for (size_t t = 1; t <= maxToken; ++t)
                if (tr.matches(t, 0, maxToken)) f(t);
            return;
        default:
            return;   // epsilon
    }
}

/// Adds the tokens a configuration at state `s` of an SLL closure matches next: EOF at a rule stop
/// state (the closure fell off a rule nothing calls) or on an EOF transition.
void AddNext(TokenSet& out, const antlr4::atn::ATNState* s, size_t maxToken) {
    if (antlr4::atn::RuleStopState::is(s)) {
        out.eof = true;
        return;
    }
    for (const auto& tr : s->transitions) {
        ForEachToken(*tr, maxToken, [&](size_t t) { out.Add(t); });
        if (tr->matches(antlr4::Token::EOF, 0, maxToken)) out.eof = true;
    }
}

/// ANTLR's SLL closure without collected predicates or full context (closureCheckingStopState and
/// closure_) from a configuration at state `p` with `context`. Nothing else about the configuration
/// steers it (predicates pass, the depth matters only to collected ones), so it is walked without
/// a configuration per step. `sink` gets what reaches the configuration set or closureBusy, in
/// ANTLR's order:
///   Add(state, context, self): configs->add;
///   Fall(stop, context, self): falls off the rule of rule stop state `stop` (empty context), where
///     closure_ chases its follow links;
///   Check(state, context): closureBusy check after an EOF transition taken as epsilon; true: walk
///     on from there, then Checked().
/// `self`: the configuration is the one the walk started with or last checked (ANTLR's object).
template <typename Sink>
void Traverse(const antlr4::atn::ParserATNSimulator& sim, Sink& sink, antlr4::atn::ATNState* p,
              const Ref<const antlr4::atn::PredictionContext>& context, bool self, bool treatEofAsEpsilon) {
    using namespace antlr4::atn;
    if (RuleStopState::is(p)) {
        if (context->isEmpty()) return sink.Fall(p, context, self);
        // SLL contexts hold EMPTY_RETURN_STATE only as the empty context itself
        for (size_t i = 0; i < context->size(); ++i)
            Traverse(sim, sink, sim.atn.states[context->getReturnState(i)], context->getParent(i), false,
                     treatEofAsEpsilon);
        return;
    }
    if (!p->epsilonOnlyTransitions) sink.Add(p, context, self);
    for (size_t i = 0; i < p->transitions.size(); ++i) {
        const Transition* t = p->transitions[i].get();
        if (i == 0 && p->getStateType() == ATNStateType::STAR_LOOP_ENTRY) {
            ATNConfig probe(p, 0, context);
            if (sim.canDropLoopEntryEdgeInLeftRecursiveRule(&probe)) continue;
        }
        switch (t->getTransitionType()) {
            case TransitionType::RULE:
                Traverse(sim, sink, t->target,
                         SingletonPredictionContext::create(
                             context, static_cast<const RuleTransition*>(t)->followState->stateNumber),
                         false, treatEofAsEpsilon);
                break;
            case TransitionType::PRECEDENCE:
            case TransitionType::PREDICATE:
            case TransitionType::ACTION:
            case TransitionType::EPSILON:
                Traverse(sim, sink, t->target, context, false, treatEofAsEpsilon);
                break;
            case TransitionType::ATOM:
            case TransitionType::RANGE:
            case TransitionType::SET:
                if (treatEofAsEpsilon && t->matches(antlr4::Token::EOF, 0, 1) && sink.Check(t->target, context)) {
                    Traverse(sim, sink, t->target, context, true, treatEofAsEpsilon);
                    sink.Checked();
                }
                break;
            default:
                break;
        }
    }
}

}  // namespace

/// ANTLR's closure_ of a configuration at a rule stop state with an empty context, which in SLL
/// prediction falls off the rule and chases every follow link: the steps of the walk that touch
/// the configuration set or closureBusy, in the order the walk takes them. Without collected
/// predicates nothing in the walk depends on the configuration's alternative, semantic context or
/// depth, and every configuration it reaches has fallen off one rule more than the one at the stop
/// state (reachesIntoOuterContext + 1); so the walk is recorded once per (stop state,
/// treatEofAsEpsilon), and ANTLR's checks are replayed against each closure's own closureBusy.
/// Falling off a further rule is a step of its own (replayed recursively, as ANTLR recurses).
/// The first few hundred falls of a new error shape otherwise dominate its parse.
struct TSqlParserATNSimulator::FallOff {
    enum Kind : uint8_t {
        kAddStop,   // configs->add of the configuration at the stop state (it has no follow links)
        kFollow,    // closureBusy check of a follow link's configuration; `end`: index after its walk
        kEofBusy,   // closureBusy check after an EOF transition taken as epsilon; `end` as kFollow
        kAdd,       // configs->add
        kFall,      // falls off rule stop state `state`
    };
    struct Step {
        Kind kind;
        bool self;    // kAdd, kFall: of the configuration of the closureBusy check just before
        size_t end;
        antlr4::atn::ATNState* state;
        Ref<const antlr4::atn::PredictionContext> context;
    };
    std::vector<Step> steps;
};

const TSqlParserATNSimulator::FallOff& TSqlParserATNSimulator::FallOffFor(antlr4::atn::ATNState* stop,
                                                                          bool treatEofAsEpsilon) {
    using namespace antlr4::atn;
    // guarded by cacheMutex; map nodes do not move
    static std::map<std::tuple<const ATN*, size_t, bool>, FallOff> cache;
    const auto key = std::make_tuple(&atn, stop->stateNumber, treatEofAsEpsilon);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    struct Recorder {
        FallOff out;
        std::vector<size_t> checks;   // the closureBusy checks the walk is inside of

        void Add(ATNState* p, const Ref<const PredictionContext>& context, bool self) {
            out.steps.push_back({FallOff::kAdd, self, 0, p, context});
        }
        void Fall(ATNState* stop, const Ref<const PredictionContext>& context, bool self) {
            out.steps.push_back({FallOff::kFall, self, 0, stop, context});
        }
        bool Check(ATNState* p, const Ref<const PredictionContext>& context) {
            // one the walk is inside of fails when replayed (ANTLR's guard against EOF loops)
            for (size_t i : checks)
                if (out.steps[i].state == p && *out.steps[i].context == *context) return false;
            Open(FallOff::kEofBusy, p, context);
            return true;
        }
        void Open(FallOff::Kind kind, ATNState* p, const Ref<const PredictionContext>& context) {
            checks.push_back(out.steps.size());
            out.steps.push_back({kind, false, 0, p, context});
        }
        void Checked() {
            out.steps[checks.back()].end = out.steps.size();
            checks.pop_back();
        }
    };
    Recorder r;
    if (!stop->epsilonOnlyTransitions) r.out.steps.push_back({FallOff::kAddStop, false, 0, stop, nullptr});
    // a rule stop state's transitions are epsilon transitions to the follow states of its calls
    for (const auto& t : stop->transitions) {
        r.Open(FallOff::kFollow, t->target, PredictionContext::EMPTY);
        Traverse(*this, r, t->target, PredictionContext::EMPTY, true, treatEofAsEpsilon);
        r.Checked();
    }
    std::lock_guard<std::mutex> lock(cacheMutex);
    return cache.emplace(key, std::move(r.out)).first->second;
}

void TSqlParserATNSimulator::Replay(const Ref<antlr4::atn::ATNConfig>& config, antlr4::atn::ATNConfigSet* configs,
                                    antlr4::atn::ATNConfig::Set& closureBusy, bool treatEofAsEpsilon,
                                    const FallOff& fallOff) {
    using antlr4::atn::ATNConfig;
    auto make = [&](const FallOff::Step& s) {
        auto c = std::make_shared<ATNConfig>(*config, s.state, s.context);
        ++c->reachesIntoOuterContext;
        return c;
    };
    Ref<ATNConfig> checked;   // the configuration of the last closureBusy check, which its walk goes on with
    const auto& steps = fallOff.steps;
    for (size_t i = 0; i < steps.size(); ++i) {
        const FallOff::Step& s = steps[i];
        switch (s.kind) {
            case FallOff::kAddStop:
                configs->add(config, &mergeCache);
                break;
            case FallOff::kFollow:
            case FallOff::kEofBusy:
                checked = make(s);
                if (!closureBusy.insert(checked).second)
                    i = s.end - 1;
                else if (s.kind == FallOff::kFollow)
                    configs->dipsIntoOuterContext = true;
                break;
            case FallOff::kAdd:
                configs->add(s.self ? checked : make(s), &mergeCache);
                break;
            case FallOff::kFall:
                Replay(s.self ? checked : make(s), configs, closureBusy, treatEofAsEpsilon,
                       FallOffFor(s.state, treatEofAsEpsilon));
                break;
        }
    }
}

/// What SqlScriptDOM's parser, generated by ANTLR 2 with k=2, tests at one decision. ANTLR 2's
/// lookahead is linear approximate: alternative i matches when LA(1) is in LOOK1(i) and, if its
/// lookahead depth is 2, LA(2) is in LOOK2(i), the tokens that can come second after *any* first
/// token. Its depth is 2 when LOOK1(i) intersects another alternative's (a loop's exit path counts
/// as one), else 1 (ANTLR 2's LLkAnalyzer.deterministic and deterministicImpliedPath). The sets
/// are SLL closures from the alternative with an empty context: rules called inside return to
/// their caller, and falling off the decision's rule follows every caller, as ANTLR 2's FOLLOW.
/// The generated code tries the alternatives in order and takes the first that matches; a
/// (...)* or (...)+ loop exits when none does, any other decision throws NoViableAlt at LT(1)
/// (a (...)? block's exit is an alternative of its own, tested against the block's follow).
/// Laid out for the tests of a prediction by LA(1) (ByToken): `tests` starts with the first
/// alternative ANTLR 2 tries for each token, which in the common case is ANTLR 4's alternative and
/// decides alone or with one test of LA(2); the others are tried from `candidates`.
struct TSqlParserATNSimulator::Antlr2Decision {
    static constexpr uint32_t kNone = UINT32_MAX;
    struct Alt {
        bool depth2 = false;
        /// depth 1 without a syntactic predicate: when ANTLR 4 chose it, LA(1) starts it (its SLL
        /// prediction consumes LA(1) and follows every caller where it falls off a rule, as LOOK1
        /// does), so ANTLR 2 chose it too
        bool direct = false;
        /// ANTLR 2 tests the semantic predicates an alternative starts with together with its
        /// lookahead (it does not hoist those of the rules the alternative calls): `predicates`
        bool predicated = false;
        /// ANTLR 2 took it only when its syntactic predicate, a speculative parse, matched; the
        /// converter drops the predicate and starts the alternative with a reference to an empty
        /// marker rule: its index, else kNone (TSql80ParserBase::Antlr2SynPred); and the
        /// predicate's rule (R_synpredN)
        uint32_t synpredMarker = kNone;
        uint32_t synpredRule = kNone;

        /// whether its tests go beyond lookahead (Antlr2PredicatesMatch)
        bool Dynamic() const { return predicated || synpredMarker != kNone; }
    };
    /// a candidate: the alternative (0-based) and what its tests need beyond LA(1)
    static constexpr uint32_t kCandidateAlt = 0x3FFFFFFF;
    static constexpr uint32_t kCandidateDepth2 = 0x40000000;
    static constexpr uint32_t kCandidateDynamic = 0x80000000;

    bool allDirect = true;
    /// enter/exit decision of a loop: ANTLR 4's alternative for the exit, else INVALID_ALT_NUMBER
    uint32_t exitAlt = antlr4::atn::ATN::INVALID_ALT_NUMBER;
    uint32_t n = 0;   // alts.size()
    size_t maxToken = 0;   // the ATN's (ByToken)
    /// ANTLR 2's alternatives: the decision's own, or for the enter/exit decision of a loop
    /// (StarLoopEntry, PlusLoopBack) those of the loop's block
    std::vector<Alt> alts;
    std::vector<Ref<const antlr4::atn::SemanticContext>> predicates;   // by alternative; null: none
    /// by alternative: the predicate transitions `predicates` is made of, in order (Antlr2Tests)
    std::vector<std::vector<const antlr4::atn::PredicateTransition*>> predicateTransitions;
    /// ByToken: by token the first alternative whose look1 has it, with flags; then by
    /// alternative its look2 (empty at depth 1) and its look1, as bit arrays
    std::vector<uint16_t> tests;
    /// by ByToken::Index(token), from candidateStart[index] up to candidateStart[index + 1]: the
    /// alternatives whose look1 has the token, in order (the others' tests fail at LA(1))
    std::vector<uint32_t> candidateStart;
    std::vector<uint32_t> candidates;
};

/// Whether some path through a syntactic predicate's rule (the converter's R_synpredN) matches the
/// tokens ahead up to the rule's end, ignoring predicates: a necessary condition for its
/// speculative parse to match, which on valid input almost always fails within a few tokens and
/// costs a C++ exception per rule frame there. The SLL closures after each token form a DFA built
/// as the input needs it (states: closures; edges: token types).
struct TSqlParserATNSimulator::SynPredDfa {
    struct State {
        std::unique_ptr<antlr4::atn::ATNConfigSet> configs;
        bool accept = false;   // a configuration is at the rule's stop state
        /// by token type, in the order the input needed them (few each); nullptr: nothing
        /// survives the token
        std::vector<std::pair<size_t, State*>> next;
    };
    std::vector<std::unique_ptr<State>> states;     // [0]: the start state
    std::multimap<size_t, State*> byHash;           // states by their configurations' hash
};

struct TSqlParserATNSimulator::Antlr2Decisions {
    /// what a prediction reads first, 4 decisions to a cache line
    struct alignas(16) Entry {
        /// bit `alt` when ANTLR 4's choice of `alt` stands without further tests
        /// (Antlr2Decision::Alt::direct; a loop's enter alternative: allDirect); set with `tests`
        std::atomic<uint64_t> direct{0};
        std::atomic<const uint16_t*> tests{nullptr};   // the decision's (Antlr2Decision::tests), once computed
    };
    std::unique_ptr<Entry[]> byDecision;
    std::unique_ptr<std::atomic<const Antlr2Decision*>[]> decisions;   // by decision, once computed
    size_t maxToken = 0;   // atn.maxTokenType (ByToken)
    /// by ATN state, a bit array: the start of a (...)? block (Antlr2OptionalBlock)
    std::vector<uint64_t> optionalBlocks;
    /// by word of optionalBlocks: the number of blocks before it, from which a block's rank (its
    /// index in the two below) follows
    std::vector<uint32_t> optionalRank;
    std::vector<uint32_t> optionalDecision;   // by rank: the block's decision
    /// by rank: Antlr2DecisionFor(its decision).tests, once computed
    std::unique_ptr<std::atomic<const uint16_t*>[]> optionalTests;
    /// by rule index: for a syntactic predicate's marker rule, the predicate's rule, else SIZE_MAX
    std::vector<size_t> synpredOfMarker;
    /// by rule index: a syntactic predicate's rule (R_synpredN), which nothing calls: its
    /// speculative parse ends where it falls off, whatever follows
    std::vector<bool> synpredRule;
    std::vector<std::unique_ptr<const Antlr2Decision>> owned;   // guarded by cacheMutex
    std::vector<std::unique_ptr<SynPredDfa>> synpredDfas;       // by rule; guarded by synpredMutex
};

void TSqlParserATNSimulator::Tables() {
    using namespace antlr4::atn;
    static std::map<const ATN*, Antlr2Decisions> tables;   // guarded by cacheMutex
    std::lock_guard<std::mutex> lock(cacheMutex);
    Antlr2Decisions& t = tables[&atn];
    if (!t.byDecision) {
        t.byDecision = std::make_unique<Antlr2Decisions::Entry[]>(atn.decisionToState.size());
        t.decisions = std::make_unique<std::atomic<const Antlr2Decision*>[]>(atn.decisionToState.size());
        for (size_t i = 0; i < atn.decisionToState.size(); ++i) t.decisions[i].store(nullptr, std::memory_order_relaxed);
        t.maxToken = atn.maxTokenType;
        // ANTLR 4 makes a (...)? block's bypass the last transition of its start state
        t.optionalBlocks.assign(atn.states.size() / 64 + 1, 0);
        for (const ATNState* s : atn.states) {
            if (s == nullptr || s->getStateType() != ATNStateType::BLOCK_START) continue;
            const auto* start = static_cast<const BlockStartState*>(s);
            if (start->decision >= 0 && !start->transitions.empty() &&
                start->transitions.back()->target == start->endState) {
                t.optionalBlocks[s->stateNumber / 64] |= uint64_t{1} << (s->stateNumber % 64);
                t.optionalDecision.push_back(static_cast<uint32_t>(start->decision));   // states are in order
            }
        }
        uint32_t rank = 0;
        for (uint64_t word : t.optionalBlocks) {
            t.optionalRank.push_back(rank);
            rank += static_cast<uint32_t>(Popcount(word));
        }
        t.optionalTests = std::make_unique<std::atomic<const uint16_t*>[]>(rank);
        for (uint32_t i = 0; i < rank; ++i) t.optionalTests[i].store(nullptr, std::memory_order_relaxed);
        const std::vector<std::string>& names = parser->getRuleNames();
        t.synpredOfMarker.assign(names.size(), SIZE_MAX);
        t.synpredRule.assign(names.size(), false);
        t.synpredDfas.resize(names.size());
        for (size_t r = 0; r < names.size(); ++r) {
            if (names[r].compare(0, 14, "antlr2SynPred_") == 0)
                t.synpredOfMarker[r] = std::find(names.begin(), names.end(), names[r].substr(14)) - names.begin();
            else
                t.synpredRule[r] = names[r].find("_synpred") != std::string::npos;
        }
    }
    optionalBlocks_ = t.optionalBlocks.data();
    antlr2_ = &t;
}

const TSqlParserATNSimulator::Antlr2Decision& TSqlParserATNSimulator::Antlr2DecisionFor(size_t decision) {
    using namespace antlr4::atn;
    if (const Antlr2Decision* d = antlr2_->decisions[decision].load(std::memory_order_acquire)) return *d;

    const size_t maxToken = atn.maxTokenType;
    DecisionState* ds = atn.getDecisionState(decision);
    auto out = std::make_unique<Antlr2Decision>();
    // the sets while they are built (`tests` and `candidates` at the end)
    struct AltSets {
        TokenSet look1, look2;
        explicit AltSets(size_t maxToken) : look1(maxToken), look2(maxToken) {}
    };
    std::vector<AltSets> sets;
    // closures outside a prediction: no DFA (closureCheckingStopState reads it); restored after
    antlr4::dfa::DFA* const savedDfa = _dfa;
    _dfa = nullptr;
    // the state whose transitions start the alternatives, and where a loop's exit path starts
    const ATNState* block = ds;
    ATNState* exit = nullptr;
    auto exitOf = [](const DecisionState* loop) { return loop->transitions[LoopExitAlternative(loop) - 1]->target; };
    switch (ds->getStateType()) {
        case ATNStateType::STAR_LOOP_ENTRY:
        case ATNStateType::PLUS_LOOP_BACK:
            out->exitAlt = static_cast<uint32_t>(LoopExitAlternative(ds));
            block = ds->transitions[out->exitAlt == 1 ? 1 : 0]->target;
            exit = exitOf(ds);
            break;
        case ATNStateType::STAR_BLOCK_START: {
            auto* loopBack = static_cast<StarLoopbackState*>(static_cast<BlockStartState*>(ds)->endState->transitions[0]->target);
            exit = exitOf(loopBack->getLoopEntryState());
            break;
        }
        case ATNStateType::PLUS_BLOCK_START:
            exit = exitOf(static_cast<PlusBlockStartState*>(ds)->loopBackState);
            break;
        default:
            break;
    }
    const size_t n = block->transitions.size();
    auto closureOf = [&](ATNState* s, size_t alt, const Ref<const PredictionContext>& context, ATNConfigSet& configs,
                         ATNConfig::Set& busy) {
        closure(std::make_shared<ATNConfig>(s, alt, context), &configs, busy, false, false, false);
    };
    // a syntactic predicate's speculative parse ends where it falls off its rule (the decisions of
    // the rules it calls keep their FOLLOW)
    const bool inSynpred = antlr2_->synpredRule[ds->ruleIndex];
    auto addNext = [&](TokenSet& set, const ATNState* s) {
        if (inSynpred && RuleStopState::is(s) && s->ruleIndex == ds->ruleIndex) set.All();
        else AddNext(set, s, maxToken);
    };
    std::vector<std::unique_ptr<ATNConfigSet>> starts;
    for (size_t i = 0; i < n; ++i) {
        starts.push_back(std::make_unique<ATNConfigSet>(false));
        ATNConfig::Set busy;
        closureOf(block->transitions[i]->target, i + 1, PredictionContext::EMPTY, *starts.back(), busy);
        Antlr2Decision::Alt& a = out->alts.emplace_back();
        Ref<const SemanticContext>& predicate = out->predicates.emplace_back();
        std::vector<const PredicateTransition*>& predicateTransitions = out->predicateTransitions.emplace_back();
        AltSets& as = sets.emplace_back(maxToken);
        for (const auto& c : starts.back()->configs) addNext(as.look1, c->state);
        // the alternative's first elements up to a token, a rule call or a nested decision:
        // predicates, and the marker of a dropped syntactic predicate
        for (const ATNState* s = block->transitions[i]->target; s->transitions.size() == 1;) {
            const Transition* t = s->transitions[0].get();
            if (t->getTransitionType() == TransitionType::RULE) {
                const size_t rule = t->target->ruleIndex;
                if (antlr2_->synpredOfMarker[rule] == SIZE_MAX) break;
                a.synpredMarker = static_cast<uint32_t>(rule);
                a.synpredRule = static_cast<uint32_t>(antlr2_->synpredOfMarker[rule]);
                s = static_cast<const RuleTransition*>(t)->followState;
                continue;
            }
            if (t->getTransitionType() == TransitionType::PREDICATE) {
                const auto* pt = static_cast<const PredicateTransition*>(t);
                Ref<const SemanticContext> p = pt->getPredicate();
                predicate = predicate ? SemanticContext::And(predicate, p) : p;
                predicateTransitions.push_back(pt);
                a.predicated = true;
            } else if (!t->isEpsilon()) {
                break;
            }
            s = t->target;
        }
    }
    TokenSet exitLook1(maxToken);
    if (exit != nullptr) {
        ATNConfigSet configs(false);
        ATNConfig::Set busy;
        closureOf(exit, n + 1, PredictionContext::EMPTY, configs, busy);
        for (const auto& c : configs.configs) addNext(exitLook1, c->state);
    }
    for (size_t i = 0; i < n; ++i) {
        Antlr2Decision::Alt& a = out->alts[i];
        AltSets& as = sets[i];
        a.depth2 = exit != nullptr && as.look1.Intersects(exitLook1);
        for (size_t j = 0; j < n && !a.depth2; ++j) a.depth2 = j != i && as.look1.Intersects(sets[j].look1);
        a.direct = !a.depth2 && a.synpredMarker == Antlr2Decision::kNone;
        out->allDirect &= a.direct;
        if (!a.depth2) continue;
        // the closure after each first token, from where its configuration consumed it
        ATNConfigSet second(false);
        ATNConfig::Set busy;
        for (const auto& c : starts[i]->configs) {
            // the speculative parse may end before a second token
            if (inSynpred && RuleStopState::is(c->state) && c->state->ruleIndex == ds->ruleIndex) as.look2.All();
            for (const auto& tr : c->state->transitions)
                if (!tr->isEpsilon()) closureOf(tr->target, i + 1, c->context, second, busy);
        }
        for (const auto& c : second.configs) addNext(as.look2, c->state);
    }
    mergeCache.clear();
    _dfa = savedDfa;
    out->n = static_cast<uint32_t>(n);
    out->maxToken = maxToken;
    // the tables by token: the first alternative wins, so the later ones are written first
    auto forEach = [maxToken](const TokenSet& set, auto f) {   // All() also sets the bits past maxToken
        if (set.eof) f(ByToken::Index(antlr4::Token::EOF));
        for (size_t w = 0; w < set.bits.size(); ++w)
            for (uint64_t b = set.bits[w]; b != 0; b &= b - 1)
                if (const size_t t = w * 64 + static_cast<size_t>(__builtin_ctzll(b)); t <= maxToken)
                    f(ByToken::Index(t));
    };
    auto addTo = [](uint16_t* bits) {
        return [bits](size_t index) { bits[index / 16] |= static_cast<uint16_t>(1u << (index % 16)); };
    };
    const uint16_t kind = out->exitAlt == ATN::INVALID_ALT_NUMBER ? 0
                          : out->exitAlt == 1                     ? ByToken::kLoop | ByToken::kExit1
                                                                  : ByToken::kLoop;
    const size_t count = ByToken::Count(maxToken);
    out->tests.assign(ByToken::Size(maxToken, n), 0);
    std::fill_n(out->tests.begin(), count, kind);
    for (size_t i = n; i-- > 0;) {
        const Antlr2Decision::Alt& a = out->alts[i];
        uint16_t first = kind | ByToken::kAlt;
        if (i + 1 < ByToken::kAlt) {
            first = static_cast<uint16_t>(kind | (i + 1) | (a.depth2 ? ByToken::kDepth2 : 0) |
                                          (a.synpredMarker == Antlr2Decision::kNone ? ByToken::kNoSynpred : 0));
        }
        forEach(sets[i].look1, [&](size_t index) { out->tests[index] = first; });
        forEach(sets[i].look1, addTo(ByToken::Look1(out->tests.data(), maxToken, n, i)));
        if (a.depth2) forEach(sets[i].look2, addTo(ByToken::Look2(out->tests.data(), maxToken, i)));
    }
    out->candidateStart.assign(count + 1, 0);
    for (size_t i = 0; i < n; ++i) forEach(sets[i].look1, [&](size_t index) { ++out->candidateStart[index + 1]; });
    for (size_t k = 0; k < count; ++k) out->candidateStart[k + 1] += out->candidateStart[k];
    out->candidates.resize(out->candidateStart[count]);
    std::vector<uint32_t> next(out->candidateStart.begin(), out->candidateStart.end() - 1);
    for (size_t i = 0; i < n; ++i) {
        const Antlr2Decision::Alt& a = out->alts[i];
        const uint32_t candidate = static_cast<uint32_t>(i) | (a.depth2 ? Antlr2Decision::kCandidateDepth2 : 0) |
                                   (a.Dynamic() ? Antlr2Decision::kCandidateDynamic : 0);
        forEach(sets[i].look1, [&](size_t index) { out->candidates[next[index]++] = candidate; });
    }
    uint64_t direct = 0;
    if (out->exitAlt != ATN::INVALID_ALT_NUMBER) {
        if (out->allDirect) direct = uint64_t{1} << (out->exitAlt == 1 ? 2 : 1);
    } else {
        for (size_t i = 0; i < n && i + 1 < 64; ++i)
            if (out->alts[i].direct) direct |= uint64_t{1} << (i + 1);
    }
    std::lock_guard<std::mutex> lock(cacheMutex);
    std::atomic<const Antlr2Decision*>& slot = antlr2_->decisions[decision];
    if (const Antlr2Decision* d = slot.load(std::memory_order_acquire)) return *d;
    Antlr2Decisions::Entry& entry = antlr2_->byDecision[decision];
    entry.direct.store(direct, std::memory_order_relaxed);
    entry.tests.store(out->tests.data(), std::memory_order_release);
    slot.store(out.get(), std::memory_order_release);
    return *antlr2_->owned.emplace_back(std::move(out));
}

Antlr2Tests TSqlParserATNSimulator::Antlr2TestsFor(size_t decision) { return Antlr2Tests(&Antlr2DecisionFor(decision)); }

size_t Antlr2Tests::Alternatives() const { return d_->n; }
size_t Antlr2Tests::LoopExit() const {
    return d_->exitAlt == antlr4::atn::ATN::INVALID_ALT_NUMBER ? 0 : d_->exitAlt;
}
bool Antlr2Tests::Depth2(size_t i) const { return d_->alts[i].depth2; }
bool Antlr2Tests::First(size_t i, size_t token) const {
    const size_t index = ByToken::Index(token);
    return index < ByToken::Count(d_->maxToken) &&
           ByToken::Test(ByToken::Look1(d_->tests.data(), d_->maxToken, d_->n, i), index);
}
bool Antlr2Tests::Second(size_t i, size_t token) const {
    const size_t index = ByToken::Index(token);
    return index < ByToken::Count(d_->maxToken) &&
           ByToken::Test(ByToken::Look2(d_->tests.data(), d_->maxToken, i), index);
}
bool Antlr2Tests::SyntacticPredicate(size_t i) const {
    return d_->alts[i].synpredMarker != TSqlParserATNSimulator::Antlr2Decision::kNone;
}
const std::vector<const antlr4::atn::PredicateTransition*>& Antlr2Tests::Predicates(size_t i) const {
    return d_->predicateTransitions[i];
}

void TSqlParserATNSimulator::Antlr2OptionalBlockAt(size_t state) {
    antlr4::TokenStream* input = parser->getTokenStream();
    const Antlr2Decisions& t = *antlr2_;
    const uint64_t below = optionalBlocks_[state / 64] & ((uint64_t{1} << (state % 64)) - 1);
    const size_t rank = t.optionalRank[state / 64] + Popcount(below);
    const uint16_t* tests = t.optionalTests[rank].load(std::memory_order_acquire);
    if (tests == nullptr) {
        tests = Antlr2DecisionFor(t.optionalDecision[rank]).tests.data();
        t.optionalTests[rank].store(tests, std::memory_order_release);
    }
    // no alternative starts with LA(1) (the bypass's starts with what follows the block)
    if ((tests[ByToken::Index(input->LA(1))] & ByToken::kAlt) == 0)
        throw antlr4::NoViableAltException(parser, input, input->LT(1), input->LT(1), nullptr, parser->getContext(),
                                           false);
}

bool TSqlParserATNSimulator::Antlr2PredicatesMatch(const Antlr2Decision& d, size_t i, antlr4::TokenStream* input,
                                                   antlr4::ParserRuleContext* outerContext, bool chosen) {
    const Antlr2Decision::Alt& a = d.alts[i];
    // ANTLR 4 has evaluated the predicates of the alternative it chose
    if (!chosen && a.predicated) {
        // the cheap necessary condition for the syntactic predicate first
        if (a.synpredRule != Antlr2Decision::kNone && !SynPredMayMatch(a.synpredRule, input)) return false;
        if (!evalSemanticContext(d.predicates[i], outerContext, i + 1, false)) return false;
    }
    auto* p = static_cast<TSql80ParserBase*>(parser);
    // inside a speculative parse, ANTLR 4's alternative is taken unchecked: checking it would
    // speculate again at every nesting level of the same construct (exponential time and memory
    // on deeply nested input); it only decides whether the outer speculation matches
    if (a.synpredMarker == Antlr2Decision::kNone || (chosen && p->Guessing())) return true;
    return p->Antlr2SynPred(a.synpredMarker, outerContext);
}

bool TSqlParserATNSimulator::SynPredMayMatch(size_t rule, antlr4::TokenStream* input) {
    using namespace antlr4::atn;
    static std::mutex synpredMutex;   // guards Antlr2Decisions::synpredDfas
    std::lock_guard<std::mutex> lock(synpredMutex);
    std::unique_ptr<SynPredDfa>& slot = antlr2_->synpredDfas[rule];
    if (!slot) slot = std::make_unique<SynPredDfa>();
    SynPredDfa& dfa = *slot;
    antlr4::dfa::DFA* const savedDfa = _dfa;   // closures outside a prediction, as in Antlr2DecisionFor
    _dfa = nullptr;
    auto add = [&](std::unique_ptr<ATNConfigSet> configs) -> SynPredDfa::State* {
        if (configs->configs.empty()) return nullptr;
        const size_t hash = configs->hashCode();
        for (auto [it, end] = dfa.byHash.equal_range(hash); it != end; ++it)
            if (*it->second->configs == *configs) return it->second;
        auto s = std::make_unique<SynPredDfa::State>();
        for (const auto& c : configs->configs) s->accept |= c->state == atn.ruleToStopState[rule];
        s->configs = std::move(configs);
        dfa.byHash.emplace(hash, s.get());
        return dfa.states.emplace_back(std::move(s)).get();
    };
    bool computed = dfa.states.empty();
    if (computed) {
        auto configs = std::make_unique<ATNConfigSet>(false);
        ATNConfig::Set busy;
        closure(std::make_shared<ATNConfig>(atn.ruleToStartState[rule], 1, PredictionContext::EMPTY), configs.get(),
                busy, false, false, false);
        add(std::move(configs));
    }
    SynPredDfa::State* s = dfa.states[0].get();
    // nothing follows EOF: past it, only a configuration at the rule's end matched
    for (ssize_t k = 1; s != nullptr && !s->accept; ++k) {
        const size_t t = input->LA(k);
        auto it = std::find_if(s->next.begin(), s->next.end(), [t](const auto& edge) { return edge.first == t; });
        if (it == s->next.end()) {
            computed = true;
            auto reach = std::make_unique<ATNConfigSet>(false);
            ATNConfig::Set busy;
            for (const auto& c : s->configs->configs)
                for (const auto& tr : c->state->transitions)
                    if (!tr->isEpsilon() && tr->matches(t, 0, atn.maxTokenType))
                        closure(std::make_shared<ATNConfig>(*c, tr->target), reach.get(), busy, false, false, false);
            SynPredDfa::State* target = add(std::move(reach));
            s->next.emplace_back(t, target);
            s = target;
        } else {
            s = it->second;
        }
        if (t == antlr4::Token::EOF && s != nullptr && !s->accept) s = nullptr;
    }
    if (computed) mergeCache.clear();
    _dfa = savedDfa;
    return s != nullptr;
}

void TSqlParserATNSimulator::closureCheckingStopState(const Ref<antlr4::atn::ATNConfig>& config,
                                                      antlr4::atn::ATNConfigSet* configs,
                                                      antlr4::atn::ATNConfig::Set& closureBusy, bool collectPredicates,
                                                      bool fullCtx, int depth, bool treatEofAsEpsilon) {
    using namespace antlr4::atn;
    // A precedence DFA marks configurations by the follow link they take (none in these grammars).
    if (collectPredicates || fullCtx || (_dfa != nullptr && _dfa->isPrecedenceDfa())) {
        ParserATNSimulator::closureCheckingStopState(config, configs, closureBusy, collectPredicates, fullCtx, depth,
                                                     treatEofAsEpsilon);
        return;
    }
    struct Live {
        TSqlParserATNSimulator& sim;
        const Ref<ATNConfig>& config;
        ATNConfigSet* configs;
        ATNConfig::Set& closureBusy;
        bool treatEofAsEpsilon;
        Ref<ATNConfig> self;   // the configuration the walk started with or last checked

        Ref<ATNConfig> Make(ATNState* p, const Ref<const PredictionContext>& context, bool isSelf) {
            return isSelf ? self : std::make_shared<ATNConfig>(*config, p, context);
        }
        void Add(ATNState* p, const Ref<const PredictionContext>& context, bool isSelf) {
            configs->add(Make(p, context, isSelf), &sim.mergeCache);
        }
        void Fall(ATNState* stop, const Ref<const PredictionContext>& context, bool isSelf) {
            sim.Replay(Make(stop, context, isSelf), configs, closureBusy, treatEofAsEpsilon,
                       sim.FallOffFor(stop, treatEofAsEpsilon));
        }
        bool Check(ATNState* p, const Ref<const PredictionContext>& context) {
            self = std::make_shared<ATNConfig>(*config, p, context);
            return closureBusy.insert(self).second;
        }
        void Checked() {}
    };
    Live live{*this, config, configs, closureBusy, treatEofAsEpsilon, config};
    Traverse(*this, live, config->state, config->context, true, treatEofAsEpsilon);
}

TSqlParserATNSimulator::TSqlParserATNSimulator(antlr4::Parser* parser, const antlr4::atn::ATN& atn,
                                               std::vector<antlr4::dfa::DFA>& decisionToDFA,
                                               antlr4::atn::PredictionContextCache& sharedContextCache)
    : ParserATNSimulator(parser, atn, decisionToDFA, sharedContextCache,
                         antlr4::atn::ParserATNSimulatorOptions().setPredictionContextMergeCacheOptions(
                             antlr4::atn::PredictionContextMergeCacheOptions().setClearEveryN(0))) {
    Tables();
}

std::unique_ptr<antlr4::atn::ATNConfigSet> TSqlParserATNSimulator::computeReachSet(antlr4::atn::ATNConfigSet* closure,
                                                                                  size_t t, bool fullCtx) {
    // before (an exception may leave entries) and after (a prediction for a predicate inside may
    // have cleared the cache meanwhile, and this one goes on filling it)
    mergeCacheUsed_ = true;
    std::unique_ptr<antlr4::atn::ATNConfigSet> reach = ParserATNSimulator::computeReachSet(closure, t, fullCtx);
    mergeCacheUsed_ = true;
    return reach;
}

std::unique_ptr<antlr4::atn::ATNConfigSet> TSqlParserATNSimulator::computeStartState(antlr4::atn::ATNState* p,
                                                                                    antlr4::RuleContext* ctx,
                                                                                    bool fullCtx) {
    mergeCacheUsed_ = true;   // as in computeReachSet
    std::unique_ptr<antlr4::atn::ATNConfigSet> start = ParserATNSimulator::computeStartState(p, ctx, fullCtx);
    mergeCacheUsed_ = true;
    return start;
}

size_t TSqlParserATNSimulator::adaptivePredict(antlr4::TokenStream* input, size_t decision,
                                               antlr4::ParserRuleContext* outerContext) {
    using antlr4::atn::ATN;
    // What the tests below read first, fetched while ANTLR 4 predicts: the decision's entry and its
    // test of LA(1). ANTLR 4's prediction reads LA(1) first, so reading it here reads nothing more.
    const Antlr2Decisions::Entry& entry = antlr2_->byDecision[decision];
    const uint16_t* tests = entry.tests.load(std::memory_order_acquire);
    size_t la1 = 0;   // LA(1) once read (no token has type 0)
    if (tests != nullptr) {
        la1 = input->LA(1);
        __builtin_prefetch(tests + ByToken::Index(la1));
    }
    size_t alt = ATN::INVALID_ALT_NUMBER;
    {
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
            ~Restore() {
                s->_input = i;
                s->_startIndex = st;
                s->_outerContext = o;
                s->_dfa = d;
                // what ANTLR's prediction does on exit by default (the constructor turns that off)
                if (s->mergeCacheUsed_) {
                    s->mergeCache.clear();
                    s->mergeCacheUsed_ = false;
                }
            }
        } restore{this, savedInput, savedStart, savedOuter, savedDfa};
        try {
            alt = antlr4::atn::ParserATNSimulator::adaptivePredict(input, decision, outerContext);
        } catch (antlr4::NoViableAltException&) {
        }
    }
    // ANTLR 2 takes the first alternative whose tests pass (a loop enters with it), which on valid
    // input is ANTLR 4's; where the input does not parse, the syntax error surfaces inside it.
    // ANTLR 4's alternative's predicates have been evaluated, the others' are when they decide;
    // syntactic predicates are, ANTLR 4's alternative's too.
    // without lookahead: no other alternative starts with LA(1) and it has no syntactic predicate,
    // or neither does any of the loop's alternatives (Antlr2Decisions::Entry::direct)
    size_t la2 = 0;   // LA(2) once read
    const uint64_t directMask = entry.direct.load(std::memory_order_relaxed);
    const bool isDirect = (alt < 64) & ((directMask >> (alt % 64) & 1) != 0);
    if (tests == nullptr) {
        if (isDirect) return alt;
    } else {
        // By LA(1): when ANTLR 2's first candidate is ANTLR 4's alternative (for a loop's
        // enter, any alternative) without a syntactic predicate, its lookahead test decides,
        // as below (its semantic predicates were ANTLR 4's); and a loop exits when no
        // alternative starts with LA(1). The outcomes are combined without branches: which
        // one holds varies from one prediction to the next.
        const uint16_t e = tests[ByToken::Index(la1)];
        const size_t f = e & ByToken::kAlt;
        const bool loop = (e & ByToken::kLoop) != 0;
        const size_t exitAlt = (e & ByToken::kExit1) != 0 ? 1 : 2;
        const bool candidate = (loop & (alt == 3 - exitAlt)) | (!loop & (f == alt));
        const bool plain = candidate & ((e & ByToken::kNoSynpred) != 0);
        const bool depth2 = (e & ByToken::kDepth2) != 0;
        if (isDirect | (loop & (alt == exitAlt) & (f == 0)) | (plain & !depth2)) return alt;
        if (plain) {
            const uint16_t* look2 = ByToken::Look2(tests, antlr2_->maxToken, f - 1);
            __builtin_prefetch(look2);
            la2 = input->LA(2);
            if (ByToken::Test(look2, ByToken::Index(la2))) return alt;
        }
    }
    const Antlr2Decision& d = Antlr2DecisionFor(decision);
    const bool loop = d.exitAlt != ATN::INVALID_ALT_NUMBER;   // a loop's enter/exit decision
    const size_t enter = d.exitAlt == 1 ? 2 : 1;
    if (alt != ATN::INVALID_ALT_NUMBER) {
        // (the entry's mask, set by now, where it covers the alternative: its line is in the cache)
        const bool direct = alt < 64 ? (entry.direct.load(std::memory_order_relaxed) >> alt & 1) != 0
                            : loop   ? alt == enter && d.allDirect
                                     : d.alts[alt - 1].direct;
        if (direct) return alt;
    }
    if (la1 == 0) la1 = input->LA(1);
    // ANTLR 2 tries the alternatives in order, lookahead then predicates (the others' lookahead
    // fails at LA(1)); a loop enters with whichever matches first
    const uint32_t* candidate = d.candidates.data() + d.candidateStart[ByToken::Index(la1)];
    const uint32_t* const end = d.candidates.data() + d.candidateStart[ByToken::Index(la1) + 1];
    for (; candidate != end; ++candidate) {
        const size_t i = *candidate & Antlr2Decision::kCandidateAlt;
        if ((*candidate & Antlr2Decision::kCandidateDepth2) != 0) {
            if (la2 == 0) la2 = input->LA(2);
            if (!ByToken::Test(ByToken::Look2(d.tests.data(), d.maxToken, i), ByToken::Index(la2))) continue;
        }
        const bool chosen = loop ? alt == enter : i + 1 == alt;
        if ((*candidate & Antlr2Decision::kCandidateDynamic) == 0 ||
            Antlr2PredicatesMatch(d, i, input, outerContext, chosen))
            return loop ? enter : i + 1;
    }
    if (loop) return d.exitAlt;
    throw antlr4::NoViableAltException(parser, input, input->LT(1), input->LT(1), nullptr, outerContext, false);
}

size_t TSqlParserATNSimulator::LoopExitAlternative(const antlr4::atn::DecisionState* d) {
    using antlr4::atn::ATNStateType;
    if (d->getStateType() == ATNStateType::STAR_LOOP_ENTRY || d->getStateType() == ATNStateType::PLUS_LOOP_BACK)
        return d->nonGreedy ? 1 : 2;
    return antlr4::atn::ATN::INVALID_ALT_NUMBER;
}

}  // namespace tsql::parser
