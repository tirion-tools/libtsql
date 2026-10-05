// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql90ParserBaseInternal.cs
#pragma once

#include "TSql80ParserBase.h"

namespace tsql::parser {

class TSql90ParserBase : public TSql80ParserBase {
public:
    explicit TSql90ParserBase(antlr4::TokenStream* input) : TSql80ParserBase(input) {}

protected:
    // TSql90ParserBaseInternal.CheckForFormatFileOptionInOpenRowsetBulkMask
    static constexpr int64_t CheckForFormatFileOptionInOpenRowsetBulkMask =
        (int64_t{1} << static_cast<int>(ast::BulkInsertOptionKind::FormatFile)) |
        (int64_t{1} << static_cast<int>(ast::BulkInsertOptionKind::SingleBlob)) |
        (int64_t{1} << static_cast<int>(ast::BulkInsertOptionKind::SingleClob)) |
        (int64_t{1} << static_cast<int>(ast::BulkInsertOptionKind::SingleNClob));
    void CheckForFormatFileOptionInOpenRowsetBulk(int64_t encounteredOptions, ast::TSqlFragment* relatedFragment);   // TSql90ParserBaseInternal.cs:74
    using TSql80ParserBase::Match;
    using TSql80ParserBase::TryMatch;

    static void CheckForDistinctInWindowedAggregate(ast::FunctionCall* functionCall, antlr4::Token* distinctToken);
    static bool IsSys(ast::Identifier* identifier);
    static bool IsXml(ast::Identifier* identifier);
    void AddConstraintToComputedColumn(ast::ConstraintDefinition* constraint, ast::ColumnDefinition* column);   // TSql90ParserBaseInternal.cs:404
    ast::AuthenticationTypes AggregateAuthenticationType(ast::AuthenticationTypes current, ast::AuthenticationTypes newOption, antlr4::Token* token);   // TSql90ParserBaseInternal.cs:54
    ast::PortTypes AggregatePortType(ast::PortTypes current, ast::PortTypes newOption, antlr4::Token* token);   // TSql90ParserBaseInternal.cs:89
    void CheckCertificateOptionDupication(ast::CertificateOptionKinds current, ast::CertificateOptionKinds newOption, antlr4::Token* token);   // TSql90ParserBaseInternal.cs:106
    void CheckDmlTriggerActionDuplication(int current, ast::TriggerAction* vTriggerAction);   // TSql90ParserBaseInternal.cs:520
    void CheckIfEndpointOptionAllowed(ast::EndpointProtocolOptions current, ast::EndpointProtocolOptions newOption, ast::EndpointProtocol protocol, antlr4::Token* token);   // TSql90ParserBaseInternal.cs:119
    void CheckIfPayloadOptionAllowed(ast::PayloadOptionKinds current, ast::PayloadOptionKinds newOption, ast::EndpointType endpointType, antlr4::Token* token);   // TSql90ParserBaseInternal.cs:137
    ast::EventGroupContainer* CreateEventGroupContainer(ast::EventNotificationEventGroup eventGroupValue, antlr4::Token* token);   // TSql90ParserBaseInternal.cs:558
    ast::EventTypeContainer* CreateEventTypeContainer(ast::EventNotificationEventType eventTypeValue, antlr4::Token* token);   // TSql90ParserBaseInternal.cs:550
    ast::Literal* CreateIntLiteralFromNumericToken(antlr4::Token* token, int textOffset, int textLength);   // TSql90ParserBaseInternal.cs:452
    template <class TValue> TValue EnableDisableMatcher(antlr4::Token* token, TValue enableValue, TValue disableValue) {
            if (TryMatch(token, CodeGenerationSupporter::Enable))
                return enableValue;
            else
            {
                Match(token, CodeGenerationSupporter::Disable);
                return disableValue;
            }
        }
    IndexAffectingStatement GetAlterIndexStatementKind(ast::AlterIndexStatement* alterIndex);   // TSql90ParserBaseInternal.cs:420
    ast::Literal* GetIPv4FragmentFromDotNumberNumeric(antlr4::Token* token);   // TSql90ParserBaseInternal.cs:490
    ast::Literal* GetIPv4FragmentFromNumberDotNumeric(antlr4::Token* token);   // TSql90ParserBaseInternal.cs:501
    void GetIPv4FragmentsFromNumberDotNumberNumeric(antlr4::Token* token, ast::Literal*& frag1, ast::Literal*& frag2);   // TSql90ParserBaseInternal.cs:512
    bool IsStatementIsNext();   // TSql90ParserBaseInternal.cs:278
    ast::SecurityObjectKind ParseSecurityObjectKind(ast::Identifier* identifier);   // TSql90ParserBaseInternal.cs:157
    ast::SecurityObjectKind ParseSecurityObjectKind(ast::Identifier* identifier1, ast::Identifier* identifier2);   // TSql90ParserBaseInternal.cs:200
    ast::SecurityObjectKind ParseSecurityObjectKind(ast::Identifier* identifier1, ast::Identifier* identifier2, ast::Identifier* identifier3);   // TSql90ParserBaseInternal.cs:242
    ast::EncryptionAlgorithmPreference RecognizeAesOrRc4(ast::Identifier* id, antlr4::Token* tokenForError);   // TSql90ParserBaseInternal.cs:326
    void RecognizeAlterLoginSecAdminPasswordOption(antlr4::Token* token, ast::PasswordAlterPrincipalOption* astNode);   // TSql90ParserBaseInternal.cs:366
    ast::AuthenticationProtocol RecognizeAuthenticationProtocol(ast::Identifier* id, antlr4::Token* tokenForError);   // TSql90ParserBaseInternal.cs:346
    bool SplitNumericIntoIpParts(antlr4::Token* token, ast::Literal*& frag1, ast::Literal*& frag2);   // TSql90ParserBaseInternal.cs:461
    void ThrowIfInvalidListenerPortValue(ast::Literal* value);   // TSql90ParserBaseInternal.cs:532
    void ThrowIfMaxdopValueOutOfRange(ast::Literal* value);   // TSql90ParserBaseInternal.cs:541
    std::string Unquote(CsStr value);   // TSql90ParserBaseInternal.cs:288
    void UpdateDmlTriggerActionEncounteredOptions(int& encountered, ast::TriggerAction* vTriggerAction);   // TSql90ParserBaseInternal.cs:527
    // TSql90ParserBaseInternal.BulkInsertOptionsProhibitedInOpenRowset
    static constexpr int64_t BulkInsertOptionsProhibitedInOpenRowset =
        (int64_t{1} << static_cast<int>(ast::BulkInsertOptionKind::BatchSize)) |
        (int64_t{1} << static_cast<int>(ast::BulkInsertOptionKind::KilobytesPerBatch));
};

}  // namespace tsql::parser
