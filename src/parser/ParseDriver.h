// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlParser.cs (Parse,
// GetTokenStream), TSql*Parser.cs (Parse), TSqlTokenFilter.cs (whitespace filtering and token
// indexes: the parser reads only the default-channel tokens, each keeping its full-stream index
// exactly like TSqlParserTokenProxyWithIndex.TokenIndex),
// TSql80ParserBaseInternal.ParseRuleWithStandardExceptionHandling.
// The per-grammar parse driver: ParseWith<Lexer, Parser> is instantiated once per grammar
// (ParseGrammar.cpp.in); everything not depending on the generated classes is in Parse.cpp.
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "tsql/parser.hpp"
#include "antlr4-runtime.h"
#include "ParserErrors.h"
#include "TSqlLexerBase.h"
#include "TSql80ParserBase.h"

namespace tsql::detail {

struct Decoded {
    std::string utf8;          // sanitized UTF-8 handed to ANTLR
    std::vector<int> utf16;    // UTF-16 offset of each code point index (+ end)
    std::vector<int> line;     // SqlScriptDOM line of each code point index (+ end)
    std::vector<int> column;   // SqlScriptDOM column (1-based, UTF-16 units, tab = 1)
};

Decoded Decode(std::string_view s);

struct TokensGuard {
    explicit TokensGuard(const ast::ScriptTokenStream* t) { parser::CurrentScriptTokens() = t; }
    ~TokensGuard() { parser::CurrentScriptTokens() = nullptr; }
};

/// A parser-facing copy of a token that keeps its full-stream index: the parser's stream holds only
/// default-channel tokens (so lookahead never steps over whitespace and comments), while
/// FirstTokenIndex/LastTokenIndex must index the full stream.
class FullStreamIndexToken : public antlr4::CommonToken {
public:
    /// CommonToken(Token*) without its dynamic_cast
    explicit FullStreamIndexToken(const antlr4::CommonToken& full)
        : antlr4::CommonToken({full.getTokenSource(), full.getInputStream()}, full.getType(), full.getChannel(),
                              full.getStartIndex(), full.getStopIndex()) {
        if (full.getType() != antlr4::Token::EOF) _text = full.getText();   // BuildScriptTokens set it
        _line = full.getLine();
        _charPositionInLine = full.getCharPositionInLine();
        _index = full.getTokenIndex();
    }
    void setTokenIndex(size_t) override {}   // the parser stream would renumber it
};

/// A token stream of CommonTokens (the lexer's, or VisibleTokens'): BufferedTokenStream::fetch
/// numbers every token it fetches after a dynamic_cast to WritableToken.
class CommonTokensStream : public antlr4::CommonTokenStream {
public:
    using antlr4::CommonTokenStream::CommonTokenStream;

protected:
    size_t fetch(size_t n) override {
        if (_fetchedEOF) return 0;
        size_t i = 0;
        while (i < n) {
            std::unique_ptr<antlr4::Token> t(_tokenSource->nextToken());
            static_cast<antlr4::CommonToken*>(t.get())->setTokenIndex(_tokens.size());
            _tokens.push_back(std::move(t));
            ++i;
            if (_tokens.back()->getType() == antlr4::Token::EOF) {
                _fetchedEOF = true;
                break;
            }
        }
        return i;
    }
};

/// The parser's token stream. Every token in it is a default-channel token (VisibleTokens), so
/// once it is filled LT(k) indexes the token list directly; CommonTokenStream::LT steps through k
/// tokens one at a time, which makes the k-token scans of hand-written predicates
/// (IsNextRuleBooleanParenthesis, ContainsVectorInLookahead) quadratic.
class VisibleTokenStream : public CommonTokensStream {
public:
    using CommonTokensStream::CommonTokensStream;

    void fill() override {
        CommonTokensStream::fill();
        filled_ = true;
    }
    antlr4::Token* LT(ssize_t k) override {
        if (!filled_) return CommonTokensStream::LT(k);
        if (k > 0) return _tokens[std::min(_p + static_cast<size_t>(k) - 1, _tokens.size() - 1)].get();
        if (k == 0 || static_cast<size_t>(-k) > _p) return nullptr;
        return _tokens[_p - static_cast<size_t>(-k)].get();
    }
    size_t LA(ssize_t i) override { return VisibleTokenStream::LT(i)->getType(); }

private:
    bool filled_ = false;
};

/// Lexer errors and the script token stream (TSqlParser.GetTokenStream + TSqlWhitespaceTokenFilter).
/// Returns false after a lexer error: `r` then holds the empty script and the error.
bool PrepareTokens(parser::TSqlLexerBase& lexer, antlr4::CommonTokenStream& stream, const Decoded& d,
                   bool initialQuotedIdentifiers, ParseResult& r);
/// PrepareTokens without the lexer-error check: fills r.tokens from `stream` and makes the
/// whitespace and comment tokens hidden (the editor support parses past lexer errors).
void BuildScriptTokens(antlr4::CommonTokenStream& stream, const Decoded& d, bool initialQuotedIdentifiers,
                       ParseResult& r);

/// The parser-facing token list: default-channel tokens keeping their full-stream indexes.
std::vector<std::unique_ptr<antlr4::Token>> VisibleTokens(const std::vector<antlr4::Token*>& all);

/// ParseRuleWithStandardExceptionHandling's catch clauses (the exception is being handled).
void ReportEscapedException(parser::TSql80ParserBase& parser, antlr4::TokenStream& parserStream, ParseResult& r);

/// TSql<ver>Parser.Parse after the script rule: HotswapCreateExternalStreamingJobStatements and
/// the VersioningVisitor (SqlEngineType.All).
void FinishParse(ast::TSqlScript* result, SqlVersion version, ParseResult& r);

template <class Lexer, class Parser>
ParseResult ParseWith(std::string_view sql, SqlVersion version, bool initialQuotedIdentifiers) {
    ParseResult r;
    r.tokens = std::make_unique<ast::ScriptTokenStream>();
    r.factory = std::make_unique<ast::FragmentFactory>();

    Decoded d = Decode(sql);
    antlr4::ANTLRInputStream input(d.utf8);
    Lexer lexer(&input);
    lexer.removeErrorListeners();
    lexer.SetUtf16Offsets(&d.utf16);
    CommonTokensStream stream(&lexer);
    stream.fill();
    if (!PrepareTokens(lexer, stream, d, initialQuotedIdentifiers, r)) return r;
    TokensGuard guard(r.tokens.get());

    const auto& all = stream.getTokens();
    antlr4::ListTokenSource source(VisibleTokens(all));
    VisibleTokenStream parserStream(&source);
    parserStream.fill();

    // ---- TSql<ver>Parser.Parse -> ParseRuleWithStandardExceptionHandling(script)
    Parser parser(&parserStream);
    parser.InitializeForNewInput(r.tokens.get(), &all, &r.errors, r.factory.get(), initialQuotedIdentifiers);
    ast::TSqlScript* result = nullptr;
    try {
        result = parser.script()->vResult;
    } catch (...) {
        ReportEscapedException(parser, parserStream, r);
    }
    FinishParse(result, version, r);
    return r;
}

}  // namespace tsql::detail
