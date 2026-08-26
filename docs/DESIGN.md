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
