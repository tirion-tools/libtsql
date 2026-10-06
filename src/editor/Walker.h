// Editor support, internal: the tokens that can stand at the caret, found by walking the
// grammar's ATN from the configuration the parser was in (Capture) over the tokens up to the
// caret, through every alternative (no prediction), like antlr4-c3's CodeCompletionCore, except
// where ANTLR 2's tests (the runtime's emulation of SqlScriptDOM's decisions) take an earlier
// alternative on those tokens.
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
    int startPending = -1;          // Capture::pendingCall: a rule call's keyword check for the next token
    size_t caret = 0;               // parser-token index of the caret (tokens->size() when it is the end)
    size_t budget = 400'000;        // configurations visited at most
    /// Runs an opaque predicate of the grammar with the real parser on the tokens before the caret
    /// (CaretSession::EvaluatePredicate: rule, predicate, token index, whether the walk still stands
    /// where the parser stood (its rule locals apply), the token at the caret or null -> kTrue, ...).
    std::function<int(size_t, size_t, size_t, bool, const ProbeToken*)> evaluate;
    /// The parser's simulator: ANTLR 2's tests at the decisions the parser predicts (null: the
    /// walk follows every alternative).
    parser::TSqlParserATNSimulator* simulator = nullptr;
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
    /// The statement the walk started in ended before the token (it follows that statement in its
    /// batch, block, IF, WHILE or module body).
    bool nextStatement = false;
    /// The path took a token after that statement ended (ELSE, an enclosing block's END, ...): a
    /// trial of that statement's text alone cannot settle a doubt about the token.
    bool beyondStatement = false;
    /// The parser may not take this token here for a reason the walk cannot decide; a trial parse of
    /// the text with the token settles it. `doubtPath`: the path to it ran an action that can reject
    /// the input (or a predicate on rule locals the walk does not have), the last such (whose
    /// outcome is the same for every token behind it) as a key, 0: none. `doubtToken`: such an
    /// action runs right after the token, before the parser looks further (its outcome may depend
    /// on the token).
    uint64_t doubtPath = 0;
    bool doubtToken = false;
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
