// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/ScriptDom/SqlServer/VersioningVisitor.cs,
// SqlScriptDom/ScriptDom/SqlServer/Versioning/VersioningVisitor.LedgerTableOption.cs,
// SqlScriptDom/ScriptDom/SqlServer/Versioning/VersioningVisitor.SystemVersioningTableOption.cs,
// SqlScriptDom/ScriptDom/SqlServer/SqlEngineType.cs
// Engine/version checks every TSql<ver>Parser.Parse runs over the parsed script.
#pragma once

#include <vector>

#include "tsql/ast/visitor.hpp"
#include "tsql/parser.hpp"

namespace tsql::parser {

/// SqlEngineType (the parsers' default is All; the public API does not expose it).
enum class SqlEngineType { All = 0, Standalone = 1, SqlAzure = 2 };

class VersioningVisitor : public ast::TSqlConcreteFragmentVisitor {
public:
    using ast::TSqlConcreteFragmentVisitor::ExplicitVisit;

    VersioningVisitor(SqlEngineType engineType, SqlVersion version) : _targetEngineType(engineType), _targetVersion(version) {}

    const std::vector<ParseError>& GetErrors() const { return _errors; }

    void ExplicitVisit(ast::LedgerTableOption* node) override;
    void ExplicitVisit(ast::SystemVersioningTableOption* node) override;

private:
    void AddVersioningError(int offset, int line, int column, const std::string& unsupportedStatement);

    SqlEngineType _targetEngineType;
    SqlVersion _targetVersion;
    std::vector<ParseError> _errors;
};

}  // namespace tsql::parser
