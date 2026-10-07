## libtsql

C++ T-SQL lexer, parser and editor support. MIT-licensed.

### Status

v0.2. Lexer: the token stream recognises bare / `[bracketed]` / `"quoted"` identifiers, 169 T-SQL reserved keywords (case-insensitive), string + numeric + Unicode literals, line and (nested) block comments, variables (`@`, `@@`), temp-name prefixes (`#`, `##`), and operators / punctuation. Byte offsets into the source are preserved so callers can stitch tokens back into rewritten output.

Parser (`-DTSQL_BUILD_PARSER=ON`): SqlScriptDOM's TSql130–TSql180 and TSqlFabricDW grammars, converted to ANTLR 4 at build time, produce SqlScriptDOM's AST and parse errors (`tsql::parse`); an editor layer on top of them completes and colours T-SQL (`tsql::editor`). The script generator is not implemented.

### Public API

```cpp
#include <tsql/tsql.hpp>

auto tokens = tsql::tokenize("SELECT [Customer].name FROM Customer");
// tokens[i] = { kind, start, length } into the input view

bool kw = tsql::is_keyword("select");  // true (case-insensitive)

std::unordered_map<std::string, std::string> m = {
    {"customer", "Tbl_1"},
    {"name",     "Col_1"},
};
auto out = tsql::anonymize_identifiers(
    "SELECT [Customer].name FROM Customer", m);
// out == "SELECT [Tbl_1].Col_1 FROM Tbl_1"
```

### Parser

```cpp
#include <tsql/parser.hpp>

tsql::ParseResult r = tsql::parse(sql, tsql::SqlVersion::Sql170);   // UTF-8 input
// r.script: the TSqlScript AST (null when an error escaped every recovering rule)
// r.errors: SqlScriptDOM's ParseError list (Number, UTF-16 Offset, Line, Column, Message)
// r.tokens: every token incl. whitespace and comments; r.factory owns the nodes
```

`tsql::parse` throws `std::invalid_argument` for a version whose grammar is not in the build (`tsql::IsParserAvailable`).

### Editor support

`#include <tsql/editor.hpp>`, CMake target `tsql::editor`. Offsets are UTF-8 byte offsets.

- `Complete(sql, caret, version, catalog)`: completion items at the caret (keywords, hints, catalog objects, columns, aliases, CTEs, variables, parameters, types, built-ins), ranked, with the range of the partial word they replace. Only what the version's parser accepts at the caret is offered.
- `Classify(sql, version)`: colour spans covering the whole text; identifiers the parser takes as keywords, data types or built-in functions get that class.
- `Document`: an editor buffer (`SetText`, `Edit`, `Complete`, `Classify(start, end)`, `ParseAhead(budget)`). It keeps tokens and the parse between calls, so after an edit only the changed text is lexed and parsed again; the text is lexed and each batch (the text between `GO`s) parsed only when a query needs it. `ParseAhead` parses (and lexes) what no query has needed yet for about `budget`, stopping at a statement boundary, and returns true once everything is parsed: call it in idle time after opening a file or a large paste. Its results equal the free functions' on its text, whether or when `ParseAhead` ran.
- `Warm(version)`: builds the grammar's tables and primes the parser's prediction caches. Run it once per version on a background thread at start-up; without it the first query of a version takes 100–300 ms longer.

Editors should keep one `Document` per open file: a free `Complete` parses its whole batch from the start up to the caret every time, which in a large script without `GO` costs tens of milliseconds.

Threading: a `Document` is not thread-safe; distinct `Document`s (of any versions) and the free functions may run on different threads at once, and `Warm` may run while they are in use.

Catalog conventions (`Catalog`, `CatalogObject`, `CatalogType`):

- An object or type with an empty `database` is a system object (fill these from `sys.all_objects` / `sys.all_columns` / `sys.all_parameters`); it exists in every database, unqualified, as `sys.` / `INFORMATION_SCHEMA.` and as `db.sys.`. Unqualified `sp_` / `xp_` names also resolve to `sys`.
- `parameters` are in declaration order and include the `@`; EXEC completion offers them as `@name = `.
- A synonym's `target` is a multi-part name resolved relative to the synonym's database; a target the catalog lacks (for example on a linked server) is still offered where a table source or procedure fits, without columns or parameters.
- `CatalogType` is an alias type, or a table type (`isTableType` with `columns`); table types are offered for `DECLARE @v` and parameters only, and table variables and READONLY parameters of that type get its columns.
- `defaultSchema` (default `dbo`) and `currentDatabase` resolve unqualified names; `USE` in the script changes the database.

