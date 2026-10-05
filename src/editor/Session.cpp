#include "Session.h"

#include <algorithm>

namespace tsql::editor::detail {

namespace {

/// Thrown by CaptureStream once the capture is recorded: nothing after it is needed.
struct CaptureDone {};

size_t ByteOf(const std::vector<uint32_t>& bytes, size_t codePoint) {
    return bytes.empty() ? codePoint : bytes[std::min(codePoint, bytes.size() - 1)];
}

}  // namespace

antlr4::Token* CaptureStream::LT(ssize_t k) {
    antlr4::Token* t = antlr4::CommonTokenStream::LT(k);
    if (armed == nullptr || k <= 0 || t == nullptr || t->getType() != antlr4::Token::EOF) return t;
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
    if (depth_++ == 0) markIndex_ = index();
    return antlr4::CommonTokenStream::mark();
}

void CaptureStream::release(ssize_t marker) {
    if (depth_ > 0) --depth_;
    antlr4::CommonTokenStream::release(marker);
}

ParseSession::ParseSession(const Grammar& grammar, std::string_view text)
    : grammar_(grammar), text_(text), decoded_(tsql::detail::Decode(text)), bytes_(CodePointByteOffsets(text)) {
    input_ = std::make_unique<antlr4::ANTLRInputStream>(decoded_.utf8);
    result_.tokens = std::make_unique<ast::ScriptTokenStream>();
    result_.factory = std::make_unique<ast::FragmentFactory>();
}

void ParseSession::Tokenize(parser::TSqlLexerBase& lexer) {
    lexer.removeErrorListeners();
    lexer.SetUtf16Offsets(&decoded_.utf16);
    lexerStream_ = std::make_unique<antlr4::CommonTokenStream>(&lexer);
    lexerStream_->fill();
    tsql::detail::BuildScriptTokens(*lexerStream_, decoded_, true, result_);
    all_ = lexerStream_->getTokens();

    fullToVisible_.assign(all_.size() + 1, 0);
    for (size_t i = 0; i < all_.size(); ++i) {
        const antlr4::Token* t = all_[i];
        fullToVisible_[i] = out_.tokens.size();
        if (t->getChannel() != antlr4::Token::DEFAULT_CHANNEL || t->getType() == antlr4::Token::EOF) continue;
        const size_t start = ByteOf(bytes_, t->getStartIndex());
        const size_t end = ByteOf(bytes_, t->getStopIndex() + 1);
        out_.tokens.push_back({static_cast<uint32_t>(t->getType()), static_cast<uint32_t>(start), static_cast<uint32_t>(end)});
        std::string upper(text_.substr(start, end - start));
        for (char& c : upper)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out_.upper.push_back(std::move(upper));
        visibleOffset_.push_back((*result_.tokens)[i].Offset);
    }
    fullToVisible_[all_.size()] = out_.tokens.size();
    visibleOffset_.push_back(decoded_.utf16.empty() ? 0 : decoded_.utf16.back());

    source_ = std::make_unique<antlr4::ListTokenSource>(tsql::detail::VisibleTokens(all_));
    parserStream_ = std::make_unique<CaptureStream>(source_.get(), *this);
    parserStream_->fill();
}

void ParseSession::Run(parser::TSql80ParserBase& parser, const std::function<void()>& script, bool capture) {
    parser.InitializeForNewInput(result_.tokens.get(), &all_, &result_.errors, result_.factory.get(), true);
    tsql::detail::TokensGuard guard(result_.tokens.get());
    if (!capture) roles_.assign(all_.size(), TokenRole{});
    parserStream_->armed = capture ? &parser : nullptr;
    try {
        script();
    } catch (const CaptureDone&) {
    } catch (...) {
        // an error escaped every recovering rule: no capture, the rest of the tokens untaken
    }
    parserStream_->armed = nullptr;
}

size_t ParseSession::VisibleIndex(const antlr4::Token* token) const {
    if (token == nullptr || token->getType() == antlr4::Token::EOF) return out_.tokens.size();
    return fullToVisible_[std::min(token->getTokenIndex(), fullToVisible_.size() - 1)];
}

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
    out_.syntaxErrors = !result_.errors.empty();
    for (const ParseError& e : result_.errors)
        out_.firstError = std::min(out_.firstError, static_cast<size_t>(std::lower_bound(visibleOffset_.begin(),
                                                                                         visibleOffset_.end(), e.Offset) -
                                                                        visibleOffset_.begin()));
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
    FollowChain(ctx, c.follow);

    for (antlr4::RuleContext* r = ctx; r != nullptr; r = static_cast<antlr4::RuleContext*>(r->parent)) {
        const size_t rule = r->getRuleIndex();
        if (std::find(grammar_.statementRules.begin(), grammar_.statementRules.end(), rule) ==
            grammar_.statementRules.end())
            continue;
        auto* pr = static_cast<antlr4::ParserRuleContext*>(r);
        c.statementState = static_cast<int>(grammar_.atn->ruleToStartState[rule]->stateNumber);
        c.statementIndex = std::min(VisibleIndex(pr->start), index);
        FollowChain(r, c.statementFollow);
        const int startOffset = visibleOffset_[std::min(c.statementIndex, visibleOffset_.size() - 1)];
        for (const ParseError& e : result_.errors) {
            if (e.Offset < startOffset) continue;
            const size_t at = static_cast<size_t>(
                std::lower_bound(visibleOffset_.begin(), visibleOffset_.end(), e.Offset) - visibleOffset_.begin());
            if (!c.errorInStatement || at < c.errorIndex) c.errorIndex = at;
            c.errorInStatement = true;
        }
        break;
    }
}

