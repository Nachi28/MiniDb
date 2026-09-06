# MiniDB Handbook

Complete technical reference for the MiniDB project. Written to serve two
audiences equally:

- **A new contributor** who needs to understand the whole system before touching
  it.
- **An AI assistant** being given this file as context for a fresh session, with
  no memory of how the code got here.

Everything needed to work on MiniDB is in this document. Where a claim is
measurable, the command that measures it is given. Where a decision could have
gone another way, the rejected alternative is named.

Companion documents: `README.md` is the shop window (what it does, with
numbers). `docs/DESIGN.md` is the decision log (why each choice was made, and
what was rejected). This handbook is the reference (what exists and how it fits
together). When they disagree, the code wins — and then this file should be
corrected.

### Using this file as context for an AI session

Paste it whole. It is written to be self-contained: sections 3 to 7 are the
reference (what exists, what the bytes mean), section 8 lists the invariants that
must not be broken, section 11 says what is deliberately missing so nothing is
reported as a bug that is a known gap, and section 12 gives step-by-step recipes
for the most likely changes. Section 13 lists the traps.

---

## Contents

| | Section | Read it for |
|---|---|---|
| 1 | What MiniDB is | Scope, size, what it is not |
| 2 | Quick start | Build, run, verify, Windows specifics |
| 3 | Repository map | Every file and its job; build targets |
| 4 | Architecture | The layer stack, ownership, error model |
| 5 | Layer reference | API and behaviour of each component |
| 6 | The SQL surface | Exactly what SQL is accepted, and its semantics |
| 7 | On-disk format | Byte-level layouts of every structure |
| 8 | Invariants | The 25 rules that must hold |
| 9 | Testing | Suites, harnesses, how to run them |
| 10 | Performance | Numbers, method, capacity arithmetic |
| 11 | Known limitations | Deliberate gaps, and where a fix would go |
| 12 | How to add things | Recipes for likely changes |
| 13 | Working on this code | Conventions and traps |
| 14 | Glossary | Terms as used here |

---

## 1. What MiniDB is

A relational database engine written from scratch in C++17 with **no third-party
dependencies**. It parses SQL, plans queries against secondary indexes, and
executes them over B+trees stored in a paged file behind an LRU buffer pool,
with a write-ahead log providing crash safety. It can be used as a library, an
interactive shell, a batch script runner, or a TCP server.

**Status: complete.** All six planned phases are implemented. Version 1.0.0.

| Metric | Value |
|---|---|
| Engine source | ~5,950 lines (`src/` + `include/` + `bench/` + `tools/crash.cpp`) |
| Tests | 2,745 lines, 213 cases, plus a crash-torture target |
| Dependencies | none (C++17 standard library only) |
| Platforms | Windows (MSVC), Linux (GCC/Clang), macOS |
| Page size | 4096 bytes, fixed |
| File format version | 2 |

### What it deliberately is not

Not a competitor to SQLite. Single-process, single-connection, no joins, no
subqueries, no `UPDATE`/`DELETE` statements (the storage layer supports row
deletion; no SQL exposes it yet). Section 11 lists every gap explicitly.

---

## 2. Quick start

### Build

Requires CMake 3.16+ and any C++17 compiler.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`-DMINIDB_WARNINGS_AS_ERRORS=ON` enables `/W4 /WX` (MSVC) or
`-Wall -Wextra -Wpedantic -Werror` (GCC/Clang). CI builds with it on, and the
tree is clean under it — keep it that way.

### Windows specifics

This project was developed on Windows with **Visual Studio 18 (2026) Community,
MSVC 14.51**. `cl.exe` is not on `PATH` by default; every build must run inside a
developer environment:

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Ninja is used because it is on `PATH` (installed via Python). The Visual Studio
generator also works. `clang++` is installed but targets MSVC and cannot compile
without the VS toolchain, so it is not a standalone option here.

### Run

```bash
./build/minidb                                  # in-memory shell
./build/minidb --db shop.db                     # persistent shell
./build/minidb --db shop.db script.sql          # batch, exits non-zero on error
./build/minidb --db shop.db --serve 5433        # TCP server
./build/minidb --connect 127.0.0.1:5433         # TCP client
```

Shell meta-commands: `.help`, `.tables`, `.indexes`, `.schema [table]`,
`.flush`, `.exit` / `.quit`.

### Verify

```bash
ctest --test-dir build --output-on-failure       # 213 unit tests + crash torture
./build/minidb_bench --rows 200000 --db bench.db # throughput numbers
./build/minidb_crash torture --db c.db --rounds 500
python tools/difftest.py --queries 700 --rows 800
```

---

## 3. Repository map

Every file, with its job. Line counts are approximate and will drift.

### Headers — `include/minidb/`

| File | Lines | Contains |
|---|---|---|
| `slice.hpp` | 39 | `Slice` (borrowed bytes), `compare_slices` |
| `page.hpp` | 71 | `PageId`, `kPageSize`, little-endian `put_*`/`get_*` codecs |
| `row.hpp` | 11 | `using Row = std::vector<Value>` |
| `value.hpp` | 56 | `ValueType`, `Value`, three-valued `compare()` |
| `diagnostic.hpp` | 41 | `DbError`, `SqlError`, `render_error` |
| `util.hpp` | 30 | `to_lower`, `iequals` (header-only) |
| `token.hpp` | 60 | `TokenType`, `Token` |
| `lexer.hpp` | 42 | `Lexer` |
| `ast.hpp` | 112 | `Expr`, all statement structs, `Statement` variant |
| `parser.hpp` | 47 | `Parser` |
| `record.hpp` | 33 | Row ⇄ bytes encoding |
| `key.hpp` | 44 | Order-preserving key encodings |
| `btree.hpp` | 73 | `BTree` |
| `pager.hpp` | 155 | `PageGuard`, `Pager` |
| `wal.hpp` | 98 | `Wal` |
| `table.hpp` | 68 | `Table` |
| `index.hpp` | 63 | `Index` |
| `database.hpp` | 91 | `Database` |
| `plan.hpp` | 45 | `AccessPath`, `Plan`, `choose_plan` |
| `executor.hpp` | 63 | `ResultSet`, `EvalContext`, `Executor`, `evaluate` |

### Implementation — `src/`

