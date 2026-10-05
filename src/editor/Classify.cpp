// tsql::editor::Classify: token classes for colouring. Every token gets its lexer class; identifiers
// the version's parser takes as keywords (actions or predicates that check their text), as built-in
// data type names or, followed by '(', as built-in function names are classed as such.
#include <algorithm>

#include "Builtins.h"
#include "Grammar.h"
#include "Names.h"
#include "tsql/ast/generated/token_types.hpp"
#include "tsql/editor.hpp"

namespace tsql::editor {

using namespace detail;

namespace {

using T = ast::TSqlTokenType;
constexpr uint32_t Ty(T t) { return static_cast<uint32_t>(t); }

TokenClass LexerClass(uint32_t type) {
    switch (static_cast<T>(type)) {
        case T::WhiteSpace: return TokenClass::Whitespace;
        case T::SingleLineComment: case T::MultilineComment: return TokenClass::Comment;
        case T::AsciiStringLiteral: case T::UnicodeStringLiteral: return TokenClass::String;
        case T::Integer: case T::Numeric: case T::Real: case T::Money: case T::HexLiteral: return TokenClass::Number;
        case T::Variable: return TokenClass::Variable;
        case T::QuotedIdentifier: case T::AsciiStringOrQuotedIdentifier: return TokenClass::QuotedIdentifier;
        case T::Identifier: case T::Label: return TokenClass::Identifier;
        case T::Go: case T::PseudoColumn: case T::DollarPartition: case T::SqlCommandIdentifier: return TokenClass::Keyword;
        case T::LeftParenthesis: case T::RightParenthesis: case T::LeftCurly: case T::RightCurly: case T::Comma:
        case T::Dot: case T::Semicolon: case T::DoubleColon: case T::Colon: case T::OdbcInitiator:
        case T::ProcNameSemicolon:
            return TokenClass::Punctuation;
        case T::Bang: case T::PercentSign: case T::Ampersand: case T::Star: case T::MultiplyEquals: case T::Plus:
        case T::Minus: case T::Divide: case T::LessThan: case T::EqualsSign: case T::RightOuterJoin:
        case T::GreaterThan: case T::Circumflex: case T::VerticalLine: case T::Tilde: case T::AddEquals:
        case T::SubtractEquals: case T::DivideEquals: case T::ModEquals: case T::BitwiseAndEquals:
        case T::BitwiseOrEquals: case T::BitwiseXorEquals: case T::LeftShift: case T::RightShift: case T::Concat:
        case T::ConcatEquals:
            return TokenClass::Operator;
        default:
            return KeywordText(type) != nullptr ? TokenClass::Keyword : TokenClass::Error;
    }
}

bool InWords(const Grammar& g, int state, const std::string& upper, bool binding) {
    auto it = g.keywordStates.find(state);
    return it != g.keywordStates.end() && it->second.binding == binding &&
           std::binary_search(it->second.words.begin(), it->second.words.end(), upper);
}

class IdentifierClassifier {
public:
    explicit IdentifierClassifier(const Grammar& g) : g_(g) {
        for (const char* n : {"scalarDataType", "dataTypeSchemaObjectName"}) {
            const size_t r = g.RuleIndex(n);
            if (r != SIZE_MAX) typeRules_.push_back(r);
        }
    }

    TokenClass Classify(const TokenRole& role, uint32_t type, const std::string& upper) const {
        if (!g_.StateMatches(role.state, type)) return TokenClass::Identifier;   // skipped by error recovery
        if (role.predicateKeyword) return TokenClass::Keyword;
        if (InWords(g_, role.state, upper, true)) return TokenClass::Keyword;
        bool typeRule = false;
        for (size_t k = 0; k < 3; ++k)
            if (std::find(typeRules_.begin(), typeRules_.end(), role.rules[k]) != typeRules_.end()) typeRule = true;
        for (int s : role.callStates) {
            if (s < 0) continue;
            if (InWords(g_, s, upper, true)) return TokenClass::Keyword;
            if (InWords(g_, s, upper, false)) return typeRule ? TokenClass::DataType : TokenClass::Keyword;
        }
        if (InWords(g_, role.state, upper, false)) return typeRule ? TokenClass::DataType : TokenClass::Keyword;
        return TokenClass::Identifier;
    }

private:
    const Grammar& g_;
    std::vector<size_t> typeRules_;
};

}  // namespace

std::vector<ColouredSpan> Classify(std::string_view sql, SqlVersion version) {
    const Grammar& g = GrammarFor(version);
    std::vector<ColouredSpan> spans;
    if (sql.empty()) return spans;
    const ParsedTokens parsed = g.ParseAll(sql);
    const IdentifierClassifier identifiers(g);
    const auto& toks = parsed.tokens;

    auto nextVisible = [&](size_t i) -> uint32_t {
        for (size_t k = i + 1; k < toks.size(); ++k)
            if (!IsHiddenType(toks[k].type)) return toks[k].type;
        return 0;
    };
    uint32_t prevVisible = 0;
    size_t pos = 0;
    spans.reserve(toks.size() + 1);
    for (size_t i = 0; i < toks.size(); ++i) {
        const LexToken& t = toks[i];
        if (t.end <= pos) continue;
        if (t.start > pos) spans.push_back({pos, t.start - pos, TokenClass::Error});
        const size_t start = std::max<size_t>(t.start, pos);
        TokenClass cls = LexerClass(t.type);
        if (cls == TokenClass::Identifier || cls == TokenClass::Keyword) {
            const std::string upper = Upper(sql.substr(t.start, t.end - t.start));
            const Builtin* b = FindBuiltin(upper);
            if (b != nullptr && b->kind != Builtin::Kind::GlobalVariable && prevVisible != Ty(T::Dot) &&
                nextVisible(i) == Ty(T::LeftParenthesis))
                cls = TokenClass::BuiltinFunction;
            else if (t.type == Ty(T::Identifier))
                cls = identifiers.Classify(parsed.roles[i], t.type, upper);
        }
        spans.push_back({start, t.end - start, cls});
        pos = t.end;
        if (!IsHiddenType(t.type)) prevVisible = t.type;
    }
    if (pos < sql.size()) spans.push_back({pos, sql.size() - pos, TokenClass::Error});
    return spans;
}

}  // namespace tsql::editor
