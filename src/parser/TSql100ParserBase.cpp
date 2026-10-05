// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql100ParserBaseInternal.cs
#include "TSql100ParserBase.h"

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

ast::SqlDataTypeOption TSql100ParserBase::ParseDataType100(CsStr token) {
    std::string t = AsciiUpper(token.get());
    if (t == "DATE") return ast::SqlDataTypeOption::Date;
    if (t == "TIME") return ast::SqlDataTypeOption::Time;
    if (t == "DATETIME2") return ast::SqlDataTypeOption::DateTime2;
    if (t == "DATETIMEOFFSET") return ast::SqlDataTypeOption::DateTimeOffset;
    return ParseDataType(token);
}

// TSql100ParserBaseInternal.cs:109 (5 C# lines)
void TSql100ParserBase::CheckBoundingBoxParameterDuplication(int current, ast::BoundingBoxParameterType newOption, antlr4::Token* token) {
            if ((current & (1 << static_cast<int>((newOption)))) != 0)
                ThrowIncorrectSyntaxErrorException(token);
        }

// TSql100ParserBaseInternal.cs:90 (5 C# lines)
void TSql100ParserBase::CheckBrokerPriorityParameterDuplication(int current, ast::BrokerPriorityParameterType newOption, antlr4::Token* token) {
            if ((current & (1 << static_cast<int>((newOption)))) != 0)
                ThrowIncorrectSyntaxErrorException(token);
        }

// TSql100ParserBaseInternal.cs:177 (35 C# lines)
void TSql100ParserBase::CheckComparisonOperandForIndexFilter(ast::ScalarExpression* rightOperand, bool convertAllowed) {
            ast::UnaryExpression* unary = dynamic_cast<ast::UnaryExpression*>(rightOperand);
            if (unary != nullptr)
            {
                CheckComparisonOperandForIndexFilter(unary->Expression, convertAllowed);
                return;
            }

            ast::Literal* literal = dynamic_cast<ast::Literal*>(rightOperand);
            if (literal != nullptr &&
                literal->LiteralType() != ast::LiteralType::Max)
                return;

            ast::ParenthesisExpression* parenthesis = dynamic_cast<ast::ParenthesisExpression*>(rightOperand);
            if (parenthesis != nullptr)
            {
                CheckComparisonOperandForIndexFilter(parenthesis->Expression, convertAllowed);
                return;
            }

            if (convertAllowed)
            {
                ast::ConvertCall* convertCall = dynamic_cast<ast::ConvertCall*>(rightOperand);
                if (convertCall != nullptr)
                {
                    CheckComparisonOperandForIndexFilter(convertCall->Parameter, false);
                    return;
                }

                ast::CastCall* castCall = dynamic_cast<ast::CastCall*>(rightOperand);
                if (castCall != nullptr)
                {
                    CheckComparisonOperandForIndexFilter(castCall->Parameter, false);
                    return;
                }
            }

            ThrowParseErrorException("SQL46059", rightOperand, TSqlParserResource::SQL46059Message);
        }

// TSql100ParserBaseInternal.cs:260 (9 C# lines)
void TSql100ParserBase::CheckForCellsPerObjectValueRange(ast::Literal* value) {
            int convertedValue{};
            if (!Int32_TryParse(value->Value, NumberStyles::Integer, CultureInfo::InvariantCulture, convertedValue) ||
                convertedValue < 1 || convertedValue > 8192)
            {
                ThrowParseErrorException("SQL46073", value, TSqlParserResource::SQL46073Message, value->Value);
            }
        }

// TSql100ParserBaseInternal.cs:121 (12 C# lines)
void TSql100ParserBase::CheckIfValidSpatialIndexOptionValue(IndexAffectingStatement statement, ast::IndexOption* option) {
            ast::IndexStateOption* indexStateOption = dynamic_cast<ast::IndexStateOption*>(option);
            if (indexStateOption != nullptr)
            {
                if (indexStateOption->OptionKind == ast::IndexOptionKind::IgnoreDupKey)
                {
                    if (indexStateOption->OptionState == ast::OptionState::On)
                        ThrowWrongIndexOptionError(statement, indexStateOption);
                }
            }
        }

// TSql100ParserBaseInternal.cs:220 (19 C# lines)
void TSql100ParserBase::CheckPartitionAllSpecifiedForIndexRebuild(ast::PartitionSpecifier* partitionSpecifier, std::vector<ast::IndexOption*>& indexOptions) {
            if (partitionSpecifier == nullptr)
            {
                for (ast::IndexOption* option : indexOptions)
                {
                    if (dynamic_cast<ast::DataCompressionOption*>(option) != nullptr && static_cast<int>((dynamic_cast<ast::DataCompressionOption*>(option))->PartitionRanges.size()) > 0)
                    {
                        ThrowParseErrorException("SQL46061", option, TSqlParserResource::SQL46061Message);
                    }
                    else if (dynamic_cast<ast::XmlCompressionOption*>(option) != nullptr && static_cast<int>((dynamic_cast<ast::XmlCompressionOption*>(option))->PartitionRanges.size()) > 0)
                    {
                        ThrowParseErrorException("SQL46061", option, TSqlParserResource::SQL46061Message);
                    }
                }
            }
        }

