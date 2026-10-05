// Editor support, internal: what the completion engine needs from one generated grammar (its ATN,
// rule names, vocabulary, keyword metadata) and the runs of its lexer and parser it makes: lexing
// from any token boundary (so an edit is lexed again only until the tokens resynchronise) and
// parsing from any point where the script's parse can be resumed (its start, a batch boundary, a
// statement boundary of a batch), over the tokens of a Buffer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tsql/parser.hpp"

namespace antlr4 {
class Parser;
class RuleContext;
namespace atn { class ATN; }
namespace dfa { class Vocabulary; }
}  // namespace antlr4

namespace tsql::editor::detail {

/// tools/g2to4/editor_meta.py output, one table per grammar.
struct KeywordStateEntry {
    int state;          // ATN state of a token match or rule call
    const char* word;   // upper-case word the action checks the matched text against
    int versionMask;    // SqlVersionFlags the word is valid in
    bool binding;       // the action rejects any other word (Match, ParseOption, ...)
};
/// The action after the token matched at `state` requires the token matched earlier at `guardState`
/// (same rule) to be `word` (one entry per word; state -1 ends the table).
struct KeywordGuardEntry {
    int state;
    int guardState;
    const char* word;
    int versionMask;
};
struct PredicateEntry {
    int index;          // predicate index (PredicateTransition::getPredIndex)
    const char* expr;   // see Walker.h EvaluatePredicate
};

/// A lexed token: ast::TSqlTokenType value and its UTF-8 byte range in the input.
/// AsciiStringOrQuotedIdentifier ("x") is stored as QuotedIdentifier (QUOTED_IDENTIFIER ON, the
/// parser's initial setting); LexedToken::dualQuoted remembers it.
struct LexToken {
    uint32_t type;
    uint32_t start;
    uint32_t end;
};

/// Whitespace and comment token types (the parser never sees them).
bool IsHiddenType(uint32_t type);

/// A token as Grammar::LexFrom produces it.
struct LexedToken {
    LexToken token;
    bool dualQuoted = false;   // lexed as AsciiStringOrQuotedIdentifier
    uint32_t lookEnd = 0;      // byte offset just past the furthest character the lexer examined so far
    bool goAfter = false;      // the lexer accepts GO right after this token (start of a line)
};

/// Parser configuration recorded the first time the parser looked at the caret (the end of the
/// parsed text): the walk from (state, index) with the follow states of the rule context chain
/// reproduces every way the parser could have reached the caret.
struct Capture {
    bool captured = false;
    int state = -1;
    size_t index = 0;            // parser-token index where the parser stood (prediction start)
    std::vector<int> follow;     // follow states of the enclosing rule calls, innermost first
    /// Rule-call state of the innermost enclosing call with a keyword check (keywordStates) whose rule
    /// has consumed no token yet, or -1: that check applies to the token at the caret.
    int pendingCall = -1;
    /// The parser's context at the capture: its rule locals as the actions before the capture set
    /// them (owned by the CaretSession's parser).
    antlr4::RuleContext* context = nullptr;
    /// Innermost statement-level rule context enclosing the capture: the walk's fallback start
    /// when a syntax error in that statement preceded the capture.
    int statementState = -1;
    size_t statementIndex = 0;
    std::vector<int> statementFollow;
    bool errorInStatement = false;
    size_t errorIndex = 0;       // parser-token index of the first such error
};

/// Grammar::ParseToCaret result. Byte offsets are offsets into the buffer's text.
struct CaretParse {
    std::vector<LexToken> tokens;      // the parser's tokens (EOF excluded)
    std::vector<std::string> upper;    // their text, ASCII upper-cased
    Capture capture;
    /// The configuration of the decision whose opaque predicate first looked at the caret (see
    /// ParseSession::EarlyCapture), if any.
    Capture early;
    bool syntaxErrors = false;         // the parser reported an error before the capture
    size_t firstError = SIZE_MAX;      // parser-token index of the first one
};

struct KeywordState {
    std::vector<std::string> words;   // sorted
    bool binding = false;
};

/// A KeywordGuardEntry whose guard token stands a fixed number of tokens before the guarded one.
struct KeywordGuard {
    size_t distance = 0;              // the guard token is `distance` tokens before the guarded one
    std::vector<std::string> words;   // sorted
};

/// How the parser took one token.
struct TokenRole {
    int32_t state = -1;                     // ATN state it was matched from (-1: never consumed)
    int32_t callStates[3] = {-1, -1, -1};   // rule-call states of the enclosing rule contexts, innermost first
    uint32_t rules[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};   // rule indexes of those contexts
    bool predicateKeyword = false;          // a NextTokenMatches(word) predicate that held looked at it
};

/// The tokens a parse reads (a Buffer's): every token incl. whitespace and comments, in order.
struct TokenSourceView {
    std::string_view text;
    const std::vector<LexToken>* tokens = nullptr;
    const std::vector<uint8_t>* dualQuoted = nullptr;   // per token
    bool invalidUtf8 = false;                           // `text` has bytes that are not valid UTF-8
};

/// A point where the script's parse can be resumed: the parser state there is fully described by
/// these fields (the rule context chain is script > batch at a statement of a batch, script at a
/// batch after GO; the error strategy keeps no state).
struct ResumePoint {
    enum class Kind : uint8_t {
        ScriptStart,   // before the script (token 0)
        InBatch,       // at a statement of a batch's statement loop (batch: ... statementOptSemi*), before
                       // the loop's decision
        BatchStart,    // at a batch after GO, in the script's loop (Go batch)*
    };
    Kind kind = Kind::ScriptStart;
    bool firstBatch = true;         // InBatch: the batch is the script's first (called from another state)
    bool quotedIdentifier = true;   // the parser's QUOTED_IDENTIFIER setting
    uint32_t token = 0;             // index of the token the parser stands at (tokens->size(): the end)

