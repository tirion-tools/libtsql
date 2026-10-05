// ANTLR 4 runtime adaptations shared by every converted parser (ANTLR 2 semantics).
#include "ParserRuntime.h"

#include <algorithm>
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

void TSqlBailErrorStrategy::sync(antlr4::Parser*) {}

void TSqlBailErrorStrategy::reportError(antlr4::Parser*, const antlr4::RecognitionException&) {}

// ============================================================================ prediction

namespace {

/// The alternatives (with their predicates, in the order tried) that the two-token fallback picks
/// from. Closures without full context collect predicates rather than evaluate them, so these
/// depend only on the decision and the tokens; computing them chases the follow of every rule, so
/// they are computed once per (ATN, decision, stage, LA(1), LA(2)) and the predicates are evaluated
/// per prediction. The tokens are read as the uncached computation reads them (LA(2) only when it
/// is needed): a caret capture (src/editor) depends on which tokens the parser looked at.
struct Fallback {
    bool needSecond = false;   // kFirst: the reach after LA(1) is not empty and LA(1) is not EOF
    std::vector<std::pair<size_t, Ref<const antlr4::atn::SemanticContext>>> candidates;
};
enum FallbackStage { kFirst, kExact };
using FallbackKey = std::tuple<const antlr4::atn::ATN*, size_t, int, size_t, size_t>;

std::mutex fallbackMutex;
std::map<FallbackKey, std::shared_ptr<const Fallback>> fallbackCache;

void AddCandidate(Fallback& out, const antlr4::atn::ATNConfig& c) {
    for (const auto& [alt, pred] : out.candidates)
        if (alt == c.alt && pred == c.semanticContext) return;
    out.candidates.emplace_back(c.alt, c.semanticContext);
}

void AddCandidates(Fallback& out, const antlr4::atn::ATNConfigSet& configs) {
    for (const auto& c : configs.configs) AddCandidate(out, *c);
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

/// ANTLR 2's k=2 lookahead is linear approximate: an alternative also matches when LA(1) can start
/// it and LA(2) can be its second token after *any* first token. The second tokens of one
/// alternative of a decision: whether it is among the alternatives of
/// computeReachSet(computeReachSet(start, t), LA(2)) for some t, for every LA(2) at once.
struct TSqlParserATNSimulator::SecondTokens {
    std::vector<uint64_t> bits;   // token t (1..maxTokenType): bit t % 64 of bits[t / 64]
    bool eof = false;

    bool Has(size_t t) const {
        if (t == antlr4::Token::EOF) return eof;
        return t / 64 < bits.size() && (bits[t / 64] >> (t % 64) & 1) != 0;
    }
};

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
    // guarded by fallbackMutex; map nodes do not move
    static std::map<std::tuple<const ATN*, size_t, bool>, FallOff> cache;
    const auto key = std::make_tuple(&atn, stop->stateNumber, treatEofAsEpsilon);
    {
        std::lock_guard<std::mutex> lock(fallbackMutex);
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
    std::lock_guard<std::mutex> lock(fallbackMutex);
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

const TSqlParserATNSimulator::SecondTokens& TSqlParserATNSimulator::SecondTokensFor(
    const antlr4::dfa::DFA& dfa, const antlr4::atn::ATNConfigSet& start, size_t alt) {
    using namespace antlr4::atn;
    // guarded by fallbackMutex; map nodes do not move
    static std::map<std::pair<const ATN*, size_t>, std::vector<size_t>> firstTokenAlts;
    static std::map<std::tuple<const ATN*, size_t, size_t>, SecondTokens> cache;
    const auto key = std::make_tuple(&atn, dfa.decision, alt);
    const std::vector<size_t>* altOf = nullptr;
    {
        std::lock_guard<std::mutex> lock(fallbackMutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
        if (auto it = firstTokenAlts.find({&atn, dfa.decision}); it != firstTokenAlts.end()) altOf = &it->second;
    }
    const size_t maxToken = atn.maxTokenType;
    if (altOf == nullptr) {
        // the alternative of the configurations each first token t reaches: 0 none, SIZE_MAX several
        std::vector<size_t> alts(maxToken + 1, 0);
        for (const auto& c : start.configs) {
            if (RuleStopState::is(c->state)) continue;
            for (const auto& tr : c->state->transitions)
                ForEachToken(*tr, maxToken, [&](size_t t) {
                    size_t& a = alts[t];
                    a = a == 0 || a == c->alt ? c->alt : SIZE_MAX;
                });
        }
        std::lock_guard<std::mutex> lock(fallbackMutex);
        altOf = &firstTokenAlts.emplace(std::make_pair(&atn, dfa.decision), std::move(alts)).first->second;
    }
    // computeReachSet(start, t) skips the closure when every configuration it reaches has the same
    // alternative, so a configuration at an epsilon-only state then matches no second token. The
    // reach of every t is a union over t of (configuration, transition) pairs, a closure is the
    // union of the closures of its configurations, and the alternative of a configuration never
    // changes: so one closure over this alternative's pairs whose tokens do not shortcut gives the
    // same states as the per-token reaches. Only the states count (their transitions are the
    // second tokens), and closureBusy only skips walks already taken, so the closure is walked
    // here without configurations, each fall-off once by its recorded steps.
    SecondTokens out;
    out.bits.assign(maxToken / 64 + 1, 0);
    struct Walker {
        TSqlParserATNSimulator& sim;
        SecondTokens& out;
        size_t maxToken;
        std::vector<std::pair<ATNState*, Ref<const PredictionContext>>> eofTargets;   // after an EOF transition
        std::vector<bool> fallen;                                                     // by stop state number

        // a configuration of the reach set
        void Add(ATNState* p, const Ref<const PredictionContext>& context, bool) {
            if (RuleStopState::is(p)) {
                out.eof = true;
                return;
            }
            for (const auto& tr : p->transitions) {
                ForEachToken(*tr, maxToken, [&](size_t t) { out.bits[t / 64] |= uint64_t{1} << (t % 64); });
                if (tr->matches(antlr4::Token::EOF, 0, maxToken)) eofTargets.emplace_back(tr->target, context);
            }
        }
        void Fall(ATNState* stop, const Ref<const PredictionContext>&, bool) {
            if (fallen[stop->stateNumber]) return;
            fallen[stop->stateNumber] = true;
            for (const FallOff::Step& s : sim.FallOffFor(stop, false).steps) {
                if (s.kind == FallOff::kAddStop)
                    out.eof = true;
                else if (s.kind == FallOff::kAdd)
                    Add(s.state, s.context, false);
                else if (s.kind == FallOff::kFall)
                    Fall(s.state, s.context, false);
            }
        }
        // without EOF as epsilon there are no such checks
        bool Check(ATNState*, const Ref<const PredictionContext>&) { return false; }
        void Checked() {}
    };
    Walker w{*this, out, maxToken, {}, std::vector<bool>(atn.states.size())};
    for (const auto& c : start.configs) {
        if (c->alt != alt || RuleStopState::is(c->state)) continue;
        for (const auto& tr : c->state->transitions) {
            bool unique = false, several = false;
            ForEachToken(*tr, maxToken, [&](size_t t) {
                if ((*altOf)[t] == SIZE_MAX) several = true;
                else unique = true;
            });
            if (unique) w.Add(tr->target, c->context, false);
            if (several) Traverse(*this, w, tr->target, c->context, false, false);
        }
    }
    // LA(2) EOF: a configuration already at a rule stop state, or one whose EOF transition closes
    // (EOF as epsilon) to one.
    if (!out.eof && !w.eofTargets.empty()) {
        ATNConfigSet eofReach(false);
        ATNConfig::Set busy;
        for (const auto& [target, context] : w.eofTargets)
            closure(std::make_shared<ATNConfig>(target, alt, context), &eofReach, busy, false, false, true);
        for (const auto& c : eofReach.configs)
            if (RuleStopState::is(c->state)) out.eof = true;
    }
    std::lock_guard<std::mutex> lock(fallbackMutex);
    return cache.emplace(key, std::move(out)).first->second;
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
        using antlr4::atn::ATNConfigSet;
        using antlr4::dfa::DFAState;
        input->seek(startIndex);
        const size_t la1 = input->LA(1);
        size_t la2 = 0;
        bool la2Read = false;
        auto readLa2 = [&] {
            if (!la2Read) la2 = input->LA(2);
            la2Read = true;
        };
        // computeReachSet(from, t), read from the DFA edge ANTLR's execATN has just added when it
        // looked at t (DFA states are deduplicated by their ordered configurations). s0 holds
        // computeStartState(decision, EMPTY, false): no decision is a precedence decision.
        auto reachOf = [&](DFAState* from, ATNConfigSet* fromConfigs, size_t t, DFAState*& to,
                           std::unique_ptr<ATNConfigSet>& owned) -> ATNConfigSet* {
            to = from != nullptr ? getExistingTargetState(from, t) : nullptr;
            if (to == ERROR.get()) {
                to = nullptr;
                return nullptr;
            }
            if (to != nullptr) return to->configs.get();
            owned = computeReachSet(fromConfigs, t, false);
            return owned.get();
        };
        DFAState* d1 = nullptr;
        std::unique_ptr<ATNConfigSet> owned1;
        bool reached1 = false;
        ATNConfigSet* reach1 = nullptr;
        auto first1 = [&] {
            if (!reached1) reach1 = reachOf(s0, s0->configs.get(), la1, d1, owned1);
            reached1 = true;
            return reach1;
        };
        auto cached = [&](FallbackStage stage, auto compute) {
            const FallbackKey key{&atn, dfa.decision, stage, la1, stage == kFirst ? 0 : la2};
            {
                std::lock_guard<std::mutex> lock(fallbackMutex);
                if (auto it = fallbackCache.find(key); it != fallbackCache.end()) return it->second;
            }
            auto made = std::make_shared<Fallback>();
            compute(*made);
            std::lock_guard<std::mutex> lock(fallbackMutex);
            return fallbackCache.emplace(key, std::move(made)).first->second;
        };
        auto choose = [&](const Fallback& f, auto keep) {
            size_t best = SIZE_MAX;
            for (const auto& [alt, pred] : f.candidates) {
                if (alt >= best || !keep(alt)) continue;
                if (pred != antlr4::atn::SemanticContext::Empty::Instance &&
                    !evalSemanticContext(pred, outerContext, alt, false))
                    continue;
                best = alt;
            }
            return best;
        };
        auto any = [](size_t) { return true; };
        // the alternatives whose first token is LA(1)
        const std::shared_ptr<const Fallback> first = cached(kFirst, [&](Fallback& out) {
            ATNConfigSet* reach = first1();
            out.needSecond = reach != nullptr && la1 != antlr4::Token::EOF;
            if (reach != nullptr) AddCandidates(out, *reach);
        });
        // the alternatives whose first two tokens are LA(1) LA(2) (or LA(1) EOF)
        std::shared_ptr<const Fallback> exact = first;
        if (first->needSecond) {
            readLa2();
            exact = cached(kExact, [&](Fallback& out) {
                ATNConfigSet* reach = first1();
                DFAState* d2 = nullptr;
                std::unique_ptr<ATNConfigSet> owned2;
                if (reach != nullptr) reach = reachOf(d1, reach, la2, d2, owned2);
                if (reach != nullptr) AddCandidates(out, *reach);
            });
        }
        size_t best = choose(*exact, any);
        if (best != SIZE_MAX) return best;
        readLa2();
        if (la1 != antlr4::Token::EOF) {
            best = choose(*first, [&](size_t alt) { return SecondTokensFor(dfa, *s0->configs, alt).Has(la2); });
            if (best != SIZE_MAX) return best;
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
