// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql80ParserBaseInternal.cs
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "antlr4-runtime.h"
#include "ParserRuntime.h"
#include "tsql/ast/ast.hpp"
#include "CsCompat.h"
#include "OptionsHelper.h"
#include "ParseError.h"
#include "ParserErrors.h"
#include "generated/support/CodeGenerationSupporter.h"
#include "generated/support/OptionHelpers.h"
#include "generated/support/ParserEnums.h"
#include "generated/support/TSqlParserResource.h"

/// Marker the converter emits for C# it could not translate (no definition: the build fails
/// until the action gets a hand override in tools/g2to4/overrides).
#define G2TO4_UNTRANSLATED(why) static_assert(false, why)

namespace tsql::parser {

namespace ast = ::tsql::ast;

/// Root of the parser-base chain (C#: TSql80ParserBaseInternal : antlr.LLkParser): the
/// ANTLR 2 runtime shims plus the TSql80 helpers. Each TSql<ver>ParserBase derives from the
/// previous version like the C# classes; only members some converted grammar uses are ported.
class TSql80ParserBase : public antlr4::Parser {
public:
    explicit TSql80ParserBase(antlr4::TokenStream* input);

    /// `fullTokens`: every token, whitespace and comments included, by full-stream index (the
    /// parser's own stream holds the default-channel tokens only).
    void InitializeForNewInput(const ast::ScriptTokenStream* tokens, const std::vector<antlr4::Token*>* fullTokens,
                               std::vector<ParseError>* errors, ast::FragmentFactory* factory,
                               bool initialQuotedIdentifiersOn);

protected:
    // ------------------------------------------------------------------ ANTLR 2 shims
    size_t LA(int i) { return _input->LA(i); }
    antlr4::Token* LT(int i) { return _input->LT(i); }

    template <class T>
    T* CreateFragment() { return _fragmentFactory->CreateFragment<T>(); }

    // ------------------------------------------------------------------ ANTLR 2 syntactic predicates
    /// inputState.guessing != 0: inside a speculative parse, where actions are skipped and
    /// exception handlers rethrow.
    bool Guessing() const { return _guessing != 0; }
    /// (alternative-prefix)=> : runs the prefix as a speculative parse from the current token and
    /// rewinds; true when it matched.
    template <class F>
    bool Speculate(F parsePrefix) {
        const size_t start = _input->index();
        const size_t state = getState();
        ++_guessing;
        bool ok = true;
        try {
            parsePrefix();
        } catch (antlr4::RecognitionException&) {
            ok = false;
        } catch (TSqlParseErrorException&) {
            ok = false;
        }
        --_guessing;
        _input->seek(start);
        setState(state);
        return ok;
    }

    // ------------------------------------------------------------------ TSql80ParserBaseInternal
    void ResetQuotedIdentifiersSettingToInitial();
    /// _tokenSource.QuotedIdentifier = on (SET QUOTED_IDENTIFIER ON|OFF)
    void SetQuotedIdentifier(bool on);
    static void UpdateTokenInfo(ast::TSqlFragment* fragment, antlr4::Token* token);
    static void CreateIdentifierFromLabel(antlr4::Token* token, ast::Identifier* identifier,
                                          ast::MultiPartIdentifier* multiPartIdentifier);

    template <class T, class U>
    static void AddAndUpdateTokenInfo(ast::TSqlFragment* node, std::vector<T*>& collection, U* item) {
        collection.push_back(item);
        if (node == nullptr) throw NullReferenceException();
        node->UpdateTokenInfo(item);
    }
    template <class T, class U>
    static void AddAndUpdateTokenInfo(ast::TSqlFragment* node, std::vector<T*>& collection,
                                      const std::vector<U*>& otherCollection) {
        for (U* item : otherCollection) AddAndUpdateTokenInfo(node, collection, item);
    }

