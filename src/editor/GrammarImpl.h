// Editor support, internal: Grammar for one generated lexer/parser pair (instantiated per grammar by
// EditorGrammar.cpp.in). Only what needs the generated classes is here (they are expensive to
// compile against); the rest is in Session.cpp.
#pragma once

#include <exception>

#include "Session.h"
#include "tsql/ast/generated/token_types.hpp"

namespace tsql::editor::detail {

template <class Lexer, class Parser>
class GrammarImpl final : public Grammar {
public:
    GrammarImpl(SqlVersion v, int flag, const KeywordStateEntry* states, size_t nStates, const KeywordGuardEntry* guards,
                const PredicateEntry* preds, size_t nPreds, const RuleActionsEntry* actions,
                const ReturningActionEntry* returning, const int* predicted) {
        antlr4::ANTLRInputStream input("");
        Lexer lexer(&input);
        antlr4::CommonTokenStream tokens(&lexer);
        Parser parser(&tokens);
        Init(v, flag, parser, states, nStates, guards, preds, nPreds, actions, returning, predicted);
    }

    void LexFrom(std::string_view text, size_t start, bool goAcceptable,
                 const std::function<bool(const LexedToken&)>& onToken) const override {
        // Lexes a window of the text at a time: a token whose look-ahead reached the window's end may
        // depend on what follows, so it is lexed again in a larger window.
        size_t window = 16 * 1024;
        size_t pos = start;
        bool go = goAcceptable;
        if (start == 0 && text.substr(0, 3) == "\xEF\xBB\xBF") {
            // tsql::parse's input stream drops the text's leading BOM: it is whitespace to the editor
            LexedToken bom;
            bom.token = {static_cast<uint32_t>(ast::TSqlTokenType::WhiteSpace), 0, 3};
            bom.lookEnd = 3;
            bom.goAfter = go;
            if (!onToken(bom)) return;
            pos = 3;
        }
        while (pos < text.size()) {
            size_t end = std::min(text.size(), pos + window);
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
            const bool whole = end == text.size();
            const std::string_view w = text.substr(pos, end - pos);
            std::string sanitized;
            WindowInput input(SanitizeUtf8(w, sanitized) ? std::string_view(sanitized) : w);
            const std::vector<uint32_t> bytes = CodePointByteOffsets(w);
            auto byteOf = [&](size_t cp) { return pos + (bytes.empty() ? cp : bytes[std::min(cp, bytes.size() - 1)]); };
            const size_t cps = input.size();
            SessionLexer lexer(&input);
            lexer.removeErrorListeners();
            lexer.SetGoAcceptable(go);
            size_t resume = pos;
            bool resumeGo = go, more = false;
            for (;;) {
                std::unique_ptr<antlr4::Token> t = lexer.nextToken();
                const bool eof = t->getType() == antlr4::Token::EOF;
                if (!whole && (eof || input.maxIndex >= cps)) {
                    more = true;   // it may continue past the window
                    break;
                }
                if (eof) break;
                LexedToken lt;
                lt.token.type = static_cast<uint32_t>(t->getType());
                // TSqlWhitespaceTokenFilter with QUOTED_IDENTIFIER ON (the parser's initial setting)
                if (t->getType() == static_cast<size_t>(ast::TSqlTokenType::AsciiStringOrQuotedIdentifier)) {
                    lt.token.type = static_cast<uint32_t>(ast::TSqlTokenType::QuotedIdentifier);
                    lt.dualQuoted = true;
                }
                lt.token.start = static_cast<uint32_t>(byteOf(t->getStartIndex()));
                lt.token.end = static_cast<uint32_t>(byteOf(t->getStopIndex() + 1));
                lt.lookEnd = static_cast<uint32_t>(input.maxIndex >= cps ? text.size() + 1 : byteOf(input.maxIndex + 1));
                lt.goAfter = lexer.GoAcceptable();
                if (!onToken(lt)) return;
                resume = lt.token.end;
                resumeGo = lt.goAfter;
            }
            if (!more) return;
            pos = resume;
            go = resumeGo;
            window *= 4;
        }
    }

