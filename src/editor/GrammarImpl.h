// Editor support, internal: Grammar for one generated lexer/parser pair (instantiated per grammar by
// EditorGrammar.cpp.in). Only what needs the generated classes is here (they are expensive to
// compile against); the rest is in Session.cpp.
#pragma once

#include "Session.h"

namespace tsql::editor::detail {

template <class Lexer, class Parser>
class GrammarImpl final : public Grammar {
public:
    GrammarImpl(SqlVersion v, int flag, const KeywordStateEntry* states, size_t nStates, const PredicateEntry* preds,
                size_t nPreds) {
        antlr4::ANTLRInputStream input("");
        Lexer lexer(&input);
        antlr4::CommonTokenStream tokens(&lexer);
        Parser parser(&tokens);
        Init(v, flag, parser, states, nStates, preds, nPreds);
    }

    void Lex(std::string_view sql, std::vector<LexToken>& out, size_t stopAfter) const override {
        std::string sanitized;
        antlr4::ANTLRInputStream input(SanitizeUtf8(sql, sanitized) ? std::string_view(sanitized) : sql);
        Lexer lexer(&input);
        LexInto(lexer, CodePointByteOffsets(sql), out, stopAfter);
    }

    CaretParse ParseToCaret(std::string_view text) const override {
        ParseSession session(*this, text);
        Lexer lexer(&session.Input());
        session.Tokenize(lexer);
        SessionParser parser(&session.ParserStream(), session, false);
        session.Run(parser, [&parser] { parser.script(); }, true);
        return session.FinishCapture();
    }

    ParsedTokens ParseAll(std::string_view sql) const override {
        ParseSession session(*this, sql);
        Lexer lexer(&session.Input());
        session.Tokenize(lexer);
        SessionParser parser(&session.ParserStream(), session, true);
        session.Run(parser, [&parser] { parser.script(); }, false);
        return session.FinishRoles();
    }

private:
    /// Tells the session which predicate is being evaluated and, when recording, every consumed
    /// token and every predicate that held.
    class SessionParser final : public Parser {
    public:
        SessionParser(antlr4::TokenStream* input, ParseSession& session, bool record)
            : Parser(input), session_(session), record_(record) {}

        antlr4::Token* consume() override {
            if (record_) session_.OnConsume(*this);
            return Parser::consume();
        }

        bool sempred(antlr4::RuleContext* ctx, size_t ruleIndex, size_t predicateIndex) override {
            struct Current {
                ParseSession& s;
                size_t index;
                ~Current() { s.LeavePredicate(index); }
            } current{session_, predicateIndex};
            session_.EnterPredicate(*this, predicateIndex);
            const bool result = Parser::sempred(ctx, ruleIndex, predicateIndex);
            if (record_) session_.OnPredicate(*this, predicateIndex, result);
            return result;
        }

    private:
        ParseSession& session_;
        bool record_;
    };
};

}  // namespace tsql::editor::detail
