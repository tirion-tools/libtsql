// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql130ParserBaseInternal.cs
#include "TSql130ParserBase.h"

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

bool TSql130ParserBase::NextIdentifierMatchesOneOf(const std::vector<std::string>& keywords) {
    std::string text;
    if (LA(1) == TT(TK::Identifier)) {
        text = LT(1)->getText();
    } else if (LA(1) == TT(TK::QuotedIdentifier)) {
        ast::QuoteType quote;
        text = ast::Identifier::DecodeIdentifier(LT(1)->getText(), quote);
    } else {
        return false;
    }
    for (const auto& keyword : keywords)
        if (EqualsIgnoreCase(keyword, text)) return true;
    return false;
}

// TSql130ParserBaseInternal.cs:465 (11 C# lines)
void TSql130ParserBase::CheckAndIncrementColumnCount(int& columnCount, antlr4::Token* token) {
            if (columnCount == 1024)
            {
                ThrowParseErrorException("TSP0017", token, TSqlParserResource::SQL46016Message);
            }
            else
            {
                columnCount++;
            }
        }

// TSql130ParserBaseInternal.cs:483 (8 C# lines)
void TSql130ParserBase::CheckCopyOptionDuplication(int& encountered, ast::CopyOptionKind newOption, antlr4::Token* token) {
            int newOptionBit = (1 << (static_cast<int>(newOption)));
            if ((encountered & newOptionBit) == newOptionBit)
                throw GetUnexpectedTokenErrorException(token);
            encountered |= newOptionBit;
        }

// TSql130ParserBaseInternal.cs:390 (11 C# lines) (hand-fixed)
void TSql130ParserBase::CheckCtasStatementHasDistributionOption(ast::CreateTableStatement* statement) {
    if (statement->SelectStatement != nullptr) {
        int count = 0;
        for (auto* o : statement->Options)
            if (dynamic_cast<ast::TableDistributionOption*>(o) != nullptr) ++count;
        if (count == 0) ThrowParseErrorException("SQL46127", statement, TSqlParserResource::SQL46127Message);
    }
}

// TSql130ParserBaseInternal.cs:407 (11 C# lines) (hand-fixed)
void TSql130ParserBase::CheckExternalTableCtasStatementHasNotRejectedRowLocationOption(ast::CreateExternalTableStatement* statement) {
    if (statement->SelectStatement != nullptr) {
        for (auto* o : statement->ExternalTableOptions)
            if (o->OptionKind == ast::ExternalTableOptionKind::RejectedRowLocation)
                ThrowParseErrorException("SQL46128", statement, TSqlParserResource::SQL46128Message);
    }
}

// TSql130ParserBaseInternal.cs:301 (24 C# lines)
void TSql130ParserBase::CheckHekatonTableForInlineFilteredIndexes(ast::CreateTableStatement* statement) {
			if(!IsMemoryOptimized(statement))
			{
				return;
			}

			for (ast::ColumnDefinition* column : statement->Definition->ColumnDefinitions)
			{
				if(column->Index != nullptr)
				{
					if(column->Index->FilterPredicate != nullptr)
					{
						ThrowParseErrorException("SQL46107", statement->Definition, TSqlParserResource::SQL46107Message);
					}
				}
			}

			for (ast::IndexDefinition* index : statement->Definition->Indexes)
			{
				if(index->FilterPredicate != nullptr)
				{
					ThrowParseErrorException("SQL46107", statement->Definition, TSqlParserResource::SQL46107Message);
				}
			}
		}

// TSql130ParserBaseInternal.cs:332 (24 C# lines)
void TSql130ParserBase::CheckHekatonTableForNonClusteredColumnStoreIndexes(ast::CreateTableStatement* statement) {
			if(!IsMemoryOptimized(statement))
			{
				return;
			}

			for (ast::ColumnDefinition* column : statement->Definition->ColumnDefinitions)
			{
				if(column->Index != nullptr)
				{
					if(column->Index->IndexType->IndexTypeKind == ast::IndexTypeKind::NonClusteredColumnStore)
					{
						ThrowParseErrorException("SQL46108", statement->Definition, TSqlParserResource::SQL46108Message);
					}
				}
			}

			for (ast::IndexDefinition* index : statement->Definition->Indexes)
			{
				if (index->IndexType->IndexTypeKind == ast::IndexTypeKind::NonClusteredColumnStore)
				{
					ThrowParseErrorException("SQL46108", statement->Definition, TSqlParserResource::SQL46108Message);
				}
			}
		}

// TSql130ParserBaseInternal.cs:278 (16 C# lines)
void TSql130ParserBase::CheckRetentionPeriodDuration(ast::ScalarExpression* duration) {
            int retentionPeriodDuration{};
            if ((dynamic_cast<ast::UnaryExpression*>(duration) != nullptr))
            {
                retentionPeriodDuration = -Int32_Parse((dynamic_cast<ast::IntegerLiteral*>((dynamic_cast<ast::UnaryExpression*>(duration))->Expression))->Value, CultureInfo::InvariantCulture);
            }
            else
            {
                retentionPeriodDuration = Int32_Parse((dynamic_cast<ast::IntegerLiteral*>(duration))->Value, CultureInfo::InvariantCulture);
            }

            if (retentionPeriodDuration <= 0)
            {
                ThrowParseErrorException("SQL46116", duration, TSqlParserResource::SQL46116Message, ToString(retentionPeriodDuration));
            }
        }