void ParseSession::OnConsume(antlr4::Parser& parser) {
    const antlr4::Token* t = parser.getCurrentToken();
    if (t == nullptr || t->getType() == antlr4::Token::EOF || t->getTokenIndex() >= roles_.size()) return;
    TokenRole& role = roles_[t->getTokenIndex()];
    role.state = static_cast<int>(parser.getState());
    antlr4::RuleContext* c = parser.getContext();
    for (size_t k = 0; k < 3 && c != nullptr && !c->isEmpty(); ++k, c = static_cast<antlr4::RuleContext*>(c->parent)) {
        role.rules[k] = c->getRuleIndex();
        role.callStates[k] = static_cast<int>(c->invokingState);
    }
}

bool ParseSession::Opaque(size_t predIndex) const {
    auto it = grammar_.predicates.find(predIndex);
    return it == grammar_.predicates.end() || it->second.find('?') != std::string::npos;
}

bool ParseSession::ModeledPredicate() const { return !predicates_.empty() && !Opaque(predicates_.back()); }

void ParseSession::EnterPredicate(antlr4::Parser& parser, size_t predIndex) {
    predicates_.push_back(predIndex);
    if (!Opaque(predIndex)) return;
    if (opaqueDepth_++ == 0) {
        opaqueStart_.state = static_cast<int>(parser.getState());
        opaqueStart_.ctx = parser.getContext();
        opaqueStart_.index = parserStream_->Position();
    }
}

void ParseSession::LeavePredicate(size_t predIndex) {
    if (!predicates_.empty()) predicates_.pop_back();
    if (Opaque(predIndex) && --opaqueDepth_ == 0) opaqueStart_.ctx = nullptr;
}

void ParseSession::OnPredicate(antlr4::Parser& parser, size_t predIndex, bool result) {
    if (!result) return;
    auto it = grammar_.predicates.find(predIndex);
    if (it == grammar_.predicates.end()) return;
    const antlr4::Token* t = parser.getTokenStream()->LT(1);
    if (t == nullptr || t->getType() == antlr4::Token::EOF || t->getTokenIndex() >= roles_.size()) return;
    // positive NextTokenMatches(word) terms of the expression (m1:WORD; not after '!')
    const std::string& e = it->second;
    const size_t vi = VisibleIndex(t);
    if (vi >= out_.upper.size()) return;
    const std::string& upper = out_.upper[vi];
    for (size_t p = e.find("m1:"); p != std::string::npos; p = e.find("m1:", p + 3)) {
        if (p > 0 && e[p - 1] == '!') continue;
        const size_t end = e.find(';', p);
        if (e.compare(p + 3, end - p - 3, upper) == 0) roles_[t->getTokenIndex()].predicateKeyword = true;
    }
}

ParsedTokens ParseSession::FinishRoles() {
    ParsedTokens out;
    out.tokens.reserve(all_.size());
    out.roles.reserve(all_.size());
    for (size_t i = 0; i < all_.size(); ++i) {
        const antlr4::Token* t = all_[i];
        if (t->getType() == antlr4::Token::EOF) continue;
        LexToken lt;
        lt.type = static_cast<uint32_t>(t->getType());
        lt.start = static_cast<uint32_t>(ByteOf(bytes_, t->getStartIndex()));
        lt.end = static_cast<uint32_t>(ByteOf(bytes_, t->getStopIndex() + 1));
        out.tokens.push_back(lt);
        out.roles.push_back(i < roles_.size() ? roles_[i] : TokenRole{});
    }
    return out;
}

void LexInto(antlr4::Lexer& lexer, const std::vector<uint32_t>& bytes, std::vector<LexToken>& out, size_t stopAfter) {
    lexer.removeErrorListeners();
    const auto go = static_cast<uint32_t>(ast::TSqlTokenType::Go);
    for (;;) {
        std::unique_ptr<antlr4::Token> t = lexer.nextToken();
        const size_t type = t->getType();
        if (type == antlr4::Token::EOF) break;
        LexToken lt;
        lt.type = static_cast<uint32_t>(type);
        // TSqlWhitespaceTokenFilter with QUOTED_IDENTIFIER ON (the parser's initial setting)
        if (type == static_cast<size_t>(ast::TSqlTokenType::AsciiStringOrQuotedIdentifier))
            lt.type = static_cast<uint32_t>(ast::TSqlTokenType::QuotedIdentifier);
        lt.start = static_cast<uint32_t>(ByteOf(bytes, t->getStartIndex()));
        lt.end = static_cast<uint32_t>(ByteOf(bytes, t->getStopIndex() + 1));
        out.push_back(lt);
        if (lt.type == go && lt.start >= stopAfter) break;
    }
}

}  // namespace tsql::editor::detail