Measured performance (Release build, 200 KB scripts, one with a `GO` every few statements and one single batch, a catalog of 2,000 user and 2,435 system objects, after `Warm`):

| operation | time |
|---|---|
| edit + `Document::Complete` at the caret | median 0.7–1.3 ms (the higher where trial parses check keywords or that the statement can end), slowest 2.7 ms |
| edit + `Document::Complete` the first time at a place whose syntax error or keyword trials the parser has not met before (it extends its prediction tables) | slowest 7.4–7.6 ms (single batch), 8.5–10.4 ms (script with `GO`s: there the edit makes `CREATE TABLE #work (Id int, A mount money)`, and ANTLR's SLL prediction of the seven optional clauses after the column type, each meeting `money ) ;` for the first time, adds two DFA states per clause of 300–1,000 configurations each, about 37 M instructions; the same edit again, with those states built, about 8 M), over 20 edits spread through the script, under a load average of 1.5–3; the same edits again: slowest 4.9–6.2 ms (single batch), 3.0–3.7 ms (`GO`s) |
| edit + `Document::Classify` of a 100-line viewport | median 0.15–0.30 ms, slowest 0.77 ms |
| edit + `Document::Classify` of the whole text | 4.5–4.7 ms (the first time up to 110 ms, lexing and parsing the rest of the file) |
| `Document::SetText` | 0.2 ms: the text is lexed on demand (all 200 KB: 22–24 ms, paid by `ParseAhead` in slices or by the first query that needs it) |
| first-screen `Classify(0, 2000)` on a new `Document` | 1.8–2.2 ms |
| first `Complete` on a new `Document`, script with `GO`s | 14–15 ms (caret mid-file), 25–32 ms (end): `Complete` lexes the text up to the end of the caret's batch, from its start (the lexer has no other place to start from) |
| first `Complete` on a new `Document`, single batch | 74–83 ms (caret mid-file), 102–148 ms (end): the text is lexed and the batch parsed from its start (see below) |
| `ParseAhead(4 ms)` until it returns true | single batch: 22–24 calls, 97–109 ms in all, slowest call 5.0–5.5 ms; script with `GO`s: 25–28 calls, 109–116 ms in all, slowest call 4.6–5.9 ms |
| first `Complete` at the end after `ParseAhead` finished | 0.6–0.7 ms (both scripts) |
| one `ParseAhead` step (`ParseAhead(0)`: one statement), SqlScriptDOM's 1,088 test scripts, 9,333 steps | first pass after `Warm`: p99 4.2 ms, p99.9 21 ms, slowest 54 ms (a statement shape the prediction caches have not met, which any query parsing it first pays too); second pass: p99 0.38 ms, p99.9 0.78 ms, slowest 11 ms (lexing a 3.8 KB script of `/*---*/` comment banners, which the lexer reads slowly) |
| `Warm` | 180–320 ms per version (6–8 ms when repeated) |

Why a single batch is parsed from its start: the parser's state at a statement inside a batch is not fully known from the tokens. `SET QUOTED_IDENTIFIER` changes how `"x"` is read for the rest of the batch, and after a syntax error the parser's recovery decides where the next statement starts. A token scanner finds the same statement starts as the parser in all 4,022 statements of the SqlScriptDOM test scripts, but its guess is not exact in general, and a wrong start gives wrong completions. So the first query in a batch parses that batch up to the caret once; later edits reuse that parse.

A `Document` can be opened on the UI thread: `SetText` costs next to nothing, and a first-screen `Classify` lexes and parses only the start of the text. Then call `ParseAhead` with a small budget (say 4 ms) whenever the editor is idle until it returns true, and again after edits until it does (an edit that changes how the text parses gives it work again); a single call overruns its budget by at most the parse of one statement (and the lexing of up to 4 KB it reads into). Alternatively build the `Document` on a worker thread (`SetText`, then `ParseAhead` with an unbounded budget: about 100–115 ms for 200 KB) and move it to the UI thread; a `Document` may change threads between calls.

Completion quality on the SqlScriptDOM test scripts (`tsql_editor_probe --overoffer 10`, 5,816 carets): none of the 118,769 offered keywords fails a trial parse of the batch text before the caret followed by the keyword. 249 end in an error after a predicate looked at the end of the text, so the text alone cannot decide them (mostly `CREATE OR`, FILESTREAM after a column type, DEFAULT as a built-in function's argument, a security object kind's second word: valid with some continuations or rejected by an action that runs only once more text follows). The walk applies the parser's ANTLR 2 decision rules (the runtime's tables, `parser::Antlr2Tests`) at every decision the generated code predicts with adaptivePredict (`kPredictedDecisions`, from editor_meta.py): an alternative that ANTLR 2 tries earlier and whose two-token look-ahead and leading predicates hold on the tokens before the caret takes the decision, so the walk drops the later ones (`close symmetric |` offers KEY, not COPY; no BETWEEN after `REGEXP_LIKE (...)`); where its second token is the caret, the caret tokens in its LA(2) set are excluded from the later alternatives (the dangling ELSE goes to the inner IF). Syntactic predicates are not run by the walk, so an earlier alternative with one never excludes. A decision's tables are only looked at when the path that took one of its later alternatives consumes the decision's first token and the ATN's LL(1) sets say an earlier alternative may start with it. Identifier words that are reserved keywords (OFF after `desired_state =`) are not offered, since they never lex as an Identifier. Keywords that the walk cannot decide on its own are settled by trial parses of the statement (from the statement where the parse resumed when the keyword's path left the caret's inner statement). This covers keywords reached through an action that can reject the input, keywords right after such an action before the parser looks at the next token, and keywords that depend on a predicate on rule locals. A trial parse runs until the parser itself stands at the end of the text. A look-ahead decision that reads to the end decides as it would for a text that ends there, and the parse goes on; the decision's reads include ANTLR 2's tests after ANTLR 4's choice (the second token; a syntactic predicate's speculative parse, which counts as a predicate). An error after such a decision counts against the keyword only if a full-context prediction shows that the decision did not depend on the end. That check runs only for a trial that fails. A trial tests one keyword per such action, or each keyword when there are at most 16, and all trials together parse at most 1,024 tokens. Opaque predicates that read the token at the caret are evaluated for each candidate keyword. The parse for the walk stops at the first decision whose reads, those tests included, reach the caret, and the walk starts there.

### Build

```
cmake -B build
cmake --build build -j
ctest --test-dir build
```

Options:

- `TSQL_BUILD_PARSER` (default `OFF`): build the AST (`tsql::ast`), the parsers (`tsql::parser`), the editor support (`tsql::editor`), their tests and the `tsql_dump`, `tsql_bench` and `tsql_editor_probe` tools. Needs CMake 3.28+, Python 3 and a Java runtime; CMake fetches pinned SqlScriptDOM sources and the ANTLR 4.13.2 tool and C++ runtime.
- `TSQL_PARSER_GRAMMARS` (default all: `TSql130;TSql140;TSql150;TSql160;TSql170;TSql180;TSqlFabricDW`): the grammars to build. Each takes about a minute to compile.
- `TSQL_PARSER_PARTS` (default 5): translation units each generated parser's rules are split into (each needs up to ~1.4 GB to compile).
- `TSQL_PARSER_COMPILE_JOBS` (default empty: half the memory free at configure time / 1.6 GB, at least 1): with Ninja, how many parser translation units compile at once, whatever `-j` says.
- `TSQL_BUILD_TESTS` (default `ON`), `TSQL_INSTALL` (default `ON`: install rules and the CMake package).

### Use from CMake

As a subdirectory: `add_subdirectory(libtsql)`, then link `tsql::tsql` (and, built with `TSQL_BUILD_PARSER=ON`, `tsql::ast`, `tsql::parser` or `tsql::editor`).

Installed (`cmake --install build --prefix <prefix>`, then `<prefix>` in `CMAKE_PREFIX_PATH`):

```cmake
find_package(tsql 0.2 CONFIG REQUIRED)                     # tsql::tsql
find_package(tsql 0.2 CONFIG REQUIRED COMPONENTS editor)   # + tsql::ast, tsql::parser, tsql::editor
```

The components `ast`, `parser` and `editor` exist when libtsql was built with `TSQL_BUILD_PARSER=ON`. That package holds the generated AST headers and the ANTLR 4 C++ runtime the parsers link (`tsql_antlr4_runtime`; its BSD-3-Clause license is installed as `share/doc/tsql/LICENSE.antlr4.txt`), so a consumer needs neither Python, Java nor an installed ANTLR. The libraries are static: with MSVC, build the consumer with the same configuration and runtime library (`CMAKE_MSVC_RUNTIME_LIBRARY`). `tests/install` is such a consumer (run by CI).

### Where it's used

- `calliper` - the v1.2 session anonymizer: maps showplan identifiers through the trace + batch SQL bodies in committed test fixtures.

### License

MIT. See `LICENSE`.