// TSql130ParserBaseInternal.cs:179 (89 C# lines)
void TSql130ParserBase::CheckTemporalGeneratedAlwaysColumns(ast::TableDefinition* definition, bool isInAlterStatement) {
            bool userIdStartExists = false;
            bool userIdEndExists = false;
            bool userNameStartExists = false;
            bool userNameEndExists = false;
            bool startTimeExists = false;
            bool endTimeExists = false;

            int genAlwaysColumnCount = 0;

            for (ast::ColumnDefinition* column : definition->ColumnDefinitions)
            {
                if (column->GeneratedAlways == ast::GeneratedAlwaysType::RowStart)
                {
                    genAlwaysColumnCount++;
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
                    genAlwaysColumnCount++;
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
                    genAlwaysColumnCount++;
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
                    genAlwaysColumnCount++;
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
                    genAlwaysColumnCount++;
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
                    genAlwaysColumnCount++;
                    if (userNameEndExists)
                    {
                        ThrowParseErrorException("SQL46112", definition, TSqlParserResource::SQL46112Message);
                    }
                    else
                    {
                        userNameEndExists = true;
                    }
                }
            }

            // In case of CREATE statement SystemTimePeriod needs to be defined
            // (table needs to be temporal in order to have GeneratedAlways as User Id/Name columns
            if (genAlwaysColumnCount > 0 && !isInAlterStatement && definition->SystemTimePeriod == nullptr)
            {
                ThrowParseErrorException("SQL46113", definition, TSqlParserResource::SQL46113Message);
            }
        }

// TSql130ParserBaseInternal.cs:95 (63 C# lines)
void TSql130ParserBase::CheckTemporalPeriodInTableDefinition(ast::TableDefinition* definition, bool isInAlterStatement) {
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
            CheckTemporalGeneratedAlwaysColumns(definition, isInAlterStatement);
        }

// TSql130ParserBaseInternal.cs:497 (8 C# lines) (hand-fixed)
void TSql130ParserBase::CheckValidWlmTimeLiteral(ast::StringLiteral* timeStringToken) {
    // TimeSpan.TryParseExact(value, @"h\:mm", CurrentCulture): hours 0-23 (one or two digits),
    // ':', exactly two minute digits 00-59 (no surrounding white space)
    const std::string v(CsStr(timeStringToken->Value).get());
    size_t colon = v.find(':');
    bool ok = colon != std::string::npos && colon >= 1 && colon <= 2 && v.size() == colon + 3;
    for (size_t i = 0; ok && i < v.size(); ++i)
        if (i != colon && (v[i] < '0' || v[i] > '9')) ok = false;
    if (ok) ok = std::stoi(v.substr(0, colon)) <= 23 && std::stoi(v.substr(colon + 1)) <= 59;
    if (!ok) ThrowParseErrorException("SQL46134", timeStringToken, TSqlParserResource::SQL46134Message);
}

// TSql130ParserBaseInternal.cs:363 (11 C# lines)
bool TSql130ParserBase::IsMemoryOptimized(ast::CreateTableStatement* statement) {
			for (ast::TableOption* option : statement->Options)
			{
				if (option->OptionKind == ast::TableOptionKind::MemoryOptimized)
				{
					return true;
				}
			}

			return false;
		}

// TSql130ParserBaseInternal.cs:419 (`new` hides TSql80's version; hand-fixed)
ast::FunctionOptionKind TSql130ParserBase::ParseAlterCreateFunctionWithOption(antlr4::Token* token) {
            { const std::string sw_(CsStr(Str_ToUpperInvariant(token->getText())).get()); if (sw_ == CodeGenerationSupporter::Encryption) {
                    return ast::FunctionOptionKind::Encryption;} else if (sw_ == CodeGenerationSupporter::SchemaBinding) {
                    return ast::FunctionOptionKind::SchemaBinding;} else if (sw_ == CodeGenerationSupporter::NativeCompilation) {
                    return ast::FunctionOptionKind::NativeCompilation;} else {
                    throw TSqlParseErrorException(CreateParseError("SQL46026", token, TSqlParserResource::SQL46026Message, token->getText()));} }
        }

// TSql130ParserBaseInternal.cs:376 (8 C# lines)
void TSql130ParserBase::ThrowIfCompressionDelayValueOutOfRange(ast::Literal* value) {
			int outValue{};
			if (!Int32_TryParse(value->Value, NumberStyles::Integer, CultureInfo::InvariantCulture, outValue) || (outValue > 10080) || (outValue < 0))
			{
				ThrowParseErrorException("SQL46114", value, TSqlParserResource::SQL46114Message, value->Value);
			}
		}

// TSql130ParserBaseInternal.cs:58
void TSql130ParserBase::VerifyAllowedIndexOption130(IndexAffectingStatement statement, ast::IndexOption* option) {
    VerifyAllowedIndexOption(statement, option, SqlVersionFlags::TSql130);
    // for a low priority lock wait (MLP) option, check if it is allowed for the statement.
    if (auto* onlineIndexOption = dynamic_cast<ast::OnlineIndexOption*>(option)) {
        if (onlineIndexOption->LowPriorityLockWaitOption != nullptr) {
            switch (statement) {
                case IndexAffectingStatement::AlterIndexRebuildOnePartition:
                case IndexAffectingStatement::AlterTableRebuildOnePartition:
                case IndexAffectingStatement::AlterIndexRebuildAllPartitions:
                case IndexAffectingStatement::AlterTableRebuildAllPartitions:
                    break;   // allowed
                default:
                    // WAIT_AT_LOW_PRIORITY is not a valid index option in the statement
                    ThrowWrongIndexOptionError(statement, onlineIndexOption->LowPriorityLockWaitOption);
                    break;
            }
        }
    }
}

}  // namespace tsql::parser