    static std::string DecodeAsciiStringLiteral(CsStr encodedValue);
    static std::string DecodeUnicodeStringLiteral(CsStr encodedValue);
    static bool IsAsciiStringLob(CsStr asciiValue);
    static bool IsUnicodeStringLob(CsStr unicodeValue);
    static bool IsBinaryLiteralLob(CsStr binaryValue);

    void AddParseError(const ParseError& parseError);
    void RecoverAtStatementLevel(int statementStartLine, int statementStartColumn);
    void RecoverAtBatchLevel();
    void ThrowPartialAstIfPhaseOne(ast::TSqlStatement* statement);
    bool NextTokenMatches(const std::string& keyword);
    bool NextTokenMatches(const std::string& keyword, int which);
    void AddBinaryExpression(ast::ScalarExpression*& result, ast::ScalarExpression* expression,
                             ast::BinaryExpressionType type);
    void AddBinaryExpression(ast::BooleanExpression*& result, ast::BooleanExpression* expression,
                             ast::BooleanBinaryExpressionType type);
    ast::Identifier* GetEmptyIdentifier(antlr4::Token* token);
    static void CheckXmlForClauseOptionDuplication(ast::XmlForClauseOptions current, ast::XmlForClauseOptions newOption,
                                                   antlr4::Token* token);
    static void AddIdentifierToListWithCheck(std::vector<ast::Identifier*>& list, ast::Identifier* item, int max);
    void CheckOptionDuplication(int64_t& encountered, int newOption, ast::TSqlFragment* vOption);
    static void CheckOptionDuplication(int64_t& encountered, int newOption, antlr4::Token* token);
    void CheckOptionDuplication(uint64_t& encountered, int newOption, ast::TSqlFragment* vOption);
    static void CheckOptionDuplication(uint64_t& encountered, int newOption, antlr4::Token* token);
    ast::IdentifierOrValueExpression* IdentifierOrValueExpression(ast::Identifier* identifier);
    ast::IdentifierOrValueExpression* IdentifierOrValueExpression(ast::ValueExpression* valueExpression);
    static ast::OdbcLiteralType ParseOdbcLiteralType(antlr4::Token* token);
    static ast::OptimizerHintKind ParseJoinOptimizerHint(antlr4::Token* token);
    static ast::OptimizerHintKind ParseUnionOptimizerHint(antlr4::Token* token);
    bool IsNextRuleSelectParenthesis();
    bool IsNextRuleBooleanParenthesis();

    static void Match(antlr4::Token* token, const std::string& keyword);
    void Match(ast::Identifier* id, const std::string& constant);
    static void Match(ast::Identifier* id, const std::string& constant, antlr4::Token* tokenForError);
    static void Match(antlr4::Token* token, const std::string& keyword, const std::string& alternate);
    static bool TryMatch(antlr4::Token* token, const std::string& keyword);
    static bool TryMatch(ast::Identifier* identifier, const std::string& keyword);
    static bool TryMatch(ast::Literal* literal, const std::string& keyword);
    template <class... K>
    void MatchString(ast::Literal* literal, const K&... keywords) {
        for (const std::string& kw : {std::string(keywords)...})
            if (String_Equals(literal->Value, kw, StringComparison::OrdinalIgnoreCase)) return;
        ThrowIncorrectSyntaxErrorException(GetFirstToken(literal));
    }