    bool operator==(const ResumePoint& o) const {
        return kind == o.kind && firstBatch == o.firstBatch && quotedIdentifier == o.quotedIdentifier && token == o.token;
    }
};

class ParseObserver {
public:
    virtual ~ParseObserver() = default;
    /// Token `index` was consumed as `role` (predicateKeyword: see OnPredicateKeyword).
    virtual void OnRole(size_t index, const TokenRole& role) = 0;
    /// A NextTokenMatches(word) predicate that held looked at token `index` (before it is consumed).
    virtual void OnPredicateKeyword(size_t index) = 0;
    /// The parse reached resume point `at`, having examined the tokens before `lookEnd` only.
    /// Returning false stops the parse there.
    virtual bool OnCheckpoint(const ResumePoint& at, size_t lookEnd) = 0;
    /// The parse ended at token `index` (the rest of the tokens were never consumed), having
    /// examined the tokens before `lookEnd` only.
    virtual void OnEnd(size_t index, size_t lookEnd) = 0;
};

/// A finished Grammar::ParseToCaret; the parser stays alive so that the walk can evaluate the
/// grammar's opaque predicates on the tokens before the caret.
class CaretSession {
public:
    virtual ~CaretSession() = default;
    CaretParse result;
    /// Runs predicate `predIndex` (of rule `ruleIndex`) with the parser at parser-token `index`. A
    /// predicate that reads only the tokens ('$' in editor_meta.py's expression) runs anywhere; one
    /// that reads rule locals ('?') only in the context of `locals` (the capture the walk stands
    /// at: same rule, same token, no action run since), else it is unknown. 1: true, 0: false,
    /// -1: unknown (also when it needs a token at or after the caret, or failed).
    virtual int EvaluatePredicate(size_t ruleIndex, size_t predIndex, size_t index, const Capture* locals) = 0;
};

/// ATN states of the script and batch rules a resumed parse replays (derived from the ATN; the
/// generated code of script() and batch() is the same in every grammar).
struct TopLevelStates {
    size_t scriptRule = 0, batchRule = 0, statementOptSemiRule = 0;
    size_t scriptFirstBatchCall = 0;   // script: rv1 = batch()
    size_t scriptLoopBatchCall = 0;    // script: (Go rv2 = batch())*
    size_t scriptGo = 0;               // script: match(Go)
    size_t scriptLoopBack = 0;         // script: the loop's (Go ...)* back state
    size_t scriptEof = 0;              // script: match(EOF)
    size_t batchStatementCall = 0;     // batch: rv3 = statementOptSemi()
    size_t batchLoopBack = 0;          // batch: statementOptSemi* loop back state
    size_t batchLoopDecision = 0;      // batch: that loop's decision
};

class Grammar {
public:
    virtual ~Grammar() = default;

