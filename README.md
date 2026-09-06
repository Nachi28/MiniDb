# MiniDB

A relational database engine written from scratch in C++17. SQL parser,
cost-based query planner, B+tree storage on disk, and a write-ahead log that
survives `kill -9`. No third-party dependencies.

[![ci](https://github.com/Nachi28/MiniDb/actions/workflows/ci.yml/badge.svg)](https://github.com/Nachi28/MiniDb/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/Nachi28/MiniDb?sort=semver)](https://github.com/Nachi28/MiniDb/releases)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)

```sql
minidb> SELECT region, SUM(amount) AS total FROM sales
   ...> GROUP BY region HAVING SUM(amount) > 100 ORDER BY total DESC;
+--------+-------+
| region | total |
+--------+-------+
| west   | 200   |
| east   | 150.5 |
+--------+-------+
2 row(s) in 0.031 ms
```

## Install

Download a prebuilt binary from [Releases](https://github.com/Nachi28/MiniDb/releases),
or build it — CMake 3.16+ and any C++17 compiler, nothing else:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Use

```bash
minidb                                  # in-memory
minidb --db shop.db                     # persistent
minidb --db shop.db script.sql          # run a script

minidb --db shop.db --serve 5433        # serve over TCP
minidb --connect 127.0.0.1:5433         # connect to it

docker build -t minidb . && docker run -p 5433:5433 minidb
```

Start with [`examples/`](examples/) — four short scripts covering everything the
engine supports.

## The interesting part: the planner refuses indexes

An index is not automatically better than a scan. Reaching a row through one
costs a random descent of the table tree; a scan walks a leaf chain already in
the buffer pool. MiniDB weights an index fetch at four scan steps and picks
whichever is cheaper:

```
EXPLAIN SELECT * FROM events WHERE score = 42;
  SEEK INDEX idx_score (score = 42)  rows=1 of 120  cost~11 (scan would be 120)

EXPLAIN SELECT * FROM events WHERE kind = 'click';
  SCAN TABLE  rows=120 of 120  cost~120 (scan would be 120)
```

There *is* an index on `kind` in that second query. The planner rejects it —
only four distinct kinds exist, so a quarter of the table matches and reaching
all of it through the index costs more than reading the table straight through.

And the row counts are **measured, not assumed**. Textbook selectivity constants
predicted 30 matching rows on this data where the truth was 4, so the planner
probes the index and counts instead — stopping at the break-even point, past
which the index has already lost.

## What it does

| | |
|---|---|
| **SQL** | `CREATE TABLE`, `CREATE INDEX`, `INSERT`, `SELECT`, `EXPLAIN` |
| **Clauses** | `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT`, aliases and positions |
| **Aggregates** | `COUNT(*)`, `COUNT`, `SUM`, `AVG`, `MIN`, `MAX` |
| **Functions** | `ABS`, `LENGTH`, `LOWER`, `UPPER`, `ROUND`, `COALESCE`, `IFNULL` |
| **Transactions** | `BEGIN` / `COMMIT` / `ROLLBACK`, autocommit outside one |
| **Storage** | Paged B+tree per table, LRU buffer pool, schema persisted in the file |
| **Durability** | Write-ahead log, checksummed frames, crash recovery, `fsync` on commit |
| **Diagnostics** | Every error carries a line, column and caret |

NULL follows real SQL three-valued logic — `WHERE x = NULL` matches nothing,
`GROUP BY` puts NULLs in one group, and `FALSE AND NULL` is `FALSE`.

## How it's verified

Three checks that catch different things:

```bash
ctest --test-dir build                             # 213 unit tests
./build/minidb_crash torture --db c.db --rounds 500 # kill -9, 500 times
python tools/difftest.py --queries 700 --rows 800   # differential vs SQLite
```

```
torture: 500 kill-and-recover rounds survived
         (233 committed, 267 killed mid-transaction), 13793 rows intact

700 queries, 800 table rows, 46925 result rows compared, 0 mismatches
```

The crash harness kills a writer process mid-transaction and checks the database
always reopens with exactly the committed rows. The differential test generates
random queries and runs them against both MiniDB and SQLite — indexes exist only
on the MiniDB side, so the planner's access paths are checked against an engine
that has never heard of them.

## Performance

`minidb_bench --rows 200000`, 1 MB buffer pool, medians of five runs:

| Operation | |
|---|---|
| Insert (storage layer) | 701,000 rows/s |
| Full scan | 2,920,000 rows/s |
| `SELECT` aggregate via SQL | 1,443,000 rows/s |
| Point lookup | 132,000 gets/s |
| Selective query, index vs scan | 0.11 ms vs 86.2 ms (**757×**) |

Buffer-pool hit rate 95.9%. Detecting that rowid inserts always append, and
giving those splits their own page instead of halving, took the same 200,000
rows from 17.7 MB to 8.8 MB.

## Architecture

```
SQL text
   |  Lexer            hand-written, tracks line/column
   v  tokens
   |  Parser           recursive descent + Pratt expressions
   v  AST
   |  Analysis         name resolution, grouping legality
   v  validated AST
   |  Planner          access-path choice, index probing
   v  Plan
   |  Executor         grouping, aggregation, projection
   v  ResultSet

Table / Index  ->  BTree  ->  Pager  ->  Wal  ->  one file
                   byte keys   LRU pool   commit markers
                   slotted     pinning    checksums
                   pages       write-back salt
```

## Documentation

| | |
|---|---|
| [`docs/HANDBOOK.md`](docs/HANDBOOK.md) | Complete reference: every file, byte layout, invariant, and recipes for extending it |
| [`docs/DESIGN.md`](docs/DESIGN.md) | Why each decision was made, and what was rejected |
| [`examples/`](examples/) | Four short scripts covering the whole feature set |

## Limitations

Deliberate, and documented rather than hidden. No joins or subqueries. No
`UPDATE`/`DELETE` statements (the storage layer supports row deletion; no SQL
reaches it yet). Deletes don't rebalance the tree. No overflow pages, so a row
is capped at 1,700 bytes. Single connection. The full list, with where a fix
would go, is in the handbook.

## License

MIT — see [LICENSE](LICENSE).
