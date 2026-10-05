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
    if (session_.InOpaquePredicate()) {
        session_.EarlyCapture();
    } else if (k == 1 || session_.ModeledPredicate()) {
        // LT(k > 1) outside a predicate the walk evaluates is a peek of an action: the parser is
        // not at the caret yet
        antlr4::Parser* p = armed;
        armed = nullptr;
        session_.CaretReached(*p, Position());
        throw CaptureDone{};
    }
    return t;
}

ssize_t CaptureStream::mark() {
    if (depth_++ == 0) {
        markIndex_ = index();
        session_.OnDecision();
    }
    return antlr4::CommonTokenStream::mark();
}

void CaptureStream::release(ssize_t marker) {
    if (depth_ > 0) --depth_;
    antlr4::CommonTokenStream::release(marker);
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
    while (next_ < limit_ && IsHiddenType(toks[next_].type)) {
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