| File | Lines | Job |
|---|---|---|
| `value.cpp` | 140 | Value accessors, display forms, SQL ordering |
| `diagnostic.cpp` | 28 | Caret diagnostic rendering |
| `lexer.cpp` | 402 | Keyword table, tokenizer, `token_type_name` |
| `ast.cpp` | 104 | Node factories, `to_string(Expr)` |
| `parser.cpp` | 384 | Recursive descent + Pratt, binding-power table |
| `record.cpp` | 112 | Row serialisation |
| `key.cpp` | 104 | Rowid / value / index-key encodings |
| `pager.cpp` | 378 | Buffer pool, LRU, pinning, commit/rollback/checkpoint |
| `wal.cpp` | 288 | Log frames, checksums, recovery, salt |
| `btree.cpp` | 489 | Slotted-page node, search, insert, splits, scans |
| `table.cpp` | 70 | Rowid assignment, row encode/decode |
| `index.cpp` | 88 | Index maintenance, seeks, bounded counting |
| `database.cpp` | 284 | Catalogue, transactions, index maintenance on insert |
| `plan.cpp` | 206 | Constraint extraction, cost model, `EXPLAIN` text |
| `executor.cpp` | 884 | Evaluation, aggregation, grouping, projection |
| `main.cpp` | 422 | CLI, shell, TCP server and client |

### Tests — `tests/`

| File | Cases | Covers |
|---|---|---|
| `test_framework.hpp` | — | 118-line zero-dependency test harness |
| `test_value.cpp` | 11 | Value semantics, NULL ordering |
| `test_key.cpp` | 10 | Key-encoding order preservation |
| `test_record.cpp` | 8 | Row round-trips, truncation rejection |
| `test_lexer.cpp` | 13 | Tokens, literals, comments, lex errors |
| `test_parser.cpp` | 27 | Precedence, associativity, all statements |
| `test_pager.cpp` | 14 | Eviction, pinning, persistence, bad files |
| `test_btree.cpp` | 18 | Splits at every level, packing, reopening |
| `test_index.cpp` | 7 | Seeks, ranges, duplicates, bounded counting |
| `test_plan.cpp` | 13 | Access-path choices, index/scan differential |
| `test_wal.cpp` | 9 | Recovery, torn frames, corruption, salt |
| `test_recovery.cpp` | 9 | Transaction semantics end to end |
| `test_storage.cpp` | 9 | SQL-level persistence |
| `test_executor.cpp` | 65 | Everything the SQL layer does |
| **Total** | **213** | |

### Tools and other

| Path | Job |
|---|---|
| `bench/main.cpp` | `minidb_bench` — throughput, pool stats, index comparison |
| `tools/crash.cpp` | `minidb_crash` — writer/verify/torture crash harness |
| `tools/difftest.py` | Randomised differential test against Python's `sqlite3` |
| `examples/demo.sql` | Runnable tour of every phase's features |
| `Dockerfile` | Multi-stage build; runs the test suite during build |
| `.github/workflows/ci.yml` | Linux/Windows/macOS build, test, difftest, round trip |

### Build targets

| Target | Kind | Sources |
|---|---|---|
| `minidb_core` | static library | all of `src/` except `main.cpp` |
| `minidb` | executable | `src/main.cpp` (+ `ws2_32` on Windows) |
| `minidb_bench` | executable | `bench/main.cpp` |
| `minidb_crash` | executable | `tools/crash.cpp` |
| `minidb_tests` | executable | all of `tests/` |

CTest registers two tests: `minidb_tests` and `minidb_crash_recovery` (20 rounds).

---

## 4. Architecture

Data flows down through translation layers. Each layer knows only the one below
it, which is why the storage engine could be replaced in phase 3 without the
executor noticing.

```
  SQL text
     |   Lexer          src/lexer.cpp      characters -> tokens (+ line/col)
     v   tokens
     |   Parser         src/parser.cpp     recursive descent + Pratt
     v   AST                               include/minidb/ast.hpp
     |   Analysis       src/executor.cpp   name resolution, grouping legality
     v   validated AST
     |   Planner        src/plan.cpp       access-path choice, index probing
     v   Plan
     |   Executor       src/executor.cpp   scan/seek, filter, group, sort, project
     v   ResultSet

  Table                 src/table.cpp      rows <-> a B+tree keyed by rowid
  Index                 src/index.cpp      (value, rowid) keys in a B+tree
    -> BTree            src/btree.cpp      byte keys, slotted pages, leaf chain
       -> Pager         src/pager.cpp      LRU buffer pool, pinning, write-back
          -> Wal        src/wal.cpp        checksummed frames, commit markers
          -> file                          4 KB pages, little-endian
```

### Ownership and lifetimes

- `Database` owns a `Pager` (via `unique_ptr`), a catalogue `BTree`, and maps of
  `Table` and `Index`.
- **Declaration order in `Database` matters.** `pager_` is declared before
  `tables_`/`indexes_`, so it is destroyed *after* them. Every `Table` and
  `Index` holds a `Pager*`; reversing the order is a use-after-free.
- `Table` and `Index` each own a `BTree`, which holds `Pager*` and a root
  `PageId`. Both are cheap to copy.
- `~Database` calls `flush()`; `~Pager` calls `commit()` then `checkpoint()`.
  Neither throws.

### Error model

Two exception types, both in `diagnostic.hpp`:

- `DbError` — anything the user did wrong or any runtime failure. Message only.
- `SqlError : DbError` — a parse or lex error, carrying `pos`, `line`, `col`.

`render_error(source, err)` turns a `SqlError` into a caret diagnostic. Catch
`SqlError` **before** `DbError`; `main.cpp` also catches `std::exception` because
`Value` accessors throw `std::logic_error` on internal misuse.

There are no error codes and no `Status` type. Exceptions were chosen because
almost every failure is fatal to the statement, and threading a result type
through a recursive-descent parser costs more than it returns.

---

## 5. Layer reference

### 5.1 `Value` — the scalar type

```cpp
enum class ValueType { Null, Integer, Real, Text };
class Value {                     // std::variant<monostate, int64_t, double, string>
    static Value null(); integer(int64_t); real(double); text(string); boolean(bool);
    ValueType type() const;  bool is_null() const;  bool is_number() const;
    int64_t as_integer() const;   // Integer or Real (truncates); else throws logic_error
    double  as_double()  const;   // Integer or Real; else throws
    const string& as_text() const;// Text only; else throws
    string to_string() const;     // display: NULL -> "NULL", text unquoted
    string to_sql()    const;     // literal: text single-quoted, '' escaped
};
std::optional<int> compare(const Value&, const Value&);
```

`boolean(b)` stores INTEGER 1/0, following SQLite — there is no boolean type.

