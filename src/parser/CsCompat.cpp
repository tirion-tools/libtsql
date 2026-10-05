// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: .NET BCL semantics used by the grammar actions.
#include "CsCompat.h"

#include <climits>
#include <cstdlib>

namespace tsql::parser {

namespace {

// Decodes one UTF-8 sequence at s[i]; returns code point and advances i (lenient: bad byte -> U+FFFD).
char32_t Next(std::string_view s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    size_t n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (n == 0 || i + n >= s.size()) { ++i; return 0xFFFD; }
    char32_t cp = c & (0x3F >> n);
    for (size_t k = 1; k <= n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += n + 1;
    return cp;
}

void Append(std::string& out, char32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool IsCsWhite(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

bool IgnoreCase(StringComparison c) {
    return c == StringComparison::OrdinalIgnoreCase || c == StringComparison::CurrentCultureIgnoreCase ||
           c == StringComparison::InvariantCultureIgnoreCase;
}

// UTF-8 byte offset of UTF-16 index `u16` in s.
size_t ByteOffsetOfUtf16(std::string_view s, int u16) {
    size_t i = 0;
    int u = 0;
    while (i < s.size() && u < u16) {
        char32_t cp = Next(s, i);
        u += cp >= 0x10000 ? 2 : 1;
    }
    if (u != u16 && u16 > 0) throw std::out_of_range("Substring");
    return i;
}

}  // namespace

int Utf16Len(std::string_view s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) n += Next(s, i) >= 0x10000 ? 2 : 1;
    return n;
}

std::string AsciiUpper(std::string_view s) {
    std::string r(s);
    for (auto& c : r)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return r;
}

std::string AsciiLower(std::string_view s) {
    std::string r(s);
    for (auto& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
        if (x != y) return false;
    }
    return true;
}

bool String_Equals(CsStr a, CsStr b, StringComparison c) {
    if (a.null || b.null) return a.null && b.null;
    return IgnoreCase(c) ? EqualsIgnoreCase(a.v, b.v) : a.v == b.v;
}

bool String_IsNullOrEmpty(CsStr a) { return a.null || a.v.empty(); }

bool Str_Equals(CsStr self, CsStr other, StringComparison c) {
    self.get();
    return String_Equals(self, other, c);
}

std::string Str_ToUpper(CsStr self, CultureInfo) { return AsciiUpper(self.get()); }
std::string Str_ToUpperInvariant(CsStr self) { return AsciiUpper(self.get()); }
std::string Str_ToLower(CsStr self, CultureInfo) { return AsciiLower(self.get()); }
std::string Str_ToLowerInvariant(CsStr self) { return AsciiLower(self.get()); }
int Str_Length(CsStr self) { return Utf16Len(self.get()); }

bool Str_StartsWith(CsStr self, CsStr prefix, StringComparison c) {
    auto s = self.get();
    auto p = prefix.get();
    if (p.size() > s.size()) return false;
    return IgnoreCase(c) ? EqualsIgnoreCase(s.substr(0, p.size()), p) : s.substr(0, p.size()) == p;
}

bool Str_EndsWith(CsStr self, CsStr suffix, StringComparison c) {
    auto s = self.get();
    auto p = suffix.get();
    if (p.size() > s.size()) return false;
    auto t = s.substr(s.size() - p.size());
    return IgnoreCase(c) ? EqualsIgnoreCase(t, p) : t == p;
}

std::string Str_Substring(CsStr self, int start) {
    auto s = self.get();
    return std::string(s.substr(ByteOffsetOfUtf16(s, start)));
}

std::string Str_Substring(CsStr self, int start, int length) {
    auto s = self.get();
    size_t b = ByteOffsetOfUtf16(s, start);
    size_t e = b + ByteOffsetOfUtf16(s.substr(b), length);
    return std::string(s.substr(b, e - b));
}

std::string Str_Trim(CsStr self) {
    auto s = self.get();
    std::u32string cps;
    for (size_t i = 0; i < s.size();) cps += Next(s, i);
    size_t a = 0, b = cps.size();
    while (a < b && IsCsWhite(cps[a])) ++a;
    while (b > a && IsCsWhite(cps[b - 1])) --b;
    std::string out;
    for (size_t k = a; k < b; ++k) Append(out, cps[k]);
    return out;
}

namespace {
std::u16string ToUtf16(std::string_view s) {
    std::u16string r;
    for (size_t i = 0; i < s.size();) {
        char32_t cp = Next(s, i);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            r += static_cast<char16_t>(0xD800 + (cp >> 10));
            r += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            r += static_cast<char16_t>(cp);
        }
    }
    return r;
}
}  // namespace

int Str_IndexOf(CsStr self, CsStr value) {
    auto h = ToUtf16(self.get()), n = ToUtf16(value.get());
    auto p = h.find(n);
    return p == std::u16string::npos ? -1 : static_cast<int>(p);
}
int Str_IndexOf(CsStr self, char value) { return Str_IndexOf(self, std::string_view(&value, 1)); }
int Str_LastIndexOf(CsStr self, CsStr value) {
    auto h = ToUtf16(self.get()), n = ToUtf16(value.get());
    auto p = h.rfind(n);
    return p == std::u16string::npos ? -1 : static_cast<int>(p);
}
int Str_LastIndexOf(CsStr self, char value) { return Str_LastIndexOf(self, std::string_view(&value, 1)); }
int Str_At(CsStr self, int index) {
    auto u = ToUtf16(self.get());
    if (index < 0 || static_cast<size_t>(index) >= u.size()) throw std::out_of_range("IndexOutOfRangeException");
    return u[static_cast<size_t>(index)];
}
bool Str_Contains(CsStr self, CsStr value) { return self.get().find(value.get()) != std::string_view::npos; }
int String_Compare(CsStr a, CsStr b, StringComparison c) {
    if (a.null || b.null) return a.null == b.null ? 0 : (a.null ? -1 : 1);
    std::string x(a.v), y(b.v);
    if (IgnoreCase(c)) { x = AsciiUpper(x); y = AsciiUpper(y); }
    return x < y ? -1 : (x > y ? 1 : 0);
}
bool UInt64_TryParse(CsStr str, uint64_t& result) {
    result = 0;
    if (str.null) return false;
    std::string t = Str_Trim(str);
    size_t i = 0;
    if (i < t.size() && t[i] == '+') ++i;
    if (i >= t.size()) return false;
    unsigned long long v = 0;
    for (; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') return false;
        unsigned long long d = static_cast<unsigned long long>(t[i] - '0');
        if (v > (~0ULL - d) / 10) return false;
        v = v * 10 + d;
    }
    result = v;
    return true;
}

std::string RemoveWhitespace(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        char32_t cp = Next(s, i);
        if (!IsCsWhite(cp)) Append(out, cp);
    }
    return out;
}

bool Int32_TryParse(CsStr s, NumberStyles, CultureInfo, int& result) { return Int32_TryParse(s, result); }

bool Int32_TryParse(CsStr str, int& result) {
    result = 0;
    if (str.null) return false;
    std::string_view s = str.v;
    size_t i = 0, n = s.size();
    while (i < n && (s[i] == ' ' || (s[i] >= 0x09 && s[i] <= 0x0D))) ++i;
    while (n > i && (s[n - 1] == ' ' || (s[n - 1] >= 0x09 && s[n - 1] <= 0x0D))) --n;
    bool neg = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    if (i >= n) return false;
    long long v = 0;
    for (; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > static_cast<long long>(INT_MAX) + 1) return false;
    }
    if (neg) v = -v;
    if (v > INT_MAX || v < INT_MIN) return false;
    result = static_cast<int>(v);
    return true;
}

