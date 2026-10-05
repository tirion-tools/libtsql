// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/ScriptDom/SqlServer/VersioningVisitor.cs,
// SqlScriptDom/ScriptDom/SqlServer/Versioning/VersioningVisitor.LedgerTableOption.cs,
// SqlScriptDom/ScriptDom/SqlServer/Versioning/VersioningVisitor.SystemVersioningTableOption.cs
#include "VersioningVisitor.h"

#include "CsCompat.h"
#include "ParserErrors.h"
#include "generated/support/TSqlParserResource.h"

namespace tsql::parser {

// C# compares the SqlVersion enum values numerically (SqlFabricDW = 10 counts as newer than Sql160).
static bool Before(SqlVersion v, SqlVersion bound) { return static_cast<int>(v) < static_cast<int>(bound); }

void VersioningVisitor::AddVersioningError(int offset, int line, int column, const std::string& unsupportedStatement) {
    // The script contains clause {0} which is not supported by the targeted version of SQL Server.
    _errors.push_back(MakeParseError("SQL46117", offset, line, column,
                                     String_Format(CultureInfo::CurrentCulture, TSqlParserResource::SQL46117Message,
                                                   unsupportedStatement)));
}

void VersioningVisitor::ExplicitVisit(ast::LedgerTableOption* node) {
    if (node->OptionState == ast::OptionState::On) {
        if (Before(_targetVersion, SqlVersion::Sql160)) {
            AddVersioningError(node->StartOffset(), node->StartLine(), node->StartColumn(),
                               String_Format(CultureInfo::InvariantCulture, TSqlParserResource::SQL46140Message));
        }
    }
}

void VersioningVisitor::ExplicitVisit(ast::SystemVersioningTableOption* node) {
    if (node->OptionState == ast::OptionState::On) {
        if (node->RetentionPeriod != nullptr) {
            if (!node->RetentionPeriod->IsInfinity) {
                if (_targetEngineType != SqlEngineType::SqlAzure && Before(_targetVersion, SqlVersion::Sql140)) {
                    AddVersioningError(node->StartOffset(), node->StartLine(), node->StartColumn(), "HISTORY_RETENTION_PERIOD");
                }
            }
        }
    }
}

}  // namespace tsql::parser
