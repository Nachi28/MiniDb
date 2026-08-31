# Design notes

Decisions worth defending, and the alternatives that were rejected. Kept as a
running log so the reasoning survives past the commit that made the change.

## Hand-written lexer and parser, not a generator

ANTLR or Bison would have saved a weekend. They would also have cost the error
messages, which are the part a user actually interacts with. `SqlError` carries
a byte offset plus line and column, and `render_error` prints the offending line
with a caret under it. That is only cheap because the lexer records a position
on every token.

## Pratt parsing for expressions

The alternative is one function per precedence level (`parse_or`, `parse_and`,
`parse_comparison`, ...). That works, but adding an operator means adding a
function and re-threading the chain. Precedence climbing puts the whole grammar
in one table:

| Operator | Left | Right |
|---|---|---|
| `OR` | 1 | 2 |
| `AND` | 3 | 4 |
| `NOT` (prefix) | — | 5 |
| `= <> < <= > >= IS` | 7 | 8 |
| `\|\|` | 9 | 10 |
| `+ -` | 11 | 12 |
| `* / %` | 13 | 14 |
| `-` (prefix) | — | 15 |

Left binding power below right makes an operator left-associative. The gaps
leave room to insert operators without renumbering.

`IS` and `IS NOT` are handled ahead of the table because `IS NOT` is two tokens;
folding that into the table would need lookahead in `binary_info`.

## One Expr struct with a kind tag

A class hierarchy with a visitor is the textbook answer. For a tree this small
it costs a virtual dispatch and a lot of boilerplate per node type. A single
struct with a discriminant keeps every node the same size, makes the AST
trivially copyable in tests, and turns "add a node type" into one enum value.

This will be revisited at phase 5: once IR lowering and optimisation passes
exist, the visitor pattern starts paying for itself.

## NULL is a state, not a sentinel

`Value` is a `std::variant` whose first alternative is `std::monostate`. There
is no magic integer meaning "absent". Consequently:

- `compare()` returns `std::optional<int>` and yields `nullopt` if either side
  is NULL, which is what makes `WHERE x = NULL` correctly match nothing.
- `AND` and `OR` cannot use the generic binary path, because `FALSE AND NULL`
  is `FALSE` while `TRUE AND NULL` is `NULL`. They are special-cased in
  `eval_binary`.
- `is_true()` treats NULL as not-true, so `WHERE` drops the row, while
  `NOT NULL` still evaluates to NULL rather than TRUE.

Getting this wrong is the single most common flaw in a toy SQL engine, and it
is invisible until someone inserts a NULL.

## Rows are borrowed during a scan, copied only at projection

`exec_select` builds a `std::vector<const Row*>` while filtering and sorting,
so `ORDER BY` moves pointers rather than rows. Values are copied once, into the
`ResultSet`, at projection time.

## Storage is deliberately behind a narrow interface

`Table` exposes `insert`, `rows`, `columns` and `column_index` and nothing else.
Phase 3 replaces the `std::vector<Row>` with a paged B+tree; the point of the
narrow interface is that the executor should not have to change when it does.

## Open questions

- Should `INSERT` coerce text to numbers the way SQLite does, or keep phase 1's
  strict rejection? Strictness catches real mistakes; SQLite compatibility
  matters once differential testing starts.
- `ORDER BY` sorts NULLs first, matching SQLite's default. Postgres sorts them
  last. Worth a flag when the planner arrives.

## Name resolution runs before the scan, not during it

`exec_select` walks every expression in the select list, `WHERE` and `ORDER BY`
and resolves each column against the table schema *before* the first row is
touched.

The obvious implementation is to let `evaluate()` raise "no such column" when it
hits an unknown name during projection. That is wrong in a way that only shows
up on an empty table: with no rows to project, the expressions are never
evaluated, and `SELECT nope FROM empty_table` quietly returns zero rows instead
of an error. Real engines validate the query against the catalogue at plan time
for exactly this reason.

`validate_columns` is the seed of the resolver that phase 5 needs anyway: once
there is a planner, it will bind names to column indices once rather than
re-looking them up per row.

## Aggregates are computed per group, then read back as leaves

`SUM(amount) / COUNT(*)` should not need a special case in the expression
evaluator. So before projecting a group, the executor walks the select list,
`HAVING` and `ORDER BY`, collects every aggregate *call site* (by `Expr*`
identity), and computes each one over the group's rows. Those values go into an
`EvalContext`, and `evaluate` treats an aggregate node as a lookup rather than a
computation.

The consequence is that aggregates compose with everything else for free:
arithmetic on them, comparisons in `HAVING`, sorting by one that is not even in
the select list. The alternative — a separate "aggregate expression" evaluator —
would have needed its own copy of every operator.

Using pointer identity as the key matters: two textually identical `COUNT(*)`
call sites are distinct nodes and are computed separately. That is wasteful but
correct; common-subexpression elimination belongs in the phase 5 planner.

## Grouping and NULL disagree with `=` on purpose

