// Editor support, internal: the tokens that can stand at the caret, found by walking the
// grammar's ATN from the configuration the parser was in (Capture) over the tokens up to the
// caret, through every alternative (no prediction), like antlr4-c3's CodeCompletionCore.
//
// SqlScriptDOM accepts most keywords as Identifier tokens whose text an action or predicate
// checks; the walk applies the grammar's keyword metadata (tools/g2to4/editor_meta.py), so an
// Identifier at the caret comes out either as the words it must be or as a free name.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "Grammar.h"

namespace tsql::editor::detail {

struct WalkInput {
    const std::vector<LexToken>* tokens = nullptr;     // parser tokens
    const std::vector<std::string>* upper = nullptr;   // their upper-cased text
    int startState = -1;
    size_t startIndex = 0;
    std::vector<int> outerFollow;   // innermost first; empty: the start state's rule is outermost
    size_t caret = 0;               // parser-token index of the caret (tokens->size() when it is the end)
    size_t budget = 400'000;        // configurations visited at most
};

/// One way a token can stand at the caret.
struct WalkCandidate {
    size_t tokenType = 0;
    /// Identifier tokens: the words it must be (a keyword recognised by text), null for a free name.
    const std::vector<std::string>* words = nullptr;
    /// Identifier tokens that are free names: words an action recognises without requiring them
    /// (e.g. built-in data type names), or null.
    const std::vector<std::string>* hints = nullptr;
    /// Rule indexes from the rule the token is matched in outwards.
    const std::vector<size_t>* rules = nullptr;
};

struct WalkStats {
    size_t visited = 0;
    size_t emitted = 0;
    bool truncated = false;   // the budget ran out: candidates are a subset
};

/// Calls `emit` for every candidate (duplicates possible).
WalkStats Walk(const Grammar& grammar, const WalkInput& in, const std::function<void(const WalkCandidate&)>& emit);

/// A predicate expression of editor_meta.py, evaluated at parser-token index `at`:
///   m<k>:<WORD>;  NextTokenMatches(WORD, k)      t<k>:<Type>;  LA(k) == Type
///   !<e>   &<n><e1>..<en>   |<n><e1>..<en>   ?  (opaque)
/// Conditions on the caret token come out as Require/Exclude word sets.
struct PredicateValue {
    enum Kind { False, True, Unknown, Require, Exclude } kind = Unknown;
    std::vector<std::string> words;   // Require / Exclude: the caret word's set (sorted)
};
PredicateValue EvaluatePredicate(const Grammar& grammar, const std::string& expr, const WalkInput& in, size_t at);

}  // namespace tsql::editor::detail
