// Editor support, internal: T-SQL built-in functions and global variables (SQL Server
// documentation, "System functions" / "@@ functions"), with the version that introduced them.
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "tsql/parser.hpp"

namespace tsql::editor::detail {

struct Builtin {
    enum class Kind { Scalar, Aggregate, Window, TableValued, GlobalVariable };
    const char* name;   // upper case as documented (@@ for global variables)
    Kind kind;
    SqlVersion since;   // first version that has it (Sql130 = 2016 or earlier)
};

/// Every built-in available in `version`.
const std::vector<const Builtin*>& BuiltinsFor(SqlVersion version);

/// The built-in named `upperName` (ASCII upper case), or nullptr.
const Builtin* FindBuiltin(std::string_view upperName);

/// Whether `a` is the same or a later version than `b` (SqlFabricDW ranks with Sql160).
bool VersionAtLeast(SqlVersion a, SqlVersion b);

}  // namespace tsql::editor::detail
