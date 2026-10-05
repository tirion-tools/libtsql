// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql140ParserBaseInternal.cs
#include "TSql140ParserBase.h"

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

void TSql140ParserBase::CheckForDataFileFormatProhibitedOptionsInOpenRowsetBulk(int64_t encounteredOptions,
                                                                                ast::TSqlFragment* relatedFragment) {
    using B = ast::BulkInsertOptionKind;
    const int64_t DataFileFormatMask = (int64_t{1} << static_cast<int>(B::DataFileFormat));
    const int64_t ProhibitedMask = (int64_t{1} << static_cast<int>(B::SingleBlob)) |
                                   (int64_t{1} << static_cast<int>(B::SingleClob)) |
                                   (int64_t{1} << static_cast<int>(B::SingleNClob));
    if ((encounteredOptions & DataFileFormatMask) != 0 && (encounteredOptions & ProhibitedMask) != 0)
        ThrowParseErrorException("SQL46119", relatedFragment, TSqlParserResource::SQL46119Message);
}

void TSql140ParserBase::CheckForParquetFormatProhibitedOptionsInOpenRowsetBulk(int64_t encounteredOptions,
                                                                               ast::BulkOpenRowset* bulkOpenRowset) {
    using B = ast::BulkInsertOptionKind;
    const int64_t DataFileFormatMask = (int64_t{1} << static_cast<int>(B::DataFileFormat));
    const int64_t InvalidParquetMask = (int64_t{1} << static_cast<int>(B::ParserVersion)) |
                                       (int64_t{1} << static_cast<int>(B::RowsetOptions));
    if ((encounteredOptions & DataFileFormatMask) != 0 && (encounteredOptions & InvalidParquetMask) != 0) {
        for (ast::BulkInsertOption* option : bulkOpenRowset->Options) {
            if (option->OptionKind == B::DataFileFormat) {
                auto* dataFileFormatOption = dynamic_cast<ast::LiteralBulkInsertOption*>(option);
                if (dataFileFormatOption == nullptr) throw NullReferenceException();
                if (TryMatch(dataFileFormatOption->Value, CodeGenerationSupporter::Parquet))
                    ThrowParseErrorException("SQL46142", bulkOpenRowset, TSqlParserResource::SQL46142Message);
            }
        }
    }
}

ast::Identifier* TSql140ParserBase::CreateIdentifierFromToken(antlr4::Token* token) {
    // C#: new Identifier { ... } (not through the factory, so no token stream / positions)
    auto* id = CreateFragment<ast::Identifier>();
    id->ScriptTokenStream = nullptr;
    id->set_Value(token->getText());
    id->set_QuoteType(ast::QuoteType::NotQuoted);
    return id;
}

// TSql140ParserBaseInternal.cs:103 (28 C# lines)
void TSql140ParserBase::CheckForDataFileFormatProhibitedOptionsBulkInsert(int64_t encounteredOptions, ast::BulkInsertStatement* statement) {
            const long DataFileFormatProhibitedOptionsBulkInsertMask =
                (1LL << static_cast<int>(ast::BulkInsertOptionKind::DataFileFormat)) |
                (1LL << static_cast<int>(ast::BulkInsertOptionKind::DataFileType));

            // Check if both data file type and data file format options are specified
            //
            if ((encounteredOptions & DataFileFormatProhibitedOptionsBulkInsertMask) == DataFileFormatProhibitedOptionsBulkInsertMask)
            {
                bool isDataFileFormatCSV = false;
                bool isDataFileTypeProhibited = false;

                // Check if unsupported values are specified
                //
                for (ast::BulkInsertOption* option : statement->Options)
                {
                    if (option->OptionKind == ast::BulkInsertOptionKind::DataFileFormat)
                    {
                        ast::LiteralBulkInsertOption* dataFileFormatOption = dynamic_cast<ast::LiteralBulkInsertOption*>(option);

                        isDataFileFormatCSV = TryMatch(dataFileFormatOption->Value, CodeGenerationSupporter::Csv);
                    }
                    else if (option->OptionKind == ast::BulkInsertOptionKind::DataFileType)
                    {
                        ast::LiteralBulkInsertOption* dataFileTypeOption = dynamic_cast<ast::LiteralBulkInsertOption*>(option);

                        isDataFileTypeProhibited = !(TryMatch(dataFileTypeOption->Value, CodeGenerationSupporter::Char) || TryMatch(dataFileTypeOption->Value, CodeGenerationSupporter::WideChar));
                    }
                }

                // Throw parser error exception if invalid combination is specified
                //
                if (isDataFileFormatCSV && isDataFileTypeProhibited)
                {
                    ThrowParseErrorException("SQL46118", statement, TSqlParserResource::SQL46118Message);
                }
            }
        }

// TSql140ParserBaseInternal.cs:211 (6 C# lines)
ast::IdentifierOrScalarExpression* TSql140ParserBase::CreateIdentifierOrScalarExpressionFromIdentifier(ast::Identifier* identifier) {
            ast::IdentifierOrScalarExpression* valExp = CreateFragment<ast::IdentifierOrScalarExpression>();
            valExp->set_Identifier(identifier);
            return valExp;
        }

// TSql140ParserBaseInternal.cs:65
void TSql140ParserBase::VerifyAllowedIndexOption140(IndexAffectingStatement statement, ast::IndexOption* option) {
    VerifyAllowedIndexOption(statement, option, SqlVersionFlags::TSql140);
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