    void ParseFrom(const TokenSourceView& view, const ResumePoint& from, ParseObserver& observer) const override {
        ParseSession session(*this, view, from.token, view.tokens->size());
        session.SetObserver(&observer);
        SessionParser parser(&session.ParserStream(), session, *this);
        parser.Attach();
        tsql::detail::TokensGuard guard(&session.Script());
        try {
            parser.Resume(from, [] {});
            session.OnEnd();
        } catch (const StopParse&) {
        } catch (...) {
            session.OnEnd();   // an error escaped every recovering rule: the rest of the tokens untaken
        }
    }

    std::unique_ptr<CaretSession> ParseToCaret(const TokenSourceView& view, const ResumePoint& from,
                                               size_t limit) const override {
        auto caret = std::make_unique<Caret>(*this, view, from.token, limit);
        caret->Run(from);
        return caret;
    }

    Trial TryParse(const TokenSourceView& view) const override {
        // the checks of choices that looked at the end matter only to a parse that fails after one:
        // such a parse runs again with them
        if (std::optional<Trial> t = RunTrial(view, false)) return *t;
        return *RunTrial(view, true);
    }

private:
    std::optional<Trial> RunTrial(const TokenSourceView& view, bool checkChoices) const {
        ParseSession session(*this, view, 0, view.tokens->size());
        session.SetTrial(checkChoices);
        SessionParser parser(&session.ParserStream(), session, *this);
        parser.Attach();
        tsql::detail::TokensGuard guard(&session.Script());
        try {
            parser.Resume(ResumePoint{}, [&] { session.Stream().armed = &parser; });
        } catch (...) {
            // reached the end (CaptureDone), or an error escaped every recovering rule
        }
        session.Stream().armed = nullptr;
        return session.TrialOutcome();
    }

    /// The lexer's input: remembers the furthest character the lexer looked at.
    class WindowInput final : public antlr4::ANTLRInputStream {
    public:
        explicit WindowInput(std::string_view s) : antlr4::ANTLRInputStream(s) {
            // ANTLRInputStream drops a leading UTF-8 BOM: inside the text U+FEFF is a character like
            // any other (LexFrom skips the text's own leading BOM itself)
            if (s.substr(0, 3) == "\xEF\xBB\xBF") _data.insert(_data.begin(), U'\uFEFF');
        }
        size_t maxIndex = 0;
        size_t LA(ssize_t i) override {
            if (i > 0) maxIndex = std::max(maxIndex, std::min(p + static_cast<size_t>(i) - 1, size()));
            return antlr4::ANTLRInputStream::LA(i);
        }
    };

    /// The lexer's only state between tokens: whether GO may start the next token (it starts a line).
    class SessionLexer final : public Lexer {
    public:
        using Lexer::Lexer;
        void SetGoAcceptable(bool on) { this->_acceptableGoOffset = on ? this->CurrentOffset() : -1; }
        bool GoAcceptable() { return this->_acceptableGoOffset == this->CurrentOffset(); }
    };

    /// The parser of a session: tells the session which predicate is being evaluated, every consumed
    /// token and every predicate that held, and every rule it leaves; resumes a parse.
    class SessionParser final : public Parser, public SessionParserHooks {
    public:
        SessionParser(antlr4::TokenStream* input, ParseSession& session, const Grammar& g)
            : Parser(input), session_(session), g_(g) {}

        bool QuotedIdentifier() const override { return this->_quotedIdentifier; }

        /// Sets the parser up for its session (ParseSession::Attach) with the session's simulator:
        /// a TrialSimulator in trial runs (same ATN, DFA cache and prediction mode).
        void Attach() {
            session_.Attach(*this, *this);
            auto* base = this->template getInterpreter<antlr4::atn::ParserATNSimulator>();
            this->UseSimulator(session_.TrialRun()
                                   ? new TrialSimulator(this, *base, session_.Stream(), session_)
                                   : new SessionSimulator(this, *base));
        }

        antlr4::Token* consume() override {
            if (session_.Recording()) session_.OnConsume(*this);
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
            if (session_.Recording()) session_.OnPredicate(*this, predicateIndex, result);
            return result;
        }

