# MiniDB

A relational database written from scratch in C++17 — SQL parser, query planner,
execution engine and storage, with no third-party dependencies.

MiniDB is built in phases, and every phase ends with something that runs.
**Phases 1-5 are complete:** SQL text is lexed, parsed into an AST, analysed,
planned against available indexes, then executed over a paged B+tree on disk,
behind an LRU buffer pool, with a write-ahead log that survives `kill -9`.

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

Shell commands: `.help`, `.tables`, `.indexes`, `.schema [table]`, `.flush`, `.exit`.

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
| Durability | Write-ahead log with checksummed frames, crash recovery, checkpointing |
| Transactions | `BEGIN` / `COMMIT` / `ROLLBACK`; autocommit outside an explicit one |
| Indexes | `CREATE INDEX ... ON t (col)`, on any column type, duplicates allowed |
| Planning | Cost-based access-path choice, `EXPLAIN` |
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

| Operation | Phase 3 | Phase 4 (WAL) |
|---|---|---|
| Insert (storage layer) | 462,000 rows/s | **701,000 rows/s** |
| Full scan (storage layer) | 1,275,000 rows/s | **2,920,000 rows/s** |
| `SELECT COUNT/SUM/AVG` (through SQL) | 788,000 rows/s | **1,443,000 rows/s** |
| Random point lookup | 53,000 gets/s | **132,000 gets/s** |

Medians of five runs; the first run after a reboot is roughly 30% slower with
a cold file cache, so a single measurement is not worth quoting.

Phase 4 is faster than phase 3 for two reasons, and only one of them is the
log: evicting a dirty page now appends sequentially to the log instead of
seeking into the middle of the database file, and the read path no longer
seeks to EOF on every page read just to bounds-check it. The second was a
phase 3 inefficiency that the rewrite exposed.

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

## Query planning

```sql
CREATE INDEX idx_score ON events (score);
EXPLAIN SELECT * FROM events WHERE score = 40;
```
```
SEEK INDEX idx_score (score = 40)  rows=4 of 600  cost~25 (scan would be 600)
  FILTER (score = 40)
```

An index is not automatically better than a scan. Reaching a row through one
costs a random descent of the table tree, while a scan walks a leaf chain that
is already in the buffer pool -- so the planner weights index fetches four
times a scan step and picks whichever is cheaper. On the same 600-row table:

```
EXPLAIN SELECT * FROM events WHERE kind = 'click';
SCAN TABLE  rows=600 of 600  cost~600 (scan would be 600)
```

There *is* an index on `kind`, and the planner rejects it: only four distinct
kinds exist, so a quarter of the table matches and reaching all of it through
the index would cost more than reading the table straight through.

### The estimate is measured, not guessed

Selectivity constants are the usual way to make this decision, and they are
usually wrong: assuming 5% for equality would have predicted 30 rows where the
truth is 4. Instead the planner **probes the index** and counts, stopping at
the break-even point -- past `rows / 4` matches the index has already lost, so
there is no reason to keep counting. The count is therefore either exact or
large enough not to matter, and it costs at most a quarter of the scan it may
avoid. `EXPLAIN` prints `rows=` for a measured count and `rows>=` for a floor.

On 200,000 rows, a point query the planner can serve from an index:

| | Time |
|---|---|
| Without an index | 86.2 ms |
| With an index | 0.11 ms |

**757x**, which is simply the difference between reading 200,000 rows and
reading one.

The safety net is that an index seek is only ever an access path: the `WHERE`
clause is re-evaluated on every row it produces. An index can hand over extra
rows but never lose one, which is what makes the lossy numeric key encoding
safe. A differential test runs 18 query shapes against two identical databases
-- one indexed, one not -- and requires the answers to match exactly.

## Durability

