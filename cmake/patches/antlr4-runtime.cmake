# Local fixes to the pinned ANTLR 4.13.2 C++ runtime: warnings (held to the same bar as libtsql),
# a missing include that breaks MSVC, and leaks and data races the sanitizers find.
# Run by FetchContent's PATCH_COMMAND in the extracted source root (cmake -P, no other tools):
#   cmake [-DTSQL_PATCH_HASH=<sha256 of this file>] -P antlr4-runtime.cmake
# (TSQL_PATCH_HASH is unused here: it only puts this file's content into the patch command, so an
# edit re-runs the patch step.)
# Each fix replaces one exact piece of upstream text, marked [libtsql]; a fix already present is
# skipped (re-configure, re-population), and text matching neither side stops the configure (the
# pin changed: re-check the fix against the new release). Drop a fix once upstream carries it;
# antlr/antlr4 dev still has all of them as of 2026-10-06 (7d57703).
cmake_minimum_required(VERSION 3.28)

function(tsql_patch file old new)
    set(path "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
    file(READ "${path}" text)
    string(FIND "${text}" "${new}" done)
    if(NOT done EQUAL -1)
        return()
    endif()
    string(FIND "${text}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "antlr4-runtime.cmake: ${file} has neither the upstream nor the patched text")
    endif()
    string(REPLACE "${old}" "${new}" text "${text}")
    file(WRITE "${path}" "${text}")
    message(STATUS "antlr4-runtime.cmake: patched ${file}")
endfunction()

# -Wdeprecated-declarations: defining the deprecated, unused Vocabulary::EMPTY_VOCABULARY warns in
# every build. Nothing in the runtime, the generated parsers or libtsql uses it (the default
# constructor replaces it), so it goes.
tsql_patch(runtime/Cpp/runtime/src/Vocabulary.h
    "    [[deprecated(\"Use the default constructor of Vocabulary instead.\")]] static const Vocabulary EMPTY_VOCABULARY;\n"
    "    // [libtsql] deprecated EMPTY_VOCABULARY removed (unused; its definition warned)\n")
tsql_patch(runtime/Cpp/runtime/src/Vocabulary.cpp
    "const Vocabulary Vocabulary::EMPTY_VOCABULARY;\n"
    "// [libtsql] deprecated EMPTY_VOCABULARY removed (unused; its definition warned)\n")

# -Wunused-parameter: the generated XPathLexer::IDAction never reads `context`.
tsql_patch(runtime/Cpp/runtime/src/tree/xpath/XPathLexer.cpp
    "void XPathLexer::IDAction(antlr4::RuleContext *context, size_t actionIndex) {\n"
    "void XPathLexer::IDAction(antlr4::RuleContext * /*context*/, size_t actionIndex) { // [libtsql] unused parameter\n")

# MSVC: ProfilingATNSimulator.cpp uses std::chrono::high_resolution_clock without including
# <chrono>; GCC's and Clang's standard headers happen to pull it in, MSVC's do not.
tsql_patch(runtime/Cpp/runtime/src/atn/ProfilingATNSimulator.cpp
    "#include \"support/CPPUtils.h\"\n\n#include \"atn/ProfilingATNSimulator.h\"\n"
    "#include \"support/CPPUtils.h\"\n\n#include <chrono> // [libtsql] used below; MSVC's headers do not include it transitively\n\n#include \"atn/ProfilingATNSimulator.h\"\n")

# MSVC C4244: the int literals in Utf8.cpp's std::pair<uint8_t, uint8_t> table go through pair's
# converting constructor, which narrows inside <utility>. The same values as uint8_t ({} is {0, 0}).
tsql_patch(runtime/Cpp/runtime/src/support/Utf8.cpp
    "      {LOW, HIGH}, {0xa0, HIGH}, {LOW, 0x9f}, {0x90, HIGH},\n      {LOW, 0x8f}, {0x0, 0x0},   {0x0, 0x0},  {0x0, 0x0},\n      {0x0, 0x0},  {0x0, 0x0},   {0x0, 0x0},  {0x0, 0x0},\n      {0x0, 0x0},  {0x0, 0x0},   {0x0, 0x0},  {0x0, 0x0},\n"
    "      // [libtsql] uint8_t values: int literals narrowed inside std::pair's constructor (MSVC C4244)\n      {LOW, HIGH}, {uint8_t{0xa0}, HIGH}, {LOW, uint8_t{0x9f}}, {uint8_t{0x90}, HIGH},\n      {LOW, uint8_t{0x8f}}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},\n")

# LeakSanitizer: splitAccordingToSemanticValidity holds its two new sets in raw pointers while it
# evaluates predicates; a predicate that throws (libtsql's editor stops a parse from inside one)
# leaks both. Owned until returned; the caller (unchanged) takes them.
tsql_patch(runtime/Cpp/runtime/src/atn/ParserATNSimulator.cpp
    "  // mem-check: both pointers must be freed by the caller.\n  ATNConfigSet *succeeded(new ATNConfigSet(configs->fullCtx));\n  ATNConfigSet *failed(new ATNConfigSet(configs->fullCtx));\n"
    "  // [libtsql] owned until returned (a predicate that throws leaked both); the caller frees them.\n  auto succeeded = std::make_unique<ATNConfigSet>(configs->fullCtx);\n  auto failed = std::make_unique<ATNConfigSet>(configs->fullCtx);\n")
tsql_patch(runtime/Cpp/runtime/src/atn/ParserATNSimulator.cpp
    "  return { succeeded, failed };\n"
    "  return { succeeded.release(), failed.release() }; // [libtsql] see above\n")

# ThreadSanitizer: computeReachSet adds the configurations at rule stop states of the DFA state it
# starts from (closure_, part of the DFA that every parser of the grammar shares) to the new reach
# set as they are, not copies. When the reach set becomes a DFA state, addDFAState's
# optimizeConfigs writes each of its configurations' context (and ATNConfigSet::add merges into
# them), while another thread predicting from the same DFA state reads the same objects: a data
# race on a shared_ptr. Copies, like every other configuration computeReachSet adds; their values
# are the same, so the predictions are.
tsql_patch(runtime/Cpp/runtime/src/atn/ParserATNSimulator.cpp
    "    for (const auto &c : skippedStopStates) {\n      reach->add(c, &mergeCache);\n"
    "    for (const auto &c : skippedStopStates) {\n      reach->add(std::make_shared<ATNConfig>(*c), &mergeCache); // [libtsql] a copy: c belongs to the shared DFA\n")
