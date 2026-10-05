// libtsql parser pilot: structural JSON dump of an AST (diffed against the .NET oracle's dump).
#pragma once

#include <string>
#include <vector>

#include "tsql/ast/fragment.hpp"

namespace tsql::ast {

/// A parse error as reported by TSql170Parser.Parse (ParseError.Number/Offset/Line/Column/Message).
struct DumpError {
    int Number = 0;
    int Offset = 0;
    int Line = 0;
    int Column = 0;
    std::string Message;
};

/// The full dump document: {"errors":[...],"tree":NODE-or-null}.
std::string DumpJson(const TSqlFragment* root, const std::vector<DumpError>& errors = {});

/// Just NODE (or null): {"$type":..,"$pos":[first,last,startOffset,length], members...}.
std::string DumpNodeJson(const TSqlFragment* node);

}  // namespace tsql::ast
