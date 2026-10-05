#include "Walker.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <map>
#include <tuple>
#include <utility>
#include <unordered_set>

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

/// Constraint on the caret word collected along a path.
struct Cond {
    bool hasRequire = false;
    Words require;                         // the caret word must be one of these (predicates)
    Words exclude;                         // the caret word must not be one of these (predicates)
    const KeywordState* pending = nullptr; // a rule call's action checks the identifier it returns
    /// The rule that call enters: a reserved keyword it matches itself (securityStatementPermission
    /// takes any) becomes the identifier, so the check applies to its text too.
    size_t pendingRule = SIZE_MAX;
    bool operator==(const Cond& o) const {
        return hasRequire == o.hasRequire && require == o.require && exclude == o.exclude && pending == o.pending &&
               pendingRule == o.pendingRule;
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
};

struct ConfigHash {
    size_t operator()(const Config& k) const {
        size_t h = k.state->stateNumber * 0x9E3779B97F4A7C15ull;
        h ^= k.index + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
        h ^= reinterpret_cast<size_t>(k.frame) + (h << 6) + (h >> 2);
        h ^= k.cond + (h << 6) + (h >> 2);
        return h ^ static_cast<size_t>(k.live) ^ (static_cast<size_t>(k.nextStatement) << 1);
    }
};
struct ConfigEq {
    bool operator()(const Config& a, const Config& b) const {
        return a.state == b.state && a.index == b.index && a.frame == b.frame && a.cond == b.cond && a.live == b.live &&
               a.nextStatement == b.nextStatement;
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

    /// The real predicate's value at configuration `c` (1, 0, -1: unknown), memoised.
    int Opaque(size_t rule, size_t pred, const Config& c) {
        if (!in_.evaluate) return -1;
        const auto key = std::make_tuple(pred, c.index, c.live);
        auto it = opaque_.find(key);
        if (it != opaque_.end()) return it->second;
        const int value = in_.evaluate(rule, pred, c.index, c.live);
        opaque_.emplace(key, value);
        return value;
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

    void Step(const Config& c) {
        const ATNState* s = c.state;
        if (s->getStateType() == ATNStateType::RULE_STOP) {
            if (c.frame == nullptr) return;   // end of the outermost rule: EOF (never offered)
            const ATNState* follow = g_.atn->states[static_cast<size_t>(c.frame->follow)];
            // a statement ended (of the batch, or of a block, IF, WHILE, module body, ...): what follows
            // in its caller comes after the caret's statement
            const bool statementEnded = s->ruleIndex == g_.top.statementOptSemiRule || s->ruleIndex == lastStatementRule_;
            work_.push_back({follow, c.index, c.frame->parent, c.cond, false, c.nextStatement || statementEnded});
            return;
        }
        for (const auto& tp : s->transitions) {
            const antlr4::atn::Transition* t = tp.get();
            switch (t->getTransitionType()) {
                case TransitionType::RULE: {
                    const auto* rt = static_cast<const antlr4::atn::RuleTransition*>(t);
                    uint32_t cond = c.cond;
                    auto kw = g_.keywordStates.find(static_cast<int>(s->stateNumber));
                    if (kw != g_.keywordStates.end()) {
                        Cond nc = conds_[cond];
                        nc.pending = &kw->second;
                        nc.pendingRule = rt->target->ruleIndex;
                        cond = Intern(std::move(nc));
                    }
                    work_.push_back({rt->target, c.index, Push(static_cast<int>(rt->followState->stateNumber), c.frame), cond,
                                     false, c.nextStatement});
                    break;
                }
                case TransitionType::PREDICATE: {
                    const auto* pt = static_cast<const antlr4::atn::PredicateTransition*>(t);
                    auto it = g_.predicates.find(pt->getPredIndex());
                    if (it == g_.predicates.end()) {
                        work_.push_back({t->target, c.index, c.frame, c.cond, c.live, c.nextStatement});
                        break;
                    }
                    PredicateValue v = EvaluatePredicate(g_, it->second, in_, c.index);
                    if (v.kind != PredicateValue::False && it->second.find_first_of("?$") != std::string::npos) {
                        // decided by the tokens before the caret: the real predicate says
                        const int real = Opaque(pt->getRuleIndex(), pt->getPredIndex(), c);
                        if (real == 0) v.kind = PredicateValue::False;
                        else if (real == 1) v = PredicateValue{PredicateValue::True, {}};
                    }
                    uint32_t cond = c.cond;
                    if (v.kind == PredicateValue::False) break;
                    if (v.kind == PredicateValue::Require || v.kind == PredicateValue::Exclude) {
                        Cond nc = conds_[cond];
                        if (v.kind == PredicateValue::Require) {
                            nc.require = nc.hasRequire ? Intersect(nc.require, v.words) : v.words;
                            nc.hasRequire = true;
                        } else {
                            nc.exclude = Union(nc.exclude, v.words);
                        }
                        if (nc.hasRequire) {
                            nc.require = Minus(nc.require, nc.exclude);
                            if (nc.require.empty()) break;
                        }
                        cond = Intern(std::move(nc));
                    }
                    work_.push_back({t->target, c.index, c.frame, cond, c.live, c.nextStatement});
                    break;
                }
                case TransitionType::EPSILON:
                case TransitionType::PRECEDENCE:
                    work_.push_back({t->target, c.index, c.frame, c.cond, c.live, c.nextStatement});
                    break;
                case TransitionType::ACTION:
                    work_.push_back({t->target, c.index, c.frame, c.cond, false, c.nextStatement});
                    break;
                case TransitionType::ATOM:
                case TransitionType::RANGE:
                case TransitionType::SET: {
                    if (c.index == in_.caret) {
                        Emit(c, t);
                    } else if (c.index < in_.tokens->size()) {
                        const size_t type = (*in_.tokens)[c.index].type;
                        if (t->matches(type, antlr4::Token::MIN_USER_TOKEN_TYPE, g_.atn->maxTokenType) &&
                            WordsAllow(s, c.cond, c.index))
                            // a token after the statement's end: the path is in a later statement now
                            work_.push_back({t->target, c.index + 1, c.frame, Consumed(c.cond), false, false});
                    }
                    break;
                }
                case TransitionType::NOT_SET:
                case TransitionType::WILDCARD:
                    if (c.index < in_.caret && c.index < in_.tokens->size())
                        work_.push_back({t->target, c.index + 1, c.frame, Consumed(c.cond), false, false});
                    break;
            }
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
        const antlr4::misc::IntervalSet label = t->label();   // by value: keep it alive for the loop
        for (const antlr4::misc::Interval& iv : label.getIntervals()) {
            if (iv.b < static_cast<ssize_t>(antlr4::Token::MIN_USER_TOKEN_TYPE)) continue;   // EOF
            for (ssize_t type = std::max<ssize_t>(iv.a, antlr4::Token::MIN_USER_TOKEN_TYPE); type <= iv.b; ++type) {
                WalkCandidate cand;
                cand.tokenType = static_cast<size_t>(type);
                cand.rules = &rules_;
                cand.nextStatement = c.nextStatement;
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
                        if (words_.empty()) continue;
                        cand.words = &words_;
                    } else if (own != nullptr || pending != nullptr) {
                        hints_ = own != nullptr && pending != nullptr ? Union(own->words, pending->words)
                                                                      : (own != nullptr ? own->words : pending->words);
                        cand.hints = &hints_;
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
                } else if (cond.hasRequire) {
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