**`compare()` returns `nullopt` when either side is NULL.** This is the single
most important detail in the type system: it is what makes `WHERE x = NULL`
correctly match nothing. Ordering across types follows SQLite's storage classes:
`NULL < numbers < text`.

`operator==` is separate and *not* the same rule: it treats `NULL == NULL` as
true, because that is what `IS` needs. Do not unify them.

### 5.2 Lexer

`Lexer(std::string source)`, `next()`, `tokenize()`. Single pass, no regex.

- Every token records `pos`, `line`, `col`. Without this the caret diagnostics
  are impossible to add later.
- Keywords are case-insensitive; identifiers preserve their case.
- Comments: `-- to end of line` and `/* block */` (unterminated block = error).
- Strings: `'...'` with `''` as the escape. Double quotes make a **quoted
  identifier**, and its `lexeme` holds the *unquoted* name so the parser can use
  it directly — the one place `lexeme` is not raw source.
- Numbers: integer, decimal, or exponent. A digit run followed immediately by an
  identifier character (`12abc`) is rejected.
- Operators: `= == <> != < <= > >= + - * / % ||`. A bare `!` or `|` is an error
  suggesting the two-character form.

**Keyword list** (case-insensitive): `select from where insert into values create
table and or not null is integer int real float double text varchar string order
by asc desc limit as if exists group having begin commit rollback transaction end
index on explain`. Note `int`/`integer`, `real`/`float`/`double`, and
`text`/`varchar`/`string` are aliases, and `end` is an alias for `commit`.

### 5.3 Parser

Recursive descent for statements, Pratt (precedence climbing) for expressions.

**Binding powers** (`src/parser.cpp`). Left below right makes an operator
left-associative; the gaps leave room to insert new operators without
renumbering.

| Operator | Left | Right |
|---|---|---|
| `OR` | 1 | 2 |
| `AND` | 3 | 4 |
| `NOT` (prefix) | — | 5 |
| `= <> < <= > >= IS IS NOT` | 7 | 8 |
| `\|\|` | 9 | 10 |
| `+ -` | 11 | 12 |
| `* / %` | 13 | 14 |
| `-` `+` (prefix) | — | 15 |

`IS` / `IS NOT` are handled ahead of the table because `IS NOT` is two tokens.

**Adding an operator** is one row in `binary_info()` plus a `BinaryOp` enum value
plus a case in `binary_op_symbol()` and `eval_binary()`. Nothing else.

`CREATE` dispatches on the *next* token (`peek(1)`) to choose table or index.
`EXPLAIN` is a prefix that parses a `SELECT` and sets `explain = true`.

### 5.4 AST

One `Expr` struct with a `kind` discriminant, rather than a class hierarchy:

```cpp
enum class ExprKind { Literal, Column, Unary, Binary, FunctionCall };
struct Expr {
    ExprKind kind;  Value literal;  std::string column;
    UnaryOp unary_op;  BinaryOp binary_op;
    ExprPtr left, right;                    // unary operand / binary sides
    std::string function;  std::vector<ExprPtr> args;  bool star_arg;
    std::size_t pos;
    // static factories: make_literal / make_column / make_unary / make_binary / make_call
};
```

`ExprPtr` is `unique_ptr<Expr>`, so the AST is move-only. `to_string(Expr)`
renders fully parenthesised — tests assert precedence with a single string
compare, and `SELECT` uses it to name unaliased result columns.

Statements are a `std::variant`: `CreateTableStmt`, `CreateIndexStmt`,
`InsertStmt`, `SelectStmt`, `TransactionStmt`. Dispatch in `Executor::execute`
uses `holds_alternative` rather than `std::visit`, to avoid the overload-set
boilerplate.

### 5.5 Analysis (inside `exec_select`)

Runs before any row is touched, in this order:

1. **ORDER BY resolution.** A bare column matching a select-list alias, or an
   integer naming an output position, is resolved to the underlying expression.
   Every later stage then sees a plain expression tree and needs no knowledge
   that aliases exist. Out-of-range positions are an error.
2. **Name resolution.** `validate_columns` walks every expression (items, WHERE,
   HAVING, GROUP BY, ORDER BY) and rejects unknown columns. *This must happen
   before the scan*: doing it during projection means an empty table never
   validates anything.
3. **Aggregate placement.** Aggregates are rejected in WHERE (use HAVING), in
   GROUP BY, and inside other aggregates.
4. **Aggregate collection.** Every aggregate call site is gathered by `Expr*`
   identity from the select list, HAVING and ORDER BY.
5. **Grouping legality.** If grouping is active, `SELECT *` is rejected, and
   every column referenced outside an aggregate must match a GROUP BY key
   (compared by rendered string).

A query is "grouped" if `group_by` is non-empty **or** any aggregate exists
**or** HAVING is present.

### 5.6 Planner

```cpp
enum class AccessPath { FullScan, IndexEqual, IndexRange };
struct Plan {
    AccessPath path;  const Index* index;
    Value equal_value;                                   // IndexEqual
    bool has_low, low_inclusive;  Value low;             // IndexRange
    bool has_high, high_inclusive; Value high;
    int64_t table_rows;  double estimated_rows, estimated_cost, scan_cost;
    bool exact_estimate;
    std::string describe() const;
};
Plan choose_plan(const Database&, const Table&, const Expr* where);
```

**Constraint extraction.** Conjuncts are collected by descending through `AND`
nodes only. An `OR` stops the walk — under OR a row failing one side may still
qualify, so no single index range covers the predicate. Each conjunct is matched
against `column OP literal` or `literal OP column` (the second is mirrored). A
NULL literal disqualifies the constraint, since comparisons never match NULL.

**Cost model.**

```
kRandomFetchCost = 4.0
scan_cost  = table_rows
index_cost = log2(table_rows + 2) + matches * kRandomFetchCost
```

The weight exists because a scan walks a leaf chain largely resident in the
buffer pool, while an index lookup is a random descent of the table tree. Without
it, any usable index always wins, which is wrong often enough to matter.

**Estimation is measured, not guessed.** `Index::count_equal` / `count_range`
walk the index and count, stopping at `cap = table_rows / 4 + 1` — the break-even
point past which the index has already lost. The count is therefore either exact
or provably irrelevant, and the probe costs at most a quarter of the scan it may
avoid. `exact_estimate` records which, and `EXPLAIN` prints `rows=` or `rows>=`.

Two constraints on the same indexed column are folded into one two-ended range.

### 5.7 Executor

