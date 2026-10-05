// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlFabricDWParserBaseInternal.cs
// The C# methods below are TSql160ParserBaseInternal's (ParseDataType160 renamed); the hand port is shared text.
#include "TSqlFabricDWParserBase.h"

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

void TSqlFabricDWParserBase::CheckForConflictingOptionsInOpenRowsetBulkCosmos(ast::OpenRowsetCosmos* openRowsetCosmos) {
    using K = ast::OpenRowsetCosmosOptionKind;
    auto has = [&](K kind) {
        for (auto* opt : openRowsetCosmos->Options)
            if (opt->OptionKind == kind) return true;
        return false;
    };
    if (!has(K::Provider)) ThrowParseErrorException("SQL46144", openRowsetCosmos, TSqlParserResource::SQL46144Message, "Provider");
    if (!has(K::Connection)) ThrowParseErrorException("SQL46144", openRowsetCosmos, TSqlParserResource::SQL46144Message, "Connection");
    if (!has(K::Object)) ThrowParseErrorException("SQL46144", openRowsetCosmos, TSqlParserResource::SQL46144Message, "Object");
    if (has(K::Credential) && has(K::Server_Credential))
        ThrowParseErrorException("SQL46143", openRowsetCosmos, TSqlParserResource::SQL46143Message);
}

ast::SqlDataTypeOption TSqlFabricDWParserBase::ParseDataTypeFabricDW(CsStr token) {
    std::string t = AsciiUpper(token.get());
    if (t == "JSON") return ast::SqlDataTypeOption::Json;
    if (t == "VECTOR") return ast::SqlDataTypeOption::Vector;
    return ParseDataType100(token);
}

// TSqlFabricDWParserBaseInternal.cs:164 (142 C# lines)
void TSqlFabricDWParserBase::CheckTemporalGeneratedAlwaysColumns(ast::TableDefinition* definition, bool isInAlterStatement, bool isLedgerSupported) {
            bool userIdStartExists = false;
            bool userIdEndExists = false;
            bool userNameStartExists = false;
            bool userNameEndExists = false;
            bool startTimeExists = false;
            bool endTimeExists = false;
            bool transactionIdStartExists = false;
            bool transactionIdEndExists = false;
            bool sequenceNumberStartExists = false;
            bool sequenceNumberEndExists = false;

            int temporalGenAlwaysColumnCount = 0;

            for (ast::ColumnDefinition* column : definition->ColumnDefinitions)
            {
                if (column->GeneratedAlways == ast::GeneratedAlwaysType::RowStart)
                {
                    temporalGenAlwaysColumnCount++;
                    if (startTimeExists)
                    {
                        ThrowParseErrorException("SQL46109", definition, TSqlParserResource::SQL46109Message);
                    }
                    else
                    {
                        startTimeExists = true;
                    }
                }
                else if (column->GeneratedAlways == ast::GeneratedAlwaysType::RowEnd)
                {
                    temporalGenAlwaysColumnCount++;
                    if (endTimeExists)
                    {
                        ThrowParseErrorException("SQL46109", definition, TSqlParserResource::SQL46109Message);
                    }
                    else
                    {
                        endTimeExists = true;
                    }
                }
                else if (column->GeneratedAlways == ast::GeneratedAlwaysType::UserIdStart)
                {
                    temporalGenAlwaysColumnCount++;
                    if (userIdStartExists)
                    {
                        ThrowParseErrorException("SQL46109", definition, TSqlParserResource::SQL46109Message);
                    }
                    else
                    {
                        userIdStartExists = true;
                    }
                }
                else if (column->GeneratedAlways == ast::GeneratedAlwaysType::UserIdEnd)
                {
                    temporalGenAlwaysColumnCount++;
                    if (userIdEndExists)
                    {
                        ThrowParseErrorException("SQL46110", definition, TSqlParserResource::SQL46110Message);
                    }
                    else
                    {
                        userIdEndExists = true;
                    }
                }
                else if (column->GeneratedAlways == ast::GeneratedAlwaysType::UserNameStart)
                {
                    temporalGenAlwaysColumnCount++;
                    if (userNameStartExists)
                    {
                        ThrowParseErrorException("SQL46111", definition, TSqlParserResource::SQL46111Message);
                    }
                    else
                    {
                        userNameStartExists = true;
                    }
                }
                else if (column->GeneratedAlways == ast::GeneratedAlwaysType::UserNameEnd)
                {
                    temporalGenAlwaysColumnCount++;
                    if (userNameEndExists)
                    {
                        ThrowParseErrorException("SQL46112", definition, TSqlParserResource::SQL46112Message);
                    }
                    else
                    {
                        userNameEndExists = true;
                    }
                }
                else if (column->GeneratedAlways == ast::GeneratedAlwaysType::TransactionIdStart && isLedgerSupported)
                {
                    if (transactionIdStartExists)
                    {
                        ThrowParseErrorException("SQL46136", definition, TSqlParserResource::SQL46136Message);
                    }
                    else
                    {
                        transactionIdStartExists = true;
                    }
                }
                else if (column->GeneratedAlways == ast::GeneratedAlwaysType::TransactionIdEnd && isLedgerSupported)
                {
                    if (transactionIdEndExists)
                    {
                        ThrowParseErrorException("SQL46137", definition, TSqlParserResource::SQL46137Message);
                    }
                    else
                    {
                        transactionIdEndExists = true;
                    }
                }
                else if (column->GeneratedAlways == ast::GeneratedAlwaysType::SequenceNumberStart && isLedgerSupported)
                {
                    if (sequenceNumberStartExists)
                    {
                        ThrowParseErrorException("SQL46138", definition, TSqlParserResource::SQL46138Message);
                    }
                    else
                    {
                        sequenceNumberStartExists = true;
                    }
                }
                else if (column->GeneratedAlways == ast::GeneratedAlwaysType::SequenceNumberEnd && isLedgerSupported)
                {
                    if (sequenceNumberEndExists)
                    {
                        ThrowParseErrorException("SQL46139", definition, TSqlParserResource::SQL46139Message);
                    }
                    else
                    {
                        sequenceNumberEndExists = true;
                    }
                }
                // Ledger is only supported for versions 160 and SqlFabricDW - throwing an error if the version is not FabricDW or above.
                //
                else if (!isLedgerSupported && (column->GeneratedAlways == ast::GeneratedAlwaysType::SequenceNumberStart || column->GeneratedAlways == ast::GeneratedAlwaysType::SequenceNumberEnd
                    || column->GeneratedAlways == ast::GeneratedAlwaysType::TransactionIdStart || column->GeneratedAlways == ast::GeneratedAlwaysType::TransactionIdEnd))
                {
                    ThrowParseErrorException("SQL46141", definition, TSqlParserResource::SQL46141Message);
                }
            }

            // In case of CREATE statement SystemTimePeriod needs to be defined
            // (table needs to be temporal in order to have GeneratedAlways as User Id/Name columns
            if (temporalGenAlwaysColumnCount > 0 && !isInAlterStatement && definition->SystemTimePeriod == nullptr)
            {
                ThrowParseErrorException("SQL46113", definition, TSqlParserResource::SQL46113Message);
            }
        }

