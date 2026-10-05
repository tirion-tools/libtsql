// libtsql parser pilot: .NET Enum.ToString() formatting for the generated enum tables.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tsql::ast::detail {

/// One enumerator; tables are sorted by value compared as unsigned (as .NET does).
struct EnumEntry {
    std::uint32_t value;
    const char* name;
};

/// Name of `value`, or its decimal text when undefined.
std::string FormatEnum(const EnumEntry* entries, std::size_t count, std::int32_t value);
/// [Flags] formatting: exact name, else "A, B" decomposition, else decimal text.
std::string FormatFlagsEnum(const EnumEntry* entries, std::size_t count, std::int32_t value);

}  // namespace tsql::ast::detail
