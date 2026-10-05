// Editor support, internal: the operations of tsql::editor::Document on its Buffer (the free
// functions are a Document used once).
#pragma once

#include <vector>

#include "Buffer.h"
#include "tsql/editor.hpp"

namespace tsql::editor::detail {

/// Document::Classify.
std::vector<ColouredSpan> ClassifyRange(Buffer& buffer, size_t start, size_t end);

/// Document::Complete.
CompletionResult CompleteAt(Buffer& buffer, size_t caret, const Catalog& catalog);

}  // namespace tsql::editor::detail
