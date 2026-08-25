# MiniDB

A relational database written from scratch in C++17 — SQL parser, query planner,
execution engine and storage, with no third-party dependencies.

MiniDB is built in phases, and every phase ends with something that runs.
**Phase 1 (this one) is complete:** SQL text is lexed, parsed into an AST, and
executed against in-memory tables.

```sql
minidb> SELECT name, score * 2 AS doubled FROM players ORDER BY score DESC LIMIT 2;
+-------+---------+
| name  | doubled |
+-------+---------+
| ada   | 181     |
| alan  | 176.5   |
+-------+---------+
2 row(s) in 0.031 ms
```

## Build and run

Requires CMake 3.16+ and any C++17 compiler (GCC, Clang or MSVC).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Then start the shell, or run a script:

```bash
./build/minidb
./build/minidb examples/demo.sql
```

Shell commands: `.help`, `.tables`, `.schema [table]`, `.exit`.

## What phase 1 supports

| Feature | Notes |
|---|---|
| `CREATE TABLE` | `INTEGER`, `REAL`, `TEXT` columns; `IF NOT EXISTS` |
| `INSERT` | Multi-row `VALUES`, optional column list, `NULL` for omitted columns |
| `SELECT` | `*`, computed expressions, `AS` aliases |
| `WHERE` | Full expression grammar with SQL three-valued logic |
| `ORDER BY` | Multiple keys, `ASC`/`DESC`, NULLs first |
| `LIMIT` | Integer literal |
| Operators | `+ - * / %`, `\|\|`, `= <> < <= > >=`, `AND OR NOT`, `IS [NOT]` |
| Diagnostics | Every error carries a line, column and caret |

Identifiers and keywords are case-insensitive. Strings use `''` to escape a
quote; double quotes make a quoted identifier.

## Errors point at the problem

```
minidb> SELECT * FROM players WHERE score > ;
error: expected an expression, found ';'
  --> line 1, column 43
   | SELECT * FROM players WHERE score > ;
   |                                     ^
```

## NULL is handled properly

The most common shortcut in a toy SQL engine is treating `NULL` as a sentinel
value. MiniDB implements the real three-valued logic instead:

```sql
SELECT * FROM players WHERE score = NULL;      -- 0 rows: NULL = NULL is unknown
SELECT * FROM players WHERE score IS NULL;     -- the null-safe test
SELECT * FROM players WHERE score > 0 AND 1=0; -- FALSE AND unknown is FALSE
```

## Architecture

```
SQL text
   |  Lexer            src/lexer.cpp        hand-written, tracks line/column
   v  tokens
   |  Parser           src/parser.cpp       recursive descent + Pratt expressions
   v  AST              include/minidb/ast.hpp
   |  Executor         src/executor.cpp     tree-walking, three-valued logic
   v  ResultSet
        Storage        src/table.cpp        vector of rows (B+tree in phase 3)
```

Expressions use precedence climbing rather than one function per level, so
adding an operator is a single row in the binding-power table in
`src/parser.cpp`. The binding powers are documented there.

## Roadmap

- [x] **Phase 1** — lexer, Pratt parser, AST, in-memory tables, `SELECT`/`WHERE`/`ORDER BY`
- [ ] **Phase 2** — operator tree execution, aggregates, `GROUP BY`
- [ ] **Phase 3** — paged B+tree storage with a buffer pool, on-disk files
- [ ] **Phase 4** — write-ahead log and crash recovery, verified by kill-and-restart tests
- [ ] **Phase 5** — cost-based planner with index selection, `EXPLAIN`
- [ ] **Phase 6** — TCP wire protocol, CLI client, Docker image

## Tests

75 test cases run under `ctest`, covering values and NULL ordering, lexing and
its error cases, parser precedence and associativity, and end-to-end execution.

The suite uses a ~100-line framework in `tests/test_framework.hpp` so the
project builds with no network access and no dependencies. It deliberately uses
the `TEST` / `REQUIRE` spelling, so switching to GoogleTest later is a
`CMakeLists.txt` change rather than a rewrite.

Once phase 3 lands, `sqlite3` becomes the correctness oracle: run identical
queries against both engines and diff the output.

## License

MIT.
