// Editor support, internal: the grammar-independent half of Grammar::ParseFrom/ParseToCaret
// (GrammarImpl.h supplies the generated lexer and parser). A ParseSession hands a Buffer's tokens
// to the parser lazily, from the token where the parse resumes, like tsql::detail::ParseWith
// prepares them (the parser sees default-channel tokens that keep their full-stream indexes; the
// script token stream and the full token list hold every token), with one difference: lexer errors
// do not stop the parse. Token indexes inside a session are relative to its first token.
//
// ParseFrom records how the parser takes each token and every point where the parse could be
// resumed (ResumePoint); ParseToCaret records the parser configuration the first time the parser
// looks at the caret (the end of its tokens) and then stops the parse.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "Grammar.h"
#include "ParseDriver.h"
#include "antlr4-runtime.h"

namespace tsql::editor::detail {

class ParseSession;

/// A token whose index is fixed (the parser stream would renumber it) and set by the session.
class SessionToken final : public antlr4::CommonToken {
public:
    SessionToken(size_t type, std::string text, size_t index) : antlr4::CommonToken(type, std::move(text)) {
        _index = index;
    }
    void setTokenIndex(size_t) override {}
};

/// Produces the session's parser-visible tokens on demand.
class SessionTokenSource final : public antlr4::TokenSource {
public:
    explicit SessionTokenSource(ParseSession& session) : session_(session) {}
    std::unique_ptr<antlr4::Token> nextToken() override;
    size_t getLine() const override { return 1; }
    size_t getCharPositionInLine() override { return 0; }
    antlr4::CharStream* getInputStream() override { return nullptr; }
    std::string getSourceName() override { return "tsql::editor"; }
    antlr4::TokenFactory<antlr4::CommonToken>* getTokenFactory() override { return nullptr; }

private:
    ParseSession& session_;
};

/// The parser's token stream (parser-visible tokens only, so stream index + k - 1 is LT(k)'s
/// index). Tracks how far the parser looked; when armed, the first LT/LA that returns EOF (the
/// caret) records the parser's state, rule context chain and position (inside adaptivePredict,
/// between mark and release, the position where the prediction started) and throws CaptureDone.
class CaptureStream final : public antlr4::CommonTokenStream {
public:
    CaptureStream(antlr4::TokenSource* source, ParseSession& session)
        : antlr4::CommonTokenStream(source), session_(session) {}

    antlr4::Parser* armed = nullptr;

    antlr4::Token* LT(ssize_t k) override;
    ssize_t mark() override;
    void release(ssize_t marker) override;
    /// Where the parser stands: inside adaptivePredict, where the prediction started.
    size_t Position() { return depth_ > 0 ? markIndex_ : index(); }
    /// The token the parser stands at, without counting as a look at it.
    antlr4::Token* Current() { return antlr4::CommonTokenStream::LT(1); }

private:
    ParseSession& session_;
    int depth_ = 0;
    size_t markIndex_ = 0;
};

/// Thrown to end a parse: the capture is recorded (CaptureStream) / the observer stopped it.
struct CaptureDone {};
struct StopParse {};

/// What GrammarImpl's parser exposes to the session.
class SessionParserHooks {
public:
    virtual ~SessionParserHooks() = default;
    virtual bool QuotedIdentifier() const = 0;
};

class ParseSession {
public:
    /// Parses `view`'s tokens from token `begin` (a parser-visible token or the end) up to token
    /// `limit` (exclusive: the parser sees EOF there).
    ParseSession(const Grammar& grammar, const TokenSourceView& view, size_t begin, size_t limit);
    ~ParseSession();

    const Grammar& grammar() const { return grammar_; }
    antlr4::TokenStream& ParserStream() { return *stream_; }
    CaptureStream& Stream() { return *stream_; }
    /// Sets up `parser` (built on ParserStream()) for this session's tokens.
    void Attach(parser::TSql80ParserBase& parser, SessionParserHooks& hooks);
    std::vector<ParseError>& Errors() { return errors_; }
    const ast::ScriptTokenStream& Script() const { return script_; }
    /// Caret runs: the parser tokens before the caret (CaretParse::tokens and upper), for the walk.
    void PrepareCaretTokens();

    // ---- the token source
    std::unique_ptr<antlr4::Token> NextToken();