`WHERE k = NULL` matches nothing, but `GROUP BY k` puts every NULL key in the
same group. Both are correct, and they are different rules. `GroupKeyLess`
therefore does not reuse `compare()` directly — it treats two NULLs as equal and
orders NULL before everything else, while `compare()` keeps returning `nullopt`
for the comparison operators.

Groups are emitted in first-seen order rather than sorted. SQL does not promise
an order without `ORDER BY`, and first-seen is both deterministic (so tests can
assert on it) and cheap.

## ORDER BY resolves aliases and positions before analysis

`ORDER BY total` where `total` is a select-list alias, and `ORDER BY 2` meaning
the second output column, are both resolved to the underlying expression in a
single pass at the top of `exec_select`. Every later stage — name resolution,
aggregate collection, the `GROUP BY` legality check, the sort itself — then sees
a plain expression tree and needs no knowledge that aliases exist.

Doing it the other way round, teaching each stage about aliases, is how this
gets subtly inconsistent: the sort would honour an alias while the grouping
check would not.

## Open question: aliases in GROUP BY and HAVING

SQLite accepts `GROUP BY total`; standard SQL does not, because `GROUP BY` is
logically evaluated before the select list exists. MiniDB currently follows the
standard. Worth revisiting when differential testing against SQLite starts.

## Slotted pages, so rows of different sizes share a page

A node page keeps a sorted array of 2-byte offsets growing forward from the
header while cell data grows backward from the end of the page. Free space is
whatever is left in the middle.

The alternative, fixed-size slots, is far simpler but forces every row to the
size of the largest one. With slots, a row costs exactly its encoded length,
and the sorted offset array still gives a binary search over keys without
touching the cells.

Deleting a cell only removes its slot; the bytes stay behind as a hole until
`compact()` rebuilds the heap. Compaction is triggered lazily, when an insert
finds too little contiguous space but enough total space. Reclaiming eagerly on
every delete would move cells far more often than it needs to.

## The buffer pool pins pages, and PageGuard makes that automatic

A pointer into a buffered page is only valid while that page cannot be evicted.
Rather than trusting callers to remember, `Pager::fetch` returns a `PageGuard`
that pins on construction and unpins on destruction, and only `mutable_data()`
marks the page dirty -- so a page cannot be modified without also being marked.

Eviction is LRU over unpinned frames only, with write-back. A B+tree insert
holds a guard at every level of the path it is descending, which is why the pool
must be larger than the tree is tall; a pool with every frame pinned raises a
clear error rather than corrupting something.

Frames are held by `unique_ptr` so that growing the pool (which an in-memory
database does instead of evicting) cannot move a buffer out from under a guard
that is already holding a pointer to it.

## Appends get their own split rule

Rowids ascend, so in practice every leaf split happens at the right edge of the
tree. Splitting those down the middle leaves each leaf permanently half full:
measured over 200,000 rows, that was 4,522 pages and a 17.7 MB file.

Detecting that the incoming key is the largest and giving it a page of its own
instead -- leaving the full page full -- produced 2,263 pages and 8.8 MB for the
same data, with 30% fewer disk reads. The mirror case handles descending
inserts. Keys landing in the middle still split evenly, because for those the
balanced split is the right answer.

This is the same optimisation SQLite and InnoDB make, and it is worth about half
the file.

## The catalogue is a table like any other

Schemas live in their own B+tree whose root page is recorded in the file header.
Each entry is encoded with the ordinary record encoder -- name, root page, next
rowid, then column name/type pairs -- so there is no second serialisation format
to keep in step with the first.

The consequence worth noting: a table's root page moves whenever its tree gains
a level, so the catalogue entry has to be rewritten. Tables track a dirty flag
and `Database::flush` writes back the ones that moved.

## Known gaps, deliberately

- **Deletes do not rebalance.** `erase` removes the cell and leaves the leaf
  where it is, even if it empties. Space inside a page is reused; a fully empty
  page is not returned to a free list. Merging siblings is real work and belongs
  with `DELETE`, which does not exist yet.
- **No overflow pages.** A row must fit in a page alongside at least one other,
  so payloads are capped at 1,980 bytes and larger ones are rejected with a clear
  error rather than silently truncated.
- **No crash safety.** A flush that is interrupted can leave a torn file. That is
  exactly what phase 4's write-ahead log is for.

## A write-ahead log, not a rollback journal

Two designs solve the same problem. A rollback journal saves each page's
*previous* contents before overwriting it, and recovery undoes; a write-ahead
log appends each page's *new* contents and recovery redoes. MiniDB writes ahead.

The reason is what a commit costs. With a rollback journal, committing means the
database file itself must be fully written and synced. With a log, committing is
an append and one sync of a file that is being written sequentially -- and the
database file is only touched later, in bulk, at a checkpoint. Evicting a dirty
page mid-transaction becomes cheap for the same reason: it is another append,
not a seek into the middle of the database.

## The commit marker is the header page

Every commit ends by appending page 0 -- the file header -- with the database
size written into the frame header. A non-zero size is what makes a frame a
commit marker.

This falls out of something that was already true: page 0 holds the page count
and the catalogue root, so it changes in almost every transaction anyway.
Reusing it means there is no separate marker record to keep consistent, and
recovery has exactly one rule: replay frames until the last one carrying a size,
and throw away whatever follows.

