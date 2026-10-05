// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSql110ParserBaseInternal.cs
#include "TSql110ParserBase.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <stack>

namespace tsql::parser {

namespace {
[[maybe_unused]] constexpr size_t TT(ast::TSqlTokenType t) { return static_cast<size_t>(t); }
using TK = ast::TSqlTokenType;
[[maybe_unused]] constexpr size_t kEOF = antlr4::Token::EOF;
}  // namespace

namespace {
bool isFollowingDelimiter(ast::WindowDelimiter* d) {
    return d != nullptr && (d->WindowDelimiterType == ast::WindowDelimiterType::ValueFollowing ||
                            d->WindowDelimiterType == ast::WindowDelimiterType::UnboundedFollowing);
}
bool isPrecedingDelimiter(ast::WindowDelimiter* d) {
    return d != nullptr && (d->WindowDelimiterType == ast::WindowDelimiterType::ValuePreceding ||
                            d->WindowDelimiterType == ast::WindowDelimiterType::UnboundedPreceding);
}
}  // namespace

void TSql110ParserBase::CheckWindowFrame(ast::WindowFrameClause* w) {
    using D = ast::WindowDelimiterType;
    bool topHasValueSpecified = w->Top != nullptr &&
                                (w->Top->WindowDelimiterType == D::ValuePreceding || w->Top->WindowDelimiterType == D::ValueFollowing);
    bool bottomHasValueSpecified = w->Bottom != nullptr && (w->Bottom->WindowDelimiterType == D::ValuePreceding ||
                                                            w->Bottom->WindowDelimiterType == D::ValueFollowing);
    if (w->WindowFrameType == ast::WindowFrameType::Range && (topHasValueSpecified || bottomHasValueSpecified))
        ThrowParseErrorException("SQL46099", w, TSqlParserResource::SQL46099Message);
    if (w->Top == nullptr) ThrowParseErrorException("SQL46100", w, TSqlParserResource::SQL46100Message);
    if (w->Bottom == nullptr && isFollowingDelimiter(w->Top))
        ThrowParseErrorException("SQL46100", w, TSqlParserResource::SQL46100Message);
    if (isFollowingDelimiter(w->Top) && isPrecedingDelimiter(w->Bottom))
        ThrowParseErrorException("SQL46100", w, TSqlParserResource::SQL46100Message);
    if (w->Top->WindowDelimiterType == D::UnboundedFollowing ||
        (w->Bottom != nullptr && w->Bottom->WindowDelimiterType == D::UnboundedPreceding))
        ThrowParseErrorException("SQL46100", w, TSqlParserResource::SQL46100Message);
    if (isFollowingDelimiter(w->Top) && w->Bottom != nullptr && w->Bottom->WindowDelimiterType == D::CurrentRow)
        ThrowParseErrorException("SQL46100", w, TSqlParserResource::SQL46100Message);
    if (w->Top->WindowDelimiterType == D::CurrentRow && isPrecedingDelimiter(w->Bottom))
        ThrowParseErrorException("SQL46100", w, TSqlParserResource::SQL46100Message);
}

// TSql110ParserBaseInternal.cs:128 (4 C# lines) (hand-fixed)
std::vector<std::string> TSql110ParserBase::OptionValidForCreateDatabase() {
    // TSql110ParserBaseInternal._optionsValidForCreateDatabase
    return {CodeGenerationSupporter::Containment, CodeGenerationSupporter::DefaultLanguage,
            CodeGenerationSupporter::DefaultFullTextLanguage, CodeGenerationSupporter::FileStream,
            CodeGenerationSupporter::NestedTriggers, CodeGenerationSupporter::TransformNoiseWords,
            CodeGenerationSupporter::TwoDigitYearCutoff};
}

}  // namespace tsql::parser
