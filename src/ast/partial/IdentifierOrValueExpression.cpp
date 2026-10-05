// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/IdentifierOrValueExpression.cs
#include "tsql/ast/ast.hpp"

namespace tsql::ast {

std::optional<std::string> IdentifierOrValueExpression::Value() const {
    if (Identifier != nullptr) return Identifier->Value;
    if (ValueExpression == nullptr) return std::nullopt;
    if (auto* literal = dynamic_cast<const Literal*>(ValueExpression)) return literal->Value;
    if (auto* variable = dynamic_cast<const VariableReference*>(ValueExpression)) return variable->Name;
    if (auto* global = dynamic_cast<const GlobalVariableExpression*>(ValueExpression)) return global->Name;
    return std::nullopt;
}

}  // namespace tsql::ast