```cpp
struct ResultSet { bool is_query; vector<string> column_names; vector<Row> rows;
                   string message; };
struct EvalContext { const Table* table; const Row* row;
                     const std::map<const Expr*, Value>* aggregates; };
Value evaluate(const Expr&, const EvalContext&);
Value evaluate(const Expr&, const Table*, const Row*);   // convenience, no aggregates
bool  is_true(const Value&);
bool  is_aggregate_name(const std::string&);
```

`exec_select` runs seven numbered stages, marked as such in the source:

1. Name resolution (§5.5)
2. Grouping decision and aggregate collection
3. Access path: full scan, index seek, or index range
4. Grouping and aggregate computation, then HAVING
5. ORDER BY (`stable_sort`)
6. LIMIT
7. Projection

**The aggregate mechanism.** Aggregates are computed once per group and stored
in a `map<const Expr*, Value>` keyed by AST node identity. `evaluate()` then
treats an aggregate node as a *lookup*. This is why `SUM(x)/COUNT(*)` needs no
special case, and why you can `ORDER BY` an aggregate that is not in the select
list. Pointer identity means two textually identical `COUNT(*)` calls are
computed twice — wasteful but correct; CSE belongs in a future planner.

**Three-valued logic.** `AND`/`OR` cannot use the generic binary path:
`FALSE AND NULL` is FALSE while `TRUE AND NULL` is NULL. They are special-cased
in `eval_binary`. `is_true()` treats NULL and TEXT as not-true.

**The safety rule.** After an index seek, the WHERE clause is re-evaluated on
every row fetched. An index is an *access path*, never an answer. This is what
makes the lossy numeric key encoding safe: an index may hand over extra
candidates, never lose one.

**Aggregates:** `COUNT(*)`, `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`. All except
`COUNT(*)` skip NULLs and return NULL over an empty set; `COUNT` returns 0. `SUM`
stays integral if every input was an integer.

**Scalar functions:** `ABS`, `LENGTH`, `LOWER`, `UPPER`, `ROUND(x[,n])`,
`COALESCE(a,b,...)`, `IFNULL(a,b)`. All propagate NULL. Adding one is a branch in
`eval_call`.

### 5.8 `Table`

```cpp
Table(name, columns, Pager&, PageId root, int64 next_rowid, int64 row_count, int64 catalog_id);
int64_t insert(Row);                      // assigns and returns a rowid
void    scan(fn(int64 rowid, Row&)) const;
bool    find(int64 rowid, Row* out) const;
bool    erase(int64 rowid);
int     column_index(const string&) const;   // case-insensitive, -1 if absent
int64_t row_count() const;                   // maintained, not counted
```

Rows are encoded (§7.2) and stored in a B+tree keyed by `encode_rowid(rowid)`.
Rowids start at 1 and only increase. `row_count_` is maintained incrementally and
persisted, because the planner needs cardinality and cannot afford to count.

`dirty()` means the catalogue entry is stale — the root page, rowid counter or row
count moved — and `Database::flush` writes it back.

**Never call `Table::insert` directly from the executor.** Use
`Database::insert_row`, which also updates every index on that table.

### 5.9 `Index`

```cpp
Index(name, table, column, Pager&, PageId root, int64 catalog_id);
void insert(const Value&, int64 rowid);
bool erase(const Value&, int64 rowid);
void seek_equal(const Value&, fn(int64 rowid)) const;
void seek_range(const Value* low, bool low_inc, const Value* high, bool high_inc,
                fn(int64 rowid)) const;
size_t count_equal(const Value&, size_t cap) const;
size_t count_range(low, low_inc, high, high_inc, size_t cap) const;
```

A B+tree keyed by `encode_value(v) + encode_rowid(rowid)` with an **empty
payload** — the rowid is already in the key. Pairing value with rowid lets
duplicates coexist while each value's rows form one contiguous seekable run.

Bounds come from the key encodings: an inclusive lower bound is
`encode_index_key(v, INT64_MIN)`, an exclusive one is
`encode_index_upper_bound(v)`, mirrored for upper bounds. Seeks return rowids in
*index* order, not rowid order.

### 5.10 `BTree`

```cpp
static constexpr size_t kMaxKey = 300;      // bytes
static constexpr size_t kMaxPayload = 1700; // bytes
static PageId create(Pager&);
BTree(Pager&, PageId root);
void insert(Slice key, Slice payload);      // upsert
bool find(Slice key, vector<uint8_t>* out) const;
bool erase(Slice key);
void scan(fn(Slice key, Slice payload)) const;
void scan_from(Slice start, fn(...)) const; // first key >= start, then leaf chain
size_t size() const;  size_t height() const;  string max_key() const;
```

Byte-string keys ordered by `memcmp` (`compare_slices`, shorter prefix first).
One implementation serves both tables and indexes because the *encodings* carry
the semantics.

**`root()` changes when the tree gains a level.** Whoever owns the tree must
notice and persist the new root. `Table`/`Index` set `dirty_`; `Database::flush`
writes it back; `Pager::set_catalog_root` handles the catalogue's own tree.

Insert is recursive and returns `Split { happened, key, right }`. The parent
absorbs it, and a split at the root creates a new root — the only way the tree
grows in height, which is why every leaf stays equidistant from the root.

**Leaf splits** rebuild the node's cells plus the new entry into a list, then
choose a split point:

- New key is largest → `at = n-1` (**append optimisation**, section 10)
- New key is smallest → `at = 1`
- Otherwise → the most balanced point at which both halves fit

Byte-based, not count-based, because cells vary in size. The size limits above
guarantee two cells always fit in a page, so a split always makes progress.

**Internal splits** promote the middle separator upward rather than copying it,
because internal keys are only signposts.

Deletion removes a slot and leaves a hole; the page reclaims it lazily via
`compact()`. **Deletes do not rebalance** — an emptied leaf stays in the tree.

### 5.11 `Pager` and `PageGuard`

```cpp
Pager(std::string path, size_t pool_pages = 64);   // ":memory:" for no file
PageGuard fetch(PageId);       // throws if id == 0 or >= page_count()
PageGuard allocate(PageId* out_id);
void commit();     // dirty pages -> WAL, commit marker, fsync
void rollback();   // discard everything since the last commit
void checkpoint(); // fold committed WAL frames into the file, reset the log
void flush();      // alias for commit()
PageId catalog_root() const;  void set_catalog_root(PageId);
// stats: cache_hits, cache_misses, disk_reads, disk_writes, evictions,
//        wal_frames, checkpoints, page_count
```

