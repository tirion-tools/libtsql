// Editor support, internal: what the completion engine needs from one generated grammar (its ATN,
// rule names, vocabulary, keyword metadata) and the two runs of its lexer/parser it makes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tsql/parser.hpp"

namespace antlr4 {
class Parser;
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
struct PredicateEntry {
    int index;          // predicate index (PredicateTransition::getPredIndex)
    const char* expr;   // see Walker.h EvaluatePredicate
};

/// A lexed token: ast::TSqlTokenType value and its UTF-8 byte range in the input.
struct LexToken {
    uint32_t type;
    uint32_t start;
    uint32_t end;
};

/// Whitespace and comment token types (the parser never sees them).
bool IsHiddenType(uint32_t type);

/// Parser configuration recorded the first time the parser looked at the caret (the end of the
/// parsed text): the walk from (state, index) with the follow states of the rule context chain
/// reproduces every way the parser could have reached the caret.
struct Capture {
    bool captured = false;
    int state = -1;
    size_t index = 0;            // parser-token index where the parser stood (prediction start)
    std::vector<int> follow;     // follow states of the enclosing rule calls, innermost first
    /// Innermost statement-level rule context enclosing the capture: the walk's fallback start
    /// when a syntax error in that statement preceded the capture.
    int statementState = -1;
    size_t statementIndex = 0;
    std::vector<int> statementFollow;
    bool errorInStatement = false;
    size_t errorIndex = 0;       // parser-token index of the first such error
};

/// Grammar::ParseToCaret result. Byte offsets are relative to the parsed text.
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

/// How the parser took one token (Grammar::ParseAll).
struct TokenRole {
    int state = -1;                  // ATN state it was matched from (-1: never consumed)
    int callStates[3] = {-1, -1, -1};   // rule-call states of the enclosing rule contexts, innermost first
    size_t rules[3] = {SIZE_MAX, SIZE_MAX, SIZE_MAX};   // rule indexes of those contexts
    bool predicateKeyword = false;   // a NextTokenMatches(word) predicate that held looked at it
};

struct ParsedTokens {
    std::vector<LexToken> tokens;   // every token incl. whitespace and comments (EOF excluded)
    std::vector<TokenRole> roles;   // per token
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
    /// predicate index -> expression
    std::unordered_map<size_t, std::string> predicates;
    /// symbolic token name -> token type
    std::unordered_map<std::string, size_t> tokenTypes;
    /// rule indexes of the statement-level rules (statement, lastStatement, ...)
    std::vector<size_t> statementRules;

    size_t RuleIndex(std::string_view name) const;   // SIZE_MAX if unknown
    /// Whether ATN state `state` has a token transition that matches `type` (a token consumed from
    /// any other state was skipped by error recovery).
    bool StateMatches(int state, uint32_t type) const;

    /// Lexes `sql` (UTF-8) with the grammar's lexer into `out` (every token incl. whitespace and
    /// comments, EOF excluded; bytes no token covers failed to lex). With `stopAfter` < SIZE_MAX,
    /// stops after the first GO token that starts at or after byte `stopAfter`.
    virtual void Lex(std::string_view sql, std::vector<LexToken>& out, size_t stopAfter = SIZE_MAX) const = 0;

    /// Parses `text` (a batch up to the caret) like tsql::parse, but past lexer errors, and stops
    /// once the parser first looks at the end of the text.
    virtual CaretParse ParseToCaret(std::string_view text) const = 0;

    /// Parses all of `sql` like tsql::parse, but past lexer errors, recording how each token was
    /// taken.
    virtual ParsedTokens ParseAll(std::string_view sql) const = 0;

protected:
    /// Takes the ATN, names and metadata (`parser` is any parser of the grammar).
    void Init(SqlVersion v, int flag, antlr4::Parser& parser, const KeywordStateEntry* states, size_t nStates,
              const PredicateEntry* preds, size_t nPreds);
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
