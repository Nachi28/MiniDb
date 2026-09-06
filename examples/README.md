# Examples

Run any of these against a fresh database file:

```bash
minidb --db play.db examples/01_basics.sql
```

| File | Covers |
|---|---|
| `01_basics.sql` | Tables, inserts, expressions, `WHERE`, `ORDER BY`, `LIMIT`, NULL semantics |
| `02_aggregates.sql` | `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`, `GROUP BY`, `HAVING`, scalar functions |
| `03_indexes.sql` | `CREATE INDEX` and `EXPLAIN` — including a query where the planner *rejects* an index |
| `04_transactions.sql` | `BEGIN` / `COMMIT` / `ROLLBACK`, and what a rollback undoes |

`01` and `02` share a database if you point them at the same file. `03` and `04`
are self-contained.

The one to read first is `03_indexes.sql` — it is the shortest demonstration of
the thing MiniDB does that a toy database usually doesn't.
