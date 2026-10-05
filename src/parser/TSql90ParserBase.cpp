// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql90ParserBaseInternal.cs
#include "TSql90ParserBase.h"

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

void TSql90ParserBase::CheckForDistinctInWindowedAggregate(ast::FunctionCall* functionCall, antlr4::Token* distinctToken) {
    if (functionCall->UniqueRowFilter == ast::UniqueRowFilter::Distinct && functionCall->OverClause != nullptr &&
        distinctToken != nullptr)
        ThrowParseErrorException("SQL46086", distinctToken, TSqlParserResource::SQL46086Message);
}

bool TSql90ParserBase::IsSys(ast::Identifier* identifier) {
    return String_Equals(identifier->Value, CodeGenerationSupporter::Sys, StringComparison::OrdinalIgnoreCase);
}

bool TSql90ParserBase::IsXml(ast::Identifier* identifier) {
    return String_Equals(identifier->Value, CodeGenerationSupporter::Xml, StringComparison::OrdinalIgnoreCase);
}

// TSql90ParserBaseInternal.cs:404 (15 C# lines)
void TSql90ParserBase::AddConstraintToComputedColumn(ast::ConstraintDefinition* constraint, ast::ColumnDefinition* column) {
            bool constraintIsNullable = false;
            if ((dynamic_cast<ast::NullableConstraintDefinition*>(constraint) != nullptr))
            {
                ast::NullableConstraintDefinition* nullableConstraint = dynamic_cast<ast::NullableConstraintDefinition*>(constraint);
                constraintIsNullable = nullableConstraint->Nullable;
            }
            if (((!column->IsPersisted) && !((dynamic_cast<ast::UniqueConstraintDefinition*>(constraint) != nullptr))) ||
                (column->IsPersisted && constraintIsNullable))
            {
                ThrowParseErrorException("SQL46011", constraint, TSqlParserResource::SQL46011Message);
            }
            AddAndUpdateTokenInfo(column, column->Constraints, constraint);
        }

// TSql90ParserBaseInternal.cs:54 (7 C# lines)
ast::AuthenticationTypes TSql90ParserBase::AggregateAuthenticationType(ast::AuthenticationTypes current, ast::AuthenticationTypes newOption, antlr4::Token* token) {
            ast::AuthenticationTypes aggregatedOptions = current | newOption;

            if (aggregatedOptions == current)
                throw GetUnexpectedTokenErrorException(token);

            return aggregatedOptions;
        }

// TSql90ParserBaseInternal.cs:89 (7 C# lines)
ast::PortTypes TSql90ParserBase::AggregatePortType(ast::PortTypes current, ast::PortTypes newOption, antlr4::Token* token) {
            ast::PortTypes aggregatedOptions = current | newOption;

            if (aggregatedOptions == current)
                throw GetUnexpectedTokenErrorException(token);

            return aggregatedOptions;
        }

// TSql90ParserBaseInternal.cs:106 (5 C# lines)
void TSql90ParserBase::CheckCertificateOptionDupication(ast::CertificateOptionKinds current, ast::CertificateOptionKinds newOption, antlr4::Token* token) {
            if ((current & newOption) == newOption)
                throw GetUnexpectedTokenErrorException(token);
        }

// TSql90ParserBaseInternal.cs:520 (5 C# lines)
void TSql90ParserBase::CheckDmlTriggerActionDuplication(int current, ast::TriggerAction* vTriggerAction) {
            if ((current & (1 << static_cast<int>((vTriggerAction->TriggerActionType)))) != 0)
                ThrowParseErrorException("SQL46090", vTriggerAction, TSqlParserResource::SQL46090Message, ToString(vTriggerAction->TriggerActionType));
        }

// TSql90ParserBaseInternal.cs:119 (9 C# lines)
void TSql90ParserBase::CheckIfEndpointOptionAllowed(ast::EndpointProtocolOptions current, ast::EndpointProtocolOptions newOption, ast::EndpointProtocol protocol, antlr4::Token* token) {
            if ((current & newOption) == newOption)
                throw GetUnexpectedTokenErrorException(token);

            if ((protocol == ast::EndpointProtocol::Tcp && (newOption & ast::EndpointProtocolOptions::TcpOptions) != newOption) ||
                (protocol == ast::EndpointProtocol::Http && (newOption & ast::EndpointProtocolOptions::HttpOptions) != newOption))
                throw GetUnexpectedTokenErrorException(token);
        }