    static ast::SqlDataTypeOption ParseDataType(CsStr token);
    void CheckIdentifierLength(ast::Identifier* value);
    void CheckIdentifierLiteralLength(ast::IdentifierLiteral* value);
    void ThrowIfPercentValueOutOfRange(ast::ScalarExpression* expr);
    void CheckSpecialColumn(ast::ColumnReferenceExpression* column);
    void CheckStarQualifier(ast::SelectStarExpression* column);
    void CheckTableNameExistsForColumn(ast::ColumnReferenceExpression* column, bool multiPartRequisite);
    void CreateInternalError(const std::string& entryPoint, const std::exception& exception);
    void PutIdentifiersIntoFunctionCall(ast::FunctionCall* functionCall, ast::MultiPartIdentifier* identifiers);
    void NormalizeDatePartFunctionFirstParameter(ast::FunctionCall* functionCall);
    static bool IsDatePartFunction(const std::string& functionName);
    void CreateSetClauseColumn(ast::AssignmentSetClause* setClause, ast::MultiPartIdentifier* multiPartIdentifier);
    void ProcessNationalAndVarying(ast::SqlDataTypeReference* type, antlr4::Token* nationalToken, bool isVarying);
    static std::string GetSqlDataTypeName(ast::SqlDataTypeOption type);
    void CheckSqlDataTypeParameters(ast::SqlDataTypeReference* dataType);
    bool IsTableReference(bool allowMultipleTableHints);
    void SetNameForDoublePrecisionType(ast::DataTypeReference* dataType, antlr4::Token* doubleToken,
                                       antlr4::Token* precisionToken);

    // error reporting
    antlr4::Token* GetFirstToken(ast::TSqlFragment* fragment);
    template <class... A>
    void ThrowParseErrorException(std::string_view identifier, ast::TSqlFragment* fragment, std::string_view messageTemplate,
                                  const A&... args) {
        throw TSqlParseErrorException(
            ::tsql::parser::CreateParseError(identifier, GetFirstToken(fragment), messageTemplate, args...));
    }
    template <class... A>
    static void ThrowParseErrorException(std::string_view identifier, antlr4::Token* token, std::string_view messageTemplate,
                                         const A&... args) {
        throw TSqlParseErrorException(::tsql::parser::CreateParseError(identifier, token, messageTemplate, args...));
    }
    template <class... A>
    static ParseError CreateParseError(std::string_view identifier, antlr4::Token* token, std::string_view messageTemplate,
                                       const A&... args) {
        return ::tsql::parser::CreateParseError(identifier, token, messageTemplate, args...);
    }
    ParseError GetFaultTolerantUnexpectedTokenError(antlr4::Token* token, const antlr4::RecognitionException& exception,
                                                    int lastOffset);
    static ParseError GetIncorrectSyntaxError(antlr4::Token* token);
    void ThrowIncorrectSyntaxErrorException(ast::TSqlFragment* fragment);
    static void ThrowIncorrectSyntaxErrorException(antlr4::Token* token);
    TSqlParseErrorException GetUnexpectedTokenErrorException();
    ParseError GetUnexpectedTokenError();
    static ParseError GetUnexpectedTokenError(antlr4::Token* token);
    static TSqlParseErrorException GetUnexpectedTokenErrorException(antlr4::Token* token);
    TSqlParseErrorException GetUnexpectedTokenErrorException(ast::Identifier* identifier);
public:
    /// antlr.RecognitionException.token. ANTLR 2 (k=2) throws NoViableAlt at LT(1) of the decision
    /// when the first two tokens already exclude every alternative; otherwise it enters an
    /// alternative and fails at the offending token, which is what ANTLR 4's ALL(*) reports.
    antlr4::Token* ErrorToken(const antlr4::RecognitionException& exception);

protected:
    /// _tokenSource.LastToken.Offset (only used when an exception carries no token).
    int LastTokenOffset();

