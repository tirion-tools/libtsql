// Editor support, internal: the grammar-independent half of Grammar::Lex/ParseToCaret/ParseAll
// (GrammarImpl.h supplies the generated lexer and parser). Both parses run
// tsql::detail::ParseWith (src/parser/ParseDriver.h) with one difference: lexer errors do not stop
// them. ParseToCaret's token stream records the parser configuration the first time the parser
// looks at the caret (the end of the text) and then stops the parse; ParseAll records how the
// parser took each token.
#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "Grammar.h"
#include "ParseDriver.h"
#include "antlr4-runtime.h"

namespace tsql::editor::detail {

class ParseSession;

/// The parser's token stream (default-channel tokens only, so stream index + k - 1 is LT(k)'s
/// index). When armed, the first LT/LA that returns EOF (the caret) records the parser's state,
/// rule context chain and position (inside adaptivePredict, between mark and release, the position
/// where the prediction started) and throws CaptureDone.
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

private:
    ParseSession& session_;
    int depth_ = 0;
    size_t markIndex_ = 0;
};

class ParseSession {
public:
    ParseSession(const Grammar& grammar, std::string_view text);

    antlr4::CharStream& Input() { return *input_; }
    /// Lexes with `lexer` (built on Input()) and sets up the parser's token stream.
    void Tokenize(parser::TSqlLexerBase& lexer);
    antlr4::TokenStream& ParserStream() { return *parserStream_; }
    /// Runs `script` (the script rule of `parser`, built on ParserStream()); with `capture`, until
    /// the parser first looks at the end of the text.
    void Run(parser::TSql80ParserBase& parser, const std::function<void()>& script, bool capture);
    CaretParse FinishCapture() { return std::move(out_); }
    ParsedTokens FinishRoles();

    /// CaptureStream: the parser looks at the caret for the first time (outside opaque predicates).
    void CaretReached(antlr4::Parser& parser, size_t index);
    /// CaptureStream: an opaque predicate (a hand-written look-ahead scan, a syntactic predicate's
    /// speculative parse) looked at the caret: the decision that evaluates it depends on the
    /// tokens up to the caret, so its configuration is recorded too (CaretParse::early).
    void EarlyCapture();
    /// The recording parser of ParseAll: before it consumes a token / after it evaluated a predicate.
    void OnConsume(antlr4::Parser& parser);
    void OnPredicate(antlr4::Parser& parser, size_t predIndex, bool result);
    /// The parser starts / ends evaluating predicate `predIndex` (SessionParser::sempred).
    void EnterPredicate(antlr4::Parser& parser, size_t predIndex);
    void LeavePredicate(size_t predIndex);
    /// Whether the innermost predicate being evaluated is one the walk evaluates (no opaque calls).
    bool ModeledPredicate() const;
    bool InOpaquePredicate() const { return opaqueDepth_ > 0; }

private:
    bool Opaque(size_t predIndex) const;
    void Record(int state, antlr4::ParserRuleContext* ctx, size_t index, Capture& c);
    void FollowChain(antlr4::RuleContext* ctx, std::vector<int>& out) const;
    size_t VisibleIndex(const antlr4::Token* token) const;

    const Grammar& grammar_;
    std::string_view text_;
    tsql::detail::Decoded decoded_;
    std::vector<uint32_t> bytes_;   // code point -> byte offset (empty: the same)
    std::unique_ptr<antlr4::ANTLRInputStream> input_;
    std::unique_ptr<antlr4::CommonTokenStream> lexerStream_;
    std::vector<antlr4::Token*> all_;
    ParseResult result_;
    std::unique_ptr<antlr4::ListTokenSource> source_;
    std::unique_ptr<CaptureStream> parserStream_;
    std::vector<size_t> fullToVisible_;
    std::vector<int> visibleOffset_;   // UTF-16 offset of each parser token (+ the end)
    CaretParse out_;
    std::vector<TokenRole> roles_;     // ParseAll: per token of all_
    std::vector<size_t> predicates_;   // being evaluated, innermost last
    int opaqueDepth_ = 0;
    struct {
        int state = -1;
        antlr4::ParserRuleContext* ctx = nullptr;
        size_t index = 0;
    } opaqueStart_;                    // the parser when the outermost opaque predicate started
};

/// Lexes all of the lexer's input into `out` (Grammar::Lex).
void LexInto(antlr4::Lexer& lexer, const std::vector<uint32_t>& bytes, std::vector<LexToken>& out, size_t stopAfter);

}  // namespace tsql::editor::detail
