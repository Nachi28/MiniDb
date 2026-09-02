#!/usr/bin/env python3
"""Differential test: run identical SQL through MiniDB and SQLite, diff results.

SQLite is the oracle. Python ships it in the standard library, so this needs no
subprocess for the reference side and no dependencies at all.

    python tools/difftest.py --queries 300 [--seed N] [--minidb PATH]
"""
import argparse, os, random, sqlite3, subprocess, sys, tempfile

SCHEMA = "CREATE TABLE t (n INTEGER, word TEXT, ratio REAL);"
INDEXES = ["CREATE INDEX ix_n ON t (n);", "CREATE INDEX ix_word ON t (word);"]
WORDS = ["alpha", "beta", "gamma", "delta", "epsilon"]


def make_rows(rng, count):
    return [(rng.randrange(200), rng.choice(WORDS), round(rng.uniform(0, 100), 2))
            for _ in range(count)]


def insert_sql(rows):
    values = ",".join("(%d,'%s',%r)" % r for r in rows)
    return "INSERT INTO t VALUES %s;" % values


def random_query(rng):
    n, m = sorted((rng.randrange(200), rng.randrange(200)))
    word = rng.choice(WORDS)
    ratio = round(rng.uniform(0, 100), 2)
    where = rng.choice([
        "n = %d" % n, "n <> %d" % n, "n > %d" % n, "n <= %d" % n,
        "n >= %d AND n < %d" % (n, m + 1),
        "word = '%s'" % word, "word > '%s'" % word,
        "ratio > %r" % ratio, "ratio <= %r" % ratio,
        "n = %d OR word = '%s'" % (n, word),
        "n > %d AND word = '%s'" % (n, word),
        "NOT n = %d" % n, "n IS NULL", "word IS NOT NULL",
    ])
    return rng.choice([
        "SELECT n, word, ratio FROM t WHERE %s ORDER BY n, word, ratio;" % where,
        "SELECT COUNT(*) FROM t WHERE %s;" % where,
        "SELECT COUNT(n), SUM(n), MIN(ratio), MAX(word) FROM t WHERE %s;" % where,
        "SELECT word, COUNT(*) FROM t WHERE %s GROUP BY word ORDER BY word;" % where,
        "SELECT n FROM t WHERE %s ORDER BY n LIMIT 15;" % where,
        "SELECT word, SUM(n) AS total FROM t GROUP BY word HAVING SUM(n) > %d "
        "ORDER BY total DESC, word;" % n,
    ])


def sqlite_run(conn, query):
    try:
        return [list(row) for row in conn.execute(query).fetchall()]
    except sqlite3.Error:
        return None


def parse_table(text):
    """Pulls result sets out of MiniDB's output.

    Each SELECT ends with a "N row(s) in ..." trailer, which is the only
    unambiguous separator: the ASCII rules appear three times per table, and
    non-query statements print a bare message and no table at all.
    """
    results, cells = [], []
    for line in text.splitlines():
        if line.startswith("|"):
            cells.append([c.strip() for c in line.strip().strip("|").split("|")])
        elif line.endswith(" ms"):  # the timing trailer; "N row(s) inserted" has none
            results.append(cells[1:])   # drop the header row
            cells = []
    return results


def same(a, b):
    """MiniDB prints text; SQLite returns typed values. Compare permissively."""
    if len(a) != len(b):
        return False
    for got, want in zip(a, b):
        if want is None:
            if got != "NULL":
                return False
            continue
        try:
            if abs(float(got) - float(want)) > 1e-6:
                return False
        except (TypeError, ValueError):
            if got != str(want):
                return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", type=int, default=200)
    ap.add_argument("--rows", type=int, default=400)
    ap.add_argument("--seed", type=int, default=20260902)
    ap.add_argument("--minidb", default=os.path.join("build", "minidb"))
    args = ap.parse_args()

    exe = args.minidb if os.path.exists(args.minidb) else args.minidb + ".exe"
    if not os.path.exists(exe):
        sys.exit("difftest: cannot find minidb at %s" % args.minidb)

    rng = random.Random(args.seed)
    rows = make_rows(rng, args.rows)
    queries = [random_query(rng) for _ in range(args.queries)]

    conn = sqlite3.connect(":memory:")
    conn.execute(SCHEMA)
    conn.execute(insert_sql(rows))

    work = tempfile.mkdtemp()
    script = os.path.join(work, "d.sql")
    with open(script, "w") as f:
        f.write(SCHEMA + "\n" + insert_sql(rows) + "\n")
        f.write("\n".join(INDEXES) + "\n")
        f.write("\n".join(queries) + "\n")

    out = subprocess.run([exe, "--db", os.path.join(work, "d.db"), script],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit("difftest: minidb failed\n" + out.stdout + out.stderr)

    blocks = parse_table(out.stdout)
    if len(blocks) != len(queries):
        sys.exit("difftest: expected %d result sets, parsed %d"
                 % (len(queries), len(blocks)))

    failures = 0
    compared = 0
    for query, mine in zip(queries, blocks):
        theirs = sqlite_run(conn, query)
        if theirs is None:
            continue                      # SQLite rejected it; not our disagreement
        compared += len(theirs)
        ok = len(mine) == len(theirs) and all(same(a, b) for a, b in zip(mine, theirs))
        if not ok:
            failures += 1
            if failures <= 5:
                print("MISMATCH  %s\n  minidb: %s\n  sqlite: %s" % (query, mine[:3], theirs[:3]))

    print("%d queries, %d table rows, %d result rows compared, %d mismatches"
          % (len(queries), args.rows, compared, failures))
    if compared == 0:
        sys.exit("difftest: nothing was compared, the harness is broken")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