// TSql100ParserBaseInternal.cs:49 (16 C# lines)
ast::AutoCleanupChangeTrackingOptionDetail* TSql100ParserBase::CreateAutoCleanupDetail(antlr4::Token* firstToken, antlr4::Token* lastToken, bool& autoCleanupEncountered) {
            Match(firstToken, CodeGenerationSupporter::AutoCleanup);
            if (autoCleanupEncountered)
            {
                ThrowParseErrorException("SQL46050", firstToken, TSqlParserResource::SQL46050Message, firstToken->getText());
            }
            autoCleanupEncountered = true;
            ast::AutoCleanupChangeTrackingOptionDetail* autoCleanup = CreateFragment<ast::AutoCleanupChangeTrackingOptionDetail>();
            UpdateTokenInfo(autoCleanup, firstToken);
            UpdateTokenInfo(autoCleanup, lastToken);
            autoCleanup->set_IsOn((static_cast<int>(lastToken->getType()) == static_cast<size_t>(ast::TSqlTokenType::On)));
            return autoCleanup;
        }

// TSql100ParserBaseInternal.cs:134 (19 C# lines)
void TSql100ParserBase::SetFileStreamStorageOption(ast::ColumnStorageOptions* storageOptions, antlr4::Token* fileStreamToken, ast::DataTypeReference* columnType, IndexAffectingStatement statementType) {
            if (statementType == IndexAffectingStatement::AlterTableAddElement ||
                statementType == IndexAffectingStatement::CreateTable)
            {
                // Filestream is only allowed on VARBINARY(MAX) columns
                ast::SqlDataTypeReference* sqlDataType = dynamic_cast<ast::SqlDataTypeReference*>(columnType);
                if (sqlDataType != nullptr &&
                    sqlDataType->SqlDataTypeOption == ast::SqlDataTypeOption::VarBinary &&
                    static_cast<int>(sqlDataType->Parameters.size()) == 1 &&
                    sqlDataType->Parameters[0]->LiteralType() == ast::LiteralType::Max)
                {
                    storageOptions->set_IsFileStream(true);
                }
                else
                    ThrowParseErrorException("SQL46051", fileStreamToken, TSqlParserResource::SQL46051Message);
            }
            else
                ThrowIncorrectSyntaxErrorException(fileStreamToken);
        }

// TSql100ParserBaseInternal.cs:155 (19 C# lines)
void TSql100ParserBase::SetSparseStorageOption(ast::ColumnStorageOptions* columnStorage, ast::SparseColumnOption option, antlr4::Token* token, IndexAffectingStatement statementType) {
            switch (statementType)
            {
                case IndexAffectingStatement::AlterTableAddElement:
                case IndexAffectingStatement::CreateTable:
                case IndexAffectingStatement::DeclareTableVariable:
                case IndexAffectingStatement::CreateOrAlterFunction:
                    {
                        // Disallow create/alter function with COLUMN_SET FOR ALL_SPARSE_COLUMNS syntax to match engine behavior
                        if (statementType == IndexAffectingStatement::CreateOrAlterFunction && option == ast::SparseColumnOption::ColumnSetForAllSparseColumns)
                            ThrowIncorrectSyntaxErrorException(token);

                        columnStorage->set_SparseOption(option);
                        break;
                    }
                default:
                    ThrowIncorrectSyntaxErrorException(token);
                    break;
            }
        }

// TSql100ParserBaseInternal.cs:250 (9 C# lines)
void TSql100ParserBase::ThrowIfTooLargeAuditFileSize(ast::Literal* size, int shift) {
            uint64_t convertedSize{};
            if (!UInt64_TryParse(size->Value, NumberStyles::Integer, CultureInfo::InvariantCulture, convertedSize) ||
                convertedSize > (std::numeric_limits<uint64_t>::max() >> (20 + shift)))
            {
                ThrowParseErrorException("SQL46054", size, TSqlParserResource::SQL46054Message);
            }
        }

// TSql100ParserBaseInternal.cs:240 (9 C# lines) (hand-fixed)
void TSql100ParserBase::ThrowIfWrongGuidFormat(ast::Literal* literal) {
    // Regex.IsMatch(value, "[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"):
    // unanchored, so any substring of that shape matches
    const std::string v(CsStr(literal->Value).get());
    static const int kGroups[] = {8, 4, 4, 4, 12};
    auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    bool found = false;
    for (size_t start = 0; !found && start + 36 <= v.size(); ++start) {
        size_t i = start;
        bool ok = true;
        for (int g = 0; ok && g < 5; ++g) {
            for (int k = 0; ok && k < kGroups[g]; ++k, ++i) ok = hex(v[i]);
            if (ok && g < 4) ok = v[i++] == '-';
        }
        found = ok;
    }
    if (!found) ThrowParseErrorException("SQL46055", literal, TSqlParserResource::SQL46055Message);
}

// TSql100ParserBaseInternal.cs:116 (4 C# lines)
void TSql100ParserBase::UpdateBoundingBoxParameterEncounteredOptions(int& encountered, ast::BoundingBoxParameter* vBoundingBoxParameter) {
            encountered = encountered | (1 << static_cast<int>((vBoundingBoxParameter->Parameter)));
        }

// TSql100ParserBaseInternal.cs:97 (4 C# lines)
void TSql100ParserBase::UpdateBrokerPriorityEncounteredOptions(int& encountered, ast::BrokerPriorityParameter* vBrokerPriorityParameter) {
            encountered = encountered | (1 << static_cast<int>((vBrokerPriorityParameter->ParameterType)));
        }

}  // namespace tsql::parser
