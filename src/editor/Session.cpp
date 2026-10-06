#include "Session.h"

#include <algorithm>
#include <exception>

#include "tsql/ast/generated/token_types.hpp"

namespace tsql::editor::detail {

namespace {
using TK = ast::TSqlTokenType;
}  // namespace

// =========================================================================================== stream

std::unique_ptr<antlr4::Token> SessionTokenSource::nextToken() { return session_.NextToken(); }

antlr4::Token* CaptureStream::LT(ssize_t k) {
    antlr4::Token* t = antlr4::CommonTokenStream::LT(k);
    if (t == nullptr || k <= 0) return t;
    session_.Looked(t);
    if (t->getType() != antlr4::Token::EOF) return t;
    session_.LookedAtEof();
    if (armed == nullptr) return t;
    if (session_.TrialRun()) {
        if (!session_.InOpaquePredicate() && get(Position())->getType() == antlr4::Token::EOF) {
            armed = nullptr;
            session_.TrialEndReached();
            throw CaptureDone{};
        }
        // a predicate's outcome there is that of a text that ends there; a prediction's reads (LT(1)
        // as it consumes; ANTLR 2's tests: LT(2), a syntactic predicate's match check) decide its
        // choice by the end
        if (session_.InPredicate()) session_.TrialPredicateAtEnd();
        else if (depth_ > 0) predictionAtEnd_ = true;
        return t;
    }
    if (session_.InDecisionSpeculation()) {
        // its decision captures once it is done (SessionParser::Antlr2SynPred)
        session_.SpeculationReachedCaret();
    } else if (session_.InOpaquePredicate()) {
        session_.EarlyCapture();
    } else if (k == 1 || depth_ > 0 || session_.ModeledPredicate()) {
        // LT(k > 1) outside a prediction and a predicate the walk evaluates is a peek of an action:
        // the parser is not at the caret yet
        CaptureHere();
    }
    return t;
}

void CaptureStream::CaptureHere() {
    antlr4::Parser* p = armed;
    armed = nullptr;
    session_.CaretReached(*p, Position());
    throw CaptureDone{};
}

ssize_t CaptureStream::mark() {
    if (depth_++ == 0) {
        markIndex_ = index();
        predictionAtEnd_ = false;
        session_.OnDecision();
    }
    return antlr4::CommonTokenStream::mark();
}

void CaptureStream::release(ssize_t marker) {
    // (a prediction releases its mark while an exception unwinds it: no alternative fitted)
    if (depth_ > 0 && --depth_ == 0 && predictionAtEnd_ && std::uncaught_exceptions() > 0 && session_.TrialRun() &&
        !session_.InPredicate())
        session_.TrialFailureAtEnd();
    antlr4::CommonTokenStream::release(marker);
}

// ===================================================================================== simulators

SessionSimulator::SessionSimulator(antlr4::Parser* parser, antlr4::atn::ParserATNSimulator& base)
    : parser::TSqlParserATNSimulator(parser, base.atn, base.decisionToDFA, base.getSharedContextCache()) {
    setPredictionMode(base.getPredictionMode());
}

size_t SessionSimulator::adaptivePredict(antlr4::TokenStream* input, size_t decision,
                                         antlr4::ParserRuleContext* outerContext) {
    struct Release {
        antlr4::TokenStream* input;
        ssize_t marker;
        ~Release() { input->release(marker); }
    } release{input, input->mark()};
    return parser::TSqlParserATNSimulator::adaptivePredict(input, decision, outerContext);
}

TrialSimulator::TrialSimulator(antlr4::Parser* parser, antlr4::atn::ParserATNSimulator& base,
                               CaptureStream& stream, ParseSession& session)
    : SessionSimulator(parser, base), stream_(stream), session_(session) {}

size_t TrialSimulator::adaptivePredict(antlr4::TokenStream* input, size_t decision,
                                       antlr4::ParserRuleContext* outerContext) {
    const size_t alt = SessionSimulator::adaptivePredict(input, decision, outerContext);
    if (!stream_.Predicting() && !session_.InPredicate() && stream_.PredictionLookedAtEnd()) {
        if (!session_.TrialChecksChoices()) session_.TrialChoiceUnchecked();
        else if (!DecidedBeforeEnd(input, decision, outerContext, alt)) session_.TrialChoiceAtEnd();
    }
    return alt;
}

bool TrialSimulator::DecidedBeforeEnd(antlr4::TokenStream* input, size_t decision,
                                      antlr4::ParserRuleContext* outerContext, size_t alt) {
    using antlr4::atn::ATN;
    using antlr4::atn::ATNConfigSet;
    // the per-prediction fields full-context closure reads (predicates are evaluated on the fly)
    auto* savedInput = _input;
    const size_t savedStart = _startIndex;
    auto* savedOuter = _outerContext;
    auto* savedDfa = _dfa;
    const size_t start = input->index();
    const ssize_t marker = input->mark();
    struct Restore {
        TrialSimulator* s;
        antlr4::TokenStream* input;
        size_t start;
        ssize_t marker;
        antlr4::TokenStream* i;
        size_t st;
        antlr4::ParserRuleContext* o;
        antlr4::dfa::DFA* d;
        ~Restore() {
            input->seek(start);
            input->release(marker);
            s->_input = i;
            s->_startIndex = st;
            s->_outerContext = o;
            s->_dfa = d;
        }
    } restore{this, input, start, marker, savedInput, savedStart, savedOuter, savedDfa};
    antlr4::dfa::DFA& dfa = decisionToDFA[decision];
    _input = input;
    _startIndex = start;
    _outerContext = outerContext;
    _dfa = &dfa;
    try {
        std::unique_ptr<ATNConfigSet> previous = computeStartState(dfa.atnStartState, outerContext, true);
        for (size_t t = input->LA(1); t != antlr4::Token::EOF; t = input->LA(1)) {
            std::unique_ptr<ATNConfigSet> reach = computeReachSet(previous.get(), t, true);
            if (reach == nullptr) break;   // (full context rejects what the parse takes: no verdict)
            const size_t unique = getUniqueAlt(reach.get());
            if (unique != ATN::INVALID_ALT_NUMBER) return unique == alt;
            previous = std::move(reach);
            input->consume();
        }
    } catch (const CaptureDone&) {
        throw;
    } catch (...) {
        // a predicate failed to evaluate: undecided
    }
    return false;
}

void CaptureStream::Probe(size_t type, std::string text) {
    std::unique_ptr<antlr4::Token> eof = std::move(_tokens.back());
    auto t = std::make_unique<SessionToken>(type, std::move(text), eof->getTokenIndex());
    t->setStartIndex(eof->getStartIndex());
    t->setStopIndex(eof->getStopIndex());
    t->setLine(eof->getLine());
    t->setCharPositionInLine(eof->getCharPositionInLine());
    _tokens.back() = std::move(t);
    _tokens.push_back(std::move(eof));
}

void CaptureStream::Unprobe() {
    std::unique_ptr<antlr4::Token> eof = std::move(_tokens.back());
    _tokens.pop_back();
    _tokens.back() = std::move(eof);
    seek(std::min(index(), _tokens.size() - 1));
}

// ========================================================================================== session

ParseSession::ParseSession(const Grammar& grammar, const TokenSourceView& view, size_t begin, size_t limit)
    : grammar_(grammar), view_(view), begin_(begin), limit_(std::max(begin, limit)), next_(begin),
      factory_(std::make_unique<ast::FragmentFactory>()) {
    factory_->SetTokenStream(&script_);
    source_ = std::make_unique<SessionTokenSource>(*this);
    stream_ = std::make_unique<CaptureStream>(source_.get(), *this);
}

void ParseSession::PrepareCaretTokens() {
    for (size_t i = begin_; i < limit_; ++i) {
        const LexToken& t = (*view_.tokens)[i];
        if (IsHiddenType(t.type)) continue;
        out_.tokens.push_back(t);
        std::string upper(view_.text.substr(t.start, t.end - t.start));
        for (char& c : upper)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out_.upper.push_back(std::move(upper));
    }
}

ParseSession::~ParseSession() = default;

void ParseSession::Attach(parser::TSql80ParserBase& parser, SessionParserHooks& hooks) {
    hooks_ = &hooks;
    parser.InitializeForNewInput(&script_, &full_, &errors_, factory_.get(), true);
}

std::string ParseSession::TokenText(size_t i) const {
    const LexToken& t = (*view_.tokens)[i];
    std::string_view text = view_.text.substr(t.start, t.end - t.start);
    std::string sanitized;
    if (view_.invalidUtf8 && SanitizeUtf8(text, sanitized)) return sanitized;
    return std::string(text);
}

bool ParseSession::Available() {
    if (next_ < limit_) return true;
    // a recording run reads to the end of the text, which its Buffer lexes on demand
    while (observer_ != nullptr && limit_ == view_.tokens->size() && observer_->MoreTokens()) {
        limit_ = view_.tokens->size();
        if (next_ < limit_) return true;
    }
    return false;
}

std::unique_ptr<antlr4::Token> ParseSession::NextToken() {
    const std::vector<LexToken>& toks = *view_.tokens;
    const bool quoted = hooks_ == nullptr || hooks_->QuotedIdentifier();
    auto add = [&](SessionToken& t, size_t i, bool hidden) {
        const LexToken& lt = toks[i];
        const bool dual = (*view_.dualQuoted)[i] != 0;
        ast::TSqlParserToken pt;
        pt.TokenType = dual ? TK::AsciiStringOrQuotedIdentifier : static_cast<TK>(lt.type);
        // byte offsets stand in for SqlScriptDOM's UTF-16 offsets and lines/columns: the parser
        // only compares positions and reports them in errors
        pt.Offset = static_cast<int>(lt.start);
        pt.Line = 1;
        pt.Column = static_cast<int>(lt.start) + 1;
        pt.Text = t.getText();
        pt.ConvertStringToIdentifier = dual && quoted;
        t.setChannel(hidden ? antlr4::Token::HIDDEN_CHANNEL : antlr4::Token::DEFAULT_CHANNEL);
        t.setLine(1);
        t.setCharPositionInLine(lt.start + 1);
        t.setStartIndex(lt.start);
        t.setStopIndex(lt.end == 0 ? 0 : lt.end - 1);
        script_.push_back(std::move(pt));
        full_.push_back(&t);
        fullToVisible_.push_back(static_cast<uint32_t>(visibleStart_.size()));
    };
    while (Available() && IsHiddenType(toks[next_].type)) {
        auto t = std::make_unique<SessionToken>(toks[next_].type, TokenText(next_), full_.size());
        add(*t, next_, true);
        hidden_.push_back(std::move(t));
        ++next_;
    }
    if (next_ >= limit_) {
        const size_t offset = limit_ < toks.size() ? toks[limit_].start : view_.text.size();
        auto t = std::make_unique<SessionToken>(antlr4::Token::EOF, "<EOF>", full_.size());
        ast::TSqlParserToken pt;
        pt.TokenType = TK::EndOfFile;
        pt.Offset = static_cast<int>(offset);
        pt.Column = static_cast<int>(offset) + 1;
        t->setLine(1);
        t->setCharPositionInLine(offset + 1);
        t->setStartIndex(offset);
        t->setStopIndex(offset == 0 ? 0 : offset - 1);
        script_.push_back(std::move(pt));
        full_.push_back(t.get());
        fullToVisible_.push_back(static_cast<uint32_t>(visibleStart_.size()));
        visibleStart_.push_back(static_cast<uint32_t>(offset));
        eofCreated_ = true;
        return t;
    }
    const bool dual = (*view_.dualQuoted)[next_] != 0;
    const size_t type = dual ? static_cast<size_t>(quoted ? TK::QuotedIdentifier : TK::AsciiStringLiteral)
                             : toks[next_].type;
    auto t = std::make_unique<SessionToken>(type, TokenText(next_), full_.size());
    const uint32_t start = toks[next_].start;
    add(*t, next_, false);
    visibleStart_.push_back(start);
    ++next_;
    return t;
}

size_t ParseSession::BufferIndex(const antlr4::Token* t) const {
    return t == nullptr ? begin_ + full_.size() : begin_ + t->getTokenIndex();
}

size_t ParseSession::VisibleIndex(const antlr4::Token* t) const {
    if (t == nullptr || t->getType() == antlr4::Token::EOF) return out_.tokens.size();
    return fullToVisible_[std::min(t->getTokenIndex(), fullToVisible_.size() - 1)];
}

size_t ParseSession::ErrorIndex(const ParseError& e) const {
    return static_cast<size_t>(std::lower_bound(visibleStart_.begin(), visibleStart_.end(),
                                                static_cast<uint32_t>(std::max(e.Offset, 0))) -
                               visibleStart_.begin());
}

void ParseSession::Looked(const antlr4::Token* t) { lookEnd_ = std::max(lookEnd_, t->getTokenIndex() + 1); }

// ------------------------------------------------------------------------------------ recording

void ParseSession::OnConsume(antlr4::Parser& parser) {
    if (observer_ == nullptr) return;
    const antlr4::Token* t = stream_->Current();
    if (t == nullptr || t->getType() == antlr4::Token::EOF) return;
    TokenRole role;
    role.state = static_cast<int32_t>(parser.getState());
    antlr4::RuleContext* c = parser.getContext();
    for (size_t k = 0; k < 3 && c != nullptr && !c->isEmpty(); ++k, c = static_cast<antlr4::RuleContext*>(c->parent)) {
        role.rules[k] = static_cast<uint32_t>(c->getRuleIndex());
        role.callStates[k] = static_cast<int32_t>(c->invokingState);
    }
    observer_->OnRole(BufferIndex(t), role);
}

void ParseSession::OnPredicate(antlr4::Parser& parser, size_t predIndex, bool result) {
    if (observer_ == nullptr || !result) return;
    auto it = grammar_.predicates.find(predIndex);
    if (it == grammar_.predicates.end()) return;
    const antlr4::Token* t = parser.getTokenStream()->LT(1);
    if (t == nullptr || t->getType() == antlr4::Token::EOF) return;
    // positive NextTokenMatches(word) terms of the expression (m1:WORD; not after '!')
    const std::string& e = it->second;
    const size_t b = BufferIndex(t);
    const LexToken& lt = (*view_.tokens)[b];
    std::string upper(view_.text.substr(lt.start, lt.end - lt.start));
    for (char& c : upper)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (size_t p = e.find("m1:"); p != std::string::npos; p = e.find("m1:", p + 3)) {
        if (p > 0 && e[p - 1] == '!') continue;
        const size_t end = e.find(';', p);
        if (e.compare(p + 3, end - p - 3, upper) == 0) observer_->OnPredicateKeyword(b);
    }
}

bool ParseSession::OnRuleEnter(antlr4::ParserRuleContext* ctx, size_t rule, antlr4::ParserRuleContext* parent,
                               bool quotedIdentifier) {
    if (observer_ == nullptr || parent == nullptr) return true;
    const TopLevelStates& top = grammar_.top;
    ResumePoint at;
    size_t lookEnd = lookEnd_;
    if (rule == top.statementOptSemiRule && parent->getRuleIndex() == top.batchRule) {
        // a statement of a batch's statement loop: the loop's decision at this token is replayed
        // on resumption, so what it looked at is not part of the parse up to here
        at.kind = ResumePoint::Kind::InBatch;
        at.firstBatch = parent->invokingState == top.scriptFirstBatchCall;
        lookEnd = lookEndAtDecision_;
    } else if (rule == top.batchRule && ctx->invokingState == top.scriptLoopBatchCall) {
        at.kind = ResumePoint::Kind::BatchStart;
    } else {
        return true;
    }
    at.quotedIdentifier = quotedIdentifier;
    at.token = static_cast<uint32_t>(BufferIndex(stream_->Current()));
    return observer_->OnCheckpoint(at, begin_ + lookEnd);
}

void ParseSession::OnEnd() {
    if (observer_ != nullptr) observer_->OnEnd(BufferIndex(stream_->Current()), begin_ + lookEnd_);
}

// ------------------------------------------------------------------------------------------- trial

void ParseSession::TrialEndReached() {
    trial_.reached = true;
    trial_.errors = !errors_.empty();
}

void ParseSession::TrialChoiceAtEnd() {
    if (errors_.empty()) trial_.endChoice = true;
}

void ParseSession::TrialChoiceUnchecked() {
    if (errors_.empty()) trial_.unchecked = true;
}

void ParseSession::TrialFailureAtEnd() {
    if (errors_.empty()) trial_.endFailed = true;
}

std::optional<Trial> ParseSession::TrialOutcome() const {
    if (trial_.reached && !trial_.errors) return Trial::Parses;
    // the parse went wrong where the text ends, or after a choice the end decided: with more text
    // it may have gone on
    if (trial_.endFailed || trial_.endChoice) return Trial::Prefix;
    // an error after choices that looked at the end: their checks decide
    if (trial_.unchecked) return std::nullopt;
    // (`t (HOLDLOCK`: IsTableReference wants the `)` after it)
    return trial_.doubt ? Trial::Undecided : Trial::Fails;
}

// ---------------------------------------------------------------------------------------- capture

void ParseSession::FollowChain(antlr4::RuleContext* ctx, std::vector<int>& out) const {
    out.clear();
    for (antlr4::RuleContext* c = ctx; c != nullptr && !c->isEmpty(); c = static_cast<antlr4::RuleContext*>(c->parent)) {
        const antlr4::atn::ATNState* invoking = grammar_.atn->states[c->invokingState];
        if (invoking->transitions.empty() ||
            invoking->transitions[0]->getTransitionType() != antlr4::atn::TransitionType::RULE)
            break;   // not entered by a rule call of the ATN (a predicate's speculative parse)
        const auto* rt = static_cast<const antlr4::atn::RuleTransition*>(invoking->transitions[0].get());
        out.push_back(static_cast<int>(rt->followState->stateNumber));
    }
}

void ParseSession::CaretReached(antlr4::Parser& parser, size_t index) {
    out_.syntaxErrors = !errors_.empty();
    for (const ParseError& e : errors_) out_.firstError = std::min(out_.firstError, ErrorIndex(e));
    Record(static_cast<int>(parser.getState()), parser.getContext(), index, out_.capture);
}

void ParseSession::EarlyCapture() {
    if (out_.early.captured || opaqueStart_.ctx == nullptr) return;
    Record(opaqueStart_.state, opaqueStart_.ctx, opaqueStart_.index, out_.early);
}

void ParseSession::Record(int state, antlr4::ParserRuleContext* ctx, size_t index, Capture& c) {
    c.captured = true;
    c.state = state;
    c.index = index;
    c.context = ctx;
    FollowChain(ctx, c.follow);
    // the innermost rule call with a keyword check (editor_meta.py) whose rule has consumed no token
    // yet: its check applies to the token at the caret (the walk starts past that call)
    c.pendingCall = -1;
    for (antlr4::RuleContext* r = ctx; r != nullptr && !r->isEmpty(); r = static_cast<antlr4::RuleContext*>(r->parent)) {
        const auto* pr = static_cast<antlr4::ParserRuleContext*>(r);
        // (start is unset while enterRule's LT(1) is what reaches the caret)
        if (pr->start != nullptr && VisibleIndex(pr->start) < index) break;
        if (grammar_.keywordStates.count(static_cast<int>(r->invokingState)) != 0) {
            c.pendingCall = static_cast<int>(r->invokingState);
            break;
        }
    }

    for (antlr4::RuleContext* r = ctx; r != nullptr; r = static_cast<antlr4::RuleContext*>(r->parent)) {
        const size_t rule = r->getRuleIndex();
        if (std::find(grammar_.statementRules.begin(), grammar_.statementRules.end(), rule) ==
            grammar_.statementRules.end())
            continue;
        auto* pr = static_cast<antlr4::ParserRuleContext*>(r);
        c.statementState = static_cast<int>(grammar_.atn->ruleToStartState[rule]->stateNumber);
        c.statementIndex = std::min(VisibleIndex(pr->start), index);
        FollowChain(r, c.statementFollow);
        const uint32_t startOffset = visibleStart_[std::min(c.statementIndex, visibleStart_.size() - 1)];
        for (const ParseError& e : errors_) {
            if (static_cast<uint32_t>(std::max(e.Offset, 0)) < startOffset) continue;
            const size_t at = ErrorIndex(e);
            if (!c.errorInStatement || at < c.errorIndex) c.errorIndex = at;
            c.errorInStatement = true;
        }
        break;
    }
}

bool ParseSession::ModeledPredicate() const {
    return !predicates_.empty() && !grammar_.OpaquePredicate(predicates_.back());
}

void ParseSession::EnterPredicate(antlr4::Parser& parser, size_t predIndex) {
    predicates_.push_back(predIndex);
    if (!grammar_.OpaquePredicate(predIndex)) return;
    if (opaqueDepth_++ == 0) {
        opaqueStart_.state = static_cast<int>(parser.getState());
        opaqueStart_.ctx = parser.getContext();
        opaqueStart_.index = stream_->Position();
    }
}

void ParseSession::LeavePredicate(size_t predIndex) {
    if (!predicates_.empty()) predicates_.pop_back();
    if (grammar_.OpaquePredicate(predIndex) && --opaqueDepth_ == 0) opaqueStart_.ctx = nullptr;
}

}  // namespace tsql::editor::detail