## Recovery needs no undo information

A frame is trusted only if its salt matches the log header and its checksum
matches its contents. Recovery walks forward from the start, stopping at the
first frame that fails either test, and remembers the offset of the last commit
marker it saw. Everything after that offset is truncated.

That covers all three failure shapes at once: a clean kill between transactions
(nothing after the last commit), a kill mid-transaction (uncommitted frames,
dropped), and a torn write (a frame whose checksum fails, dropped along with
everything after it -- including any commit marker that followed, because the
frames it depended on can no longer be trusted).

## The salt exists to kill stale generations

After a checkpoint the log is truncated and its salt incremented. Without the
salt, a crash that left a longer log behind, followed by a checkpoint and a
shorter log, could leave old frames sitting past the new end of file with valid
checksums. The salt makes every one of them unrecognisable.

## Rollback purges the buffer pool

Undoing a transaction cannot just discard dirty pages, because some of them were
already spilled into the log by eviction. So rollback truncates the log back to
the last commit and then drops every cached page, forcing the next read to come
from durable state. Pages allocated inside the aborted transaction stay
allocated in the file; they are simply unreferenced, and reclaiming them needs a
free list that does not exist yet.

`Database::rollback` also rebuilds the in-memory catalogue, because a table's
root page and rowid counter may have moved inside the transaction, and a table
created inside it must stop existing.

## fsync, not just fflush

`fflush` moves bytes from the process into the operating system, which is not
the same as moving them onto the disk. Commit calls `_commit` on Windows and
`fsync` elsewhere. Without that, a power cut loses committed transactions that
the process was told were durable -- and the crash test, which kills a process
but not the OS, would never notice the difference.

## One B+tree, two jobs, because the keys do the work

Phase 5 needed secondary indexes, and an index cannot be keyed by an int64: two
rows can share a value, so the key has to be (value, rowid), and values are not
integers. Rather than write a second tree, the B+tree was generalised to
byte-string keys ordered by plain `memcmp`, and the *encodings* were made to
carry the semantics.

- A table tree is keyed by `encode_rowid(rowid)`: eight big-endian bytes with
  the sign bit flipped, so integer order survives exactly.
- An index tree is keyed by `encode_value(v) + encode_rowid(rowid)`, so
  duplicates coexist and each value's rows form one contiguous, seekable run.

A leading tag byte separates the storage classes, which makes NULL < numbers <
text fall straight out of the byte comparison rather than needing a special
case. Text escapes its 0x00 bytes as `00 FF` and terminates with `00 00`, so a
value can contain nulls without colliding with the rowid that follows it.

Numbers encode through the standard IEEE-754 trick: flip every bit of a
negative, set the top bit of a positive, then store big-endian. Integers go
through `double`, so two integers more than 2^53 apart can collide -- which is
safe only because of the rule in the next section.

## An index is an access path, never an answer

Every row an index produces is still run through the `WHERE` clause. That looks
redundant, and it is what makes several other decisions safe: the lossy numeric
key encoding can only ever yield extra candidates, never lose one, and a
predicate the index only partially covers still gets fully enforced.

The test that matters here is differential: eighteen query shapes run against
two databases holding identical rows, one indexed and one not, and the answers
must match as multisets. Sorted comparison, because an index scan returns rows
in index order rather than rowid order and SQL promises no order without
`ORDER BY`.

## The planner probes instead of guessing

The textbook approach is fixed selectivity constants -- 5% for equality, 30% for
a range. Measured on the demo table, 5% predicted 30 matching rows where the
real answer was 4, which is the difference between choosing an index and
refusing it.

So the planner counts. `Index::count_equal` and `count_range` walk the index and
return the number of matches, giving up once they reach `rows / 4` -- the point
at which four-times-a-scan-step random fetches already exceed the cost of just
reading the table, so any larger answer leads to the same decision. The count is
therefore either exact or provably irrelevant, and the probe costs at most a
quarter of the scan it might avoid.

`EXPLAIN` distinguishes the two: `rows=` is a measured count, `rows>=` is a
floor from a probe that stopped early.

## Why the cost model has a random-fetch weight at all

Without one, any usable index always wins, which is wrong often enough to
matter. A scan walks a leaf chain that is largely resident in the buffer pool;
an index lookup descends the table tree once per row. Weighting an index fetch
at four scan steps is a rough number, but having *a* number is what lets the
planner reject an index on a column with four distinct values while accepting
one on a column with six hundred.

## Only top-level ANDs are usable

Conjuncts are collected by walking down through `AND` nodes. An `OR` stops the
walk: a row that fails one side may still qualify through the other, so no
single index range covers the predicate and the scan is the only correct path.
Two constraints on the same indexed column are folded into one range with both
ends, which is both cheaper and a tighter estimate.

## Statistics live in the catalogue

The planner needs a row count, and counting rows per query would defeat the
purpose. `Table` maintains `row_count_` incrementally and it is persisted as a
catalogue field, alongside the root page and the rowid counter. That is the
first genuine *statistic* in the system; histograms, which would let ranges be
estimated without probing, would go in the same place.