        /// The speculative parse of a syntactic predicate ANTLR 2's decision emulation runs: an
        /// opaque predicate; when it looked at the caret, the parser captures at its decision.
        bool Antlr2SynPred(size_t marker, antlr4::ParserRuleContext* ctx) override {
            bool matched;
            {
                struct Current {
                    ParseSession& s;
                    ~Current() { s.LeavePredicate(ParseSession::kSyntacticPredicate); }
                } current{session_};
                session_.EnterPredicate(*this, ParseSession::kSyntacticPredicate);
                matched = Parser::Antlr2SynPred(marker, ctx);
            }
            // (after the speculation has restored the parser: the decision's state and context)
            if (session_.TakeSpeculationAtCaret()) session_.Stream().CaptureHere();
            return matched;
        }

        void enterRule(antlr4::ParserRuleContext* localctx, size_t state, size_t ruleIndex) override {
            if (!replaying_ && !this->Guessing() && session_.Recording() &&
                !session_.OnRuleEnter(localctx, ruleIndex, this->_ctx, this->_quotedIdentifier))
                throw StopParse{};
            Parser::enterRule(localctx, state, ruleIndex);
        }

        /// Parses from `from` as the script's own parse continues there: script() at its start;
        /// otherwise the script > batch context chain is rebuilt and the rest of the batch's
        /// statement loop and the script's batch loop are replayed (the code of the generated
        /// batch() and script() after that point). `ready` runs once the parser stands at `from`.
        template <class Ready>
        void Resume(const ResumePoint& from, const Ready& ready) {
            using Script = typename Parser::ScriptContext;
            using Batch = typename Parser::BatchContext;
            const TopLevelStates& t = g_.top;
            if (from.kind == ResumePoint::Kind::ScriptStart) {
                ready();
                this->script();
                return;
            }
            if (!from.quotedIdentifier) this->SetQuotedIdentifier(false);
            replaying_ = true;
            this->setState(antlr4::atn::ATNState::INVALID_STATE_NUMBER);
            auto* script = this->_tracker.template createInstance<Script>(this->_ctx, this->getState());
            this->enterRule(script, StartState(t.scriptRule), t.scriptRule);
            if (from.kind == ResumePoint::Kind::InBatch) {
                this->setState(from.firstBatch ? t.scriptFirstBatchCall : t.scriptLoopBatchCall);
                auto* batch = this->_tracker.template createInstance<Batch>(this->_ctx, this->getState());
                this->enterRule(batch, StartState(t.batchRule), t.batchRule);
                replaying_ = false;
                ready();
                BatchLoop();
                this->exitRule();
            } else {
                replaying_ = false;
                this->setState(t.scriptLoopBatchCall);
                ready();
                this->batch();
            }
            ScriptLoop();
            this->exitRule();
        }

    private:
        size_t StartState(size_t rule) const { return g_.atn->ruleToStartState[rule]->stateNumber; }

        /// batch(): the statementOptSemi* loop and its exception handlers.
        void BatchLoop() {
            const TopLevelStates& t = g_.top;
            try {
                for (;;) {
                    this->setState(t.batchLoopBack);
                    this->_errHandler->sync(this);
                    const size_t alt = this->template getInterpreter<antlr4::atn::ParserATNSimulator>()->adaptivePredict(
                        this->_input, t.batchLoopDecision, this->_ctx);
                    if (alt == 2 || alt == antlr4::atn::ATN::INVALID_ALT_NUMBER) break;
                    if (alt == 1) {
                        this->setState(t.batchStatementCall);
                        this->statementOptSemi();
                    }
                }
            } catch (parser::TSqlParseErrorException& exception) {
                if (!exception.DoNotLog) this->AddParseError(exception.ParseError);
                this->RecoverAtBatchLevel();
            } catch (antlr4::NoViableAltException& exception) {
                ParseError error =
                    this->GetFaultTolerantUnexpectedTokenError(this->ErrorToken(exception), exception, this->LastTokenOffset());
                this->AddParseError(error);
                this->RecoverAtBatchLevel();
            } catch (antlr4::InputMismatchException& exception) {
                ParseError error =
                    this->GetFaultTolerantUnexpectedTokenError(this->ErrorToken(exception), exception, this->LastTokenOffset());
                this->AddParseError(error);
                this->RecoverAtBatchLevel();
            } catch (antlr4::RecognitionException&) {
                ParseError error = this->GetUnexpectedTokenError();
                this->AddParseError(error);
                this->RecoverAtBatchLevel();
            } catch (antlr4::RuntimeException& exception) {
                this->CreateInternalError("batch", exception);
            }
        }