`PageGuard` is an RAII pin. While one is alive the page cannot be evicted, so its
pointer stays valid. `mutable_data()` marks the page dirty; `data()` and
`read_data()` do not. `read_data()` returns a non-const pointer for code that
builds a mutable view (the `Node` class) purely to read — **it must never be used
to modify a page**, or the change will silently never reach disk.

Eviction is LRU over unpinned frames only, with write-back. A B+tree insert holds
a guard at every level of its descent, so **the pool must be larger than the tree
is tall**; a fully pinned pool raises `DbError` rather than corrupting anything.

Frames are held by `unique_ptr` so that growing the pool (which an in-memory
database does instead of evicting) cannot move a buffer out from under an
outstanding guard.

`file_size_` is cached at open and updated on write, so a read never seeks to EOF
just to bounds-check. That was a real hot-path cost before phase 4.

**In-memory mode** (`":memory:"`): no file, no WAL, the pool grows on demand and
never evicts.

### 5.12 `Wal`

```cpp
Wal(std::string path, bool memory_only);   // the constructor performs recovery
bool read(PageId, uint8_t* into) const;
void append(PageId, const uint8_t* image, PageId db_size);  // db_size != 0 = commit
void sync();                    // fflush + fsync / _commit
void rollback_to_commit();      // truncate the tail
void for_each_committed(fn(PageId, const uint8_t*)) const;
void reset();                   // empty the log, roll the salt
```

The log file is `<db>-wal`. Changed pages are appended rather than written in
place.

**A frame carrying a non-zero database size is the commit marker.** Recovery
replays forward to the last one and discards everything after it — no undo
information is needed. Every commit ends by appending page 0 (the file header)
with the page count, which works because page 0 changes in nearly every
transaction anyway.

A frame is trusted only if its **salt** matches the header and its **checksum**
(FNV-1a over the frame-header prefix and the page image) matches. Recovery stops
at the first frame failing either test — including discarding any commit marker
that followed it, because the frames that commit depended on are now suspect.

The salt increments on every `reset()`. Without it, a crash leaving a long log,
followed by a checkpoint and a shorter log, could leave stale frames past the new
end of file with valid checksums.

**Checkpoint order matters:** write committed frames into the database file,
`fflush`, and only then `reset()` the log. Resetting first would lose those pages
on a crash in between.

### 5.13 `Database`

```cpp
explicit Database(std::string path = ":memory:");
Table*  find_table(const std::string&);      Table&  create_table(name, columns);
Index*  find_index(const std::string&);      Index&  create_index(name, table, column);
const Index* index_for(const std::string& table, const std::string& column) const;
int64_t insert_row(Table&, Row);             // maintains indexes
void flush();  void begin();  void commit();  void rollback();  void autocommit();
bool in_transaction() const;
```

Opening a database: the `Pager` constructor opens the file, constructs the `Wal`
(which recovers), checkpoints anything committed so the file is authoritative,
then reads the header. `Database` then walks the catalogue tree and rebuilds
`tables_` and `indexes_`.

`create_index` **backfills** from rows already in the table, so an index can be
added to a populated table.

**Transactions.** Outside an explicit transaction, `Executor` calls
`db_.autocommit()` after every mutating statement — so a crash loses at most the
statement in flight. `begin()` flushes first so the snapshot is clean.
`rollback()` rolls back the pager **and rebuilds the in-memory catalogue**,
because a table's root page and rowid counter may have moved inside the aborted
transaction, and a table created inside it must stop existing.

### 5.14 Server (`src/main.cpp`)

A line protocol, not a binary one. The client sends SQL terminated by `;`; the
server replies with the same table the shell prints, then a single `0x04` byte.
Telnet-debuggable, and it reuses `print_result` unchanged — the only edit needed
was giving that function an `std::ostream&` parameter.

Raw sockets behind one `#ifdef`: Winsock on Windows (links `ws2_32`, and needs
`NOMINMAX` before `winsock2.h` or its `max` macro clobbers `std::max`), BSD
sockets elsewhere.

**Single-threaded, one client at a time**, marked with a `ponytail:` comment in
the source. `Database` has no locking, so concurrent clients would need a thread
per connection *and* a lock around the database.

---

## 6. The SQL surface

Complete list of what MiniDB accepts. Anything not here is a syntax error.

### Statements

```sql
CREATE TABLE [IF NOT EXISTS] name (col TYPE, col TYPE, ...);
CREATE INDEX [IF NOT EXISTS] name ON table (column);

INSERT INTO table [(col, ...)] VALUES (expr, ...) [, (expr, ...)]...;

SELECT  { * | expr [[AS] alias] } [, ...]
        [FROM table]
        [WHERE expr]
        [GROUP BY expr [, ...]]
        [HAVING expr]
        [ORDER BY { expr | alias | position } [ASC|DESC] [, ...]]
        [LIMIT integer];

EXPLAIN SELECT ...;

BEGIN [TRANSACTION];   COMMIT;   END;   ROLLBACK;
```

Column types: `INTEGER`/`INT`, `REAL`/`FLOAT`/`DOUBLE`, `TEXT`/`VARCHAR`/`STRING`.

### Expressions

| Category | Operators |
|---|---|
| Arithmetic | `+ - * / %` |
| Concatenation | two pipe characters |
| Comparison | `= == <> != < <= > >=` |
| Null-safe | `IS`, `IS NOT` |
| Logical | `AND OR NOT` |
| Prefix | `-` `+` `NOT` |
| Grouping | parentheses |

Literals: integers, reals (including exponent form), single-quoted text with
doubled-quote escaping, and `NULL`. Identifiers may be double-quoted.

### Functions

| Kind | Functions |
|---|---|
| Aggregate | `COUNT(*)`, `COUNT(x)`, `SUM`, `AVG`, `MIN`, `MAX` |
| Scalar | `ABS`, `LENGTH`, `LOWER`, `UPPER`, `ROUND(x[,n])`, `COALESCE(a,b,...)`, `IFNULL(a,b)` |

### Semantics that differ from a naive implementation

- **`WHERE x = NULL` matches nothing**, always. Use `IS NULL`.
- **`GROUP BY x` groups all NULLs together** — a different rule from `=`, on
  purpose.
- `FALSE AND NULL` is FALSE; `TRUE AND NULL` is NULL. `NOT NULL` is NULL.
- Integer division truncates (7/2 = 3); if either side is REAL the result is REAL
  (7.0/2 = 3.5).
