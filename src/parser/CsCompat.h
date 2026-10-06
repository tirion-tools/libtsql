// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: .NET BCL semantics used by the grammar actions
// (System.String, Int32.TryParse, List<T>) as free functions the converter's rewrites target:
//   String.X(a, ...)  -> String_X(a, ...)      s.X(...) -> Str_X(s, ...)      list.X(...) -> Vec_X(list, ...)
// Strings are UTF-8; lengths/indexes are UTF-16 code units like .NET. C# null strings are
// std::nullopt / nullptr const char*.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "tsql/ast/ast.hpp"

namespace tsql::parser {

enum class StringComparison { CurrentCulture, CurrentCultureIgnoreCase, InvariantCulture, InvariantCultureIgnoreCase, Ordinal, OrdinalIgnoreCase };
enum class CultureInfo { InvariantCulture, CurrentCulture };
enum class NumberStyles { None, Integer, Float, Number, AllowLeadingSign };

/// Thrown where C# would throw NullReferenceException (caught as an internal error).
struct NullReferenceException : std::runtime_error {
    NullReferenceException() : std::runtime_error("NullReferenceException") {}
};

/// A C# string argument: view + null flag.
struct CsStr {
    std::string_view v;
    bool null = false;
    CsStr(const std::string& s) : v(s) {}
    CsStr(const char* s) : v(s ? std::string_view(s) : std::string_view()), null(s == nullptr) {}
    CsStr(std::string_view s) : v(s) {}
    CsStr(const std::optional<std::string>& s) : v(s ? std::string_view(*s) : std::string_view()), null(!s) {}
    const std::string_view& get() const {
        if (null) throw NullReferenceException();
        return v;
    }
    operator std::string() const { return std::string(get()); }
};

int Utf16Len(std::string_view s);
/// ASCII-only case folding (all keywords/constants compared against are ASCII).
std::string AsciiUpper(std::string_view s);
std::string AsciiLower(std::string_view s);
bool EqualsIgnoreCase(std::string_view a, std::string_view b);

bool String_Equals(CsStr a, CsStr b, StringComparison c = StringComparison::Ordinal);
bool String_IsNullOrEmpty(CsStr a);

bool Str_Equals(CsStr self, CsStr other, StringComparison c = StringComparison::Ordinal);
std::string Str_ToUpper(CsStr self, CultureInfo = CultureInfo::CurrentCulture);
std::string Str_ToUpperInvariant(CsStr self);
/// self.ToUpper[Invariant]() == other, without building the upper-case copy
bool Str_UpperEquals(CsStr self, CsStr other);
std::string Str_ToLower(CsStr self, CultureInfo = CultureInfo::CurrentCulture);
std::string Str_ToLowerInvariant(CsStr self);
int Str_Length(CsStr self);
bool Str_StartsWith(CsStr self, CsStr prefix, StringComparison c = StringComparison::CurrentCulture);
bool Str_EndsWith(CsStr self, CsStr suffix, StringComparison c = StringComparison::CurrentCulture);
/// UTF-16 based Substring.
std::string Str_Substring(CsStr self, int start);
std::string Str_Substring(CsStr self, int start, int length);
std::string Str_Trim(CsStr self);
/// Ordinal IndexOf/LastIndexOf in UTF-16 units (-1 when absent).
int Str_IndexOf(CsStr self, CsStr value);
int Str_IndexOf(CsStr self, char value);
int Str_LastIndexOf(CsStr self, CsStr value);
int Str_LastIndexOf(CsStr self, char value);
/// s[i] (UTF-16 code unit at index i).
int Str_At(CsStr self, int index);
bool Str_Contains(CsStr self, CsStr value);
/// String.Compare(a, b, StringComparison) sign.
int String_Compare(CsStr a, CsStr b, StringComparison c = StringComparison::CurrentCulture);
bool UInt64_TryParse(CsStr s, uint64_t& result);
inline bool UInt64_TryParse(CsStr s, NumberStyles, CultureInfo, uint64_t& result) { return UInt64_TryParse(s, result); }
/// Removes every Unicode white-space character (Regex \s).
std::string RemoveWhitespace(std::string_view s);

/// Int32.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out result)
bool Int32_TryParse(CsStr s, NumberStyles styles, CultureInfo culture, int& result);
bool Int32_TryParse(CsStr s, int& result);

template <class T, class U>
void Vec_Insert(std::vector<T>& v, int index, U item) {
    v.insert(v.begin() + index, item);
}
template <class T, class U>
bool Vec_Contains(const std::vector<T>& v, const U& item) {
    for (const auto& x : v)
        if (x == item) return true;
    return false;
}

/// C# `a ?? b` for references (both operands are evaluated; the converter only emits it for
/// side-effect-free operands). Returns by value: `a ? a : b` on two lvalues is itself an lvalue.
template <class A, class B>
auto CsCoalesce(A a, B b) -> std::decay_t<decltype(a ? a : b)> { return a ? a : b; }

/// Name table of a C# enum (specialized in generated/support/ParserEnums.h).
template <class E>
struct EnumName {
    const char* name;
    E value;
};
template <class E>
struct EnumNames;

/// System.Enum.TryParse<E>(value, ignoreCase, out result): a name, an integer (defined or not)
/// or a comma-separated list of either (OR-ed), surrounded by optional white space.
template <class E>
bool Enum_TryParse(CsStr value, bool ignoreCase, E& result) {
    result = E{};
    if (value.null) return false;
    std::string_view s = value.v;
    long long acc = 0;
    size_t pos = 0;
    bool any = false;
    while (pos <= s.size()) {
        size_t comma = s.find(',', pos);
        std::string_view part = s.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        while (!part.empty() && (part.front() == ' ' || (part.front() >= 9 && part.front() <= 13))) part.remove_prefix(1);
        while (!part.empty() && (part.back() == ' ' || (part.back() >= 9 && part.back() <= 13))) part.remove_suffix(1);
        if (part.empty()) return false;
        bool found = false;
        for (const auto& e : EnumNames<E>::entries) {
            if (ignoreCase ? EqualsIgnoreCase(part, e.name) : part == e.name) {
                acc |= static_cast<long long>(e.value);
                found = true;
                break;
            }
        }
        if (!found) {
            const char c = part.front();
            if (!((c >= '0' && c <= '9') || c == '-' || c == '+')) return false;
            int n = 0;
            if (!Int32_TryParse(part, n)) return false;
            acc |= n;
        }
        any = true;
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    if (!any) return false;
    result = static_cast<E>(acc);
    return true;
}

/// Decimal.TryParse(s, NumberStyles.Float, InvariantCulture, out d) (as double; see cs2cpp.py)
bool Decimal_TryParse(CsStr s, NumberStyles styles, CultureInfo culture, double& result);
/// Int32.Parse(s, NumberStyles.Integer, InvariantCulture): throws on bad input like .NET (FormatException
/// or OverflowException, which SqlScriptDOM's parser does not catch: an internal error).
int Int32_Parse(CsStr s, NumberStyles styles = NumberStyles::Integer, CultureInfo culture = CultureInfo::InvariantCulture);
inline int Int32_Parse(CsStr s, CultureInfo culture) { return Int32_Parse(s, NumberStyles::Integer, culture); }

/// String.Format(culture, format, args...)
template <class... A>
std::string String_Format(CultureInfo, std::string_view templ, const A&... args);

/// switch over a C# nullable enum: null matches no case label.
template <class E>
E CsSwitchValue(const std::optional<E>& v) {
    return v ? *v : static_cast<E>(std::numeric_limits<std::underlying_type_t<E>>::min());
}

inline std::string ToString(int v) { return std::to_string(v); }
using ::tsql::ast::ToString;

/// C# string.Format with {0}..{n} placeholders (argument rendering like .NET for strings/ints).
namespace detail {
inline std::string FormatArg(const std::string& s) { return s; }
inline std::string FormatArg(const char* s) { return s ? s : ""; }
inline std::string FormatArg(std::string_view s) { return std::string(s); }
inline std::string FormatArg(const std::optional<std::string>& s) { return s ? *s : std::string(); }
inline std::string FormatArg(int v) { return std::to_string(v); }
inline std::string FormatArg(const CsStr& s) { return std::string(s.get()); }
std::string Format(std::string_view templ, const std::vector<std::string>& args);
}  // namespace detail

template <class... A>
std::string FormatMessage(std::string_view templ, const A&... args) {
    return detail::Format(templ, std::vector<std::string>{detail::FormatArg(args)...});
}

template <class... A>
std::string String_Format(CultureInfo, std::string_view templ, const A&... args) {
    return FormatMessage(templ, args...);
}

}  // namespace tsql::parser
