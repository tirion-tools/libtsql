// libtsql AST: C++ AST of Microsoft SqlScriptDOM (MIT) @ eaf3a6e.
//
// Every Ast.xml class is a struct of the same name in tsql::ast deriving from its Ast.xml base
// (TSqlFragment by default). Members are public fields with the Ast.xml names:
//   fragment -> T* (nullptr), collection -> std::vector<T*>, bool -> bool, int -> std::int32_t,
//   string -> std::optional<std::string> (null != ""), enum -> enum class (value-initialized to 0),
//   nullable value types (bool?, Enum?) -> std::optional<T>.
// Each non-collection member X also has set_X(value), which mirrors the C# property setter:
// fragment members with GenerateUpdatePositionInfoCall call UpdateTokenInfo(value) first.
// CollectionFirstItem members are accessor pairs X()/set_X() over the Xs collection.
// Ast.xml interfaces (ICollationSetter, ...) are abstract structs with virtual get_X/set_X.
// Nodes are created and owned by FragmentFactory. Every node has Accept/AcceptChildren for
// TSqlFragmentVisitor / TSqlConcreteFragmentVisitor (tsql/ast/visitor.hpp).
#pragma once

#include "tsql/ast/token.hpp"
#include "tsql/ast/fragment.hpp"
#include "tsql/ast/generated/enums.hpp"
#include "tsql/ast/generated/nodes.hpp"
#include "tsql/ast/visitor.hpp"
#include "tsql/ast/fragment_factory.hpp"
#include "tsql/ast/dump.hpp"
