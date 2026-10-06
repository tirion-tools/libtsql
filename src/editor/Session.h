// Editor support, internal: the grammar-independent half of Grammar::ParseFrom/ParseToCaret
// (GrammarImpl.h supplies the generated lexer and parser). A ParseSession hands a Buffer's tokens
// to the parser lazily, from the token where the parse resumes, like tsql::detail::ParseWith
// prepares them (the parser sees default-channel tokens that keep their full-stream indexes; the
// script token stream and the full token list hold every token), with one difference: lexer errors
// do not stop the parse. Token indexes inside a session are relative to its first token.
//
// ParseFrom records how the parser takes each token and every point where the parse could be
// resumed (ResumePoint), and asks its observer to lex more when it reads past the tokens lexed so
// far (a Buffer lexes on demand); ParseToCaret records the parser configuration the first time the
// parser looks at the caret (the end of its tokens) and then stops the parse; a trial run
// (Grammar::TryParse) goes on until the parser itself stands at the end.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "Grammar.h"
#include "ParseDriver.h"
#include "ParserRuntime.h"
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
/// caret) records the parser's state, rule context chain and position (inside a prediction,
/// between its outermost mark and release, the position where the prediction started) and throws
/// CaptureDone. In a trial run only the parser standing at EOF does; a prediction reads on
/// (TrialSimulator).
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
    /// Predicate evaluation for the walk: the parser sees a token of `type` and `text` where EOF
    /// stood (the caret) and EOF after it, until Unprobe(). The stream must have fetched EOF.
    void Probe(size_t type, std::string text);
    void Unprobe();
    /// Trial runs: whether the last outermost prediction looked at EOF (outside predicates), and
    /// whether one is under way.
    bool PredictionLookedAtEnd() const { return predictionAtEnd_; }
    bool Predicting() const { return depth_ > 0; }
    /// Caret runs (armed): records the parser's configuration where it stands (Position()) and
    /// throws CaptureDone.
    [[noreturn]] void CaptureHere();

private:
    ParseSession& session_;
    int depth_ = 0;
    size_t markIndex_ = 0;
    bool predictionAtEnd_ = false;
};

/// Every session's simulator: a prediction, ANTLR 2's tests after ANTLR 4's choice included (their
/// LA(2), a syntactic predicate's match check and speculative parse: TSqlParserATNSimulator), reads
/// the stream between one mark and release, so CaptureStream takes each of its reads for the
/// decision's, from where the prediction started.
class SessionSimulator : public parser::TSqlParserATNSimulator {
public:
    /// The same ATN, DFA cache and prediction mode as `base`.
    SessionSimulator(antlr4::Parser* parser, antlr4::atn::ParserATNSimulator& base);
    size_t adaptivePredict(antlr4::TokenStream* input, size_t decision, antlr4::ParserRuleContext* outerContext) override;
};

/// Trial runs: the parser's prediction (SLL, as in every parse) with a check of each prediction
/// that looked at the end of the text: whether its choice depended on where the text ends. It did
/// not when a full-context prediction (exact rule context, predicates evaluated) is down to the
/// chosen alternative alone (or to none) before the end: more tokens leave at most that one, and
/// SLL only read on to the end where it could not tell them apart (`a.b` where SLL keeps both
/// "more name parts" and "end of name" alive); any other alternative ANTLR 2's tests could take
/// with more tokens fails on the tokens before the end. Otherwise (both alive until the end, or the
/// end decided) the parse is undecided from there on (ParseSession::TrialChoiceAtEnd).
/// The check costs a full-context prediction, and only a parse that fails after such a choice needs
/// it: a trial runs without it first and only such a parse runs again with it (Grammar::TryParse).
class TrialSimulator final : public SessionSimulator {
public:
    TrialSimulator(antlr4::Parser* parser, antlr4::atn::ParserATNSimulator& base, CaptureStream& stream,
                   ParseSession& session);
    size_t adaptivePredict(antlr4::TokenStream* input, size_t decision, antlr4::ParserRuleContext* outerContext) override;

private:
    /// Whether full-context prediction of `decision` from the stream's position is down to `alt`
    /// alone (or to no alternative) before EOF.
    bool DecidedBeforeEnd(antlr4::TokenStream* input, size_t decision, antlr4::ParserRuleContext* outerContext,
                          size_t alt);

    CaptureStream& stream_;
    ParseSession& session_;
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

