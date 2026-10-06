// Editor support, internal: the text of a Document with its tokens and its parse, kept up to date
// incrementally.
//
// Lexing: every token remembers how far the lexer looked to produce it and the lexer's state after
// it; an edit is lexed again from the last token whose look did not reach the edit until a token
// ends where an old token ended (shifted) in the same state: from there the tokens are the old ones.
// The text is lexed on demand, from its start as far as a query, a parse or ParseAhead needs (the
// lexer resumes at a token's end in the state after it, as after an edit), so SetText costs nothing
// in proportion to the text. Unless everything is lexed, the last token lexed is parser-visible and
// not GO, so the start of each batch region lexed so far is known.
//
// Parsing: the script's parse is recorded lazily, as far as a query needs it, as the role of each
// token and the resume points it passes (statement and batch boundaries, Grammar::ResumePoint),
// each with how far the parse had looked to get there. The parse of each batch region (from the
// script start or a GO to the next GO) is a run of its own: GO is matched only by the script rule,
// and after it the script's parse stands at BatchStart with QUOTED_IDENTIFIER reset, whatever the
// text before was. So a query parses only the batches it needs (opening a long script and
// completing in its last batch does not parse the batches before it). One difference from
// tsql::parse, by design: when an error the script rule cannot recover from ends the parse of a
// batch (tsql::parse then has no tree), the batches after the next GO are still parsed.
// An edit keeps the resume points of a run whose look ended before the first changed token; the
// parse resumes from the last of them, and once it reaches a resume point of the run's parse
// before the edit (past the edit, same state) the rest of that parse is taken over. Runs after
// the edit's GO are kept as they are.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Grammar.h"

namespace tsql::editor::detail {

class Buffer final : private ParseObserver {
public:
    explicit Buffer(const Grammar& grammar);

    const Grammar& grammar() const { return *grammar_; }
    void SetText(std::string_view sql);
    /// Replaces bytes [start, start+length) with `replacement` (clamped to the text).
    void Edit(size_t start, size_t length, std::string_view replacement);
    const std::string& Text() const { return text_; }

    /// Every token incl. whitespace and comments (bytes no token covers failed to lex): lexes the
    /// rest of the text first.
    const std::vector<LexToken>& Tokens();
    /// The tokens lexed so far: a prefix of Tokens(). Lexing (LexTo, Tokens, parses) only appends.
    const std::vector<LexToken>& Lexed() const { return tokens_; }
    /// Lexes up to the first parser-visible token (not GO) that starts at or after byte `offset`,
    /// or to the end.
    void LexTo(size_t offset);
    /// Lexes through the batch region that holds byte `offset`: up to the first GO at or after it
    /// (and the token after that, as LexTo), or to the end.
    void LexBatchOf(size_t offset);
    TokenSourceView View() const;
    /// The parser-visible tokens lexed so far and the index in Lexed() of each (kept as lexing
    /// appends and edits splice: a parse that lexes more may reallocate them).
    const std::vector<LexToken>& Visible() const { return visible_; }
    const std::vector<uint32_t>& VisibleToToken() const { return visibleToToken_; }
    /// Index of the first token lexed so far starting at or after byte `offset`.
    size_t TokenAt(size_t offset) const;

    /// Parses the batch regions that hold tokens [first, end) until the roles of those tokens are
    /// known, and every resume point of them at or before `end` (or the end of the region).
    void EnsureParsed(size_t first, size_t end);
    const std::vector<TokenRole>& Roles() const { return roles_; }
    /// The last resume point (in parse order) of the batch holding token `index`, at or before
    /// `index`, whose parse looked only at tokens before `lookLimit` (so a parse of the text cut
    /// at `lookLimit` passes it too); EnsureParsed(index, index + 1) first.
    ResumePoint ResumeAtOrBefore(size_t index, size_t lookLimit = SIZE_MAX) const;
    /// Where the parse of the batch holding token `index` starts (the script start or BatchStart).
    ResumePoint BatchResumeAtOrBefore(size_t index) const;
    /// Parses every batch region in order, from where its parse stands, until all are parsed (true)
    /// or the first resume point reached at or after `deadline` (false): Document::ParseAhead. The
    /// parse is the one queries make (they resume from the same points).
    bool ParseAhead(std::chrono::steady_clock::time_point deadline);
    /// How many parses (Grammar::ParseFrom) the buffer has run: lets tests see that a query parsed nothing.
    size_t ParseCount() const { return parseCount_; }

private:
    void OnRole(size_t index, const TokenRole& role) override;
    void OnPredicateKeyword(size_t index) override;
    bool OnCheckpoint(const ResumePoint& at, size_t lookEnd) override;
    void OnEnd(size_t index, size_t lookEnd) override;
    bool MoreTokens() override;

