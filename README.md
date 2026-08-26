# MiniDB

A relational database written from scratch in C++17 — SQL parser, query planner,
execution engine and storage, with no third-party dependencies.

MiniDB is built in phases, and every phase ends with something that runs.
**Phases 1-3 are complete:** SQL text is lexed, parsed into an AST, analysed,
then executed against tables stored in a paged B+tree on disk, behind an
LRU buffer pool.

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
./build/minidb                                  # in-memory
./build/minidb --db shop.db                     # persistent
./build/minidb --db shop.db examples/demo.sql   # run a script
```

Shell commands: `.help`, `.tables`, `.schema [table]`, `.exit`.

## What works today

| Feature | Notes |
|---|---|
| `CREATE TABLE` | `INTEGER`, `REAL`, `TEXT` columns; `IF NOT EXISTS` |
| `INSERT` | Multi-row `VALUES`, optional column list, `NULL` for omitted columns |
| `SELECT` | `*`, computed expressions, `AS` aliases |
| `WHERE` | Full expression grammar with SQL three-valued logic |
| `ORDER BY` | Multiple keys, `ASC`/`DESC`, NULLs first, select-list aliases and positions |
| `LIMIT` | Integer literal |
| Operators | `+ - * / %`, `\|\|`, `= <> < <= > >=`, `AND OR NOT`, `IS [NOT]` |
| `GROUP BY` | Multiple keys, computed keys, NULL keys grouped together |
| `HAVING` | Filters groups after aggregation; allowed without `GROUP BY` |
| Aggregates | `COUNT(*)`, `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`, composable in expressions |
| Functions | `ABS`, `LENGTH`, `LOWER`, `UPPER`, `ROUND`, `COALESCE`, `IFNULL` |
| Storage | Paged B+tree per table, LRU buffer pool, catalogue persisted in the file |
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

## Aggregation

```sql
SELECT region, COUNT(*) AS deals, SUM(amount) AS total
FROM sales GROUP BY region HAVING SUM(amount) > 100 ORDER BY total DESC;
```

Aggregates are computed once per group and then read back as ordinary leaves,
so `SUM(amount) / COUNT(*)` is just an expression tree. Bare columns outside an
aggregate must appear in `GROUP BY`, and the error says which one does not.

## NULL is handled properly

The most common shortcut in a toy SQL engine is treating `NULL` as a sentinel
value. MiniDB implements the real three-valued logic instead:

```sql
SELECT * FROM players WHERE score = NULL;      -- 0 rows: NULL = NULL is unknown
SELECT * FROM players WHERE score IS NULL;     -- the null-safe test
SELECT * FROM players WHERE score > 0 AND 1=0; -- FALSE AND unknown is FALSE
```

## Storage

Each table is a B+tree keyed by an implicit rowid, stored in slotted 4 KB
pages behind an LRU buffer pool. The schema lives in its own B+tree whose
root is recorded in the file header, so opening a database is: read the
header, walk the catalogue, done.

```bash
./build/minidb_bench --rows 200000 --db bench.db
```

200,000 rows of `(INTEGER, TEXT, REAL)`, 256-page (1 MB) pool, MSVC release:

| Operation | Throughput |
|---|---|
| Insert (storage layer) | 462,000 rows/s |
| Full scan (storage layer) | 1,275,000 rows/s |
| `SELECT COUNT/SUM/AVG` (through SQL) | 788,000 rows/s |
| `SELECT ... WHERE` (through SQL) | 992,000 rows/s |
| Random point lookup | 53,000 gets/s |

Buffer-pool hit rate 95.9% on that run, with 2,263 pages for 8.8 MB of file.

Point lookups are the slow column on purpose: with a 1 MB pool over an 8.8 MB
file, most random probes miss the cache and pay for a read. Sequential work is
an order of magnitude faster because it is what the leaf chain is built for.

### Appends pack, rather than splitting down the middle

Rowids arrive in ascending order, so every leaf split is an append. Splitting
those 50/50 leaves each leaf permanently half full. Detecting the append and
giving the new row a page of its own instead:

| | Pages | File | Disk reads | Hit rate |
|---|---|---|---|---|
| Balanced split | 4,522 | 17.7 MB | 38,425 | 94.3% |
| Append-aware split | 2,263 | 8.8 MB | 27,003 | 95.9% |

Same rows, half the file. Random-order inserts still split down the middle;
`InsertsInTheMiddleStillSplitEvenly` guards that.

## Architecture

```
SQL text
   |  Lexer            src/lexer.cpp        hand-written, tracks line/column
   v  tokens
   |  Parser           src/parser.cpp       recursive descent + Pratt expressions
   v  AST              include/minidb/ast.hpp
   |  Analysis         src/executor.cpp     name resolution, grouping legality
   v  validated AST
   |  Executor         src/executor.cpp     grouping, aggregation, projection
   v  ResultSet

   Table              src/table.cpp        rows encoded into a B+tree
     -> BTree         src/btree.cpp        slotted pages, splits, leaf chain
        -> Pager      src/pager.cpp        LRU buffer pool, pinning, write-back
           -> file    4 KB pages, little-endian, header on page 0
```

Expressions use precedence climbing rather than one function per level, so
adding an operator is a single row in the binding-power table in
`src/parser.cpp`. The binding powers are documented there.

## Roadmap

- [x] **Phase 1** - lexer, Pratt parser, AST, in-memory tables, `SELECT`/`WHERE`/`ORDER BY`
- [x] **Phase 2** - aggregates, `GROUP BY`, `HAVING`, scalar functions, name resolution
- [x] **Phase 3** - paged B+tree storage, LRU buffer pool, on-disk catalogue
- [ ] **Phase 4** — write-ahead log and crash recovery, verified by kill-and-restart tests
- [ ] **Phase 5** — cost-based planner with index selection, `EXPLAIN`
- [ ] **Phase 6** — TCP wire protocol, CLI client, Docker image

## Tests

165 test cases run under `ctest`, covering values and NULL ordering, lexing and
its error cases, parser precedence and associativity, aggregation and grouping
semantics, record encoding, buffer-pool eviction and pinning, B+tree splits at
every level, and persistence across reopening.

The suite uses a ~100-line framework in `tests/test_framework.hpp` so the
project builds with no network access and no dependencies. It deliberately uses
the `TEST` / `REQUIRE` spelling, so switching to GoogleTest later is a
`CMakeLists.txt` change rather than a rewrite.

Once phase 3 lands, `sqlite3` becomes the correctness oracle: run identical
queries against both engines and diff the output.

## License

MIT.