// TSqlFabricDWParserBaseInternal.cs:79 (63 C# lines)
void TSqlFabricDWParserBase::CheckTemporalPeriodInTableDefinition(ast::TableDefinition* definition, bool isInAlterStatement, bool isLedgerSupported) {
            // The column check should be executed for every create statement and for alter/add statements that contain both
            // column and period definitions.
            //
            bool createOrNonEmptyAlter = !isInAlterStatement || ( (static_cast<void>(definition->ColumnDefinitions), true) && static_cast<int>(definition->ColumnDefinitions.size()) > 0);

            if (definition->SystemTimePeriod != nullptr && createOrNonEmptyAlter)
            {
                bool startTimeColumnIsCorrect = false;
                bool endTimeColumnIsCorrect = false;

                for (ast::ColumnDefinition* column : definition->ColumnDefinitions)
                {
                    if (definition->SystemTimePeriod->StartTimeColumn->Value == column->ColumnIdentifier->Value)
                    {
                        if (column->GeneratedAlways != ast::GeneratedAlwaysType::RowStart)
                        {
                            ThrowParseErrorException("SQL46103", definition, TSqlParserResource::SQL46103Message);
                        }
                        else
                        {
                            startTimeColumnIsCorrect = true;
                        }
                    }
                    else
                    {
                        if (definition->SystemTimePeriod->EndTimeColumn->Value == column->ColumnIdentifier->Value)
                        {
                            if (column->GeneratedAlways != ast::GeneratedAlwaysType::RowEnd)
                            {
                                ThrowParseErrorException("SQL46104", definition, TSqlParserResource::SQL46104Message);
                            }
                            else
                            {
                                endTimeColumnIsCorrect = true;
                            }
                        }
                        else
                        {
                            if (column->GeneratedAlways != std::nullopt)
                            {
                                if (column->GeneratedAlways == ast::GeneratedAlwaysType::RowStart)
                                {
                                    ThrowParseErrorException("SQL46103", definition, TSqlParserResource::SQL46103Message);
                                }
                                else if (column->GeneratedAlways == ast::GeneratedAlwaysType::RowEnd)
                                {
                                    ThrowParseErrorException("SQL46104", definition, TSqlParserResource::SQL46104Message);
                                }
                            }
                        }
                    }
                }

                // In case that ALTER statement is executed for:
                // 1. adding system time period
                // 2. adding columns that are not included in the period
                // we will have here both startTimeColumnIsCorrect and endTimeColumnIsCorrect set to false
                // and the alter statement is still valid.
                if (!(isInAlterStatement && !startTimeColumnIsCorrect && !endTimeColumnIsCorrect))
                {
                    if (!startTimeColumnIsCorrect)
                    {
                        ThrowParseErrorException("SQL46105", definition, TSqlParserResource::SQL46105Message);
                    }

                    if (!endTimeColumnIsCorrect)
                    {
                        ThrowParseErrorException("SQL46106", definition, TSqlParserResource::SQL46106Message);
                    }
                }
            }

            // Do the check for other GeneratedAlways columns
            CheckTemporalGeneratedAlwaysColumns(definition, isInAlterStatement, isLedgerSupported);
        }

// TSqlFabricDWParserBaseInternal.cs:53
void TSqlFabricDWParserBase::VerifyAllowedIndexOptionFabricDW(IndexAffectingStatement statement, ast::IndexOption* option) {
    VerifyAllowedIndexOption(statement, option, SqlVersionFlags::TSqlFabricDW);
    VerifyAllowedOnlineIndexOptionLowPriorityLockWait(statement, option, SqlVersionFlags::TSqlFabricDW);
}

}  // namespace tsql::parser
