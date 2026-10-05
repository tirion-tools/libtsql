## libtsql

C++ T-SQL lexer, parser and editor support. MIT-licensed.

### Status

v0.1: lexer. The token stream recognises bare / `[bracketed]` / `"quoted"` identifiers, 169 T-SQL reserved keywords (case-insensitive), string + numeric + Unicode literals, line and (nested) block comments, variables (`@`, `@@`), temp-name prefixes (`#`, `##`), and operators / punctuation. Byte offsets into the source are preserved so callers can stitch tokens back into rewritten output.

Parser pilot (`-DTSQL_BUILD_PARSER_PILOT=ON`): SqlScriptDOM's TSql130–TSql180 and TSqlFabricDW grammars, converted to ANTLR 4 at build time, produce SqlScriptDOM's AST and parse errors (`tsql::parse`); an editor layer on top of them completes and colours T-SQL (`tsql::editor`). The script generator is not implemented.

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

### Parser (pilot)

```cpp
#include <tsql/parser.hpp>

tsql::ParseResult r = tsql::parse(sql, tsql::SqlVersion::Sql170);   // UTF-8 input
// r.script: the TSqlScript AST (null when an error escaped every recovering rule)
// r.errors: SqlScriptDOM's ParseError list (Number, UTF-16 Offset, Line, Column, Message)
// r.tokens: every token incl. whitespace and comments; r.factory owns the nodes
```

`tsql::parse` throws `std::invalid_argument` for a version whose grammar is not in the build (`tsql::IsParserAvailable`).

### Editor support (pilot)

`#include <tsql/editor.hpp>`, CMake target `tsql::editor`. Offsets are UTF-8 byte offsets.

- `Complete(sql, caret, version, catalog)`: completion items at the caret (keywords, hints, catalog objects, columns, aliases, CTEs, variables, parameters, types, built-ins), ranked, with the range of the partial word they replace. Only what the version's parser accepts at the caret is offered.
- `Classify(sql, version)`: colour spans covering the whole text; identifiers the parser takes as keywords, data types or built-in functions get that class.
- `Document`: an editor buffer (`SetText`, `Edit`, `Complete`, `Classify(start, end)`). It keeps tokens and the parse between calls, so after an edit only the changed text is lexed and parsed again, and each batch (the text between `GO`s) is parsed only when a query needs it. Its results equal the free functions' on its text.
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
| edit + `Document::Complete` at the caret | median 0.6–0.9 ms (1.5 ms after a complete expression, where a trial parse checks that the statement can end), slowest 3.3 ms |
| edit + `Document::Complete` where the edit makes a syntax error of a shape the parser has not met before (it extends its prediction tables) | slowest 7.2 ms (single batch), 9.1 ms (script with `GO`s), over 20 edits spread through the script |
| edit + `Document::Classify` of a 100-line viewport | median 0.15–0.29 ms, slowest 0.33 ms |
| edit + `Document::Classify` of the whole text | 4.3–4.6 ms (the first time up to 100 ms, parsing the rest of the file) |
| `Document::SetText` (lexing 200 KB) | 24 ms |
| first `Complete` on a new `Document`, script with `GO`s | 1.3 ms (caret mid-file), 1.6 ms (end) |
| first `Complete` on a new `Document`, single batch | 57 ms (caret mid-file), 115 ms (end): its batch is parsed from its start (see below) |
| `Warm` | 180–320 ms per version (6–8 ms when repeated) |

Why a single batch is parsed from its start: the parser's state at a statement inside a batch is not fully known from the tokens. `SET QUOTED_IDENTIFIER` changes how `"x"` is read for the rest of the batch, and after a syntax error the parser's recovery decides where the next statement starts. A token scanner finds the same statement starts as the parser in all 4,022 statements of the SqlScriptDOM test scripts, but its guess is not exact in general, and a wrong start gives wrong completions. So the first query in a batch parses that batch up to the caret once; later edits reuse that parse.

To open a large file without blocking the UI thread, build its `Document` on a worker thread (`SetText`, then `Classify(0, Text().size())` to parse it all) and move it to the UI thread; a `Document` may change threads between calls. Opening a 200 KB script this way takes about 150–180 ms on the worker.

Completion quality on the SqlScriptDOM test scripts (`tsql_editor_probe --overoffer 10`, 5,809 carets): 0.06% of the offered keywords (77 of 119,727) make the text before the caret fail to parse.

### Build

```
cmake -B build
cmake --build build -j
ctest --test-dir build
```

Options:

- `TSQL_BUILD_PARSER_PILOT` (default `OFF`): build the AST, the parsers (`tsql::parser`), the editor support (`tsql::editor`), their tests and the `tsql_dump`, `tsql_bench` and `tsql_editor_probe` tools. Needs Python 3 and a Java runtime; CMake fetches pinned SqlScriptDOM sources and the ANTLR 4.13.2 tool and C++ runtime.
- `TSQL_PARSER_GRAMMARS` (default all: `TSql130;TSql140;TSql150;TSql160;TSql170;TSql180;TSqlFabricDW`): the grammars to build. Each takes about a minute to compile.
- `TSQL_PARSER_PARTS` (default 5): translation units each generated parser's rules are split into (each needs up to ~1.4 GB to compile).
- `TSQL_PARSER_COMPILE_JOBS` (default empty: half the memory free at configure time / 1.6 GB, at least 1): with Ninja, how many parser translation units compile at once, whatever `-j` says.
- `TSQL_BUILD_TESTS` (default `ON`), `TSQL_INSTALL` (default `ON`).

### Where it's used

- `calliper` - the v1.2 session anonymizer: maps showplan identifiers through the trace + batch SQL bodies in committed test fixtures.

### License

MIT. See `LICENSE`.