bool Decimal_TryParse(CsStr s, NumberStyles, CultureInfo, double& result) {
    result = 0;
    if (s.null) return false;
    std::string t = Str_Trim(s);
    if (t.empty()) return false;
    // NumberStyles.Float: [ws][sign]digits[.digits][e[sign]digits][ws]
    size_t i = 0;
    if (t[i] == '+' || t[i] == '-') ++i;
    bool digits = false;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') { ++i; digits = true; }
    if (i < t.size() && t[i] == '.') {
        ++i;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') { ++i; digits = true; }
    }
    if (!digits) return false;
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
        bool exp = false;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') { ++i; exp = true; }
        if (!exp) return false;
    }
    if (i != t.size()) return false;
    result = std::strtod(t.c_str(), nullptr);
    return true;
}

int Int32_Parse(CsStr s, NumberStyles, CultureInfo) {
    int v = 0;
    if (s.null) throw std::invalid_argument("ArgumentNullException");
    if (!Int32_TryParse(s, v)) throw std::runtime_error("FormatException");
    return v;
}

namespace detail {
std::string Format(std::string_view templ, const std::vector<std::string>& args) {
    std::string out;
    for (size_t i = 0; i < templ.size(); ++i) {
        char c = templ[i];
        if (c == '{' && i + 1 < templ.size() && templ[i + 1] == '{') { out += '{'; ++i; continue; }
        if (c == '}' && i + 1 < templ.size() && templ[i + 1] == '}') { out += '}'; ++i; continue; }
        if (c == '{') {
            size_t j = templ.find('}', i);
            if (j != std::string_view::npos) {
                size_t idx = 0;
                bool ok = j > i + 1;
                for (size_t k = i + 1; k < j; ++k) {
                    if (templ[k] < '0' || templ[k] > '9') { ok = false; break; }
                    idx = idx * 10 + static_cast<size_t>(templ[k] - '0');
                }
                if (ok && idx < args.size()) {
                    out += args[idx];
                    i = j;
                    continue;
                }
            }
        }
        out += c;
    }
    return out;
}
}  // namespace detail

}  // namespace tsql::parser
