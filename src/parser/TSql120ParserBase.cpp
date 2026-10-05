// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql120ParserBaseInternal.cs
#include "TSql120ParserBase.h"

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

// TSql120ParserBaseInternal.cs:49 (12 C# lines)
void TSql120ParserBase::CheckLowPriorityLockWaitValue(ast::IntegerLiteral* maxDuration, ast::AbortAfterWaitType abortAfterWait) {
            int outValue{};
            if (!Int32_TryParse(maxDuration->Value, NumberStyles::Integer, CultureInfo::InvariantCulture, outValue) || (outValue > 71582) || (outValue < 0))
            {
                ThrowParseErrorException("SQL46101", maxDuration, TSqlParserResource::SQL46101Message, maxDuration->Value);
            }

            if (outValue == 0 && abortAfterWait == ast::AbortAfterWaitType::Self)
            {
                ThrowParseErrorException("SQL46102", maxDuration, TSqlParserResource::SQL46102Message, maxDuration->Value, CodeGenerationSupporter::Self);
            }
        }

// TSql120ParserBaseInternal.cs:69 (31 C# lines)
void TSql120ParserBase::VerifyAllowedOnlineIndexOptionLowPriorityLockWait(IndexAffectingStatement statement, ast::IndexOption* option, SqlVersionFlags versionFlags) {
            // for a low priority lock wait (MLP) option, check if it is allowed for the statement.
            //
            if ((dynamic_cast<ast::OnlineIndexOption*>(option) != nullptr))
            {
                ast::OnlineIndexOption* onlineIndexOption = dynamic_cast<ast::OnlineIndexOption*>(option);
                if (onlineIndexOption->LowPriorityLockWaitOption != nullptr)
                {
                    // This syntax for CREATE INDEX currently applies to SQL Server 2022 (16.x), Azure SQL Database, and Azure SQL Managed Instance only. For ALTER INDEX, this syntax applies to SQL Server (Starting with SQL Server 2014 (12.x)) and Azure SQL Database.
                    switch (statement)
                    {
                        case IndexAffectingStatement::AlterIndexRebuildOnePartition:
                        case IndexAffectingStatement::AlterTableRebuildOnePartition:
                        case IndexAffectingStatement::AlterIndexRebuildAllPartitions:
                        case IndexAffectingStatement::AlterTableRebuildAllPartitions:
                            // allowed
                            //
                            break;

                        case IndexAffectingStatement::CreateIndex:
                            // allowed in Sql160 and higher only
                            //
                            if (versionFlags > SqlVersionFlags::TSql150)
                            {
                                break;
                            }
                            else
                            {
                                ThrowWrongIndexOptionError(statement, onlineIndexOption->LowPriorityLockWaitOption);
                                break;
                            }

                        default:
                            // WAIT_AT_LOW_PRIORITY is not a valid index option in the statement
                            //
                            ThrowWrongIndexOptionError(statement, onlineIndexOption->LowPriorityLockWaitOption);
                            break;
                    }
                }
            }
        }

}  // namespace tsql::parser