    // ---- recording runs (ParseFrom)
    void SetObserver(ParseObserver* observer) { observer_ = observer; }
    bool Recording() const { return observer_ != nullptr; }
    /// The parser before it consumes a token / after it evaluated a predicate.
    void OnConsume(antlr4::Parser& parser);
    void OnPredicate(antlr4::Parser& parser, size_t predIndex, bool result);
    /// The parser enters rule `rule` with context `ctx` (`parent`: the context it is called from).
    /// Returns false when the observer stops the parse there.
    bool OnRuleEnter(antlr4::ParserRuleContext* ctx, size_t rule, antlr4::ParserRuleContext* parent,
                     bool quotedIdentifier);
    /// CaptureStream: a prediction starts (outside any other).
    void OnDecision() { lookEndAtDecision_ = lookEnd_; }
    /// The parse ended (normally or by an escaped error) at the parser's position.
    void OnEnd();
    /// Full-stream index (into the buffer) of token `t` of this session.
    size_t BufferIndex(const antlr4::Token* t) const;

    // ---- caret runs (ParseToCaret)
    /// CaptureStream: the parser looks at the caret for the first time (outside opaque predicates).
    void CaretReached(antlr4::Parser& parser, size_t index);
    /// CaptureStream: an opaque predicate (a hand-written look-ahead scan, a syntactic predicate's
    /// speculative parse) looked at the caret: the decision that evaluates it depends on the
    /// tokens up to the caret, so its configuration is recorded too (CaretParse::early).
    void EarlyCapture();
    /// The parser starts / ends evaluating predicate `predIndex` (SessionParser::sempred).
    void EnterPredicate(antlr4::Parser& parser, size_t predIndex);
    void LeavePredicate(size_t predIndex);
    /// Whether the innermost predicate being evaluated is one the walk evaluates (no opaque calls).
    bool ModeledPredicate() const;
    bool InOpaquePredicate() const { return opaqueDepth_ > 0; }
    CaretParse& Result() { return out_; }

    /// CaptureStream: token `t` was looked at / it is EOF.
    void Looked(const antlr4::Token* t);
    void LookedAtEof() { if (evaluating_) evaluationReachedEof_ = true; }
    /// Predicate evaluation for the walk: from `Begin...` to `End...`, any look at EOF is recorded.
    void BeginEvaluation() { evaluating_ = true; evaluationReachedEof_ = false; }
    bool EndEvaluation() { evaluating_ = false; return evaluationReachedEof_; }

private:
    void Record(int state, antlr4::ParserRuleContext* ctx, size_t index, Capture& c);
    void FollowChain(antlr4::RuleContext* ctx, std::vector<int>& out) const;
    /// Stream index (parser-token index) of token `t` of this session.
    size_t VisibleIndex(const antlr4::Token* t) const;
    size_t ErrorIndex(const ParseError& e) const;
    std::string TokenText(size_t i) const;

    const Grammar& grammar_;
    TokenSourceView view_;
    size_t begin_, limit_;
    size_t next_;                      // buffer index of the next token to hand out
    bool eofCreated_ = false;
    std::vector<std::unique_ptr<antlr4::Token>> hidden_;   // whitespace and comments (the stream owns the rest)
    std::vector<antlr4::Token*> full_;                     // session index -> token (every token)
    ast::ScriptTokenStream script_;                        // session index -> script token
    std::vector<uint32_t> fullToVisible_;                  // session index -> stream index (of it or the next)
    std::vector<uint32_t> visibleStart_;                   // stream index -> byte offset (EOF: where the tokens end)
    std::unique_ptr<ast::FragmentFactory> factory_;
    std::vector<ParseError> errors_;
    std::unique_ptr<SessionTokenSource> source_;
    std::unique_ptr<CaptureStream> stream_;
    SessionParserHooks* hooks_ = nullptr;

    ParseObserver* observer_ = nullptr;
    size_t lookEnd_ = 0;               // session index just past the furthest token looked at
    size_t lookEndAtDecision_ = 0;     // lookEnd_ when the last outermost prediction started

    CaretParse out_;
    std::vector<size_t> predicates_;   // being evaluated, innermost last
    int opaqueDepth_ = 0;
    struct {
        int state = -1;
        antlr4::ParserRuleContext* ctx = nullptr;
        size_t index = 0;
    } opaqueStart_;                    // the parser when the outermost opaque predicate started
    bool evaluating_ = false, evaluationReachedEof_ = false;
};

}  // namespace tsql::editor::detail