// TSql90ParserBaseInternal.cs:137 (12 C# lines)
void TSql90ParserBase::CheckIfPayloadOptionAllowed(ast::PayloadOptionKinds current, ast::PayloadOptionKinds newOption, ast::EndpointType endpointType, antlr4::Token* token) {
            if (endpointType == ast::EndpointType::TSql) // No options for TSql
                throw GetUnexpectedTokenErrorException(token);

            if ((endpointType == ast::EndpointType::Soap && (newOption & ast::PayloadOptionKinds::SoapOptions) != newOption) ||
                (endpointType == ast::EndpointType::DatabaseMirroring && (newOption & ast::PayloadOptionKinds::DatabaseMirroringOptions) != newOption) ||
                (endpointType == ast::EndpointType::ServiceBroker && (newOption & ast::PayloadOptionKinds::ServiceBrokerOptions) != newOption))
                throw GetUnexpectedTokenErrorException(token);

            if ((current & newOption) == newOption && newOption != ast::PayloadOptionKinds::WebMethod)
                throw GetUnexpectedTokenErrorException(token);
        }

// TSql90ParserBaseInternal.cs:558 (7 C# lines)
ast::EventGroupContainer* TSql90ParserBase::CreateEventGroupContainer(ast::EventNotificationEventGroup eventGroupValue, antlr4::Token* token) {
            ast::EventGroupContainer* eventGroupOption = CreateFragment<ast::EventGroupContainer>();
            eventGroupOption->set_EventGroup(eventGroupValue);
            UpdateTokenInfo(eventGroupOption, token);
            return eventGroupOption;
        }

// TSql90ParserBaseInternal.cs:550 (7 C# lines)
ast::EventTypeContainer* TSql90ParserBase::CreateEventTypeContainer(ast::EventNotificationEventType eventTypeValue, antlr4::Token* token) {
            ast::EventTypeContainer* eventTypeOption = CreateFragment<ast::EventTypeContainer>();
            eventTypeOption->set_EventType(eventTypeValue);
            UpdateTokenInfo(eventTypeOption, token);
            return eventTypeOption;
        }

// TSql90ParserBaseInternal.cs:452 (7 C# lines)
ast::Literal* TSql90ParserBase::CreateIntLiteralFromNumericToken(antlr4::Token* token, int textOffset, int textLength) {
            ast::IntegerLiteral* literal = CreateFragment<ast::IntegerLiteral>();
            UpdateTokenInfo(literal, token);
            literal->set_Value(Str_Substring(token->getText(), textOffset, textLength));
            return literal;
        }

// TSql90ParserBaseInternal.cs:420 (18 C# lines)
IndexAffectingStatement TSql90ParserBase::GetAlterIndexStatementKind(ast::AlterIndexStatement* alterIndex) {
            if (alterIndex->AlterIndexType == ast::AlterIndexType::Reorganize)
            {
                return IndexAffectingStatement::AlterIndexReorganize;
            }
            else if (alterIndex->AlterIndexType == ast::AlterIndexType::Resume)
            {
                return IndexAffectingStatement::AlterIndexResume;
            }
            else
            {
                if (alterIndex->Partition != nullptr && !alterIndex->Partition->All)
                    return IndexAffectingStatement::AlterIndexRebuildOnePartition;
                else
                    return IndexAffectingStatement::AlterIndexRebuildAllPartitions;
            }
        }

// TSql90ParserBaseInternal.cs:490 (8 C# lines)
ast::Literal* TSql90ParserBase::GetIPv4FragmentFromDotNumberNumeric(antlr4::Token* token) {
            ast::Literal* frag1{}; ast::Literal* frag2{};

            SplitNumericIntoIpParts(token, frag1, frag2);
            if (frag1 != nullptr || frag2 == nullptr)
                throw GetUnexpectedTokenErrorException(token);

            return frag2;
        }

