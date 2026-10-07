// libtsql AST: .NET Enum.ToString() formatting (mirrors System.Enum's name lookup and
// [Flags] decomposition: largest-first greedy match, names joined ascending with ", ").
#include "enum_format.hpp"

#include <vector>

namespace tsql::ast::detail {

std::string FormatEnum(const EnumEntry* entries, std::size_t count, std::int32_t value) {
    const std::uint32_t v = static_cast<std::uint32_t>(value);
    for (std::size_t i = 0; i < count; ++i) {
        if (entries[i].value == v) return entries[i].name;
    }
    return std::to_string(value);
}

std::string FormatFlagsEnum(const EnumEntry* entries, std::size_t count, std::int32_t value) {
    std::uint32_t rest = static_cast<std::uint32_t>(value);
    if (rest == 0) return count > 0 && entries[0].value == 0 ? std::string(entries[0].name) : std::string("0");

    // Walk from the largest value; an exact match wins outright.
    std::size_t index = count;
    while (index > 0) {
        const std::uint32_t current = entries[index - 1].value;
        if (current == rest) return entries[index - 1].name;
        if (current < rest) break;
        --index;
    }
    std::vector<std::size_t> found;
    while (index > 0) {
        const std::uint32_t current = entries[index - 1].value;
        if (index == 1 && current == 0) break;
        if ((rest & current) == current) {
            rest -= current;
            found.push_back(index - 1);
        }
        --index;
    }
    if (rest != 0) return std::to_string(value);

    std::string text;
    for (auto it = found.rbegin(); it != found.rend(); ++it) {
        if (!text.empty()) text += ", ";
        text += entries[*it].name;
    }
    return text;
}

}  // namespace tsql::ast::detail
