// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlFragment.cs
#include "tsql/ast/fragment.hpp"

#include <utility>

namespace tsql::ast {

// Intentionally left empty (C#: TSqlFragment.Accept / AcceptChildren).
void TSqlFragment::Accept(TSqlFragmentVisitor*) {}
void TSqlFragment::AcceptChildren(TSqlFragmentVisitor*) {}

void TSqlFragment::UpdateTokenInfo(const TSqlFragment* fragment) {
    // It is possible that the fragment is null.
    if (fragment == nullptr) return;
    UpdateTokenInfo(fragment->FirstTokenIndex, fragment->LastTokenIndex);
    if (fragment->ScriptTokenStream != nullptr) ScriptTokenStream = fragment->ScriptTokenStream;
}

void TSqlFragment::UpdateTokenInfo(int firstIndex, int lastIndex) {
    // Disregard invalid values
    if (firstIndex < 0 || lastIndex < 0) return;
    if (firstIndex > lastIndex) std::swap(firstIndex, lastIndex);
    if (firstIndex < FirstTokenIndex || FirstTokenIndex == Uninitialized) FirstTokenIndex = firstIndex;
    if (lastIndex > LastTokenIndex || LastTokenIndex == Uninitialized) LastTokenIndex = lastIndex;
}

// Out-of-range token indexes throw std::out_of_range (C#: ArgumentOutOfRangeException).
int TSqlFragment::StartOffset() const {
    if (FirstTokenIndex == Uninitialized || ScriptTokenStream == nullptr) return Uninitialized;
    return ScriptTokenStream->at(FirstTokenIndex).Offset;
}

int TSqlFragment::FragmentLength() const {
    if (FirstTokenIndex == Uninitialized || LastTokenIndex == Uninitialized || ScriptTokenStream == nullptr)
        return Uninitialized;
    const TSqlParserToken& lastToken = ScriptTokenStream->at(LastTokenIndex);
    return lastToken.Offset - ScriptTokenStream->at(FirstTokenIndex).Offset + Utf16Length(lastToken.Text);
}

int TSqlFragment::StartLine() const {
    if (FirstTokenIndex == Uninitialized || ScriptTokenStream == nullptr) return Uninitialized;
    return ScriptTokenStream->at(FirstTokenIndex).Line;
}

int TSqlFragment::StartColumn() const {
    if (FirstTokenIndex == Uninitialized || ScriptTokenStream == nullptr) return Uninitialized;
    return ScriptTokenStream->at(FirstTokenIndex).Column;
}

}  // namespace tsql::ast