// TSql90ParserBaseInternal.cs:501 (8 C# lines)
ast::Literal* TSql90ParserBase::GetIPv4FragmentFromNumberDotNumeric(antlr4::Token* token) {
            ast::Literal* frag1{}; ast::Literal* frag2{};

            SplitNumericIntoIpParts(token, frag1, frag2);
            if (frag1 == nullptr || frag2 != nullptr )
                throw GetUnexpectedTokenErrorException(token);

            return frag1;
        }

// TSql90ParserBaseInternal.cs:512 (5 C# lines)
void TSql90ParserBase::GetIPv4FragmentsFromNumberDotNumberNumeric(antlr4::Token* token, ast::Literal*& frag1, ast::Literal*& frag2) {
            if (!SplitNumericIntoIpParts(token, frag1, frag2))
                throw GetUnexpectedTokenErrorException(token);
        }

// TSql90ParserBaseInternal.cs:278 (4 C# lines)
bool TSql90ParserBase::IsStatementIsNext() {
            return(LA(1) != static_cast<size_t>(ast::TSqlTokenType::End) || NextTokenMatches(CodeGenerationSupporter::Conversation, 2));
        }

// TSql90ParserBaseInternal.cs:157 (36 C# lines)
ast::SecurityObjectKind TSql90ParserBase::ParseSecurityObjectKind(ast::Identifier* identifier) {
            { const std::string sw_(CsStr(Str_ToUpperInvariant(identifier->Value)).get()); if (sw_ == CodeGenerationSupporter::Assembly) {
                    return ast::SecurityObjectKind::Assembly;} else if (sw_ == CodeGenerationSupporter::Certificate) {
                    return ast::SecurityObjectKind::Certificate;} else if (sw_ == CodeGenerationSupporter::Contract) {
                    return ast::SecurityObjectKind::Contract;} else if (sw_ == CodeGenerationSupporter::Database) {
                    return ast::SecurityObjectKind::Database;} else if (sw_ == CodeGenerationSupporter::Endpoint) {
                    return ast::SecurityObjectKind::Endpoint;} else if (sw_ == CodeGenerationSupporter::Login) {
                    return ast::SecurityObjectKind::Login;} else if (sw_ == CodeGenerationSupporter::Object) {
                    return ast::SecurityObjectKind::Object;} else if (sw_ == CodeGenerationSupporter::Role) {
                    return ast::SecurityObjectKind::Role;} else if (sw_ == CodeGenerationSupporter::Route) {
                    return ast::SecurityObjectKind::Route;} else if (sw_ == CodeGenerationSupporter::Schema) {
                    return ast::SecurityObjectKind::Schema;} else if (sw_ == CodeGenerationSupporter::Server) {
                    return ast::SecurityObjectKind::Server;} else if (sw_ == CodeGenerationSupporter::Service) {
                    return ast::SecurityObjectKind::Service;} else if (sw_ == CodeGenerationSupporter::Type) {
                    return ast::SecurityObjectKind::Type;} else if (sw_ == CodeGenerationSupporter::User) {
                    return ast::SecurityObjectKind::User;} else {
                    throw GetUnexpectedTokenErrorException(identifier);} }
        }

// TSql90ParserBaseInternal.cs:200 (34 C# lines)
ast::SecurityObjectKind TSql90ParserBase::ParseSecurityObjectKind(ast::Identifier* identifier1, ast::Identifier* identifier2) {
            { const std::string sw_(CsStr(Str_ToUpperInvariant(identifier1->Value)).get()); if (sw_ == CodeGenerationSupporter::Application) {
                    Match(identifier2, CodeGenerationSupporter::Role);
                    return ast::SecurityObjectKind::ApplicationRole;} else if (sw_ == CodeGenerationSupporter::Asymmetric) {
                    Match(identifier2, CodeGenerationSupporter::Key);
                    return ast::SecurityObjectKind::AsymmetricKey;} else if (sw_ == CodeGenerationSupporter::Availability) {
                    Match(identifier2, CodeGenerationSupporter::Group);
                    return ast::SecurityObjectKind::AvailabilityGroup;} else if (sw_ == CodeGenerationSupporter::Fulltext) {
                    if (TryMatch(identifier2, CodeGenerationSupporter::Catalog))
                        return ast::SecurityObjectKind::FullTextCatalog;
                    else
                    {
                        Match(identifier2, CodeGenerationSupporter::StopList);
                        return ast::SecurityObjectKind::FullTextStopList;
                    }} else if (sw_ == CodeGenerationSupporter::Message) {
                    Match(identifier2, CodeGenerationSupporter::Type);
                    return ast::SecurityObjectKind::MessageType;} else if (sw_ == CodeGenerationSupporter::Server) {
                    Match(identifier2, CodeGenerationSupporter::Role);
                    return ast::SecurityObjectKind::ServerRole;} else if (sw_ == CodeGenerationSupporter::Symmetric) {
                    Match(identifier2, CodeGenerationSupporter::Key);
                    return ast::SecurityObjectKind::SymmetricKey;} else {
                    throw GetUnexpectedTokenErrorException(identifier1);} }
        }