    /// Replaces bytes of the text (Edit's clamped range), keeping invalidUtf8_ up to date.
    void ReplaceText(size_t start, size_t length, std::string_view replacement);
    /// Lexes on from the last token lexed: LexTo(until).
    void LexMore(size_t until);
    void Append(const LexedToken& t);

    struct Point {
        ResumePoint at;
        uint32_t lookEnd;   // the run's parse up to here examined only tokens before this (cumulative)
    };
    /// The parse of one batch region: tokens [start, the next GO], parsed from the root.
    struct Run {
        uint32_t start = 0;          // 0, or the token after the region's GO
        std::vector<Point> points;   // resume points so far, in parse order; points[0] is the root
        bool done = false;           // the parse left the region (next batch, end, escaped error)
        uint32_t doneLook = 0;       // if done: how far that parse looked
        std::vector<Point> stale;    // resume points of an earlier parse, at or after dirtyEnd
        bool staleDone = false;      // that parse was done (its roles after the last stale point hold)
        uint32_t staleDoneLook = 0;
        uint32_t dirtyEnd = 0;       // tokens from here on are as that parse saw them
        size_t staleHead = 0;        // stale[staleHead..] are still candidates
    };
    size_t RegionStart(size_t index) const;   // first token of the batch region holding token `index`
    size_t RegionEnd(size_t start) const;     // the token after that region's GO (or the end)
    Point Root(size_t start) const;           // where the parse of the region starting at `start` starts
    const Run* FindRun(size_t start) const;
    Run& RunAt(size_t start);                 // made (unparsed) if new
    void Extend(Run& run, size_t end);
    void Finish(Run& run, uint32_t look);
    /// Edit of tokens [first, oldEnd) to [first, newEnd) in a run that starts at or before `first`.
    void Truncate(Run& run, size_t first, size_t oldEnd, size_t newEnd);
    static void Shift(Run& run, std::ptrdiff_t shift);
    static std::vector<Point>::const_iterator PointsEnd(const Run& run, size_t index, size_t lookLimit);

    const Grammar* grammar_;
    std::string text_;
    bool invalidUtf8_ = false;
    std::vector<LexToken> tokens_;
    std::vector<uint8_t> dual_;          // per token: LexedToken::dualQuoted
    std::vector<uint32_t> lookEnd_;      // per token: LexedToken::lookEnd, made cumulative (non-decreasing)
    std::vector<uint8_t> goAfter_;       // per token: LexedToken::goAfter
    std::vector<uint32_t> gos_;          // indexes of the GO tokens
    uint32_t lexEnd_ = 0;                // where lexing stopped (the end of the last token lexed)
    bool lexGo_ = true;                  // the lexer's state there
    bool lexedAll_ = false;              // lexing reached the end of the text

    std::vector<LexToken> visible_;
    std::vector<uint32_t> visibleToToken_;

    std::vector<TokenRole> roles_;       // valid in each run's region up to its last point (all when done)
    std::vector<Run> runs_;              // by start: the regions parsed so far
    Run* current_ = nullptr;             // the run a parse extends
    size_t target_ = 0;                  // Extend's goal while a parse runs
    std::chrono::steady_clock::time_point deadline_ = std::chrono::steady_clock::time_point::max();   // ParseAhead's
    bool timeUp_ = false;                // a parse stopped at deadline_
    size_t parseCount_ = 0;
    std::vector<uint32_t> pendingKeyword_;
};

}  // namespace tsql::editor::detail
