// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql80ParserBaseInternal.cs
#include "TSql80ParserBase.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <stack>

namespace tsql::parser {

namespace {
[[maybe_unused]] constexpr size_t TT(ast::TSqlTokenType t) { return static_cast<size_t>(t); }
using TK = ast::TSqlTokenType;
[[maybe_unused]] constexpr size_t kEOF = antlr4::Token::EOF;
}  // namespace

TSql80ParserBase::TSql80ParserBase(antlr4::TokenStream* input) : antlr4::Parser(input) {
    setErrorHandler(std::make_shared<TSqlBailErrorStrategy>());
    removeErrorListeners();
    setBuildParseTree(false);
}

void TSql80ParserBase::InitializeForNewInput(const ast::ScriptTokenStream* tokens,
                                              const std::vector<antlr4::Token*>* fullTokens,
                                              std::vector<ParseError>* errors, ast::FragmentFactory* factory,
                                              bool initialQuotedIdentifiersOn) {
    // swap in the simulator with ANTLR 2 no-viable-alternative semantics (same ATN/DFA cache)
    auto* sim = getInterpreter<antlr4::atn::ParserATNSimulator>();
    auto* ours = new TSqlParserATNSimulator(this, sim->atn, sim->decisionToDFA, sim->getSharedContextCache());
    delete _interpreter;
    _interpreter = ours;
    // SLL prediction: SqlScriptDOM's ANTLR 2 parser decided with two tokens of lookahead plus
    // ordered syntactic predicates, never full-context lookahead. Against full LL: identical on
    // every valid corpus, 7 of 1,914 broken scripts reported an error at another token, and full
    // LL took up to 20x longer on scripts with errors.
    ours->setPredictionMode(antlr4::atn::PredictionMode::SLL);

    _scriptTokens = tokens;
    _fullTokens = fullTokens;
    _parseErrors = errors;
    _fragmentFactory = factory;
    _initialQuotedIdentifiersOn = initialQuotedIdentifiersOn;
    _quotedIdentifier = initialQuotedIdentifiersOn;
}

void TSql80ParserBase::ResetQuotedIdentifiersSettingToInitial() { SetQuotedIdentifier(_initialQuotedIdentifiersOn); }

void TSql80ParserBase::SetQuotedIdentifier(bool on) {
    // TSqlWhitespaceTokenFilter decides an AsciiStringOrQuotedIdentifier token's type when the
    // parser fetches it, from the QUOTED_IDENTIFIER setting at that moment. SqlScriptDOM changes the
    // setting in actions that run before the next token is fetched (after SET QUOTED_IDENTIFIER's
    // ON/OFF and after GO), so retyping every token not yet consumed gives the same types.
    if (on == _quotedIdentifier) return;
    _quotedIdentifier = on;
    const size_t type = static_cast<size_t>(on ? TK::QuotedIdentifier : TK::AsciiStringLiteral);
    for (size_t i = _input->index(); i < _input->size(); ++i) {
        antlr4::Token* t = _input->get(i);
        const size_t full = t->getTokenIndex();
        if (full < _scriptTokens->size() && (*_scriptTokens)[full].TokenType == TK::AsciiStringOrQuotedIdentifier)
            static_cast<antlr4::CommonToken*>(t)->setType(type);
    }
}

// C# dereferences the token, then the fragment only for a valid index; a null either way is a
// NullReferenceException there (internal error 46001), e.g. CREATE OR ALTER PROCEDURE whose body has
// a syntax error: the statement is dropped and createOrAlterStatements extends it to CREATE.
void TSql80ParserBase::UpdateTokenInfo(ast::TSqlFragment* fragment, antlr4::Token* token) {
    if (token == nullptr) throw NullReferenceException();
    size_t tokenIndex = token->getTokenIndex();
    if (tokenIndex != INVALID_INDEX) {
        if (fragment == nullptr) throw NullReferenceException();
        fragment->UpdateTokenInfo(static_cast<int>(tokenIndex), static_cast<int>(tokenIndex));
    }
}

void TSql80ParserBase::CreateIdentifierFromLabel(antlr4::Token* token, ast::Identifier* identifier,
                                                  ast::MultiPartIdentifier* multiPartIdentifier) {
    std::string tokenText = token ? TokenText(token) : std::string();
    if (token == nullptr || tokenText.empty()) throw ::tsql::parser::GetUnexpectedTokenErrorException(token);
    std::string identifierName =
        (!tokenText.empty() && tokenText.back() == ':') ? tokenText.substr(0, tokenText.size() - 1) : tokenText;
    identifier->SetIdentifier(identifierName);
    UpdateTokenInfo(identifier, token);
    AddAndUpdateTokenInfo(multiPartIdentifier, multiPartIdentifier->Identifiers, identifier);
}

static std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string TSql80ParserBase::DecodeAsciiStringLiteral(CsStr encoded) {
    const std::string encodedValue(encoded.get());
    // quotes are ASCII, so stripping one byte at each end equals the UTF-16 Substring
    std::string valueWithoutQuotes = encodedValue.substr(1, encodedValue.size() - 2);
    if (encodedValue[0] == '"') return ReplaceAll(valueWithoutQuotes, "\"\"", "\"");
    return ReplaceAll(valueWithoutQuotes, "''", "'");
}

std::string TSql80ParserBase::DecodeUnicodeStringLiteral(CsStr encoded) {
    const std::string encodedValue(encoded.get());
    return ReplaceAll(encodedValue.substr(2, encodedValue.size() - 3), "''", "'");
}

bool TSql80ParserBase::IsAsciiStringLob(CsStr asciiValue) { return Utf16Len(asciiValue.get()) > 8000; }
bool TSql80ParserBase::IsUnicodeStringLob(CsStr unicodeValue) { return Utf16Len(unicodeValue.get()) > 8000; }
bool TSql80ParserBase::IsBinaryLiteralLob(CsStr binaryValue) { return Utf16Len(binaryValue.get()) - 2 > 16000; }

void TSql80ParserBase::AddParseError(const ParseError& parseError) { _parseErrors->push_back(parseError); }

void TSql80ParserBase::ConsumeUntil(std::initializer_list<size_t> types) {
    for (size_t t = LA(1); t != kEOF; t = LA(1)) {
        if (std::find(types.begin(), types.end(), t) != types.end()) return;
        consume();
    }
}

void TSql80ParserBase::RecoverAtStatementLevel(int statementStartLine, int statementStartColumn) {
    ConsumeUntil({TT(TK::Go), TT(TK::Semicolon), TT(TK::Create), TT(TK::Alter)});
    int nextTokenLine = static_cast<int>(LT(1)->getLine());
    int nextTokenColumn = static_cast<int>(LT(1)->getCharPositionInLine());
    if (nextTokenLine == statementStartLine && nextTokenColumn == statementStartColumn) {
        // phase-one re-entry (PhaseOneBatchException) is not part of the pilot
        consume();
    }
}

void TSql80ParserBase::RecoverAtBatchLevel() { ConsumeUntil({TT(TK::Go)}); }

void TSql80ParserBase::ThrowPartialAstIfPhaseOne(ast::TSqlStatement*) {
    // PhaseOne is always false in the pilot (PhaseOneParse / TryParseSqlModuleObjectName are not ported)
}

bool TSql80ParserBase::NextTokenMatches(const std::string& keyword) {
    return LA(1) != kEOF && EqualsIgnoreCase(LT(1)->getText(), keyword);
}

bool TSql80ParserBase::NextTokenMatches(const std::string& keyword, int which) {
    return LA(which) != kEOF && EqualsIgnoreCase(LT(which)->getText(), keyword);
}

void TSql80ParserBase::AddBinaryExpression(ast::ScalarExpression*& result, ast::ScalarExpression* expression,
                                            ast::BinaryExpressionType type) {
    auto* binaryExpression = CreateFragment<ast::BinaryExpression>();
    binaryExpression->set_FirstExpression(result);
    binaryExpression->set_SecondExpression(expression);
    binaryExpression->set_BinaryExpressionType(type);
    result = binaryExpression;
}

void TSql80ParserBase::AddBinaryExpression(ast::BooleanExpression*& result, ast::BooleanExpression* expression,
                                            ast::BooleanBinaryExpressionType type) {
    auto* binaryExpression = CreateFragment<ast::BooleanBinaryExpression>();
    binaryExpression->set_FirstExpression(result);
    binaryExpression->set_SecondExpression(expression);
    binaryExpression->set_BinaryExpressionType(type);
    result = binaryExpression;
}

ast::Identifier* TSql80ParserBase::GetEmptyIdentifier(antlr4::Token* token) {
    auto* identifier = CreateFragment<ast::Identifier>();
    UpdateTokenInfo(identifier, token);
    identifier->SetIdentifier(std::string());
    return identifier;
}

void TSql80ParserBase::CheckXmlForClauseOptionDuplication(ast::XmlForClauseOptions current,
                                                           ast::XmlForClauseOptions newOption, antlr4::Token* token) {
    using X = ast::XmlForClauseOptions;
    if ((current & newOption) != X{}) throw GetUnexpectedTokenErrorException(token);
    if ((newOption & X::ElementsAll) != X{} && (current & X::ElementsAll) != X{})
        throw GetUnexpectedTokenErrorException(token);
}

void TSql80ParserBase::AddIdentifierToListWithCheck(std::vector<ast::Identifier*>& list, ast::Identifier* item,
                                                     int max) {
    if (static_cast<int>(list.size()) == max) {
        // static GetUnexpectedTokenErrorException(Identifier) needs the token stream: use the thread's
        auto* toks = CurrentScriptTokens();
        std::string text = item->QuoteType != ast::QuoteType::NotQuoted ? ast::Identifier::EncodeIdentifier(item->Value.value_or(""))
                                                                        : item->Value.value_or("");
        int offset = 0, line = 1, col = 1;
        if (toks && item->FirstTokenIndex >= 0) {
            const auto& t = (*toks)[static_cast<size_t>(item->FirstTokenIndex)];
            offset = t.Offset; line = t.Line; col = t.Column;
        }
        throw TSqlParseErrorException(
            ::tsql::parser::CreateParseError("SQL46010", offset, line, col, TSqlParserResource::SQL46010Message, text));
    }
    list.push_back(item);
}

void TSql80ParserBase::CheckOptionDuplication(int64_t& encountered, int newOption, ast::TSqlFragment* vOption) {
    CheckOptionDuplication(encountered, newOption, GetFirstToken(vOption));
}

void TSql80ParserBase::CheckOptionDuplication(int64_t& encountered, int newOption, antlr4::Token* token) {
    int64_t newOptionBit = (int64_t{1} << newOption);
    if ((encountered & newOptionBit) == newOptionBit)
        ThrowParseErrorException("SQL46049", token, TSqlParserResource::SQL46049Message, TokenText(token));
    encountered |= newOptionBit;
}

void TSql80ParserBase::CheckOptionDuplication(uint64_t& encountered, int newOption, ast::TSqlFragment* vOption) {
    CheckOptionDuplication(encountered, newOption, GetFirstToken(vOption));
}

void TSql80ParserBase::CheckOptionDuplication(uint64_t& encountered, int newOption, antlr4::Token* token) {
    uint64_t newOptionBit = (uint64_t{1} << newOption);
    if ((encountered & newOptionBit) == newOptionBit)
        ThrowParseErrorException("SQL46049", token, TSqlParserResource::SQL46049Message, TokenText(token));
    encountered |= newOptionBit;
}

ast::IdentifierOrValueExpression* TSql80ParserBase::IdentifierOrValueExpression(ast::Identifier* identifier) {
    auto* v = CreateFragment<ast::IdentifierOrValueExpression>();
    v->set_Identifier(identifier);
    return v;
}

ast::IdentifierOrValueExpression* TSql80ParserBase::IdentifierOrValueExpression(ast::ValueExpression* valueExpression) {
    auto* v = CreateFragment<ast::IdentifierOrValueExpression>();
    v->set_ValueExpression(valueExpression);
    return v;
}

ast::OdbcLiteralType TSql80ParserBase::ParseOdbcLiteralType(antlr4::Token* token) {
    if (TryMatch(token, CodeGenerationSupporter::T)) return ast::OdbcLiteralType::Time;
    if (TryMatch(token, CodeGenerationSupporter::D)) return ast::OdbcLiteralType::Date;
    if (TryMatch(token, CodeGenerationSupporter::TS)) return ast::OdbcLiteralType::Timestamp;
    if (TryMatch(token, CodeGenerationSupporter::Guid)) return ast::OdbcLiteralType::Guid;
    throw GetUnexpectedTokenErrorException(token);
}

ast::OptimizerHintKind TSql80ParserBase::ParseJoinOptimizerHint(antlr4::Token* token) {
    std::string t = AsciiUpper(token->getText());
    if (t == CodeGenerationSupporter::Merge) return ast::OptimizerHintKind::MergeJoin;
    if (t == CodeGenerationSupporter::Hash) return ast::OptimizerHintKind::HashJoin;
    if (t == CodeGenerationSupporter::Loop) return ast::OptimizerHintKind::LoopJoin;
    throw GetUnexpectedTokenErrorException(token);
}

ast::OptimizerHintKind TSql80ParserBase::ParseUnionOptimizerHint(antlr4::Token* token) {
    std::string t = AsciiUpper(token->getText());
    if (t == CodeGenerationSupporter::Concat) return ast::OptimizerHintKind::ConcatUnion;
    if (t == CodeGenerationSupporter::Hash) return ast::OptimizerHintKind::HashUnion;
    if (t == CodeGenerationSupporter::Merge) return ast::OptimizerHintKind::MergeUnion;
    if (t == CodeGenerationSupporter::Keep) return ast::OptimizerHintKind::KeepUnion;
    throw GetUnexpectedTokenErrorException(token);
}

// mark()/consume()/rewind() scans become LA(k) scans: same tokens, no input movement.
bool TSql80ParserBase::IsNextRuleSelectParenthesis() {
    if (LA(1) == TT(TK::LeftParenthesis) && LA(2) == TT(TK::Select)) return true;
    int openParens = 1;
    for (int k = 2;; ++k) {
        size_t t = LA(k);
        if (t == TT(TK::LeftParenthesis)) {
            ++openParens;
        } else if (t == TT(TK::RightParenthesis)) {
            if (--openParens == 0) return false;
        } else if (t == kEOF) {
            return false;
        } else if (t == TT(TK::Join) || t == TT(TK::Inner) || t == TT(TK::Full) || t == TT(TK::Cross) ||
                   t == TT(TK::Outer) || t == TT(TK::Union) || t == TT(TK::Except) || t == TT(TK::Intersect)) {
            if (openParens == 1) return true;
        }
    }
}

bool TSql80ParserBase::IsNextRuleBooleanParenthesis() {
    if (LA(1) != TT(TK::LeftParenthesis)) return false;
    int openParens = 1, caseDepth = 0, topmostSelect = 0;
    std::stack<int> iifParenLevels;
    bool pendingIIf = false;
    for (int k = 2;; ++k) {
        size_t t = LA(k);
        bool isIIfOpeningParen = pendingIIf && t == TT(TK::LeftParenthesis);
        pendingIIf = false;
        if (t == TT(TK::Identifier)) {
            const std::string text = LT(k)->getText();
            if (EqualsIgnoreCase(text, CodeGenerationSupporter::IIf)) {
                pendingIIf = true;
            } else if (EqualsIgnoreCase(text, CodeGenerationSupporter::RegexpLike)) {
                if (caseDepth == 0 && topmostSelect == 0 && iifParenLevels.empty()) return true;
            }
        } else if (t == TT(TK::LeftParenthesis)) {
            ++openParens;
            if (isIIfOpeningParen) iifParenLevels.push(openParens);
        } else if (t == TT(TK::RightParenthesis)) {
            if (openParens == topmostSelect) topmostSelect = 0;
            if (!iifParenLevels.empty() && iifParenLevels.top() == openParens) iifParenLevels.pop();
            if (--openParens == 0) return false;
        } else if (t == kEOF) {
            return false;
        } else if (t == TT(TK::And) || t == TT(TK::Or) || t == TT(TK::Not) || t == TT(TK::EqualsSign) ||
                   t == TT(TK::GreaterThan) || t == TT(TK::LessThan) || t == TT(TK::Bang) ||
                   t == TT(TK::MultiplyEquals) || t == TT(TK::RightOuterJoin) || t == TT(TK::Is) || t == TT(TK::In) ||
                   t == TT(TK::Like) || t == TT(TK::Between) || t == TT(TK::Contains) || t == TT(TK::FreeText) ||
                   t == TT(TK::Exists) || t == TT(TK::TSEqual) || t == TT(TK::Update)) {
            if (caseDepth == 0 && topmostSelect == 0 && iifParenLevels.empty()) return true;
        } else if (t == TT(TK::Case)) {
            ++caseDepth;
        } else if (t == TT(TK::End)) {
            --caseDepth;
        } else if (t == TT(TK::Select)) {
            if (topmostSelect == 0) topmostSelect = openParens;
        }
    }
}

void TSql80ParserBase::Match(antlr4::Token* token, const std::string& keyword) {
    if (!EqualsIgnoreCase(token->getText(), keyword))
        ThrowParseErrorException("SQL46005", token, TSqlParserResource::SQL46005Message, keyword, TokenText(token));
}

void TSql80ParserBase::Match(ast::Identifier* id, const std::string& constant) {
    if (!String_Equals(id->Value, constant, StringComparison::OrdinalIgnoreCase)) throw GetUnexpectedTokenErrorException(id);
}

void TSql80ParserBase::Match(ast::Identifier* id, const std::string& constant, antlr4::Token* tokenForError) {
    if (!String_Equals(id->Value, constant, StringComparison::OrdinalIgnoreCase))
        throw GetUnexpectedTokenErrorException(tokenForError);
}

void TSql80ParserBase::Match(antlr4::Token* token, const std::string& keyword, const std::string& alternate) {
    if (!EqualsIgnoreCase(token->getText(), keyword) && !EqualsIgnoreCase(token->getText(), alternate))
        throw GetUnexpectedTokenErrorException(token);
}

bool TSql80ParserBase::TryMatch(antlr4::Token* token, const std::string& keyword) {
    return EqualsIgnoreCase(token->getText(), keyword);
}

bool TSql80ParserBase::TryMatch(ast::Identifier* identifier, const std::string& keyword) {
    return String_Equals(identifier->Value, keyword, StringComparison::OrdinalIgnoreCase);
}

// TSql140ParserBaseInternal
bool TSql80ParserBase::TryMatch(ast::Literal* literal, const std::string& keyword) {
    return String_Equals(literal->Value, keyword, StringComparison::OrdinalIgnoreCase);
}

ast::SqlDataTypeOption TSql80ParserBase::ParseDataType(CsStr token) {
    using S = ast::SqlDataTypeOption;
    static const std::map<std::string, S> kTypes = {
        {"BIGINT", S::BigInt}, {"INTEGER", S::Int}, {"INT", S::Int}, {"SMALLINT", S::SmallInt},
        {"TINYINT", S::TinyInt}, {"BIT", S::Bit}, {"DEC", S::Decimal}, {"DECIMAL", S::Decimal},
        {"NUMERIC", S::Numeric}, {"MONEY", S::Money}, {"SMALLMONEY", S::SmallMoney}, {"FLOAT", S::Float},
        {"REAL", S::Real}, {"DATETIME", S::DateTime}, {"SMALLDATETIME", S::SmallDateTime}, {"CHARACTER", S::Char},
        {"CHAR", S::Char}, {"VARCHAR", S::VarChar}, {"TEXT", S::Text}, {"NCHAR", S::NChar},
        {"NCHARACTER", S::NChar}, {"NVARCHAR", S::NVarChar}, {"NTEXT", S::NText}, {"BINARY", S::Binary},
        {"VARBINARY", S::VarBinary}, {"IMAGE", S::Image}, {"CURSOR", S::Cursor}, {"SQL_VARIANT", S::Sql_Variant},
        {"TABLE", S::Table}, {"ROWVERSION", S::Rowversion}, {"TIMESTAMP", S::Timestamp},
        {"UNIQUEIDENTIFIER", S::UniqueIdentifier}};
    auto it = kTypes.find(AsciiUpper(token.get()));
    return it == kTypes.end() ? S::None : it->second;
}

void TSql80ParserBase::CheckIdentifierLength(ast::Identifier* value) {
    if (Str_Length(value->Value) > 128)   // Identifier.MaxIdentifierLength
        ThrowParseErrorException("SQL46095", value, TSqlParserResource::SQL46095Message, Str_Substring(value->Value, 0, 128));
}

void TSql80ParserBase::CheckIdentifierLiteralLength(ast::IdentifierLiteral* value) {
    if (Str_Length(value->Value) > 128)
        ThrowParseErrorException("SQL46095", value, TSqlParserResource::SQL46095Message, Str_Substring(value->Value, 0, 128));
}

void TSql80ParserBase::ThrowIfPercentValueOutOfRange(ast::ScalarExpression* expr) {
    if (auto* pExpr = dynamic_cast<ast::ParenthesisExpression*>(expr)) {
        ThrowIfPercentValueOutOfRange(pExpr->Expression);
    } else if (auto* unaryExpr = dynamic_cast<ast::UnaryExpression*>(expr)) {
        if (unaryExpr->UnaryExpressionType == ast::UnaryExpressionType::Negative)
            ThrowParseErrorException("SQL46094", expr, TSqlParserResource::SQL46094Message);
        else
            ThrowIfPercentValueOutOfRange(unaryExpr->Expression);
    } else if (auto* literalValue = dynamic_cast<ast::Literal*>(expr)) {
        auto lt = literalValue->LiteralType();
        if (lt == ast::LiteralType::Real || lt == ast::LiteralType::Numeric || lt == ast::LiteralType::Integer) {
            // Double.TryParse(NumberStyles.Float, InvariantCulture)
            const std::string v = Str_Trim(literalValue->Value);
            char* end = nullptr;
            double convertedValue = v.empty() ? 0 : std::strtod(v.c_str(), &end);
            bool ok = !v.empty() && end && *end == '\0';
            if (!ok || convertedValue < 0 || convertedValue > 100)
                ThrowParseErrorException("SQL46094", expr, TSqlParserResource::SQL46094Message);
        }
    }
}

void TSql80ParserBase::CheckSpecialColumn(ast::ColumnReferenceExpression* column) {
    if (column->ColumnType != ast::ColumnType::Regular && column->MultiPartIdentifier != nullptr &&
        column->MultiPartIdentifier->Count() >= 4)
        throw TSqlParseErrorException(CreateParseError("SQL46028", GetFirstToken(column), TSqlParserResource::SQL46028Message));
}

void TSql80ParserBase::CheckStarQualifier(ast::SelectStarExpression* column) {
    if (column->Qualifier != nullptr) {
        int count = column->Qualifier->Count();
        if (count >= 4)
            throw TSqlParseErrorException(CreateParseError("SQL46028", GetFirstToken(column), TSqlParserResource::SQL46028Message));
        if (count == 0 || (count >= 1 && !String_IsNullOrEmpty((*column->Qualifier)[count - 1]->Value))) return;
        ThrowParseErrorException("SQL46016", column, TSqlParserResource::SQL46016Message);
    }
}

void TSql80ParserBase::CheckTableNameExistsForColumn(ast::ColumnReferenceExpression* column, bool multiPartRequisite) {
    int count = column->MultiPartIdentifier == nullptr ? 0 : column->MultiPartIdentifier->Count();
    if (!multiPartRequisite) {
        if ((column->ColumnType == ast::ColumnType::Regular && count == 1) ||
            (column->ColumnType != ast::ColumnType::Regular && count == 0))
            return;
    }
    bool tableNameDefined = false;
    if (column->ColumnType == ast::ColumnType::Regular) {
        if (count >= 2 && !String_IsNullOrEmpty((*column->MultiPartIdentifier)[count - 2]->Value)) tableNameDefined = true;
    } else {
        if (count >= 1 && !String_IsNullOrEmpty((*column->MultiPartIdentifier)[count - 1]->Value)) tableNameDefined = true;
    }
    if (!tableNameDefined) ThrowParseErrorException("SQL46016", column, TSqlParserResource::SQL46016Message);
}

void TSql80ParserBase::CreateInternalError(const std::string&, const std::exception&) {
    TokenPosition p = PositionOf(LT(1));
    AddParseError(MakeParseError("SQL46001", p.Offset, p.Line, p.Column, TSqlParserResource::SQL46001Message));
}

void TSql80ParserBase::PutIdentifiersIntoFunctionCall(ast::FunctionCall* functionCall,
                                                       ast::MultiPartIdentifier* identifiers) {
    int count = identifiers->Count();
    functionCall->set_FunctionName((*identifiers)[count - 1]);
    if (count > 1) {
        auto* callTarget = CreateFragment<ast::MultiPartIdentifierCallTarget>();
        auto* multiPartIdentifier = CreateFragment<ast::MultiPartIdentifier>();
        for (int i = 0; i < count - 1; ++i)
            AddAndUpdateTokenInfo(multiPartIdentifier, multiPartIdentifier->Identifiers, (*identifiers)[i]);
        callTarget->set_MultiPartIdentifier(multiPartIdentifier);
        functionCall->set_CallTarget(callTarget);
    }
}

void TSql80ParserBase::NormalizeDatePartFunctionFirstParameter(ast::FunctionCall* functionCall) {
    if (functionCall == nullptr || functionCall->FunctionName == nullptr ||
        String_IsNullOrEmpty(functionCall->FunctionName->Value) || functionCall->Parameters.empty() ||
        !IsDatePartFunction(*functionCall->FunctionName->Value))
        return;
    auto* columnReference = dynamic_cast<ast::ColumnReferenceExpression*>(functionCall->Parameters[0]);
    if (columnReference == nullptr || columnReference->MultiPartIdentifier == nullptr ||
        columnReference->MultiPartIdentifier->Count() != 1)
        return;
    ast::Identifier* identifier = (*columnReference->MultiPartIdentifier)[0];
    auto* literal = CreateFragment<ast::IdentifierLiteral>();
    literal->set_Value(identifier->Value);
    literal->set_QuoteType(identifier->QuoteType);
    literal->UpdateTokenInfo(columnReference);
    functionCall->Parameters[0] = literal;
}

bool TSql80ParserBase::IsDatePartFunction(const std::string& functionName) {
    std::string f = AsciiUpper(functionName);
    for (const char* n : {CodeGenerationSupporter::DateAdd, CodeGenerationSupporter::DateBucket,
                          CodeGenerationSupporter::DateDiff, CodeGenerationSupporter::DateDiffBig,
                          CodeGenerationSupporter::DateName, CodeGenerationSupporter::DatePart,
                          CodeGenerationSupporter::DateTrunc})
        if (f == n) return true;
    return false;
}

void TSql80ParserBase::CreateSetClauseColumn(ast::AssignmentSetClause* setClause,
                                              ast::MultiPartIdentifier* multiPartIdentifier) {
    auto* column = CreateFragment<ast::ColumnReferenceExpression>();
    column->set_ColumnType(ast::ColumnType::Regular);
    column->set_MultiPartIdentifier(multiPartIdentifier);
    setClause->set_Column(column);
}

void TSql80ParserBase::ProcessNationalAndVarying(ast::SqlDataTypeReference* type, antlr4::Token* nationalToken,
                                                  bool isVarying) {
    using S = ast::SqlDataTypeOption;
    if (nationalToken != nullptr && isVarying) {
        if (type->SqlDataTypeOption == S::Char)
            type->set_SqlDataTypeOption(S::NVarChar);
        else
            ThrowParseErrorException("SQL46002", nationalToken, TSqlParserResource::SQL46002Message,
                                     GetSqlDataTypeName(type->SqlDataTypeOption));
    } else if (nationalToken != nullptr) {
        switch (type->SqlDataTypeOption) {
            case S::Char: type->set_SqlDataTypeOption(S::NChar); break;
            case S::Text: type->set_SqlDataTypeOption(S::NText); break;
            default:
                ThrowParseErrorException("SQL46003", nationalToken, TSqlParserResource::SQL46003Message,
                                         GetSqlDataTypeName(type->SqlDataTypeOption));
        }
    } else if (isVarying) {
        switch (type->SqlDataTypeOption) {
            case S::Binary: type->set_SqlDataTypeOption(S::VarBinary); break;
            case S::Char: type->set_SqlDataTypeOption(S::VarChar); break;
            case S::NChar: type->set_SqlDataTypeOption(S::NVarChar); break;
            default:
                ThrowParseErrorException("SQL46004", type, TSqlParserResource::SQL46004Message,
                                         GetSqlDataTypeName(type->SqlDataTypeOption));
        }
    }
}

std::string TSql80ParserBase::GetSqlDataTypeName(ast::SqlDataTypeOption type) {
    if (type == ast::SqlDataTypeOption::None) return TSqlParserResource::UserDefined;
    return ast::ToString(type);
}

void TSql80ParserBase::CheckSqlDataTypeParameters(ast::SqlDataTypeReference* dataType) {
    using S = ast::SqlDataTypeOption;
    switch (dataType->Parameters.size()) {
        case 0:
            break;
        case 1: {
            static const S kSingle[] = {S::Char, S::VarChar, S::NChar, S::NVarChar, S::Decimal, S::Float, S::Numeric,
                                        S::Binary, S::VarBinary, S::Time, S::DateTime2, S::DateTimeOffset, S::Vector};
            if (std::find(std::begin(kSingle), std::end(kSingle), dataType->SqlDataTypeOption) == std::end(kSingle))
                ThrowParseErrorException("SQL46008", dataType, TSqlParserResource::SQL46008Message,
                                         ast::ToString(dataType->SqlDataTypeOption));
            if (dataType->Parameters[0]->LiteralType() == ast::LiteralType::Max &&
                (dataType->SqlDataTypeOption == S::Char || dataType->SqlDataTypeOption == S::NChar ||
                 dataType->SqlDataTypeOption == S::Binary))
                ThrowIncorrectSyntaxErrorException(GetFirstToken(dataType->Parameters[0]));
            break;
        }
        case 2:
            if (dataType->SqlDataTypeOption != S::Decimal && dataType->SqlDataTypeOption != S::Numeric)
                ThrowParseErrorException("SQL46009", dataType, TSqlParserResource::SQL46009Message,
                                         ast::ToString(dataType->SqlDataTypeOption));
            break;
        default:
            break;
    }
}

bool TSql80ParserBase::IsTableReference(bool allowMultipleTableHints) {
    if (LA(1) != TT(TK::LeftParenthesis)) return true;
    if ((LA(2) == TT(TK::Identifier) || LA(2) == TT(TK::HoldLock)) &&
        (LA(3) == TT(TK::RightParenthesis) || allowMultipleTableHints)) {
        ast::TableHintKind hintKind;
        ast::QuoteType quote;
        std::string idValue = ast::Identifier::DecodeIdentifier(LT(2)->getText(), quote);
        if (TableHintOptionsHelper::Instance().TryParseOption(idValue, SqlVersionFlags::TSql80, hintKind)) return true;
    }
    return false;
}

void TSql80ParserBase::SetNameForDoublePrecisionType(ast::DataTypeReference* dataType, antlr4::Token* doubleToken,
                                                      antlr4::Token* precisionToken) {
    auto* identifier = CreateFragment<ast::Identifier>();
    identifier->set_Value(std::string(CodeGenerationSupporter::Float));
    UpdateTokenInfo(identifier, doubleToken);
    UpdateTokenInfo(identifier, precisionToken);
    dataType->set_Name(CreateFragment<ast::SchemaObjectName>());
    AddAndUpdateTokenInfo(dataType->Name, dataType->Name->Identifiers, identifier);
    UpdateTokenInfo(dataType, doubleToken);
    UpdateTokenInfo(dataType, precisionToken);
}

antlr4::Token* TSql80ParserBase::GetFirstToken(ast::TSqlFragment* fragment) {
    if (fragment->ScriptTokenStream != nullptr && fragment->FirstTokenIndex != ast::TSqlFragment::Uninitialized)
        return (*_fullTokens)[static_cast<size_t>(fragment->FirstTokenIndex)];
    return nullptr;
}

ParseError TSql80ParserBase::GetFaultTolerantUnexpectedTokenError(antlr4::Token* token,
                                                                   const antlr4::RecognitionException&, int lastOffset) {
    if (token == nullptr) {
        TokenPosition p = PositionOf(LT(1));
        return MakeParseError("SQL46001", lastOffset, p.Line, p.Column, TSqlParserResource::SQL46001Message);
    }
    return GetUnexpectedTokenError(token);
}

ParseError TSql80ParserBase::GetIncorrectSyntaxError(antlr4::Token* token) { return ::tsql::parser::GetIncorrectSyntaxError(token); }

void TSql80ParserBase::ThrowIncorrectSyntaxErrorException(ast::TSqlFragment* fragment) {
    ThrowIncorrectSyntaxErrorException(GetFirstToken(fragment));
}

void TSql80ParserBase::ThrowIncorrectSyntaxErrorException(antlr4::Token* token) {
    throw TSqlParseErrorException(::tsql::parser::GetIncorrectSyntaxError(token));
}

TSqlParseErrorException TSql80ParserBase::GetUnexpectedTokenErrorException() { return GetUnexpectedTokenErrorException(LT(1)); }
ParseError TSql80ParserBase::GetUnexpectedTokenError() { return GetUnexpectedTokenError(LT(1)); }
ParseError TSql80ParserBase::GetUnexpectedTokenError(antlr4::Token* token) { return ::tsql::parser::GetUnexpectedTokenError(token); }
TSqlParseErrorException TSql80ParserBase::GetUnexpectedTokenErrorException(antlr4::Token* token) {
    return ::tsql::parser::GetUnexpectedTokenErrorException(token);
}

TSqlParseErrorException TSql80ParserBase::GetUnexpectedTokenErrorException(ast::Identifier* identifier) {
    std::string text = identifier->QuoteType != ast::QuoteType::NotQuoted
                           ? ast::Identifier::EncodeIdentifier(identifier->Value.value_or(""))
                           : identifier->Value.value_or("");
    return TSqlParseErrorException(
        CreateParseError("SQL46010", GetFirstToken(identifier), TSqlParserResource::SQL46010Message, text));
}

antlr4::Token* TSql80ParserBase::ErrorToken(const antlr4::RecognitionException& exception) {
    antlr4::Token* offending = exception.getOffendingToken();
    auto* nva = dynamic_cast<const antlr4::NoViableAltException*>(&exception);
    if (nva == nullptr || offending == nullptr || nva->getStartToken() == nullptr) return offending;
    antlr4::Token* start = nva->getStartToken();
    int onChannel = 0;   // default-channel tokens after the decision start up to the offending one
    for (size_t i = start->getTokenIndex() + 1; i <= offending->getTokenIndex() && i < _fullTokens->size(); ++i)
        if ((*_fullTokens)[i]->getChannel() == antlr4::Token::DEFAULT_CHANNEL) ++onChannel;
    return onChannel < 2 ? start : offending;
}

int TSql80ParserBase::LastTokenOffset() { return PositionOf(LT(1)).Offset; }

// TSql80ParserBaseInternal.cs:1946 (12 C# lines)
void TSql80ParserBase::AddConstraintToColumn(ast::ConstraintDefinition* constraint, ast::ColumnDefinition* column) {
            // Special treatment for Default constraint, 
            // there can be at most one of it in a column definition.
            ast::DefaultConstraintDefinition* defaultConstraint = dynamic_cast<ast::DefaultConstraintDefinition*>(constraint);
            if (defaultConstraint != nullptr)
            {
                if (column->DefaultConstraint != nullptr)
                    ThrowParseErrorException("SQL46012", constraint, TSqlParserResource::SQL46012Message);

                column->set_DefaultConstraint(defaultConstraint);
            }
            else
                AddAndUpdateTokenInfo(column, column->Constraints, constraint);
        }

// TSql80ParserBaseInternal.cs:1304 (9 C# lines)
void TSql80ParserBase::CheckFillFactorRange(ast::Literal* value) {
            int convertedValue{};
            if (!Int32_TryParse(value->Value, NumberStyles::Integer, CultureInfo::InvariantCulture, convertedValue) ||
                convertedValue < 1 || convertedValue > 100)
            {
                ThrowParseErrorException("SQL46060", value, TSqlParserResource::SQL46060Message, value->Value);
            }
        }

// TSql80ParserBaseInternal.cs:2257 (9 C# lines)
void TSql80ParserBase::CheckForTemporaryFunction(ast::SchemaObjectName* name) {
            if (name->BaseIdentifier() != nullptr && name->BaseIdentifier()->Value != std::nullopt &&
                Str_StartsWith(name->BaseIdentifier()->Value, "#", StringComparison::Ordinal))
            {
                ThrowParseErrorException("SQL46093", name, TSqlParserResource::SQL46093Message, name->BaseIdentifier()->Value);
            }
        }

// TSql80ParserBaseInternal.cs:2267 (9 C# lines)
void TSql80ParserBase::CheckForTemporaryView(ast::SchemaObjectName* name) {
            if (name->BaseIdentifier() != nullptr && name->BaseIdentifier()->Value != std::nullopt &&
                Str_StartsWith(name->BaseIdentifier()->Value, "#", StringComparison::Ordinal))
            {
                ThrowParseErrorException("SQL46092", name, TSqlParserResource::SQL46092Message, name->BaseIdentifier()->Value);
            }
        }

// TSql80ParserBaseInternal.cs:1864 (3 C# lines)
void TSql80ParserBase::CheckIfValidLanguageHex(ast::Literal* inputValue) {
            //int hexValue;
            //string hexString = inputValue.Value.Remove(0, 2);
            //if (!Int32.TryParse(hexString, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out hexValue))
            //    ThrowParseErrorException("SQL46053", inputValue, TSqlParserResource.SQL46053Message, inputValue.Value);
            //else if (!(_languageInteger.Contains(hexValue)))
            //    ThrowParseErrorException("SQL46053", inputValue, TSqlParserResource.SQL46053Message, inputValue.Value);
        }

// TSql80ParserBaseInternal.cs:1847 (3 C# lines)
void TSql80ParserBase::CheckIfValidLanguageIdentifier(ast::Identifier* inputString) {
            //if (!(_languageIdentifier.Contains(inputString.Value)))
            //    ThrowParseErrorException("SQL46052", inputString, TSqlParserResource.SQL46052Message, inputString.Value);
        }

// TSql80ParserBaseInternal.cs:1854 (3 C# lines)
void TSql80ParserBase::CheckIfValidLanguageInteger(ast::Literal* inputValue) {
            //int integerValue;
            //if (!(Int32.TryParse(inputValue.Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out integerValue)))
            //    ThrowParseErrorException("SQL46053", inputValue, TSqlParserResource.SQL46053Message, inputValue.Value);
            //else if (!(_languageInteger.Contains(integerValue)))
            //    ThrowParseErrorException("SQL46053", inputValue, TSqlParserResource.SQL46053Message, inputValue.Value);
        }

// TSql80ParserBaseInternal.cs:1840 (3 C# lines)
void TSql80ParserBase::CheckIfValidLanguageString(ast::Literal* inputString) {
            //if (!(_languageString.Contains(inputString.Value)))
            //    ThrowParseErrorException("SQL46052", inputString, TSqlParserResource.SQL46052Message, inputString.Value);
        }

// TSql80ParserBaseInternal.cs:1826 (8 C# lines)
void TSql80ParserBase::CheckTwoPartNameForSchemaObjectName(ast::SchemaObjectName* name, CsStr statementType) {
            if (name->DatabaseIdentifier() != nullptr && !String_IsNullOrEmpty(name->DatabaseIdentifier()->Value))
            {
                throw TSqlParseErrorException(CreateParseError("SQL46021", GetFirstToken(name), TSqlParserResource::SQL46021Message, statementType));
            }
        }

// TSql80ParserBaseInternal.cs:1911 (7 C# lines)
ast::ScalarExpressionRestoreOption* TSql80ParserBase::CreateSimpleRestoreOptionWithValue(antlr4::Token* optionBeginning, ast::ScalarExpression* optionValue) {
            ast::ScalarExpressionRestoreOption* option = CreateFragment<ast::ScalarExpressionRestoreOption>();
            option->set_OptionKind(RestoreOptionWithValueHelper::Instance().ParseOption(optionBeginning));
            option->set_Value(optionValue);
            return option;
        }

// TSql80ParserBaseInternal.cs:1891 (17 C# lines)
ast::StopRestoreOption* TSql80ParserBase::CreateStopRestoreOption(antlr4::Token* optionBeginning, ast::ValueExpression* mark, ast::ValueExpression* afterClause) {
            ast::StopRestoreOption* option = CreateFragment<ast::StopRestoreOption>();

            if (TryMatch(optionBeginning, CodeGenerationSupporter::StopAtMark))
            {
                option->set_IsStopAt(true);
                option->set_OptionKind(ast::RestoreOptionKind::StopAt);
            }
            else
            {
                option->set_OptionKind(ast::RestoreOptionKind::Stop);
            }

            option->set_Mark(mark);
            if (afterClause != nullptr)
                option->set_After(afterClause);
            return option;
        }

// TSql80ParserBaseInternal.cs:1879 (4 C# lines)
bool TSql80ParserBase::IsStopAtBeforeMarkRestoreOption(antlr4::Token* token) {
            return TryMatch(token, CodeGenerationSupporter::StopAtMark) || TryMatch(token, CodeGenerationSupporter::StopBeforeMark);
        }

// TSql80ParserBaseInternal.cs:486 (12 C# lines)
bool TSql80ParserBase::NextTokenMatchesOneOf(const std::vector<std::string>& keywords) {
            if (LA(1) == antlr4::Token::EOF)
                return false;

            std::string text = LT(1)->getText();
            for (std::string keyword : keywords)
            {
                if (String_Equals(keyword, text, StringComparison::OrdinalIgnoreCase))
                    return true;
            }

            return false;
        }

// TSql80ParserBaseInternal.cs:1689 (12 C# lines)
ast::StatisticsOptionKind TSql80ParserBase::ParseCreateStatisticsWithOption(antlr4::Token* token) {
            { const std::string sw_(CsStr(Str_ToUpperInvariant(token->getText())).get()); if (sw_ == CodeGenerationSupporter::FullScan) {
                    return ast::StatisticsOptionKind::FullScan;} else if (sw_ == CodeGenerationSupporter::NoRecompute) {
                    return ast::StatisticsOptionKind::NoRecompute;} else {
                    throw TSqlParseErrorException(CreateParseError("SQL46018", token, TSqlParserResource::SQL46018Message, token->getText()));} }
        }

// TSql80ParserBaseInternal.cs:1243 (10 C# lines)
ast::IndexOptionKind TSql80ParserBase::ParseIndexLegacyWithOption(antlr4::Token* token) {
            ast::IndexOptionKind indexOption{};
            if (!IndexOptionHelper::Instance().TryParseOption(token, SqlVersionFlags::TSql80, indexOption))
            {
                ThrowParseErrorException("SQL46015", token, TSqlParserResource::SQL46015Message, token->getText());
            }

            return indexOption;
        }

// TSql80ParserBaseInternal.cs:1707 (12 C# lines)
ast::StatisticsOptionKind TSql80ParserBase::ParseSampleOptionsWithOption(antlr4::Token* token) {
            // Percent is already a reserved word.
            if (String_Compare(CodeGenerationSupporter::Rows, token->getText(), StringComparison::OrdinalIgnoreCase) == 0)
            {
                return ast::StatisticsOptionKind::SampleRows;
            }
            else
            {
                throw TSqlParseErrorException(CreateParseError("SQL46019", token, TSqlParserResource::SQL46019Message, token->getText()));
            }
        }

// TSql80ParserBaseInternal.cs:1726 (12 C# lines) (hand-fixed)
ast::TriggerEnforcement TSql80ParserBase::ParseTriggerEnforcement(antlr4::Token* token) {
    const std::string t = Str_ToUpperInvariant(token->getText());
    if (t == CodeGenerationSupporter::Enable) return ast::TriggerEnforcement::Enable;
    if (t == CodeGenerationSupporter::Disable) return ast::TriggerEnforcement::Disable;
    // antlr.NoViableAltException(token): "no viable alternative" at the token
    throw antlr4::NoViableAltException(this, _input, token, token, nullptr, _ctx, false);
}

// TSql80ParserBaseInternal.cs:1936 (9 C# lines)
void TSql80ParserBase::SetFunctionBodyStatement(ast::FunctionStatementBody* parent, ast::BeginEndBlockStatement* compoundStatement) {
            if (compoundStatement != nullptr)
            {
                ast::StatementList* statementList = CreateFragment<ast::StatementList>();
                AddAndUpdateTokenInfo(statementList, statementList->Statements, compoundStatement);
                parent->set_StatementList(statementList);
            }
        }

// TSql80ParserBaseInternal.cs:459 (5 C# lines) (hand-fixed)
void TSql80ParserBase::ThrowConstraintIfPhaseOne(ast::ConstraintDefinition*) {
    // PhaseOne is always false: TSqlParser.PhaseOneParse is not ported
}

// TSql80ParserBaseInternal.cs:504 (7 C# lines)
void TSql80ParserBase::ThrowIfEndOfFileOrBatch() {
            if ((LA(1) == antlr4::Token::EOF) || (LA(1) == static_cast<size_t>(ast::TSqlTokenType::Go)))
            {
                throw TSqlParseErrorException(nullptr, true);
            }
        }

// TSql80ParserBaseInternal.cs:1656 (8 C# lines)
void TSql80ParserBase::ThrowSyntaxErrorIfNotCreateAlterTable(IndexAffectingStatement statement, antlr4::Token* atToken) {
            if (statement != IndexAffectingStatement::CreateTable &&
                statement != IndexAffectingStatement::AlterTableAddElement)
            {
                ThrowIncorrectSyntaxErrorException(atToken);
            }
        }

// TSql80ParserBaseInternal.cs:1278 (22 C# lines) (hand-fixed)
void TSql80ParserBase::ThrowWrongIndexOptionError(IndexAffectingStatement statement, ast::TSqlFragment* option) {
    std::string optionName;
    if (option->FirstTokenIndex >= 0 && option->ScriptTokenStream != nullptr &&
        option->FirstTokenIndex < static_cast<int>(option->ScriptTokenStream->size()))
        optionName = (*option->ScriptTokenStream)[static_cast<size_t>(option->FirstTokenIndex)].Text;
    // TSql80ParserBaseInternal._indexOptionContainerStatementNames
    using S = IndexAffectingStatement;
    std::string statementName;
    switch (statement) {
        case S::AlterTableAddElement: statementName = "ALTER TABLE"; break;
        case S::AlterTableRebuildAllPartitions: statementName = "ALTER TABLE REBUILD PARTITION"; break;
        case S::AlterTableRebuildOnePartition: statementName = "ALTER TABLE REBUILD PARTITION"; break;
        case S::AlterIndexRebuildAllPartitions: statementName = "ALTER INDEX REBUILD PARTITION"; break;
        case S::AlterIndexRebuildOnePartition: statementName = "ALTER INDEX REBUILD PARTITION"; break;
        case S::AlterIndexSet: statementName = "ALTER INDEX"; break;
        case S::AlterIndexReorganize: statementName = "ALTER INDEX REORGANIZE"; break;
        case S::CreateColumnStoreIndex: statementName = "CREATE COLUMNSTORE INDEX"; break;
        case S::CreateIndex: statementName = "CREATE INDEX"; break;
        case S::CreateTable: statementName = "CREATE TABLE"; break;
        case S::CreateTableInlineIndex: statementName = "CREATE TABLE (inline index)"; break;
        case S::CreateType: statementName = "CREATE TYPE"; break;
        case S::CreateXmlIndex: statementName = "CREATE XML INDEX"; break;
        case S::CreateOrAlterFunction: statementName = "CREATE/ALTER FUNCTION"; break;
        case S::DeclareTableVariable: statementName = "DECLARE"; break;
        case S::CreateSpatialIndex: statementName = "CREATE SPATIAL INDEX"; break;
        case S::AlterTableAlterIndexRebuild: statementName = "ALTER TABLE ALTER INDEX REBUILD"; break;
        case S::AlterTableAlterColumn: statementName = "ALTER TABLE ALTER COLUMN"; break;
        case S::AlterIndexResume: statementName = "ALTER INDEX RESUME"; break;
        default: break;
    }
    ThrowParseErrorException("SQL46057", option, TSqlParserResource::SQL46057Message, optionName, statementName);
}

// TSql80ParserBaseInternal.cs:1374 (4 C# lines)
void TSql80ParserBase::VerifyAllowedIndexOption(IndexAffectingStatement statement, ast::IndexOption* option) {
            VerifyAllowedIndexOption(statement, option, SqlVersionFlags::None);
        }

// TSql80ParserBaseInternal.cs:1387 (254 C# lines)
void TSql80ParserBase::VerifyAllowedIndexOption(IndexAffectingStatement statement, ast::IndexOption* option, SqlVersionFlags versionFlags) {
            bool invalidOption = false;

            if (option->OptionKind == ast::IndexOptionKind::FileStreamOn &&
                statement != IndexAffectingStatement::AlterTableAddElement)
            {
                invalidOption = true;
            }            

            if (option->OptionKind == ast::IndexOptionKind::BucketCount &&
                !(statement == IndexAffectingStatement::CreateTable || statement == IndexAffectingStatement::CreateTableInlineIndex ||
                  statement == IndexAffectingStatement::CreateType || statement == IndexAffectingStatement::AlterTableAlterIndexRebuild))   // StatementsWithBucketCount (hand-fixed)
            {
                invalidOption = true;
            }

            bool columnStoreIndexDataCompressionOptionLevels = false;
            bool indexDataCompressionOptionLevels = false;
            if (option->OptionKind == ast::IndexOptionKind::DataCompression)
            {
                ast::DataCompressionOption* dcOption = dynamic_cast<ast::DataCompressionOption*>(option);
                columnStoreIndexDataCompressionOptionLevels = dcOption->CompressionLevel == ast::DataCompressionLevel::ColumnStore ||
                               dcOption->CompressionLevel == ast::DataCompressionLevel::ColumnStoreArchive;

                indexDataCompressionOptionLevels = dcOption->CompressionLevel == ast::DataCompressionLevel::None ||
                                dcOption->CompressionLevel == ast::DataCompressionLevel::Row ||
                                dcOption->CompressionLevel == ast::DataCompressionLevel::Page;

                // Column store compression levels are only supported in 120 and above
                if ((versionFlags & SqlVersionFlags::TSql120AndAbove) == SqlVersionFlags{} && columnStoreIndexDataCompressionOptionLevels)
                {
                    invalidOption = true;
                }
            }

            // XML compression is only supported for 160 and above
            if (option->OptionKind == ast::IndexOptionKind::XmlCompression)
            {
                if ((versionFlags & SqlVersionFlags::TSql160AndAbove) == SqlVersionFlags{})
                {
                    invalidOption = true;
                }
            }

            switch (statement)
            {
                case IndexAffectingStatement::AlterIndexRebuildOnePartition:
                    if (option->OptionKind != ast::IndexOptionKind::SortInTempDB &&
                        option->OptionKind != ast::IndexOptionKind::MaxDop &&
                        option->OptionKind != ast::IndexOptionKind::DataCompression &&
                        option->OptionKind != ast::IndexOptionKind::XmlCompression &&
                        option->OptionKind != ast::IndexOptionKind::Resumable &&
                        option->OptionKind != ast::IndexOptionKind::MaxDuration &&
                        (option->OptionKind != ast::IndexOptionKind::Online || (versionFlags & SqlVersionFlags::TSql120AndAbove) == SqlVersionFlags{}))
                    {
                        invalidOption = true;
                    }
                    break;
                case IndexAffectingStatement::AlterTableRebuildOnePartition:
                    if (option->OptionKind != ast::IndexOptionKind::SortInTempDB &&
                        option->OptionKind != ast::IndexOptionKind::MaxDop &&
                        option->OptionKind != ast::IndexOptionKind::DataCompression &&
                        option->OptionKind != ast::IndexOptionKind::XmlCompression &&
                        (option->OptionKind != ast::IndexOptionKind::Online || (versionFlags & SqlVersionFlags::TSql120AndAbove) == SqlVersionFlags{})) // SPOIR
                    {
                        invalidOption = true;
                    }
                    break;
                case IndexAffectingStatement::AlterIndexRebuildAllPartitions:
                    if (option->OptionKind == ast::IndexOptionKind::DropExisting ||
                        option->OptionKind == ast::IndexOptionKind::LobCompaction ||
                        option->OptionKind == ast::IndexOptionKind::Order ||
                        option->OptionKind == ast::IndexOptionKind::OptimizeForSequentialKey)
                    {
                        invalidOption = true;
                    }
                    break;
                case IndexAffectingStatement::AlterTableRebuildAllPartitions:
                    if (option->OptionKind == ast::IndexOptionKind::DropExisting ||
                        option->OptionKind == ast::IndexOptionKind::LobCompaction ||
                        option->OptionKind == ast::IndexOptionKind::Order || 
                        option->OptionKind == ast::IndexOptionKind::Resumable ||
                        option->OptionKind == ast::IndexOptionKind::MaxDuration ||
                        option->OptionKind == ast::IndexOptionKind::OptimizeForSequentialKey)
                    {
                        invalidOption = true;
                    }
                    break;
                case IndexAffectingStatement::AlterIndexReorganize:
                    if (option->OptionKind != ast::IndexOptionKind::LobCompaction &&
                       (option->OptionKind != ast::IndexOptionKind::CompressAllRowGroups || (versionFlags & SqlVersionFlags::TSql130AndAbove) == SqlVersionFlags{} ))
                        invalidOption = true;
                    break;
                case IndexAffectingStatement::AlterIndexSet:
                    if (option->OptionKind != ast::IndexOptionKind::AllowRowLocks &&
                        option->OptionKind != ast::IndexOptionKind::AllowPageLocks &&
                        option->OptionKind != ast::IndexOptionKind::IgnoreDupKey &&
                        option->OptionKind != ast::IndexOptionKind::StatisticsNoRecompute &&
                        (option->OptionKind != ast::IndexOptionKind::CompressionDelay || (versionFlags & SqlVersionFlags::TSql130AndAbove) == SqlVersionFlags{} ) &&
                        (option->OptionKind != ast::IndexOptionKind::OptimizeForSequentialKey || (versionFlags & SqlVersionFlags::TSql150AndAbove) == SqlVersionFlags{} ))
                    {
                        invalidOption = true;
                    }
                    break;
                case IndexAffectingStatement::AlterIndexResume:
                    if (option->OptionKind != ast::IndexOptionKind::MaxDop &&
                        option->OptionKind != ast::IndexOptionKind::MaxDuration &&
                        option->OptionKind != ast::IndexOptionKind::WaitAtLowPriority)
                    {
                        invalidOption = true;
                    }
                    break;
                case IndexAffectingStatement::AlterTableAddElement:
                    if (option->OptionKind == ast::IndexOptionKind::DropExisting ||
                        option->OptionKind == ast::IndexOptionKind::LobCompaction ||
                        option->OptionKind == ast::IndexOptionKind::Order || 
                        ((versionFlags & SqlVersionFlags::TSql160AndAbove) == SqlVersionFlags{} && option->OptionKind == ast::IndexOptionKind::Resumable) ||
                        ((versionFlags & SqlVersionFlags::TSql120AndAbove) == SqlVersionFlags{} && option->OptionKind == ast::IndexOptionKind::MaxDuration))
                    {
                        invalidOption = true;
                    }
                    break;
                case IndexAffectingStatement::CreateTable:
                case IndexAffectingStatement::DeclareTableVariable:
                case IndexAffectingStatement::CreateOrAlterFunction:
                    if (option->OptionKind == ast::IndexOptionKind::SortInTempDB ||
                        option->OptionKind == ast::IndexOptionKind::Online ||
                        option->OptionKind == ast::IndexOptionKind::MaxDop ||
                        option->OptionKind == ast::IndexOptionKind::LobCompaction ||
                        option->OptionKind == ast::IndexOptionKind::DropExisting ||
                        option->OptionKind == ast::IndexOptionKind::Order ||
                        option->OptionKind == ast::IndexOptionKind::Resumable ||
                        option->OptionKind == ast::IndexOptionKind::MaxDuration)
                    {
                        invalidOption = true;
                    }
                    else if (option->OptionKind == ast::IndexOptionKind::DataCompression)
                    {
                        //Specifying COLUMNSTORE or COLUMNSTORE_ARCHIVE as the data compression for create table
                        //is a syntax error
                        if (columnStoreIndexDataCompressionOptionLevels)
                        {
                            invalidOption = true;
                        }
                    }
                    break;
                case IndexAffectingStatement::CreateColumnStoreIndex:
                    if (option->OptionKind == ast::IndexOptionKind::DataCompression)
                    {
                        if ((versionFlags & SqlVersionFlags::TSql120AndAbove) == SqlVersionFlags{} ||
                            indexDataCompressionOptionLevels)
                        {
                            invalidOption = true;
                        }
                    }
                    else if (option->OptionKind == ast::IndexOptionKind::XmlCompression)
                    {
                        invalidOption = true;
                    }
                    else if (option->OptionKind == ast::IndexOptionKind::SortInTempDB ||
                             option->OptionKind == ast::IndexOptionKind::Order ||
                             option->OptionKind == ast::IndexOptionKind::CompressionDelay)
                    {
                        if ((versionFlags & SqlVersionFlags::TSql130AndAbove) == SqlVersionFlags{})
                        {
                            invalidOption = true;
                        }
                    }
                    else if (option->OptionKind == ast::IndexOptionKind::Online)
                    {
                        if ((versionFlags & SqlVersionFlags::TSql140AndAbove) == SqlVersionFlags{})
                        {
                            invalidOption = true;
                        }
                    }
                    else if (option->OptionKind != ast::IndexOptionKind::DropExisting &&
                             option->OptionKind != ast::IndexOptionKind::MaxDop)
                    {
                        invalidOption = true;
                    }
                    break;
                case IndexAffectingStatement::CreateType:
                    if (option->OptionKind != ast::IndexOptionKind::IgnoreDupKey &&
                        option->OptionKind != ast::IndexOptionKind::BucketCount)
                        invalidOption = true;
                    break;
                case IndexAffectingStatement::CreateIndex:
                case IndexAffectingStatement::CreateTableInlineIndex:
                    if (option->OptionKind == ast::IndexOptionKind::LobCompaction ||
                        option->OptionKind == ast::IndexOptionKind::Order ||
                        ((versionFlags & SqlVersionFlags::TSql150AndAbove) == SqlVersionFlags{} && option->OptionKind == ast::IndexOptionKind::Resumable) ||
                        ((versionFlags & SqlVersionFlags::TSql150AndAbove) == SqlVersionFlags{} && option->OptionKind == ast::IndexOptionKind::MaxDuration))
                    {
                        invalidOption = true;
                    }
                    else if (option->OptionKind == ast::IndexOptionKind::DataCompression)
                    {
                        //Specifying COLUMNSTORE or COLUMNSTORE_ARCHIVE as the data compression for create (non-columnstore) index
                        //is a syntax error
                        if (columnStoreIndexDataCompressionOptionLevels)
                        {
                            invalidOption = true;
                        }
                    }
                    break;
                case IndexAffectingStatement::CreateXmlIndex:
                    if (option->OptionKind == ast::IndexOptionKind::DataCompression ||
                        option->OptionKind == ast::IndexOptionKind::LobCompaction ||
                        option->OptionKind == ast::IndexOptionKind::CompressAllRowGroups ||
                        option->OptionKind == ast::IndexOptionKind::CompressionDelay ||
                        option->OptionKind == ast::IndexOptionKind::Resumable ||
                        option->OptionKind == ast::IndexOptionKind::MaxDuration ||
                        option->OptionKind == ast::IndexOptionKind::OptimizeForSequentialKey)
                    {
                        invalidOption = true;
                    }
                    else if (option->OptionKind == ast::IndexOptionKind::IgnoreDupKey)
                    {
                        ast::IndexStateOption* indexStateOption = dynamic_cast<ast::IndexStateOption*>(option);
                        if (indexStateOption != nullptr)
                        {
                            invalidOption = indexStateOption->OptionState == ast::OptionState::On;
                        }
                    }
                    break;
                case IndexAffectingStatement::CreateSpatialIndex:
                    if (option->OptionKind == ast::IndexOptionKind::DataCompression)
                    {
                        if ((versionFlags & SqlVersionFlags::TSql110AndAbove) == SqlVersionFlags{} ||
                            columnStoreIndexDataCompressionOptionLevels)
                        {
                            invalidOption = true;
                        }
                    }
                    else if (option->OptionKind == ast::IndexOptionKind::LobCompaction ||
                             option->OptionKind == ast::IndexOptionKind::FileStreamOn ||
                             option->OptionKind == ast::IndexOptionKind::CompressAllRowGroups ||
                             option->OptionKind == ast::IndexOptionKind::CompressionDelay ||
                             option->OptionKind == ast::IndexOptionKind::Resumable ||
                             option->OptionKind == ast::IndexOptionKind::MaxDuration ||
                             option->OptionKind == ast::IndexOptionKind::XmlCompression ||
                             option->OptionKind == ast::IndexOptionKind::OptimizeForSequentialKey)
                    {
                        invalidOption = true;
                    }
                    break;
                case IndexAffectingStatement::AlterTableAlterIndexRebuild:
                    if ((versionFlags & SqlVersionFlags::TSql130AndAbove) == SqlVersionFlags{} ||
                        option->OptionKind != ast::IndexOptionKind::BucketCount)
                    {
                        invalidOption = true;
                    }
                    break;
                case IndexAffectingStatement::AlterTableAlterColumn:
                    if ((versionFlags & SqlVersionFlags::TSql130AndAbove) == SqlVersionFlags{} ||
                        option->OptionKind != ast::IndexOptionKind::Online)
                    {
                        invalidOption = true;
                    }
                    break;
                default:
                    assert(false); // Bug - unknown statement with index options
                    break;
            }

            if (invalidOption)
                ThrowWrongIndexOptionError(statement, option);
        }

// TSql80ParserBaseInternal.cs:2027 (7 C# lines)
void TSql80ParserBase::VerifyColumnDataType(ast::ColumnDefinition* column) {
            // If the scalarDataType is not parsed, the ColumnIdentifier has to be a timestamp.
            if ((column->DataType == nullptr) && !String_Equals(column->ColumnIdentifier->Value, CodeGenerationSupporter::TimeStamp, StringComparison::OrdinalIgnoreCase))
            {
                throw GetUnexpectedTokenErrorException();
            }
        }

}  // namespace tsql::parser