// TSql90ParserBaseInternal.cs:242 (20 C# lines)
ast::SecurityObjectKind TSql90ParserBase::ParseSecurityObjectKind(ast::Identifier* identifier1, ast::Identifier* identifier2, ast::Identifier* identifier3) {
            { const std::string sw_(CsStr(Str_ToUpperInvariant(identifier1->Value)).get()); if (sw_ == CodeGenerationSupporter::Xml) {
                    Match(identifier2, CodeGenerationSupporter::Schema);
                    Match(identifier3, CodeGenerationSupporter::Collection);
                    return ast::SecurityObjectKind::XmlSchemaCollection;} else if (sw_ == CodeGenerationSupporter::Remote) {
                    Match(identifier2, CodeGenerationSupporter::Service);
                    Match(identifier3, CodeGenerationSupporter::Binding);
                    return ast::SecurityObjectKind::RemoteServiceBinding;} else if (sw_ == CodeGenerationSupporter::Search) {
                    Match(identifier2, CodeGenerationSupporter::Property);
                    Match(identifier3, CodeGenerationSupporter::List);
                    return ast::SecurityObjectKind::SearchPropertyList;} else {
                    throw GetUnexpectedTokenErrorException(identifier1);} }
        }

// TSql90ParserBaseInternal.cs:326 (9 C# lines)
ast::EncryptionAlgorithmPreference TSql90ParserBase::RecognizeAesOrRc4(ast::Identifier* id, antlr4::Token* tokenForError) {
            std::string unquotedId = Unquote(id->Value);
            if (String_Equals(unquotedId, CodeGenerationSupporter::Aes, StringComparison::OrdinalIgnoreCase))
                return ast::EncryptionAlgorithmPreference::Aes;

            if (String_Equals(unquotedId, CodeGenerationSupporter::RC4, StringComparison::OrdinalIgnoreCase))
                return ast::EncryptionAlgorithmPreference::Rc4;

            throw TSqlParseErrorException(GetUnexpectedTokenError(tokenForError));
        }

// TSql90ParserBaseInternal.cs:366 (26 C# lines)
void TSql90ParserBase::RecognizeAlterLoginSecAdminPasswordOption(antlr4::Token* token, ast::PasswordAlterPrincipalOption* astNode) {
            if (TryMatch(token, CodeGenerationSupporter::MustChange))
            {
                if (astNode->MustChange)
                    throw GetUnexpectedTokenErrorException(token);
                else
                    astNode->set_MustChange(true);
            }
            else if (TryMatch(token, CodeGenerationSupporter::Hashed))
            {
                if (astNode->Hashed)
                    throw GetUnexpectedTokenErrorException(token);
                else
                    astNode->set_Hashed(true);
            }
            else
            {
                Match(token, CodeGenerationSupporter::Unlock);
                if (astNode->Unlock)
                    throw GetUnexpectedTokenErrorException(token);
                else
                    astNode->set_Unlock(true);
            }
            UpdateTokenInfo(astNode, token);
        }

// TSql90ParserBaseInternal.cs:346 (11 C# lines)
ast::AuthenticationProtocol TSql90ParserBase::RecognizeAuthenticationProtocol(ast::Identifier* id, antlr4::Token* tokenForError) {
            std::string unquotedId = Unquote(id->Value);
            if (String_Equals(unquotedId, CodeGenerationSupporter::Ntlm, StringComparison::OrdinalIgnoreCase))
                return ast::AuthenticationProtocol::WindowsNtlm;

            if (String_Equals(unquotedId, CodeGenerationSupporter::Kerberos, StringComparison::OrdinalIgnoreCase))
                return ast::AuthenticationProtocol::WindowsKerberos;

            if (String_Equals(unquotedId, CodeGenerationSupporter::Negotiate, StringComparison::OrdinalIgnoreCase))
                return ast::AuthenticationProtocol::WindowsNegotiate;

            throw TSqlParseErrorException(GetUnexpectedTokenError(tokenForError));
        }