        /// script(): the (Go batch)* loop and EOF after a batch.
        void ScriptLoop() {
            const TopLevelStates& t = g_.top;
            try {
                this->setState(t.scriptLoopBack);
                this->_errHandler->sync(this);
                size_t la = this->_input->LA(1);
                while (la == Parser::Go) {
                    this->setState(t.scriptGo);
                    this->match(Parser::Go);
                    this->ResetQuotedIdentifiersSettingToInitial();
                    this->ThrowPartialAstIfPhaseOne(nullptr);
                    this->setState(t.scriptLoopBatchCall);
                    this->batch();
                    this->setState(t.scriptLoopBack);
                    this->_errHandler->sync(this);
                    la = this->_input->LA(1);
                }
                this->setState(t.scriptEof);
                this->match(Parser::EOF);
            } catch (antlr4::RecognitionException& e) {
                this->_errHandler->reportError(this, e);
                this->_errHandler->recover(this, std::current_exception());
            }
        }

        ParseSession& session_;
        const Grammar& g_;
        bool replaying_ = false;   // rebuilding the context chain of a resume point
    };

    class Caret final : public CaretSession {
    public:
        Caret(const GrammarImpl& g, const TokenSourceView& view, size_t begin, size_t limit)
            : g_(g), session_(g, view, begin, limit), parser_(&session_.ParserStream(), session_, g) {
            session_.PrepareCaretTokens();
            parser_.Attach();
        }

        void Run(const ResumePoint& from) {
            tsql::detail::TokensGuard guard(&session_.Script());
            try {
                parser_.Resume(from, [this] { session_.Stream().armed = &parser_; });
            } catch (const CaptureDone&) {
            } catch (...) {
                // an error escaped every recovering rule: no capture
            }
            session_.Stream().armed = nullptr;
            result = std::move(session_.Result());
        }

        int EvaluatePredicate(size_t ruleIndex, size_t predIndex, size_t index, const Capture* locals,
                              const ProbeToken* probe) override {
            auto it = g_.predicates.find(predIndex);
            if (it == g_.predicates.end()) return kUnknown;
            // a predicate that reads rule locals runs only in the captured context (the walk runs
            // no actions, so elsewhere its locals are not what the parse would have set)
            antlr4::ParserRuleContext unused;   // a predicate of the tokens does not read its context
            antlr4::ParserRuleContext* context = &unused;
            if (it->second.find('?') != std::string::npos) {
                if (locals == nullptr || locals->context == nullptr || locals->index != index ||
                    locals->context->getRuleIndex() != ruleIndex)
                    return kUnknown;
                context = static_cast<antlr4::ParserRuleContext*>(locals->context);
            }
            CaptureStream& stream = session_.Stream();
            if (probe != nullptr) {
                // the caret is where the stream's EOF stands
                if (stream.size() == 0 || stream.get(stream.size() - 1)->getType() != antlr4::Token::EOF ||
                    index >= stream.size())
                    return kUnknown;
                stream.Probe(probe->type, std::string(probe->text));
            }
            tsql::detail::TokensGuard guard(&session_.Script());
            stream.armed = nullptr;
            stream.seek(index);
            session_.BeginEvaluation();
            int value;
            try {
                value = parser_.sempred(context, ruleIndex, predIndex) ? kTrue : kFalse;
            } catch (...) {
                value = kUnknown;
            }
            // it read the caret (without a probe: it needs that token), or past the probe
            if (session_.EndEvaluation() && value != kUnknown) value = probe != nullptr ? kUnknown : kReadsCaret;
            if (probe != nullptr) stream.Unprobe();
            return value;
        }

        parser::TSqlParserATNSimulator& Simulator() override {
            return *parser_.template getInterpreter<parser::TSqlParserATNSimulator>();
        }

    private:
        const GrammarImpl& g_;
        ParseSession session_;
        SessionParser parser_;
    };
};

}  // namespace tsql::editor::detail