    void AddConstraintToColumn(ast::ConstraintDefinition* constraint, ast::ColumnDefinition* column);   // TSql80ParserBaseInternal.cs:1946
    void CheckFillFactorRange(ast::Literal* value);   // TSql80ParserBaseInternal.cs:1304
    void CheckForTemporaryFunction(ast::SchemaObjectName* name);   // TSql80ParserBaseInternal.cs:2257
    void CheckForTemporaryView(ast::SchemaObjectName* name);   // TSql80ParserBaseInternal.cs:2267
    void CheckIfValidLanguageHex(ast::Literal* inputValue);   // TSql80ParserBaseInternal.cs:1864
    void CheckIfValidLanguageIdentifier(ast::Identifier* inputString);   // TSql80ParserBaseInternal.cs:1847
    void CheckIfValidLanguageInteger(ast::Literal* inputValue);   // TSql80ParserBaseInternal.cs:1854
    void CheckIfValidLanguageString(ast::Literal* inputString);   // TSql80ParserBaseInternal.cs:1840
    void CheckTwoPartNameForSchemaObjectName(ast::SchemaObjectName* name, CsStr statementType);   // TSql80ParserBaseInternal.cs:1826
    ast::ScalarExpressionRestoreOption* CreateSimpleRestoreOptionWithValue(antlr4::Token* optionBeginning, ast::ScalarExpression* optionValue);   // TSql80ParserBaseInternal.cs:1911
    ast::StopRestoreOption* CreateStopRestoreOption(antlr4::Token* optionBeginning, ast::ValueExpression* mark, ast::ValueExpression* afterClause);   // TSql80ParserBaseInternal.cs:1891
    bool IsStopAtBeforeMarkRestoreOption(antlr4::Token* token);   // TSql80ParserBaseInternal.cs:1879
    bool NextTokenMatchesOneOf(const std::vector<std::string>& keywords);   // TSql80ParserBaseInternal.cs:486
    /// params string[] form
    template <class... K>
    bool NextTokenMatchesOneOf(const char* first, const K&... rest) {
        return NextTokenMatchesOneOf(std::vector<std::string>{first, rest...});
    }
    ast::StatisticsOptionKind ParseCreateStatisticsWithOption(antlr4::Token* token);   // TSql80ParserBaseInternal.cs:1689
    ast::IndexOptionKind ParseIndexLegacyWithOption(antlr4::Token* token);   // TSql80ParserBaseInternal.cs:1243
    ast::StatisticsOptionKind ParseSampleOptionsWithOption(antlr4::Token* token);   // TSql80ParserBaseInternal.cs:1707
    ast::TriggerEnforcement ParseTriggerEnforcement(antlr4::Token* token);   // TSql80ParserBaseInternal.cs:1726
    void SetFunctionBodyStatement(ast::FunctionStatementBody* parent, ast::BeginEndBlockStatement* compoundStatement);   // TSql80ParserBaseInternal.cs:1936
    void ThrowConstraintIfPhaseOne(ast::ConstraintDefinition* constraint);   // TSql80ParserBaseInternal.cs:459
    void ThrowIfEndOfFileOrBatch();   // TSql80ParserBaseInternal.cs:504
    void ThrowSyntaxErrorIfNotCreateAlterTable(IndexAffectingStatement statement, antlr4::Token* atToken);   // TSql80ParserBaseInternal.cs:1656
    void ThrowWrongIndexOptionError(IndexAffectingStatement statement, ast::TSqlFragment* option);   // TSql80ParserBaseInternal.cs:1278
    void VerifyAllowedIndexOption(IndexAffectingStatement statement, ast::IndexOption* option);   // TSql80ParserBaseInternal.cs:1374
    void VerifyAllowedIndexOption(IndexAffectingStatement statement, ast::IndexOption* option, SqlVersionFlags versionFlags);   // TSql80ParserBaseInternal.cs:1387
    void VerifyColumnDataType(ast::ColumnDefinition* column);   // TSql80ParserBaseInternal.cs:2027

    bool PhaseOne = false;


protected:
    void ConsumeUntil(std::initializer_list<size_t> types);

    ast::FragmentFactory* _fragmentFactory = nullptr;
    std::vector<ParseError>* _parseErrors = nullptr;
    const ast::ScriptTokenStream* _scriptTokens = nullptr;
    const std::vector<antlr4::Token*>* _fullTokens = nullptr;
    bool _initialQuotedIdentifiersOn = true;
    bool _quotedIdentifier = true;   // setting the not-yet-consumed tokens are typed with
    int _guessing = 0;
};

}  // namespace tsql::parser