// TSql90ParserBaseInternal.cs:461 (25 C# lines)
bool TSql90ParserBase::SplitNumericIntoIpParts(antlr4::Token* token, ast::Literal*& frag1, ast::Literal*& frag2) {
            std::string text = token->getText();

            int textLen = Str_Length(text);

            int dotIndex = Str_IndexOf(text, '.');
            assert(dotIndex != -1);

            if (dotIndex == 0) // only second part, .2
            {
                frag1 = nullptr;
                frag2 = CreateIntLiteralFromNumericToken(token, 1, textLen - 1);
                return false;
            }
            else if (dotIndex == textLen - 1) // only first part, 1.
            {
                frag1 = CreateIntLiteralFromNumericToken(token, 0, dotIndex);
                frag2 = nullptr;
                return false;
            }
            else // both parts, 1.2
            {
                frag1 = CreateIntLiteralFromNumericToken(token, 0, dotIndex);
                frag2 = CreateIntLiteralFromNumericToken(token, dotIndex + 1, textLen - dotIndex - 1);
                return true;
            }
        }

// TSql90ParserBaseInternal.cs:532 (8 C# lines)
void TSql90ParserBase::ThrowIfInvalidListenerPortValue(ast::Literal* value) {
            int outValue{};
            if (!Int32_TryParse(value->Value, NumberStyles::Integer, CultureInfo::InvariantCulture, outValue) || (outValue > 32767) || (outValue < 1024))
            {
                ThrowParseErrorException("SQL46087", value, TSqlParserResource::SQL46087Message, value->Value);
            }
        }

// TSql90ParserBaseInternal.cs:541 (8 C# lines)
void TSql90ParserBase::ThrowIfMaxdopValueOutOfRange(ast::Literal* value) {
            int outValue{};
            if (!Int32_TryParse(value->Value, NumberStyles::Integer, CultureInfo::InvariantCulture, outValue) || (outValue > 32767) || (outValue < 0))
            {
                ThrowParseErrorException("SQL46091", value, TSqlParserResource::SQL46091Message, value->Value);
            }
        }

// TSql90ParserBaseInternal.cs:288 (25 C# lines)
std::string TSql90ParserBase::Unquote(CsStr value) {
            if (String_IsNullOrEmpty(value))
                return value;

            int nFirst = Str_IndexOf(value, '\'');
            int nLast = Str_LastIndexOf(value, '\'');
            std::string retVal = value;
            if (nFirst == -1 || nLast == nFirst)
                return retVal;

            if (nFirst < 2 && nLast != nFirst && nLast == Str_Length(value) - 1)  //this means it is [N]'blah'
            {
                if (nFirst == 1)
                {
                    if (Str_At(value, 0) == 'N')    //only if this started with an N
                    {
                        retVal = Str_Substring(value, nFirst + 1, nLast - nFirst - 1);
                    }
                }
                else
                {       //this is a 'blah' (no N)
                    retVal = Str_Substring(value, nFirst + 1, nLast - nFirst);
                }

            }
            return retVal;
        }

// TSql90ParserBaseInternal.cs:527 (4 C# lines)
void TSql90ParserBase::UpdateDmlTriggerActionEncounteredOptions(int& encountered, ast::TriggerAction* vTriggerAction) {
            encountered = encountered | (1 << static_cast<int>((vTriggerAction->TriggerActionType)));
        }

// TSql90ParserBaseInternal.cs:74
void TSql90ParserBase::CheckForFormatFileOptionInOpenRowsetBulk(int64_t encounteredOptions, ast::TSqlFragment* relatedFragment) {
    if ((encounteredOptions & CheckForFormatFileOptionInOpenRowsetBulkMask) == 0)
        ThrowParseErrorException("SQL46082", relatedFragment, TSqlParserResource::SQL46082Message);
}

}  // namespace tsql::parser
