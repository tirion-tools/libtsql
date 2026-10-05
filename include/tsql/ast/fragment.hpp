// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlFragment.cs
#pragma once

#include "tsql/ast/generated/fragment_type_id.hpp"
#include "tsql/ast/token.hpp"

namespace tsql::ast {

class TSqlFragmentVisitor;

/// Root of every AST node. Positions are token indexes into ScriptTokenStream.
struct TSqlFragment {
    static constexpr int Uninitialized = -1;

    int FirstTokenIndex = Uninitialized;
    int LastTokenIndex = Uninitialized;
    /// The token stream of the script this fragment belongs to (not owned).
    ::tsql::ast::ScriptTokenStream* ScriptTokenStream = nullptr;

    TSqlFragment() = default;
    TSqlFragment(const TSqlFragment&) = delete;
    TSqlFragment& operator=(const TSqlFragment&) = delete;
    virtual ~TSqlFragment() = default;

    /// Dynamic type of the node (concrete Ast.xml class).
    virtual FragmentTypeId TypeId() const = 0;
    /// Ast.xml class name of the dynamic type.
    virtual const char* TypeName() const = 0;

    /// Concrete classes call visitor->ExplicitVisit(this) (no-op for a null visitor); empty here.
    virtual void Accept(TSqlFragmentVisitor* visitor);
    /// Visits the children in tools/AstGen order; empty here.
    virtual void AcceptChildren(TSqlFragmentVisitor* visitor);

    /// Offset (UTF-16 code units) of the first token, or Uninitialized.
    int StartOffset() const;
    /// Length (UTF-16 code units) from the first token to the end of the last token, or Uninitialized.
    int FragmentLength() const;
    int StartLine() const;
    int StartColumn() const;

    /// Extends this fragment's token range to cover `fragment` (no-op for null) and adopts its token stream.
    void UpdateTokenInfo(const TSqlFragment* fragment);
    /// Extends this fragment's token range to cover [firstIndex, lastIndex]; negative indexes are ignored.
    void UpdateTokenInfo(int firstIndex, int lastIndex);
};

}  // namespace tsql::ast
