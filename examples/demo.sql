-- MiniDB phase 1 demo. Run with:  minidb examples/demo.sql
CREATE TABLE players (
    id     INTEGER,
    name   TEXT,
    score  REAL
);

INSERT INTO players VALUES (1, 'ada',   90.5),
                           (2, 'linus', 72.0),
                           (3, 'grace', NULL),
                           (4, 'alan',  88.25);

-- Projection, computed columns and aliases.
SELECT name, score, score * 2 AS doubled FROM players ORDER BY score DESC;

-- NULL is not a value you can compare with '='; this returns nothing.
SELECT name FROM players WHERE score = NULL;

-- ...IS NULL is the null-safe test, and this finds grace.
SELECT name FROM players WHERE score IS NULL;

-- Three-valued logic: grace's NULL score makes the first conjunct unknown.
SELECT name FROM players WHERE score > 80 AND id < 4;

-- Constant expressions need no table at all.
SELECT 1 + 2 * 3, 7 / 2, 7.0 / 2, 'mini' || 'db';

-- ---------------------------------------------------------------- phase 2 --
CREATE TABLE sales (region TEXT, rep TEXT, amount REAL, units INTEGER);
INSERT INTO sales VALUES ('east',  'ada',    100.0, 3),
                         ('east',  'linus',   50.5, 1),
                         ('west',  'grace',  200.0, 7),
                         ('west',  'alan',   NULL,  2),
                         ('north', 'hopper',  75.0, NULL);

-- Aggregates over the whole table. COUNT(*) counts rows; everything else
-- skips NULLs, which is why COUNT(amount) is 4 rather than 5.
SELECT COUNT(*), COUNT(amount), SUM(units), AVG(amount), MIN(amount), MAX(amount)
FROM sales;

-- GROUP BY, with ORDER BY naming a select-list alias.
SELECT region, COUNT(*) AS deals, SUM(amount) AS total, AVG(units) AS avg_units
FROM sales
GROUP BY region
ORDER BY total DESC;

-- HAVING filters groups, after aggregation; WHERE filters rows, before it.
SELECT region, SUM(amount) AS total
FROM sales
GROUP BY region
HAVING SUM(amount) > 100
ORDER BY region;

-- Aggregates compose into ordinary expressions.
SELECT region, SUM(amount) / COUNT(*) AS per_deal
FROM sales GROUP BY region ORDER BY 2 DESC;

-- Scalar functions.
SELECT UPPER(rep), LENGTH(rep), ABS(-units), ROUND(amount, 1), COALESCE(amount, 0)
FROM sales WHERE region = 'west';

-- ---------------------------------------------------------------- phase 4 --
-- Outside a transaction each statement commits by itself. Inside one, nothing
-- is durable until COMMIT, and ROLLBACK puts the database back as it was.
CREATE TABLE ledger (entry TEXT, amount REAL);
INSERT INTO ledger VALUES ('opening', 100.0);

BEGIN;
INSERT INTO ledger VALUES ('mistake', -999.0), ('another mistake', -999.0);
SELECT COUNT(*) AS during_transaction FROM ledger;
ROLLBACK;

SELECT COUNT(*) AS after_rollback, SUM(amount) AS balance FROM ledger;

BEGIN;
INSERT INTO ledger VALUES ('deposit', 50.0);
COMMIT;

SELECT entry, amount FROM ledger ORDER BY amount DESC;
