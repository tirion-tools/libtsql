// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlParser.cs (Parse,
// GetTokenStream), TSql*Parser.cs (Parse, HotswapCreateExternalStreamingJobStatements),
// TSqlTokenFilter.cs, TSql80ParserBaseInternal.ParseRuleWithStandardExceptionHandling.
// Version-independent parts of the parse driver (ParseDriver.h) and the SqlVersion dispatch.
#include "ParseDriver.h"

#include <stdexcept>

#include "VersioningVisitor.h"
#include "generated/support/TSqlParserResource.h"

namespace tsql {

namespace detail {

using namespace ::tsql::parser;

static void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Lenient UTF-8 decoding (U+FFFD per invalid sequence) + position tables. Lines advance on
// "\r\n", "\r" and "\n" (ANTLR 2 EndOfLine + newline()).
Decoded Decode(std::string_view s) {
    Decoded d;
    std::vector<char32_t> cps;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0xFFFD;
        size_t n = 0;
        if (c < 0x80) { cp = c; n = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
        else { ++i; cps.push_back(0xFFFD); continue; }
        bool ok = true;
        for (size_t k = 1; ok && k <= n; ++k) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        static const char32_t kMin[] = {0, 0x80, 0x800, 0x10000};
        if (!ok || cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            cps.push_back(0xFFFD);
            ++i;
            continue;
        }
        cps.push_back(cp);
        i += n + 1;
    }
    d.utf8.reserve(s.size());
    d.utf16.reserve(cps.size() + 1);
    d.line.reserve(cps.size() + 1);
    d.column.reserve(cps.size() + 1);
    int off = 0, line = 1, col = 1;
    for (size_t i = 0; i < cps.size(); ++i) {
        char32_t cp = cps[i];
        AppendUtf8(d.utf8, cp);
        d.utf16.push_back(off);
        d.line.push_back(line);
        d.column.push_back(col);
        int units = cp >= 0x10000 ? 2 : 1;
        off += units;
        if (cp == '\n' || (cp == '\r' && !(i + 1 < cps.size() && cps[i + 1] == '\n'))) {
            ++line;
            col = 1;
        } else {
            col += units;
        }
    }
    d.utf16.push_back(off);
    d.line.push_back(line);
    d.column.push_back(col);
    return d;
}

ParseError LexerErrorToParseError(const LexerError& e, const Decoded& d) {
    auto at = [&](size_t i) { return std::min(i, d.utf16.size() - 1); };
    size_t s = at(e.startIndex), x = at(e.endIndex);
    switch (e.kind) {
        case LexerError::Kind::UnterminatedString:
            return CreateParseError("SQL46030", d.utf16[s], d.line[s], d.column[s], TSqlParserResource::SQL46030Message, e.text);
        case LexerError::Kind::UnterminatedQuotedIdentifier:
            return CreateParseError("SQL46031", d.utf16[s], d.line[s], d.column[s], TSqlParserResource::SQL46031Message, e.text);
        case LexerError::Kind::UnterminatedSqlCommandIdentifier:
            return CreateParseError("SQL46033", d.utf16[s], d.line[s], d.column[s], TSqlParserResource::SQL46033Message, e.text);
        case LexerError::Kind::UnterminatedMultiLineComment:
            return CreateParseError("SQL46032", d.utf16[x], d.line[x], d.column[x], TSqlParserResource::SQL46032Message);
        case LexerError::Kind::UnexpectedChar:
        default:
            // NoViableAltForCharException: "Incorrect syntax near '<char>'." at the lexer position
            return CreateParseError("SQL46010", d.utf16[s], d.line[s], d.column[s], TSqlParserResource::SQL46010Message, e.text);
    }
}

// TSql150..180/FabricDW Parser.HotswapCreateExternalStreamingJobStatements / ConvertToExecutableProcedureReference:
// a batch starting with `EXEC sys.sp_create_streaming_job 'name', 'statement'` becomes a
// CreateExternalStreamingJobStatement (nodes created with `new`: no token stream of their own).
// TSql150/160/FabricDW do not check the parameter count (C#: ArgumentOutOfRangeException with
// fewer than two, like the InvalidCastException below); both C# exceptions skip the batch here.
static void HotswapCreateExternalStreamingJobStatements(ast::TSqlScript* script, ast::FragmentFactory& factory) {
    if (script == nullptr) return;
    for (ast::TSqlBatch* batch : script->Batches) {
        ast::TSqlStatement* statement = batch->Statements[0];
        auto* exec = dynamic_cast<ast::ExecuteStatement*>(statement);
        if (exec == nullptr || exec->ExecuteSpecification == nullptr) continue;
        auto* proc = dynamic_cast<ast::ExecutableProcedureReference*>(exec->ExecuteSpecification->ExecutableEntity);
        if (proc == nullptr || proc->ProcedureReference == nullptr || proc->ProcedureReference->ProcedureReference == nullptr)
            continue;
        const auto& ids = proc->ProcedureReference->ProcedureReference->Name->Identifiers;
        if (ids.size() != 2 || AsciiLower(CsStr(ids[0]->Value).get()) != "sys" ||
            AsciiLower(CsStr(ids[1]->Value).get()) != "sp_create_streaming_job")
            continue;
        if (proc->Parameters.size() < 2) continue;
        auto* name = dynamic_cast<ast::Literal*>(proc->Parameters[0]->ParameterValue);
        auto* text = dynamic_cast<ast::Literal*>(proc->Parameters[1]->ParameterValue);
        if (name == nullptr || text == nullptr) continue;   // C#: InvalidCastException
        auto* esj = factory.CreateFragment<ast::CreateExternalStreamingJobStatement>();
        esj->ScriptTokenStream = proc->ScriptTokenStream;
        esj->FirstTokenIndex = statement->FirstTokenIndex;
        esj->LastTokenIndex = proc->LastTokenIndex;
        auto* nameLiteral = factory.CreateFragment<ast::StringLiteral>();
        nameLiteral->ScriptTokenStream = nullptr;
        nameLiteral->set_Value(name->Value);
        esj->set_Name(nameLiteral);
        auto* statementLiteral = factory.CreateFragment<ast::StringLiteral>();
        statementLiteral->ScriptTokenStream = nullptr;
        statementLiteral->set_Value(text->Value);
        esj->set_Statement(statementLiteral);
        batch->Statements[0] = esj;
    }
}

bool PrepareTokens(parser::TSqlLexerBase& lexer, antlr4::CommonTokenStream& stream, const Decoded& d,
                   bool initialQuotedIdentifiers, ParseResult& r) {
    // ---- TSqlParser.GetTokenStream: lexer errors -> empty script (no token stream)
    if (lexer.Error()) {
        r.errors.push_back(LexerErrorToParseError(*lexer.Error(), d));
        r.script = r.factory->CreateFragment<ast::TSqlScript>();
        r.script->ScriptTokenStream = nullptr;
        return false;
    }

    // ---- script token stream with SqlScriptDOM positions; parser-facing token types
    const auto& all = stream.getTokens();
    r.tokens->reserve(all.size());
    for (antlr4::Token* t : all) {
        ast::TSqlParserToken pt;
        size_t start = t->getType() == antlr4::Token::EOF ? d.utf16.size() - 1 : t->getStartIndex();
        start = std::min(start, d.utf16.size() - 1);
        pt.TokenType = t->getType() == antlr4::Token::EOF ? ast::TSqlTokenType::EndOfFile
                                                          : static_cast<ast::TSqlTokenType>(t->getType());
        pt.Offset = d.utf16[start];
        pt.Line = d.line[start];
        pt.Column = d.column[start];
        if (t->getType() != antlr4::Token::EOF) pt.Text = t->getText();
        auto* ct = static_cast<antlr4::CommonToken*>(t);
        if (pt.TokenType == ast::TSqlTokenType::AsciiStringOrQuotedIdentifier) {
            // TSqlWhitespaceTokenFilter: ConvertStringToIdentifier = QUOTED_IDENTIFIER setting
            pt.ConvertStringToIdentifier = initialQuotedIdentifiers;
            ct->setType(static_cast<size_t>(initialQuotedIdentifiers ? ast::TSqlTokenType::QuotedIdentifier
                                                                     : ast::TSqlTokenType::AsciiStringLiteral));
        }
        // TSqlWhitespaceTokenFilter: the parser skips these token types (by type, not by rule)
        if (pt.TokenType == ast::TSqlTokenType::WhiteSpace || pt.TokenType == ast::TSqlTokenType::SingleLineComment ||
            pt.TokenType == ast::TSqlTokenType::MultilineComment)
            ct->setChannel(antlr4::Token::HIDDEN_CHANNEL);
        ct->setLine(static_cast<size_t>(pt.Line));
        ct->setCharPositionInLine(static_cast<size_t>(pt.Column));
        r.tokens->push_back(std::move(pt));
    }
    r.factory->SetTokenStream(r.tokens.get());

    return true;
}

std::vector<std::unique_ptr<antlr4::Token>> VisibleTokens(const std::vector<antlr4::Token*>& all) {
    // The parser sees the default-channel tokens only (TSqlWhitespaceTokenFilter); they keep their
    // full-stream indexes, and the parser looks up hidden tokens in the full list.
    std::vector<std::unique_ptr<antlr4::Token>> visible;
    for (antlr4::Token* t : all)
        if (t->getChannel() == antlr4::Token::DEFAULT_CHANNEL) visible.push_back(std::make_unique<FullStreamIndexToken>(t));
    return visible;
}

void ReportEscapedException(parser::TSql80ParserBase& parser, antlr4::TokenStream& parserStream, ParseResult& r) {
    try {
        throw;
    } catch (TSqlParseErrorException& exception) {
        if (!exception.DoNotLog) r.errors.push_back(exception.ParseError);
    } catch (antlr4::NoViableAltException& exception) {
        r.errors.push_back(GetUnexpectedTokenError(parser.ErrorToken(exception)));
    } catch (antlr4::InputMismatchException& exception) {
        r.errors.push_back(GetUnexpectedTokenError(parser.ErrorToken(exception)));
    } catch (antlr4::RecognitionException&) {
        r.errors.push_back(GetUnexpectedTokenError(parserStream.LT(1)));
    } catch (std::exception&) {
        TokenPosition p = PositionOf(parserStream.LT(1));
        r.errors.push_back(MakeParseError("SQL46001", p.Offset, p.Line, p.Column, TSqlParserResource::SQL46001Message));
    }
}

void FinishParse(ast::TSqlScript* result, SqlVersion version, ParseResult& r) {
    // TSql130/TSql140Parser.Parse have no streaming-job hot swap
    if (version != SqlVersion::Sql130 && version != SqlVersion::Sql140)
        HotswapCreateExternalStreamingJobStatements(result, *r.factory);
    if (result != nullptr) {
        VersioningVisitor versioningVisitor(SqlEngineType::All, version);
        result->Accept(&versioningVisitor);
        for (const ParseError& p : versioningVisitor.GetErrors()) r.errors.push_back(p);
    }
    r.script = result;   // null when an error escaped every recovering rule (as in C#)
}

// One per built grammar (ParseGrammar.cpp.in); the list is generated by CMake.
#define TSQL_GRAMMAR(G, V) ParseResult Parse##G(std::string_view sql, bool initialQuotedIdentifiers);
#include "tsql_parser_grammars.inc"
#undef TSQL_GRAMMAR

}  // namespace detail

namespace {

struct GrammarEntry {
    SqlVersion version;
    const char* name;
};

constexpr GrammarEntry kGrammars[] = {
    {SqlVersion::Sql130, "TSql130"}, {SqlVersion::Sql140, "TSql140"}, {SqlVersion::Sql150, "TSql150"},
    {SqlVersion::Sql160, "TSql160"}, {SqlVersion::Sql170, "TSql170"}, {SqlVersion::Sql180, "TSql180"},
    {SqlVersion::SqlFabricDW, "TSqlFabricDW"},
};

using ParseFn = ParseResult (*)(std::string_view, bool);

ParseFn ParserFor(SqlVersion version) {
#define TSQL_GRAMMAR(G, V) \
    if (version == SqlVersion::V) return &detail::Parse##G;
#include "tsql_parser_grammars.inc"
#undef TSQL_GRAMMAR
    return nullptr;
}

}  // namespace

const char* GrammarName(SqlVersion version) {
    for (const GrammarEntry& g : kGrammars)
        if (g.version == version) return g.name;
    return nullptr;
}

bool SqlVersionFromGrammarName(std::string_view name, SqlVersion& version) {
    for (const GrammarEntry& g : kGrammars)
        if (name == g.name) {
            version = g.version;
            return true;
        }
    return false;
}

bool IsParserAvailable(SqlVersion version) { return ParserFor(version) != nullptr; }

ParseResult parse(std::string_view sql, SqlVersion version, bool initialQuotedIdentifiers) {
    ParseFn fn = ParserFor(version);
    if (fn == nullptr) {
        const char* name = GrammarName(version);
        throw std::invalid_argument(std::string("tsql::parse: no parser for ") +
                                    (name ? std::string(name) + " in this build (TSQL_PARSER_GRAMMARS)"
                                          : "SqlVersion " + std::to_string(static_cast<int>(version))));
    }
    return fn(sql, initialQuotedIdentifiers);
}

}  // namespace tsql