Changed pages are appended to a write-ahead log rather than written in place.
A frame carrying a non-zero database size is a commit marker: everything up
to it is durable, everything after it is not. Recovery therefore needs no undo
information -- it replays forward to the last commit and discards the tail,
which is exactly what an interrupted write leaves behind.

```sql
BEGIN;
INSERT INTO ledger VALUES ('mistake', -999.0);
ROLLBACK;          -- the row was never there

BEGIN;
INSERT INTO ledger VALUES ('deposit', 50.0);
COMMIT;            -- fsynced; survives kill -9
```

Every frame is checksummed, and the log header carries a salt that changes on
every checkpoint, so a stale frame from a previous generation can never be
mistaken for a current one.

### Proving it

```bash
./build/minidb_crash torture --db crash.db --rounds 500
```

Each round spawns a writer process, has it insert a random number of rows
inside a transaction, randomly commits or not, then kills it with `_Exit` --
no destructors, no flush, no checkpoint. The parent then reopens the database
and checks two invariants: the row count matches exactly the committed rows,
and `SUM(id)` equals `n(n+1)/2`, so a lost, duplicated or half-written row
breaks one of them.

```
torture: 500 kill-and-recover rounds survived
         (233 committed, 267 killed mid-transaction), 13793 rows intact
```

A 20-round version runs in CI on every push.

## Architecture

```
SQL text
   |  Lexer            src/lexer.cpp        hand-written, tracks line/column
   v  tokens
   |  Parser           src/parser.cpp       recursive descent + Pratt expressions
   v  AST              include/minidb/ast.hpp
   |  Analysis         src/executor.cpp     name resolution, grouping legality
   v  validated AST
   |  Planner          src/plan.cpp         access-path choice, index probing
   v  Plan
   |  Executor         src/executor.cpp     grouping, aggregation, projection
   v  ResultSet

   Table              src/table.cpp        rows encoded into a B+tree
   Index              src/index.cpp        (value, rowid) keys in a B+tree
     -> BTree         src/btree.cpp        byte keys, slotted pages, leaf chain
        -> Pager      src/pager.cpp        LRU buffer pool, pinning, write-back
           -> Wal     src/wal.cpp          checksummed frames, commit markers
           -> file    4 KB pages, little-endian, header on page 0
```

Expressions use precedence climbing rather than one function per level, so
adding an operator is a single row in the binding-power table in
`src/parser.cpp`. The binding powers are documented there.

## Roadmap

- [x] **Phase 1** - lexer, Pratt parser, AST, in-memory tables, `SELECT`/`WHERE`/`ORDER BY`
- [x] **Phase 2** - aggregates, `GROUP BY`, `HAVING`, scalar functions, name resolution
- [x] **Phase 3** - paged B+tree storage, LRU buffer pool, on-disk catalogue
- [x] **Phase 4** - write-ahead log, crash recovery, transactions
- [x] **Phase 5** - secondary indexes, cost-based access-path planning, `EXPLAIN`
- [ ] **Phase 6** — TCP wire protocol, CLI client, Docker image

## Tests

213 test cases run under `ctest`, covering values and NULL ordering, lexing and
its error cases, parser precedence and associativity, aggregation and grouping
semantics, record encoding, buffer-pool eviction and pinning, B+tree splits at
every level, log recovery from torn and corrupted frames, transaction rollback,
key-encoding order preservation, index seeks and ranges, planner choices,
and persistence across reopening.

A second CTest target, `minidb_crash_recovery`, repeatedly spawns a writer
process, kills it mid-transaction with `_Exit`, and checks that the database
reopens holding exactly the committed rows and nothing else.

The suite uses a ~100-line framework in `tests/test_framework.hpp` so the
project builds with no network access and no dependencies. It deliberately uses
the `TEST` / `REQUIRE` spelling, so switching to GoogleTest later is a
`CMakeLists.txt` change rather than a rewrite.

Once phase 3 lands, `sqlite3` becomes the correctness oracle: run identical
queries against both engines and diff the output.

## License

MIT.
