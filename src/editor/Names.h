// Editor support, internal: T-SQL name handling (case-insensitive comparison, quoting, unquoting).
#pragma once

#include <string>
#include <string_view>

namespace tsql::editor::detail {

/// ASCII upper case (identifiers compare case-insensitively; non-ASCII bytes compare exactly).
std::string Upper(std::string_view s);
bool EqualsI(std::string_view a, std::string_view b);
bool StartsWithI(std::string_view s, std::string_view prefix);

/// The name a bare, [bracketed] or "quoted" identifier token stands for.
std::string Unquote(std::string_view token);

/// Whether `upper` (ASCII upper case) is a reserved keyword of the lexer.
bool IsReservedKeyword(std::string_view upper);

/// Whether `name` has to be written [bracketed]: not a regular identifier (letter, _, @ or #
/// first, then letters, digits, _, @, #, $; non-ASCII counts as a letter) or a reserved keyword.
bool NeedsQuoting(std::string_view name);

/// `name` as it is written in a script: bare when it can be, otherwise [bracketed] with ] doubled.
std::string QuoteName(std::string_view name);

}  // namespace tsql::editor::detail