- `SUM` stays integral if every input was an integer; `AVG` is always REAL.
- Aggregates over an empty set: `COUNT` is 0, everything else is NULL.
- `ORDER BY` sorts NULLs **first** (SQLite's default; Postgres sorts them last).
- Table, column and index names are case-insensitive; string comparison is not.
- Identifiers are resolved before the scan, so a bad column is an error even on
  an empty table.
- Unaliased result columns are named by their rendered expression, e.g.
  `(id + 1)` or `COUNT(*)`.
- Rows come back in rowid order after a scan, but in **index order** after an
  index seek. Without `ORDER BY`, no order is promised.

### Representative errors

```
error: expected an expression, found ';'
error: no such column: nope in table t
error: column b must appear in GROUP BY or be used in an aggregate function
error: aggregate functions are not allowed in WHERE; use HAVING
error: aggregate functions cannot be nested inside SUM()
error: SELECT * cannot be combined with GROUP BY or aggregates
error: ORDER BY position 99 is out of range
error: cannot store TEXT in INTEGER column 'a'
error: table t expects 3 value(s), but 2 were supplied
error: row is too large to store: 3010 bytes, limit is 1700
error: division by zero
error: a transaction is already open
```

---

## 7. On-disk format

Everything is little-endian unless stated. Format version 2.

### 7.1 Database file

A flat array of 4096-byte pages. Page 0 is the header, so `kInvalidPage == 0`
also serves as the "no such page" sentinel.

**Page 0 — file header**

| Offset | Size | Field |
|---|---|---|
| 0 | 6 | magic `MINIDB` |
| 6 | 1 | format version (2) |
| 7 | 1 | reserved |
| 8 | 4 | page count, including page 0 |
| 12 | 4 | catalogue B+tree root page |
| 16 | 4080 | unused |

### 7.2 Record encoding (`record.cpp`)

A row is a flat byte string: one tag byte per column, then its payload. Nothing
is aligned or padded, so the encoding is identical on every platform.

| Tag | Type | Payload |
|---|---|---|
| 0 | NULL | none |
| 1 | INTEGER | 8 bytes, little-endian two's complement |
| 2 | REAL | 8 bytes, little-endian IEEE-754 |
| 3 | TEXT | 4-byte length, then that many bytes |

The column count is not stored — the caller knows the schema, and a short row
decodes to fewer values.

### 7.3 Key encodings (`key.cpp`)

Designed so `memcmp` over encoded keys reproduces SQL ordering exactly.

**Rowid key** — 8 bytes, big-endian, sign bit flipped. Exact for all `int64`.

**Value key** — a tag byte, then:

| Tag | Type | Encoding |
|---|---|---|
| `0x00` | NULL | nothing |
| `0x01` | number | 8 bytes big-endian order-preserving IEEE-754 |
| `0x02` | text | bytes with `0x00` escaped as `00 FF`, terminated by `00 00` |

Order-preserving double: flip every bit if negative, else set the top bit, then
store big-endian. Integers convert to `double` first, so **two integers more than
2^53 apart can share a key** — safe only because an index seek re-checks the
predicate (section 5.7).

Text escaping matters because an index key is value-then-rowid; without it a
value containing `0x00` could be confused with the rowid following it.

**Index key** — value encoding followed by rowid encoding.

**Index upper bound** — the value encoding followed by **nine** `0xFF` bytes.
Nine, not eight: the rowid encoding of `INT64_MAX` is itself eight `0xFF` bytes,
so an eight-byte bound would tie rather than exceed. Nine ties over the first
eight and then wins on length.

### 7.4 B+tree node page (`btree.cpp`)

**Node header, 12 bytes**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | node type: 1 leaf, 2 internal |
| 1 | 1 | reserved |
| 2 | 2 | cell count |
| 4 | 2 | heap start (cells occupy heap_start .. 4095) |
| 6 | 2 | reserved |
| 8 | 4 | leaf: next-leaf page. internal: rightmost child |

Then `cell_count` 2-byte slot offsets, sorted by key, growing forward from offset
12, while cell data grows backward from the end of the page.

```
+--------+--------------------+..........+--------+--------+
| header | slot0 slot1 slot2  |   free   | cell 2 | cell 0 |
+--------+--------------------+..........+--------+--------+
 0     11  ---> slots grow                cells grow <---  4095
```

- **Leaf cell:** `uint16 key_len | key | uint16 payload_len | payload`
- **Internal cell:** `uint16 key_len | key | uint32 child_page`

Invariant: cell `i` of an internal node covers keys **strictly less than**
`key_at(i)`; the rightmost child covers everything from the last key upward.
That is why child selection uses `upper_bound`, not `lower_bound`.

Capacity is `4096 - 12 = 4084` bytes for slots plus cells. `kMaxKey + kMaxPayload
+ 6 = 2006`, so two maximal cells always fit and a split always makes progress.

### 7.5 Write-ahead log file (`<db>-wal`)

**Header, 32 bytes**

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `MDBWAL` and a NUL |
| 8 | 4 | format version (1) |
| 12 | 4 | page size (4096) |
| 16 | 4 | salt |
| 20 | 4 | header checksum, FNV-1a over bytes 0 to 19 |
| 24 | 8 | reserved |

**Frame, 24 bytes + 4096 bytes**

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | page number |
| 4 | 4 | database size after commit — **0 means not a commit** |
| 8 | 4 | salt, must match the header |
| 12 | 4 | checksum: FNV-1a over frame bytes 0 to 11, then the page image |
| 16 | 8 | reserved |
| 24 | 4096 | page image |

### 7.6 Catalogue entries

Stored in their own B+tree (root recorded in the file header), keyed by the rowid
encoding of the catalogue id, with the entry itself written by the ordinary
record encoder. The leading field says how to read the rest.

| Kind | Layout |
|---|---|
| `0` table | `0, name, root_page, next_rowid, row_count, (col_name, col_type)*` |
| `1` index | `1, name, root_page, table_name, column_name` |

`col_type` is the integer value of `ValueType`: 0 NULL, 1 INTEGER, 2 REAL, 3 TEXT.

Reusing the record encoder means there is no second serialisation format to keep
in step with the first.

---

## 8. Invariants

Rules the code depends on. Breaking one produces silent corruption rather than a
clean failure, so check these before and after any change to storage.

**Pager and buffer pool**

1. A raw page pointer is valid only while a `PageGuard` for it is alive.
2. Modifying a page requires `mutable_data()`. Writing through `read_data()`
   leaves the page unmarked and the change never reaches disk.
3. The pool must hold more frames than the tallest tree has levels, since an
   insert pins one page per level.
4. Frames live behind `unique_ptr`; growing `frames_` must never move a buffer.
5. `page_count_` includes page 0. The first allocatable page is 1.

**B+tree**

6. Keys within a node are sorted, and `slot[i]` points at the cell for key `i`.
7. Internal cell `i` covers keys strictly less than `key_at(i)`; the rightmost
   child covers the rest.
8. All leaves are the same distance from the root. Only a root split changes the
   height.
9. Leaves are chained left to right through `extra()`; the last leaf's is
   `kInvalidPage`.
10. `root()` may change on any insert. The owner must persist the new value.
11. `kMaxKey + kMaxPayload + 6 <= 4084 / 2` must hold, or splits can stop making
    progress.

**WAL and durability**

12. A frame is valid only if its salt matches the header *and* its checksum
    matches.
13. Recovery must stop at the first invalid frame, discarding any later commit
    marker too.
14. Checkpoint writes the database file **before** resetting the log, never after.
15. `reset()` must increment the salt, or stale frames past the new EOF can be
    mistaken for live ones.
16. Commit ends with page 0 carrying a non-zero database size. That frame *is*
    the commit record.
17. Rollback must purge the buffer pool — some dirty pages were already spilled
    to the log by eviction.

**Catalogue and schema**

18. `Database` declares `pager_` before `tables_`/`indexes_`, so the pager
    outlives everything holding a `Pager*`.
19. Every mutation that moves a root page, rowid counter or row count must set
    `dirty_` so `flush()` writes the catalogue entry back.
20. `Database::rollback` must rebuild `tables_` and `indexes_` from the
    catalogue; in-memory schema state is stale after an abort.
21. Rows are inserted through `Database::insert_row`, never `Table::insert`, or
    indexes fall behind.

**Query semantics**

22. `compare()` returns `nullopt` if either side is NULL; `operator==` treats two
    NULLs as equal. They are different rules and must stay separate.
23. An index seek is an access path only. WHERE is re-evaluated on every row it
    produces, which is what makes the lossy numeric key encoding safe.
24. Name resolution happens before the scan, not during projection.
25. Only top-level `AND` conjuncts may drive an index. `OR` forces a scan.

---

## 9. Testing

### Running

```bash
ctest --test-dir build --output-on-failure     # both registered tests
./build/minidb_tests                            # 213 unit cases directly
./build/minidb_crash torture --db c.db --rounds 500
python tools/difftest.py --queries 700 --rows 800
./build/minidb --db d.db examples/demo.sql      # feature tour
```

### The test framework

`tests/test_framework.hpp`, 118 lines, no dependencies — so the suite builds with
no network access. Deliberately uses the `TEST` / `REQUIRE` spelling, so swapping
to GoogleTest later would be a `CMakeLists.txt` change rather than a rewrite.

```cpp
TEST(Suite, Name) { ... }
REQUIRE(condition);
REQUIRE_EQ(actual, expected);
REQUIRE_THROWS_AS(expression, ExceptionType);
```

**`REQUIRE_EQ` copies both operands rather than binding references.** This is not
an oversight. A test may legitimately write `REQUIRE_EQ(f().rows[0][0], ...)`,
where the operand is a sub-object of a temporary; binding a reference to it does
*not* extend the temporary's lifetime, so the reference would dangle before the
comparison ran. That bug existed for two phases and made string comparisons pass
for the wrong reason. Do not "optimise" the copy away.

### The crash harness (`tools/crash.cpp`)

```
minidb_crash writer  --db P --rows N [--commit]   # inserts, then _Exit(9)
minidb_crash verify  --db P [--expect N]          # reopens and checks invariants
minidb_crash torture --db P --rounds N [--seed S] # spawns writers, kills, verifies
```

The writer terminates with `_Exit` — no destructors, no flush, no checkpoint,
which is as close to `kill -9` as a program can do to itself. Invariants checked
after each round: row count equals exactly the committed rows, and `SUM(id)`
equals `n(n+1)/2`. A lost, duplicated or half-written row breaks one of them.

```
torture: 500 kill-and-recover rounds survived
         (233 committed, 267 killed mid-transaction), 13793 rows intact
```

### The differential test (`tools/difftest.py`)

Generates random queries from templates, runs each against both MiniDB and
Python's standard-library `sqlite3`, and compares the rows. SQLite is the oracle;
no subprocess is needed for the reference side and the test has no dependencies.

Indexes are created on the MiniDB side **only**, so the planner's access paths are
checked against an engine that has never heard of them.

Comparison is permissive about formatting (MiniDB prints text, SQLite returns
typed values) and strict about content. The harness counts the rows it actually
compared and **fails if that count is zero** — an early version parsed the ASCII
output on the wrong separator and would have passed vacuously.

```
700 queries, 800 table rows, 46925 result rows compared, 0 mismatches
```

### Where coverage is thin

- No fuzzing of the parser against random bytes.
- No multi-threaded stress (there is no threading).
- The differential test does not exercise transactions or `EXPLAIN`.
- No test opens a database file written by a *different* format version.

---

## 10. Performance

### Method

`minidb_bench --rows 200000 --db bench.db`, 256-page (1 MB) pool, MSVC release
build. Numbers are **medians of five runs**; the first run after a reboot is
roughly 30% slower with a cold file cache, so a single measurement is not worth
quoting.

| Operation | Phase 3 | Phase 4+ |
|---|---|---|
| Insert (storage layer) | 462,000 rows/s | 701,000 rows/s |
| Full scan (storage layer) | 1,275,000 rows/s | 2,920,000 rows/s |
| `SELECT COUNT/SUM/AVG` via SQL | 788,000 rows/s | 1,443,000 rows/s |
| Random point lookup | 53,000 gets/s | 132,000 gets/s |

Buffer-pool hit rate 95.9%; 2,263 pages for 8.8 MB of file.

The phase 4 improvement has **two** causes and only one is the log: evicting a
dirty page appends sequentially instead of seeking into the database file, *and*
the read path stopped seeking to EOF on every page read just to bounds-check it.
The second was a phase 3 inefficiency the rewrite exposed, fixed separately so
the comparison stays honest.

### The append-aware split

Rowids ascend, so in practice every leaf split happens at the right edge, and
splitting those down the middle leaves each leaf permanently half full.

| 200,000 rows | Pages | File | Disk reads | Hit rate |
|---|---|---|---|---|
| Balanced split | 4,522 | 17.7 MB | 38,425 | 94.3% |
| Append-aware split | 2,263 | 8.8 MB | 27,003 | 95.9% |

Same data, half the file. A mirror case handles descending inserts, and keys
landing in the middle still split evenly — `InsertsInTheMiddleStillSplitEvenly`
guards that.

### Index versus scan

| Point query on 200,000 rows | Time |
|---|---|
| Without an index | 86.2 ms |
| With an index | 0.11 ms |

757x, which is the difference between reading 200,000 rows and reading one.

### Rough capacity arithmetic

- Page capacity for slots plus cells: **4084 bytes**.
- A 40-byte row makes a 52-byte leaf entry, so about **78 rows per leaf**.
- An internal cell for an 8-byte rowid key is 14 bytes plus a 2-byte slot, so
  about **255 children per internal node**.
- So roughly 20,000 rows fit in a 2-level tree and 5,000,000 in a 3-level one.
  A lookup is 2 or 3 page reads.

---

## 11. Known limitations

All deliberate, all documented, none hidden. Roughly in the order worth fixing.

| Gap | Consequence | Where a fix would go |
|---|---|---|
| No `UPDATE` / `DELETE` statements | Storage supports `erase`; no SQL reaches it | parser + executor |
| Deletes do not rebalance | An emptied leaf stays; pages are never freed | `btree.cpp`, free list in the header |
| No overflow pages | Rows capped at 1,700 bytes, rejected beyond | `btree.cpp` |
| No joins or subqueries | Single-table queries only | parser, planner, executor |
| One column per index | No composite indexes | `index.cpp`, catalogue format |
| No histograms | Ranges are probed, which costs work | `plan.cpp` + catalogue statistics |
| Single connection | Server handles one client at a time | `main.cpp` + a lock in `Database` |
| No `DROP TABLE` / `DROP INDEX` | Schema objects cannot be removed | `database.cpp` |
| Integers keyed through `double` | Two integers 2^53 apart share a key (safe, wasteful) | `key.cpp` |
| Shell splits on trailing `;` | A semicolon inside a literal confuses interactive editing only | `main.cpp` |
| No `VACUUM` | Space from aborted transactions is never reclaimed | free list |
| Aggregates not CSE'd | Two identical `COUNT(*)` calls compute twice | `plan.cpp` |

---

## 12. How to add things

Recipes for the changes most likely to be wanted next.

### A new operator

1. Add a `TokenType` and its keyword or punctuation in `lexer.cpp`, plus a case
   in `token_type_name`.
2. Add a `BinaryOp` (or `UnaryOp`) in `ast.hpp` and a case in `binary_op_symbol`.
3. Add one row to `binary_info()` in `parser.cpp` with its binding powers.
4. Add a case in `eval_binary` in `executor.cpp`.
5. If it should drive an index, teach `as_constraint` in `plan.cpp`.
6. Test precedence in `test_parser.cpp`, behaviour in `test_executor.cpp`.

### A new scalar function

One branch in `eval_call` (`executor.cpp`) plus a test. Nothing else — the parser
already accepts arbitrary `name(args)`.

### A new aggregate

Add the name to `is_aggregate_name`, handle it in `compute_aggregate`, and test
the empty-set and all-NULL cases, which is where aggregates go wrong.

### `DELETE FROM t WHERE ...`

1. `DeleteStmt` in `ast.hpp`, added to the `Statement` variant.
2. Parse it, reusing `parse_expression` for the WHERE.
3. `exec_delete`: plan the access path exactly as `exec_select` does, collect
   rowids **first** (never mutate the tree while scanning it), then for each one
   call `table.erase(rowid)` and `index.erase(old_value, rowid)` for every index
   on that table.
4. Call `db_.autocommit()`.
5. Test that indexes stop returning the deleted rows.

### Composite indexes

The key encoding already composes: value, value, rowid. The work is the catalogue
format (store a column list), the `Index` API, and `plan.cpp` matching a *prefix*
of the index columns against the available constraints.

### Making the server concurrent

1. A thread per accepted connection.
2. One mutex around `Database` — correct and trivially reviewable.
3. Reader-writer locking, then page latches in the pager, only if measurement
   says the global lock is actually the bottleneck.

### A new file-format field

Bump `kFormatVersion` in `pager.cpp`. Old files are then rejected with a clear
message rather than misread. There is no migration path, by design.

---

## 13. Working on this code

- **Read `docs/DESIGN.md` before changing a design decision.** It records what
  was rejected and why; several choices that look arbitrary are not.
- **The build must stay clean under warnings-as-errors.** CI enforces it on three
  platforms.
- **Comments explain why, not what.** A comment restating the code is noise; a
  comment naming the invariant a line protects is the point.
- **Deliberate shortcuts carry a `ponytail:` comment** naming the ceiling and the
  upgrade path. There is one, on the single-threaded accept loop.
- **Tests come with the change.** 213 cases exist because each phase added its
  own; a change with no test is not finished.
- **Scripted edits, beware heredocs.** Parts of this repo were written through
  shell heredocs, where backslash escapes can be silently collapsed. If you
  generate C++ from a script, verify the escapes survived before building.
- **Stray file:** `stop` in the repository root contains the text `PawnIO` and has
  nothing to do with this project. It is untracked and safe to delete.

### Style

C++17, 4-space indent, ~90 column limit, `snake_case` for functions and
variables, `PascalCase` for types, trailing underscore on private members,
`k`-prefixed constants. Implementation details live in anonymous namespaces.
Headers include what they use.

---

## 14. Glossary

| Term | Meaning here |
|---|---|
| **Page** | A fixed 4096-byte unit of the file, the granularity of all I/O |
| **Frame** | A slot in the buffer pool holding one page, or a WAL record |
| **Pin** | A count preventing eviction while a pointer into a page is held |
| **Slotted page** | A page with a sorted offset array growing one way and variable-size cells the other |
| **Cell** | One key-plus-payload (leaf) or key-plus-child (internal) record |
| **Rowid** | The implicit ascending integer primary key of every table row |
| **Catalogue** | The B+tree describing tables and indexes, rooted from the file header |
| **Commit marker** | A WAL frame carrying a non-zero database size; everything up to it is durable |
| **Salt** | A generation counter in the WAL header that invalidates stale frames |
| **Checkpoint** | Folding committed WAL frames into the file and emptying the log |
| **Access path** | How the executor reaches rows: full scan, index seek, or index range |
| **Selectivity** | The fraction of rows a predicate matches; MiniDB measures rather than assumes |
| **Break-even** | `rows / 4`, past which an index costs more than a scan and the probe stops counting |
| **Differential test** | Running the same query against a reference engine and comparing results |

---

*Last verified against commit `1cd7b25` (phase 6 complete, version 1.0.0), with
213 tests passing and a clean warnings-as-errors build on MSVC 14.51.*
