#include "Walker.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <unordered_set>

#include "Names.h"
#include "ParserRuntime.h"
#include "antlr4-runtime.h"

namespace tsql::editor::detail {

using antlr4::atn::ATNState;
using antlr4::atn::ATNStateType;
using antlr4::atn::TransitionType;

namespace {
using Words = std::vector<std::string>;

Words Intersect(const Words& a, const Words& b) {
    Words out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}
Words Union(const Words& a, const Words& b) {
    Words out;
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}
Words Minus(const Words& a, const Words& b) {
    Words out;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

PredicateValue Make(PredicateValue::Kind k, Words w = {}) {
    PredicateValue v;
    v.kind = k;
    v.words = std::move(w);
    return v;
}

PredicateValue Not(PredicateValue a) {
    switch (a.kind) {
        case PredicateValue::False: return Make(PredicateValue::True);
        case PredicateValue::True: return Make(PredicateValue::False);
        case PredicateValue::Unknown: return a;
        case PredicateValue::Require: return Make(PredicateValue::Exclude, std::move(a.words));
        case PredicateValue::Exclude: return Make(PredicateValue::Require, std::move(a.words));
    }
    return a;
}

PredicateValue And(const PredicateValue& a, const PredicateValue& b) {
    using K = PredicateValue;
    if (a.kind == K::False || b.kind == K::False) return Make(K::False);
    if (a.kind == K::True || a.kind == K::Unknown) return b.kind == K::True ? a : b;
    if (b.kind == K::True || b.kind == K::Unknown) return a;
    if (a.kind == K::Require && b.kind == K::Require) {
        Words w = Intersect(a.words, b.words);
        return w.empty() ? Make(K::False) : Make(K::Require, std::move(w));
    }
    if (a.kind == K::Exclude && b.kind == K::Exclude) return Make(K::Exclude, Union(a.words, b.words));
    const PredicateValue& req = a.kind == K::Require ? a : b;
    const PredicateValue& excl = a.kind == K::Require ? b : a;
    Words w = Minus(req.words, excl.words);
    return w.empty() ? Make(K::False) : Make(K::Require, std::move(w));
}

PredicateValue Or(const PredicateValue& a, const PredicateValue& b) {
    using K = PredicateValue;
    if (a.kind == K::True || b.kind == K::True) return Make(K::True);
    if (a.kind == K::False) return b;
    if (b.kind == K::False) return a;
    if (a.kind == K::Unknown || b.kind == K::Unknown) return Make(K::Unknown);
    if (a.kind == K::Require && b.kind == K::Require) return Make(K::Require, Union(a.words, b.words));
    if (a.kind == K::Exclude && b.kind == K::Exclude) {
        Words w = Intersect(a.words, b.words);
        return w.empty() ? Make(K::True) : Make(K::Exclude, std::move(w));
    }
    const PredicateValue& req = a.kind == K::Require ? a : b;
    const PredicateValue& excl = a.kind == K::Require ? b : a;
    Words w = Minus(excl.words, req.words);
    return w.empty() ? Make(K::True) : Make(K::Exclude, std::move(w));
}

class PredicateEvaluator {
public:
    PredicateEvaluator(const Grammar& g, const std::string& s, const WalkInput& in, size_t at)
        : g_(g), s_(s), in_(in), at_(at) {}

    PredicateValue Parse() {
        if (pos_ >= s_.size()) return Make(PredicateValue::Unknown);
        const char c = s_[pos_++];
        switch (c) {
            case '!': return Not(Parse());
            case '&':
            case '|': {
                const size_t n = Number();
                PredicateValue v = Parse();
                for (size_t i = 1; i < n; ++i) v = c == '&' ? And(v, Parse()) : Or(v, Parse());
                return v;
            }
            case 'm':
            case 't': {
                const size_t k = Number();
                ++pos_;   // ':'
                size_t end = s_.find(';', pos_);
                if (end == std::string::npos) end = s_.size();
                const std::string arg = s_.substr(pos_, end - pos_);
                pos_ = end + 1;
                if (k == 0) return Make(PredicateValue::Unknown);
                const size_t target = at_ + k - 1;
                if (target > in_.caret) return Make(PredicateValue::Unknown);
                if (target == in_.caret)
                    return c == 'm' ? Make(PredicateValue::Require, {arg}) : Make(PredicateValue::Unknown);
                if (target >= in_.tokens->size()) return Make(PredicateValue::False);   // EOF
                bool match;
                if (c == 'm') {
                    match = (*in_.upper)[target] == arg;
                } else {
                    auto it = g_.tokenTypes.find(arg);
                    if (it == g_.tokenTypes.end()) return Make(PredicateValue::Unknown);
                    match = (*in_.tokens)[target].type == it->second;
                }
                return Make(match ? PredicateValue::True : PredicateValue::False);
            }
            case 'T': return Make(PredicateValue::True);
            case 'b': {   // the token k before the current one is the word
                const size_t k = Number();
                ++pos_;   // ':'
                size_t end = s_.find(';', pos_);
                if (end == std::string::npos) end = s_.size();
                const std::string arg = s_.substr(pos_, end - pos_);
                pos_ = end + 1;
                if (k == 0 || k > at_ || at_ - k >= in_.upper->size()) return Make(PredicateValue::Unknown);
                return Make((*in_.upper)[at_ - k] == arg ? PredicateValue::True : PredicateValue::False);
            }
            default: return Make(PredicateValue::Unknown);   // '?'
        }
    }

private:
    size_t Number() {
        size_t n = 0;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') n = n * 10 + static_cast<size_t>(s_[pos_++] - '0');
        return n;
    }

    const Grammar& g_;
    const std::string& s_;
    const WalkInput& in_;
    size_t at_;
    size_t pos_ = 0;
};

struct Frame {
    int follow;
    const Frame* parent;
};

/// An opaque predicate that reads the token at the caret: evaluated for each candidate token.
struct Deferred {
    size_t rule, pred, index;
    bool live;
    bool operator==(const Deferred& o) const {
        return rule == o.rule && pred == o.pred && index == o.index && live == o.live;
    }
};

/// An alternative that ANTLR 2's tests at a decision the parser predicts try before the one the
/// walk follows and that take the caret token when it is in that alternative's LA(2) set
/// (`second`; else any token) and its word is in `words` (kOnly) / not in it (kExcept): then the
/// parser takes that alternative, not the walk's.
struct CaretExclusion {
    enum WordTest : uint8_t { kAny, kOnly, kExcept };
    size_t decision = 0, alt = 0;
    bool second = true;
    WordTest wordTest = kAny;
    Words words;   // sorted
    bool operator==(const CaretExclusion& o) const {
        return decision == o.decision && alt == o.alt && second == o.second && wordTest == o.wordTest &&
               words == o.words;
    }
};

/// Constraint on the caret word collected along a path.
struct Cond {
    bool hasRequire = false;
    Words require;                         // the caret word must be one of these (predicates)
    Words exclude;                         // the caret word must not be one of these (predicates)
    const KeywordState* pending = nullptr; // a rule call's action checks the identifier it returns
    /// The rule that call enters: a reserved keyword it matches itself (securityStatementPermission
    /// takes any) becomes the identifier, so the check applies to its text too.
    size_t pendingRule = SIZE_MAX;
    std::vector<Deferred> deferred;        // predicates the caret token must satisfy
    std::vector<CaretExclusion> excluded;  // caret tokens ANTLR 2 takes elsewhere
    bool operator==(const Cond& o) const {
        return hasRequire == o.hasRequire && require == o.require && exclude == o.exclude && pending == o.pending &&
               pendingRule == o.pendingRule && deferred == o.deferred && excluded == o.excluded;
    }
};

struct Config {
    const ATNState* state;
    size_t index;
    const Frame* frame;
    uint32_t cond;
    /// Still where the parser stood: the start's rule context and token, no action or rule call on
    /// the way (so that context's locals are the ones here).
    bool live = false;
    /// The statement the walk started in has ended: what follows comes after it (the next statement
    /// of its batch or block, ELSE, END, ...).
    bool nextStatement = false;
    /// The last action on the path that can reject the input (Grammar::rejectingAction), or
    /// predicate on rule locals the walk does not have: the real parser may not get past it.
    /// 0: none, else an index + 1 into the walk's sources.
    uint32_t source = 0;
    /// The path took a token after the statement the walk started in ended (WalkCandidate::beyondStatement).
    bool beyond = false;
    /// Decisions before the token at `index` whose ANTLR 2 tests this path has yet to pass (index + 1
    /// into the walk's pending tests, 0: none).
    uint32_t tests = 0;
};

struct ConfigHash {
    size_t operator()(const Config& k) const {
        size_t h = k.state->stateNumber * 0x9E3779B97F4A7C15ull;
        h ^= k.index + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
        h ^= reinterpret_cast<size_t>(k.frame) + (h << 6) + (h >> 2);
        h ^= k.cond + (h << 6) + (h >> 2);
        h ^= k.source + (h << 6) + (h >> 2);
        h ^= k.tests + (h << 6) + (h >> 2);
        return h ^ static_cast<size_t>(k.live) ^ (static_cast<size_t>(k.nextStatement) << 1) ^
               (static_cast<size_t>(k.beyond) << 2);
    }
};
struct ConfigEq {
    bool operator()(const Config& a, const Config& b) const {
        return a.state == b.state && a.index == b.index && a.frame == b.frame && a.cond == b.cond && a.live == b.live &&
               a.nextStatement == b.nextStatement && a.source == b.source && a.beyond == b.beyond && a.tests == b.tests;
    }
};

bool IsName(size_t type) {
    return type == static_cast<size_t>(ast::TSqlTokenType::Identifier) ||
           type == static_cast<size_t>(ast::TSqlTokenType::QuotedIdentifier);
}

class WalkerImpl {
public:
    WalkerImpl(const Grammar& g, const WalkInput& in, const std::function<void(const WalkCandidate&)>& emit)
        : g_(g), in_(in), emit_(emit), lastStatementRule_(g.RuleIndex("lastStatementOptSemi")) {
        conds_.emplace_back();
    }

    WalkStats Run() {
        const Frame* outer = nullptr;
        for (auto it = in_.outerFollow.rbegin(); it != in_.outerFollow.rend(); ++it) outer = Push(*it, outer);
        uint32_t cond = 0;
        auto kw = g_.keywordStates.find(in_.startPending);
        if (in_.startPending >= 0 && kw != g_.keywordStates.end()) {
            Cond nc;
            nc.pending = &kw->second;
            nc.pendingRule = CalledRule(g_.atn->states[static_cast<size_t>(in_.startPending)]);
            cond = Intern(std::move(nc));
        }
        work_.push_back({g_.atn->states[static_cast<size_t>(in_.startState)], in_.startIndex, outer, cond, true});
        while (!work_.empty()) {
            const Config c = work_.back();
            work_.pop_back();
            if (!visited_.insert(c).second) continue;
            if (++stats_.visited > in_.budget) {
                stats_.truncated = true;
                break;
            }
            Step(c);
        }
        return stats_;
    }

private:
    const Frame* Push(int follow, const Frame* parent) {
        auto key = std::make_pair(follow, parent);
        auto it = frames_.find(key);
        if (it != frames_.end()) return it->second;
        frameStore_.push_back({follow, parent});
        frames_.emplace(key, &frameStore_.back());
        return &frameStore_.back();
    }

    uint32_t Intern(Cond c) {
        for (uint32_t i = 0; i < conds_.size(); ++i)
            if (conds_[i] == c) return i;
        conds_.push_back(std::move(c));
        return static_cast<uint32_t>(conds_.size() - 1);
    }

    /// `cond` with predicate value `v` applied (the caret word's Require / Exclude sets), or nullopt
    /// when no word satisfies both (False: none).
    std::optional<uint32_t> Constrain(uint32_t cond, const PredicateValue& v) {
        if (v.kind == PredicateValue::False) return std::nullopt;
        if (v.kind != PredicateValue::Require && v.kind != PredicateValue::Exclude) return cond;
        Cond nc = conds_[cond];
        if (v.kind == PredicateValue::Require) {
            nc.require = nc.hasRequire ? Intersect(nc.require, v.words) : v.words;
            nc.hasRequire = true;
        } else {
            nc.exclude = Union(nc.exclude, v.words);
        }
        if (nc.hasRequire) {
            nc.require = Minus(nc.require, nc.exclude);
            if (nc.require.empty()) return std::nullopt;
        }
        return Intern(std::move(nc));
    }

    /// The real predicate's value at configuration `c` (CaretSession::kTrue, ...), memoised.
    int Opaque(size_t rule, size_t pred, const Config& c) {
        if (!in_.evaluate) return CaretSession::kUnknown;
        const auto key = std::make_tuple(pred, c.index, c.live);
        auto it = opaque_.find(key);
        if (it != opaque_.end()) return it->second;
        const int value = in_.evaluate(rule, pred, c.index, c.live, nullptr);
        opaque_.emplace(key, value);
        return value;
    }

    /// Whether the token of `type` and `text` at the caret satisfies the deferred predicates of
    /// `cond` (those the real parser decides false reject it; undecided ones let it through).
    bool DeferredHold(const Cond& cond, uint32_t type, const std::string& text) {
        for (const Deferred& d : cond.deferred) {
            const auto key = std::make_tuple(d.pred, d.index, d.live, type, text);
            auto it = probed_.find(key);
            if (it == probed_.end()) {
                ProbeToken probe;
                probe.type = type;
                probe.text = text;
                it = probed_.emplace(key, in_.evaluate(d.rule, d.pred, d.index, d.live, &probe)).first;
            }
            if (it->second == CaretSession::kFalse) return false;
        }
        return true;
    }

    /// The source (Config::source) for an action or predicate at state `s` of configuration `c`.
    uint32_t Source(const ATNState* s, const Config& c) {
        const auto key = std::make_tuple(s->stateNumber, c.index, c.frame);
        auto it = sources_.find(key);
        if (it != sources_.end()) return it->second;
        // a key that names it in any walk: the state, the token and the rule calls
        uint64_t h = (static_cast<uint64_t>(s->stateNumber) << 32) ^ (c.index * 0x9E3779B97F4A7C15ull);
        for (const Frame* f = c.frame; f != nullptr; f = f->parent)
            h ^= static_cast<uint64_t>(f->follow) + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
        sourceKeys_.push_back(h | 1);   // never 0
        const uint32_t id = static_cast<uint32_t>(sourceKeys_.size());
        sources_.emplace(key, id);
        return id;
    }

    /// The rule a rule-call state calls (SIZE_MAX if it is not one).
    static size_t CalledRule(const ATNState* s) {
        if (s->transitions.empty() || s->transitions[0]->getTransitionType() != TransitionType::RULE) return SIZE_MAX;
        return s->transitions[0]->target->ruleIndex;
    }

    /// The condition after consuming a token (a pending rule-call check applies to it only).
    uint32_t Consumed(uint32_t cond) {
        if (conds_[cond].pending == nullptr) return cond;
        Cond nc = conds_[cond];
        nc.pending = nullptr;
        nc.pendingRule = SIZE_MAX;
        return Intern(std::move(nc));
    }

    /// Whether a reserved keyword matched at `state` is the identifier a binding pending check
    /// tests (see Cond::pendingRule) and fails it.
    bool KeywordFailsPending(const ATNState* state, const Cond& cond, const std::string& upper) const {
        const KeywordState* pending = cond.pending;
        return pending != nullptr && pending->binding && state->ruleIndex == cond.pendingRule &&
               !std::binary_search(pending->words.begin(), pending->words.end(), upper);
    }

    /// Whether the guards of token-match state `state` (Grammar::keywordGuards) hold for a token at
    /// `index`: the earlier tokens they check are among their words.
    bool GuardsHold(const ATNState* state, size_t index) const {
        auto it = g_.keywordGuards.find(static_cast<int>(state->stateNumber));
        if (it == g_.keywordGuards.end()) return true;
        for (const KeywordGuard& guard : it->second) {
            if (guard.distance > index) continue;   // before the parsed tokens: unknown
            const std::string& word = (*in_.upper)[index - guard.distance];
            if (!std::binary_search(guard.words.begin(), guard.words.end(), word)) return false;
        }
        return true;
    }

    /// Whether the token at `index` satisfies the binding word checks of `state` and of `cond`.
    bool WordsAllow(const ATNState* state, uint32_t cond, size_t index) const {
        if (!GuardsHold(state, index)) return false;
        const size_t type = (*in_.tokens)[index].type;
        if (!IsName(type)) return KeywordText(static_cast<uint32_t>(type)) == nullptr ||
                                  !KeywordFailsPending(state, conds_[cond], (*in_.upper)[index]);
        const bool quoted = type == static_cast<size_t>(ast::TSqlTokenType::QuotedIdentifier);
        auto check = [&](const KeywordState& ks) {
            if (!ks.binding) return true;
            return !quoted && std::binary_search(ks.words.begin(), ks.words.end(), (*in_.upper)[index]);
        };
        auto kw = g_.keywordStates.find(static_cast<int>(state->stateNumber));
        if (kw != g_.keywordStates.end() && !check(kw->second)) return false;
        const KeywordState* pending = conds_[cond].pending;
        return pending == nullptr || check(*pending);
    }

    /// What ANTLR 2's tests at a decision the parser predicts decide from the tokens at and after
    /// one token: ANTLR 2 takes the first alternative whose tests pass (a loop exits when none does).
    struct Antlr2Prefix {
        size_t certain = SIZE_MAX;        // the first alternative whose tests surely pass
        std::vector<CaretExclusion> caret;   // before it, those whose tests pass for some caret tokens, in order
    };

    /// A predicate an alternative starts with, at configuration `c`: True, False, Require / Exclude
    /// (the caret word) or Unknown (the walk cannot tell).
    PredicateValue AlternativePredicate(const antlr4::atn::PredicateTransition* pt, const Config& c) {
        auto it = g_.predicates.find(pt->getPredIndex());
        if (it == g_.predicates.end()) return Make(PredicateValue::Unknown);
        PredicateValue v = EvaluatePredicate(g_, it->second, in_, c.index);
        if (v.kind == PredicateValue::False || it->second.find_first_of("?$") == std::string::npos) return v;
        const int real = Opaque(pt->getRuleIndex(), pt->getPredIndex(), c);
        if (real == CaretSession::kTrue) return Make(PredicateValue::True);
        if (real == CaretSession::kFalse) return Make(PredicateValue::False);
        return Make(PredicateValue::Unknown);
    }

    /// ANTLR 2's tests at decision state `s` of a decision the parser predicts, with LA(1) at token
    /// `index` (before the caret; `live`: Config::live there). The caret as LA(1) is not tested: an
    /// earlier alternative the caret token could start meets the walk's in LA(1) only when both
    /// have depth 2, and LA(2) is the unknown token after it.
    /// Only alternatives of depth 2 matter: one of depth 1 shares no LA(1) token with the others
    /// (nor with a loop's exit), so the walk's alternative cannot start with LA(1) when it does.
    const Antlr2Prefix& PrefixAt(const ATNState* s, size_t index, bool live) {
        const auto key = std::make_tuple(s->stateNumber, index, live);
        auto found = prefixes_.find(key);
        if (found != prefixes_.end()) return found->second;
        Antlr2Prefix& p = prefixes_[key];
        const size_t decision = static_cast<size_t>(static_cast<const antlr4::atn::DecisionState*>(s)->decision);
        const parser::Antlr2Tests tests = Tests(decision);
        const Config at{s, index, nullptr, 0, live};   // for the predicates
        const size_t la1 = (*in_.tokens)[index].type;
        const bool caretSecond = index + 1 == in_.caret;
        for (size_t i = 0; i < tests.Alternatives(); ++i) {
            if (!tests.Depth2(i) || !tests.First(i, la1)) continue;
            if (!caretSecond && !tests.Second(i, (*in_.tokens)[index + 1].type)) continue;
            if (tests.SyntacticPredicate(i)) continue;   // a speculative parse the walk does not run
            PredicateValue v = Make(PredicateValue::True);
            for (const antlr4::atn::PredicateTransition* pt : tests.Predicates(i)) {
                const PredicateValue one = AlternativePredicate(pt, at);
                v = one.kind == PredicateValue::Unknown ? one : And(v, one);
                if (v.kind == PredicateValue::Unknown || v.kind == PredicateValue::False) break;
            }
            if (v.kind == PredicateValue::Unknown || v.kind == PredicateValue::False) continue;
            if (!caretSecond && v.kind == PredicateValue::True) {
                p.certain = i;
                break;
            }
            CaretExclusion e;
            e.decision = decision;
            e.alt = i;
            e.second = caretSecond;
            e.wordTest = v.kind == PredicateValue::Require   ? CaretExclusion::kOnly
                         : v.kind == PredicateValue::Exclude ? CaretExclusion::kExcept
                                                             : CaretExclusion::kAny;
            e.words = std::move(v.words);
            p.caret.push_back(std::move(e));
        }
        return p;
    }

    /// Config::tests: the decisions on a path whose ANTLR 2 tests its alternative has to pass,
    /// settled when the path takes the token it stands at (Settle). Most alternatives of a decision
    /// do not fit LA(1); those never need the decision's tests.
    struct PendingTest {
        const ATNState* decision;
        size_t before;   // ANTLR 2's alternatives tried before the path's (SIZE_MAX: all, a loop's exit)
        bool live;       // Config::live at the decision
        bool operator==(const PendingTest& o) const {
            return decision == o.decision && before == o.before && live == o.live;
        }
    };

    /// `tests` (Config::tests) and `t`
    uint32_t WithTest(uint32_t tests, const PendingTest& t) {
        std::vector<PendingTest> list = tests == 0 ? std::vector<PendingTest>{} : pendingTests_[tests - 1];
        if (std::find(list.begin(), list.end(), t) != list.end()) return tests;
        list.push_back(t);
        for (uint32_t i = 0; i < pendingTests_.size(); ++i)
            if (pendingTests_[i] == list) return i + 1;
        pendingTests_.push_back(std::move(list));
        return static_cast<uint32_t>(pendingTests_.size());
    }

    /// The condition of `c` taking the token at its index past its pending decisions (Config::tests),
    /// or nullopt when ANTLR 2 takes an earlier alternative at one of them.
    std::optional<uint32_t> Settle(const Config& c) {
        uint32_t cond = c.cond;
        if (c.tests == 0) return cond;
        for (const PendingTest& t : pendingTests_[c.tests - 1]) {
            const Antlr2Prefix& p = PrefixAt(t.decision, c.index, t.live);
            if (p.certain < t.before) return std::nullopt;
            cond = Excluding(cond, p, t.before);
        }
        return cond;
    }

    /// The first of ANTLR 2's alternatives at decision state `s` that may start with the token at
    /// `index` (SIZE_MAX: none), from the ATN's LL(1) sets (ATN::nextTokens, cached on the states):
    /// one without it in its own first tokens that cannot end its rule before a token has it in no
    /// LA(1) set ANTLR 2 tests. Most decisions are settled by this alone, without their ANTLR 2
    /// tables (whose closures are costly the first time).
    size_t FirstThatMayStart(const ATNState* s, size_t index) {
        const auto key = std::make_pair(s->stateNumber, index);
        auto it = mayStart_.find(key);
        if (it != mayStart_.end()) return it->second;
        const ATNState* block = s;
        if (s->getStateType() == ATNStateType::STAR_LOOP_ENTRY || s->getStateType() == ATNStateType::PLUS_LOOP_BACK)
            block = s->transitions[static_cast<const antlr4::atn::DecisionState*>(s)->nonGreedy ? 1 : 0]->target;
        const ssize_t la1 = static_cast<ssize_t>((*in_.tokens)[index].type);
        size_t first = SIZE_MAX;
        for (size_t i = 0; i < block->transitions.size() && first == SIZE_MAX; ++i) {
            const antlr4::misc::IntervalSet& next = g_.atn->nextTokens(block->transitions[i]->target);
            if (next.contains(la1) || next.contains(antlr4::Token::EPSILON)) first = i;
        }
        mayStart_.emplace(key, first);
        return first;
    }

    parser::Antlr2Tests Tests(size_t decision) {
        auto it = tests_.find(decision);
        if (it == tests_.end()) it = tests_.emplace(decision, in_.simulator->Antlr2TestsFor(decision)).first;
        return it->second;
    }

    /// `cond` with the exclusions of `p`'s alternatives before `before`.
    uint32_t Excluding(uint32_t cond, const Antlr2Prefix& p, size_t before) {
        size_t n = 0;
        while (n < p.caret.size() && p.caret[n].alt < before) ++n;
        if (n == 0) return cond;
        const auto key = std::make_tuple(cond, &p, n);
        auto it = excluding_.find(key);
        if (it != excluding_.end()) return it->second;
        Cond nc = conds_[cond];
        for (size_t k = 0; k < n; ++k)
            if (std::find(nc.excluded.begin(), nc.excluded.end(), p.caret[k]) == nc.excluded.end())
                nc.excluded.push_back(p.caret[k]);
        const uint32_t out = Intern(std::move(nc));
        excluding_.emplace(key, out);
        return out;
    }

    /// Whether ANTLR 2 takes another alternative for a caret token of `type` with `word` (upper
    /// case; null: a name of any text) at a decision on the path (Cond::excluded).
    bool Excluded(const Cond& cond, size_t type, const std::string* word) {
        for (const CaretExclusion& e : cond.excluded) {
            if (e.second && !Tests(e.decision).Second(e.alt, type)) continue;
            if (e.wordTest == CaretExclusion::kAny) return true;
            if (word == nullptr) continue;
            if (std::binary_search(e.words.begin(), e.words.end(), *word) == (e.wordTest == CaretExclusion::kOnly))
                return true;
        }
        return false;
    }

    /// `c` moved to `state`, its other fields as they are
    static Config At(const Config& c, const ATNState* state) {
        Config n = c;
        n.state = state;
        return n;
    }

    /// `c` taking the token at its index along `t`, past its pending decisions (Settle).
    void Take(const Config& c, const antlr4::atn::Transition* t) {
        const std::optional<uint32_t> cond = Settle(c);
        if (!cond) return;
        Config n = At(c, t->target);
        ++n.index;
        n.cond = Consumed(*cond);
        n.live = false;
        // a token after the statement's end: the path is in a later statement now
        n.nextStatement = false;
        n.beyond = c.beyond || c.nextStatement;
        n.tests = 0;
        work_.push_back(n);
    }

    void Step(const Config& c) {
        const ATNState* s = c.state;
        if (s->getStateType() == ATNStateType::RULE_STOP) {
            if (c.frame == nullptr) return;   // end of the outermost rule: EOF (never offered)
            Config n = At(c, g_.atn->states[static_cast<size_t>(c.frame->follow)]);
            n.frame = c.frame->parent;
            n.live = false;
            // a statement ended (of the batch, or of a block, IF, WHILE, module body, ...): what follows
            // in its caller comes after the caret's statement
            n.nextStatement =
                c.nextStatement || s->ruleIndex == g_.top.statementOptSemiRule || s->ruleIndex == lastStatementRule_;
            work_.push_back(n);
            return;
        }
        // a decision the parser predicts, before the caret: each alternative but the first (a loop's
        // exit) has ANTLR 2's tests of the alternatives tried before it to pass
        const bool tested = in_.simulator != nullptr && c.index < in_.caret && g_.predictedDecision[s->stateNumber] != 0;
        const bool loop =
            s->getStateType() == ATNStateType::STAR_LOOP_ENTRY || s->getStateType() == ATNStateType::PLUS_LOOP_BACK;
        const size_t loopExit = !loop ? 0 : static_cast<const antlr4::atn::DecisionState*>(s)->nonGreedy ? 1 : 2;
        for (size_t k = 0; k < s->transitions.size(); ++k) {
            const antlr4::atn::Transition* t = s->transitions[k].get();
            if (tested && t->getTransitionType() == TransitionType::EPSILON) {
                const size_t before = loopExit == 0 ? k : (k + 1 == loopExit ? SIZE_MAX : 0);
                Config n = At(c, t->target);
                if (FirstThatMayStart(s, c.index) < before) n.tests = WithTest(c.tests, {s, before, c.live});
                work_.push_back(n);
                continue;
            }
            switch (t->getTransitionType()) {
                case TransitionType::RULE: {
                    const auto* rt = static_cast<const antlr4::atn::RuleTransition*>(t);
                    Config n = At(c, rt->target);
                    auto kw = g_.keywordStates.find(static_cast<int>(s->stateNumber));
                    if (kw != g_.keywordStates.end()) {
                        Cond nc = conds_[c.cond];
                        nc.pending = &kw->second;
                        nc.pendingRule = rt->target->ruleIndex;
                        n.cond = Intern(std::move(nc));
                    }
                    n.frame = Push(static_cast<int>(rt->followState->stateNumber), c.frame);
                    n.live = false;
                    work_.push_back(n);
                    break;
                }
                case TransitionType::PREDICATE: {
                    const auto* pt = static_cast<const antlr4::atn::PredicateTransition*>(t);
                    auto it = g_.predicates.find(pt->getPredIndex());
                    if (it == g_.predicates.end()) {
                        work_.push_back(At(c, t->target));
                        break;
                    }
                    PredicateValue v = EvaluatePredicate(g_, it->second, in_, c.index);
                    uint32_t source = c.source;
                    bool defer = false;
                    if (v.kind != PredicateValue::False && it->second.find_first_of("?$") != std::string::npos) {
                        // decided by the tokens before the caret: the real predicate says
                        const int real = Opaque(pt->getRuleIndex(), pt->getPredIndex(), c);
                        if (real == CaretSession::kFalse) v.kind = PredicateValue::False;
                        else if (real == CaretSession::kTrue) v = PredicateValue{PredicateValue::True, {}};
                        // it reads the caret token: asked again for each candidate
                        else if (real == CaretSession::kReadsCaret) defer = true;
                        // rule locals the walk does not have, before the caret: the parser may stop here
                        else if (c.index < in_.caret) source = Source(s, c);
                    }
                    if (v.kind == PredicateValue::False) break;
                    const std::optional<uint32_t> constrained = Constrain(c.cond, v);
                    if (!constrained) break;
                    Config n = At(c, t->target);
                    n.cond = *constrained;
                    if (defer) {
                        Cond nc = conds_[n.cond];
                        const Deferred d{pt->getRuleIndex(), pt->getPredIndex(), c.index, c.live};
                        if (std::find(nc.deferred.begin(), nc.deferred.end(), d) == nc.deferred.end()) nc.deferred.push_back(d);
                        n.cond = Intern(std::move(nc));
                    }
                    n.source = source;
                    work_.push_back(n);
                    break;
                }
                case TransitionType::EPSILON:
                case TransitionType::PRECEDENCE:
                    work_.push_back(At(c, t->target));
                    break;
                case TransitionType::ACTION: {
                    auto ret = g_.returningAction.find(s->stateNumber);
                    if (ret != g_.returningAction.end()) {
                        // it returns from the rule when its condition holds: both ways, by the condition
                        const PredicateValue v = EvaluatePredicate(g_, ret->second, in_, c.index);
                        if (auto cond = Constrain(c.cond, v)) {
                            Config n = At(c, g_.atn->ruleToStopState[s->ruleIndex]);
                            n.cond = *cond;
                            n.live = false;
                            work_.push_back(n);
                        }
                        if (auto cond = Constrain(c.cond, Not(v))) {
                            Config n = At(c, t->target);
                            n.cond = *cond;
                            n.live = false;
                            work_.push_back(n);
                        }
                        break;
                    }
                    Config n = At(c, t->target);
                    n.live = false;
                    if (g_.rejectingAction[s->stateNumber] != 0) n.source = Source(s, c);
                    work_.push_back(n);
                    break;
                }
                case TransitionType::ATOM:
                case TransitionType::RANGE:
                case TransitionType::SET: {
                    if (c.index == in_.caret) {
                        Emit(c, t);
                    } else if (c.index < in_.tokens->size()) {
                        const size_t type = (*in_.tokens)[c.index].type;
                        if (t->matches(type, antlr4::Token::MIN_USER_TOKEN_TYPE, g_.atn->maxTokenType) &&
                            WordsAllow(s, c.cond, c.index))
                            Take(c, t);
                    }
                    break;
                }
                case TransitionType::NOT_SET:
                case TransitionType::WILDCARD:
                    if (c.index < in_.caret && c.index < in_.tokens->size()) Take(c, t);
                    break;
            }
        }
    }

    /// Whether, after a token taken to `next` (with rule calls `frame`), the parser can run an action
    /// that rejects the input before it looks at the next token (Grammar::actionAhead).
    bool RejectsAfter(const ATNState* next, const Frame* frame) const {
        for (size_t s = next->stateNumber;; frame = frame->parent) {
            const uint8_t ahead = g_.actionAhead[s];
            if (ahead & Grammar::kRejectAhead) return true;
            if (!(ahead & Grammar::kEndAhead) || frame == nullptr) return false;
            s = static_cast<size_t>(frame->follow);
        }
    }

    void Emit(const Config& c, const antlr4::atn::Transition* t) {
        const Cond& cond = conds_[c.cond];
        if (!GuardsHold(c.state, c.index)) return;
        rules_.clear();
        rules_.push_back(c.state->ruleIndex);
        for (const Frame* f = c.frame; f != nullptr; f = f->parent)
            rules_.push_back(g_.atn->states[static_cast<size_t>(f->follow)]->ruleIndex);
        auto kw = g_.keywordStates.find(static_cast<int>(c.state->stateNumber));
        const KeywordState* own = kw != g_.keywordStates.end() ? &kw->second : nullptr;
        const KeywordState* pending = cond.pending;
        const uint64_t doubtPath = c.source != 0 ? sourceKeys_[c.source - 1] : 0;
        const bool doubtToken = RejectsAfter(t->target, c.frame);
        const antlr4::misc::IntervalSet label = t->label();   // by value: keep it alive for the loop
        for (const antlr4::misc::Interval& iv : label.getIntervals()) {
            if (iv.b < static_cast<ssize_t>(antlr4::Token::MIN_USER_TOKEN_TYPE)) continue;   // EOF
            for (ssize_t type = std::max<ssize_t>(iv.a, antlr4::Token::MIN_USER_TOKEN_TYPE); type <= iv.b; ++type) {
                WalkCandidate cand;
                cand.tokenType = static_cast<size_t>(type);
                cand.rules = &rules_;
                cand.nextStatement = c.nextStatement;
                cand.beyondStatement = c.beyond;
                cand.doubtPath = doubtPath;
                cand.doubtToken = doubtToken;
                if (IsName(cand.tokenType)) {
                    const bool bound = (own != nullptr && own->binding) || (pending != nullptr && pending->binding);
                    if (bound || cond.hasRequire) {
                        if (cand.tokenType == static_cast<size_t>(ast::TSqlTokenType::QuotedIdentifier)) continue;
                        bool first = true;
                        auto narrow = [&](const Words& w) {
                            words_ = first ? w : Intersect(words_, w);
                            first = false;
                        };
                        if (own != nullptr && own->binding) narrow(own->words);
                        if (pending != nullptr && pending->binding) narrow(pending->words);
                        if (cond.hasRequire) narrow(cond.require);
                        words_ = Minus(words_, cond.exclude);
                        // a reserved keyword lexes as its own token type, never as an Identifier
                        words_.erase(std::remove_if(words_.begin(), words_.end(),
                                                    [](const std::string& w) { return IsReservedKeyword(w); }),
                                     words_.end());
                        if (!cond.deferred.empty())
                            words_.erase(std::remove_if(words_.begin(), words_.end(),
                                                        [&](const std::string& w) {
                                                            return !DeferredHold(cond, static_cast<uint32_t>(type), w);
                                                        }),
                                         words_.end());
                        if (!cond.excluded.empty())
                            words_.erase(std::remove_if(words_.begin(), words_.end(),
                                                        [&](const std::string& w) {
                                                            return Excluded(cond, cand.tokenType, &w);
                                                        }),
                                         words_.end());
                        if (words_.empty()) continue;
                        cand.words = &words_;
                    } else {
                        if (!cond.excluded.empty() && Excluded(cond, cand.tokenType, nullptr)) continue;
                        if (own != nullptr || pending != nullptr) {
                            hints_ = own != nullptr && pending != nullptr ? Union(own->words, pending->words)
                                                                          : (own != nullptr ? own->words : pending->words);
                            cand.hints = &hints_;
                        }
                    }
                } else if (const char* text = KeywordText(static_cast<uint32_t>(cand.tokenType))) {
                    // a reserved keyword: checked by text (NextTokenMatches works on any token), and
                    // by a pending check when it becomes the checked identifier
                    std::string upper(text);
                    for (char& ch : upper)
                        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
                    if (cond.hasRequire && !std::binary_search(cond.require.begin(), cond.require.end(), upper)) continue;
                    if (std::binary_search(cond.exclude.begin(), cond.exclude.end(), upper)) continue;
                    if (KeywordFailsPending(c.state, cond, upper)) continue;
                    if (!cond.deferred.empty() && !DeferredHold(cond, static_cast<uint32_t>(type), text)) continue;
                    if (!cond.excluded.empty() && Excluded(cond, cand.tokenType, &upper)) continue;
                } else if (cond.hasRequire || (!cond.excluded.empty() && Excluded(cond, cand.tokenType, nullptr))) {
                    continue;
                }
                ++stats_.emitted;
                emit_(cand);
            }
        }
    }

    const Grammar& g_;
    const WalkInput& in_;
    const std::function<void(const WalkCandidate&)>& emit_;
    std::vector<Config> work_;
    std::unordered_set<Config, ConfigHash, ConfigEq> visited_;
    std::map<std::pair<int, const Frame*>, const Frame*> frames_;
    std::deque<Frame> frameStore_;
    std::vector<Cond> conds_;
    std::vector<size_t> rules_;
    Words words_, hints_;
    WalkStats stats_;
    std::map<std::tuple<size_t, size_t, bool>, int> opaque_;   // (predicate, token index, live) -> real value
    // (predicate, token index, live, probe type, probe text) -> real value
    std::map<std::tuple<size_t, size_t, bool, uint32_t, std::string>, int> probed_;
    std::map<std::tuple<size_t, size_t, const Frame*>, uint32_t> sources_;   // (state, token index, frame) -> source
    std::vector<uint64_t> sourceKeys_;                                         // by source - 1
    // (decision state, token index, live) -> ANTLR 2's tests there (stable: Excluding's keys point to them)
    std::map<std::tuple<size_t, size_t, bool>, Antlr2Prefix> prefixes_;
    std::unordered_map<size_t, parser::Antlr2Tests> tests_;   // by decision
    // (condition, prefix, number of its exclusions) -> the condition with them
    std::map<std::tuple<uint32_t, const Antlr2Prefix*, size_t>, uint32_t> excluding_;
    std::vector<std::vector<PendingTest>> pendingTests_;   // Config::tests - 1 -> its decisions
    std::map<std::pair<size_t, size_t>, size_t> mayStart_;   // (decision state, token index) -> FirstThatMayStart
    const size_t lastStatementRule_;
};

}  // namespace

PredicateValue EvaluatePredicate(const Grammar& grammar, const std::string& expr, const WalkInput& in, size_t at) {
    return PredicateEvaluator(grammar, expr, in, at).Parse();
}

WalkStats Walk(const Grammar& grammar, const WalkInput& in, const std::function<void(const WalkCandidate&)>& emit) {
    if (in.startState < 0 || static_cast<size_t>(in.startState) >= grammar.atn->states.size()) return {};
    return WalkerImpl(grammar, in, emit).Run();
}

}  // namespace tsql::editor::detail