    // ---- trial runs (TryParse)
    /// `checkChoices`: each outermost prediction whose choice looked at EOF gets TrialSimulator's
    /// check; without it such a choice is only noted, and TrialOutcome is nullopt when the check
    /// would decide the outcome (an error after it).
    void SetTrial(bool checkChoices) {
        trial_.on = true;
        trial_.check = checkChoices;
    }
    bool TrialRun() const { return trial_.on; }
    bool TrialChecksChoices() const { return trial_.check; }
    /// CaptureStream / TrialSimulator: the parser stands at EOF / a predicate looked at it / the
    /// outermost prediction's choice depended on it / looked at it (unchecked) / it failed after
    /// looking at it.
    void TrialEndReached();
    void TrialPredicateAtEnd() { trial_.doubt = true; }
    void TrialChoiceAtEnd();
    void TrialChoiceUnchecked();
    void TrialFailureAtEnd();
    std::optional<Trial> TrialOutcome() const;

    // ---- caret runs (ParseToCaret)
    /// CaptureStream: the parser looks at the caret for the first time (outside opaque predicates).
    void CaretReached(antlr4::Parser& parser, size_t index);
    /// CaptureStream: an opaque predicate (a hand-written look-ahead scan, a syntactic predicate's
    /// speculative parse) looked at the caret: the decision that evaluates it depends on the
    /// tokens up to the caret, so its configuration is recorded too (CaretParse::early).
    void EarlyCapture();
    /// The parser starts / ends evaluating predicate `predIndex` (SessionParser::sempred), or
    /// kSyntacticPredicate: the speculative parse of a syntactic predicate the runtime's emulation
    /// of ANTLR 2's decisions runs (SessionParser::Antlr2SynPred), an opaque predicate.
    static constexpr size_t kSyntacticPredicate = SIZE_MAX;
    void EnterPredicate(antlr4::Parser& parser, size_t predIndex);
    void LeavePredicate(size_t predIndex);
    /// Whether the innermost predicate being evaluated is one the walk evaluates (no opaque calls).
    bool ModeledPredicate() const;
    bool InOpaquePredicate() const { return opaqueDepth_ > 0; }
    bool InPredicate() const { return !predicates_.empty(); }
    /// Whether the outermost predicate being evaluated is a kSyntacticPredicate: a decision's own
    /// speculative parse, inside the decision's prediction.
    bool InDecisionSpeculation() const { return !predicates_.empty() && predicates_.front() == kSyntacticPredicate; }
    /// CaptureStream: such a speculative parse looked at the caret, so its decision depends on the
    /// tokens up to the caret, as when ANTLR 4's prediction reads it.
    void SpeculationReachedCaret() { speculationAtCaret_ = true; }
    /// SessionParser::Antlr2SynPred: whether the outermost speculative parse, just done, looked at
    /// the caret (once).
    bool TakeSpeculationAtCaret() {
        const bool at = speculationAtCaret_ && predicates_.empty();
        if (at) speculationAtCaret_ = false;
        return at;
    }
    CaretParse& Result() { return out_; }

    /// CaptureStream: token `t` was looked at / it is EOF.
    void Looked(const antlr4::Token* t);
    void LookedAtEof() { if (evaluating_) evaluationReachedEof_ = true; }
    /// Predicate evaluation for the walk: from `Begin...` to `End...`, any look at EOF is recorded.
    void BeginEvaluation() { evaluating_ = true; evaluationReachedEof_ = false; }
    bool EndEvaluation() { evaluating_ = false; return evaluationReachedEof_; }

private:
    /// Whether a token is left to hand out (a recording run first has its observer lex more).
    bool Available();
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
    bool speculationAtCaret_ = false;   // SpeculationReachedCaret
    bool evaluating_ = false, evaluationReachedEof_ = false;
    struct {
        bool on = false;
        bool check = false;       // choices that looked at EOF are checked
        bool unchecked = false;   // before any error, a prediction's choice looked at EOF (not checked)
        bool reached = false;     // the parser stood at EOF
        bool errors = false;      // an error was reported before
        bool doubt = false;       // a predicate looked at EOF
        bool endChoice = false;   // before any error, a prediction's choice depended on EOF
        bool endFailed = false;   // before any error, a prediction that looked at EOF failed
    } trial_;
};

}  // namespace tsql::editor::detail