    SqlVersion version{};
    int versionFlag = 0;   // SqlVersionFlags bit of the version
    const antlr4::atn::ATN* atn = nullptr;
    const std::vector<std::string>* ruleNames = nullptr;
    const antlr4::dfa::Vocabulary* vocabulary = nullptr;

    /// words recognised at a keyword state (filtered for the version)
    std::unordered_map<int, KeywordState> keywordStates;
    /// guards of a token-match state (KeywordGuardEntry with a fixed distance, filtered for the version)
    std::unordered_map<int, std::vector<KeywordGuard>> keywordGuards;
    /// predicate index -> expression
    std::unordered_map<size_t, std::string> predicates;
    /// symbolic token name -> token type
    std::unordered_map<std::string, size_t> tokenTypes;
    /// rule indexes of the statement-level rules (statement, lastStatement, ...)
    std::vector<size_t> statementRules;
    TopLevelStates top;

    size_t RuleIndex(std::string_view name) const;   // SIZE_MAX if unknown
    /// Whether ATN state `state` has a token transition that matches `type` (a token consumed from
    /// any other state was skipped by error recovery).
    bool StateMatches(int state, uint32_t type) const;
    /// Whether predicate `predIndex` is opaque to the walk (a hand-written look-ahead scan, a
    /// syntactic predicate, a test of rule locals): its expression has a '?' or '$' term.
    bool OpaquePredicate(size_t predIndex) const;

    /// Lexes `text` from byte `start` (a token boundary of an earlier lex of the same bytes before
    /// it, or 0) with the lexer in the state it has there (`goAcceptable`: GO may start a token
    /// there), calling `onToken` for each token until it returns false or the text ends.
    virtual void LexFrom(std::string_view text, size_t start, bool goAcceptable,
                         const std::function<bool(const LexedToken&)>& onToken) const = 0;

    /// Parses from `from` on (to the end of the tokens) like tsql::parse, but past lexer errors,
    /// reporting to `observer` how each token is taken and every resume point reached.
    virtual void ParseFrom(const TokenSourceView& tokens, const ResumePoint& from, ParseObserver& observer) const = 0;

    /// Parses from `from` up to token `limit` (the caret: tokens from `limit` on are not read) and
    /// stops once the parser first looks at the caret.
    virtual std::unique_ptr<CaretSession> ParseToCaret(const TokenSourceView& tokens, const ResumePoint& from,
                                                       size_t limit) const = 0;

    /// Convenience: every token of `sql` (Buffer::SetText's lex).
    void Lex(std::string_view sql, std::vector<LexToken>& out) const;

protected:
    /// Takes the ATN, names and metadata (`parser` is any parser of the grammar).
    void Init(SqlVersion v, int flag, antlr4::Parser& parser, const KeywordStateEntry* states, size_t nStates,
              const KeywordGuardEntry* guards, const PredicateEntry* preds, size_t nPreds);

private:
    void InitTopLevel();
};

/// The grammar for `version`; throws std::invalid_argument when this build lacks it.
const Grammar& GrammarFor(SqlVersion version);

/// Byte offset of each code point of `s` as tsql::detail::Decode splits it (+ the end); empty when
/// `s` is ASCII (code point index = byte offset).
std::vector<uint32_t> CodePointByteOffsets(std::string_view s);

/// Whether `s` has bytes that are not part of a valid UTF-8 sequence; if so, `out` is `s` with each
/// replaced by U+FFFD (ANTLR's input stream needs valid UTF-8).
bool SanitizeUtf8(std::string_view s, std::string& out);

/// Reserved keyword text (lower case) of a keyword token type, or nullptr.
const char* KeywordText(uint32_t type);

}  // namespace tsql::editor::detail
