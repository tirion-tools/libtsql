## libtsql

C++ T-SQL lexer + (in time) parser. MIT-licensed.

### Status

v0.1: lexer only. The token stream recognises bare / `[bracketed]` / `"quoted"` identifiers, 169 T-SQL reserved keywords (case-insensitive), string + numeric + Unicode literals, line and (nested) block comments, variables (`@`, `@@`), temp-name prefixes (`#`, `##`), and operators / punctuation. Byte offsets into the source are preserved so callers can stitch tokens back into rewritten output.

The parser, AST hierarchy and script generator are not yet implemented. They will follow as later releases.

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

### Build

```
cmake -B build
cmake --build build -j
ctest --test-dir build
```

### Where it's used

- `calliper` - the v1.2 session anonymizer: maps showplan identifiers through the trace + batch SQL bodies in committed test fixtures.

### License

MIT. See `LICENSE`.
