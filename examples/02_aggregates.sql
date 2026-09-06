-- Aggregates, grouping, and the built-in scalar functions.
--   minidb --db play.db examples/02_aggregates.sql
-- Run 01_basics.sql first, or against the same database file.

CREATE TABLE IF NOT EXISTS sales (region TEXT, rep TEXT, amount REAL, units INTEGER);
INSERT INTO sales VALUES ('east',  'ada',    100.0, 3),
                         ('east',  'linus',   50.5, 1),
                         ('west',  'grace',  200.0, 7),
                         ('west',  'alan',   NULL,  2),
                         ('north', 'hopper',  75.0, NULL);

-- COUNT(*) counts rows. Everything else skips NULLs, which is why
-- COUNT(amount) is 4 and not 5.
SELECT COUNT(*), COUNT(amount), SUM(units), AVG(amount), MIN(amount), MAX(amount)
FROM sales;

-- GROUP BY, with ORDER BY naming a select-list alias.
SELECT region, COUNT(*) AS deals, SUM(amount) AS total, AVG(units) AS avg_units
FROM sales GROUP BY region ORDER BY total DESC;

-- HAVING filters groups after aggregation; WHERE filters rows before it.
SELECT region, SUM(amount) AS total
FROM sales GROUP BY region HAVING SUM(amount) > 100 ORDER BY region;

-- Aggregates compose into ordinary expressions, and ORDER BY can name an
-- output position.
SELECT region, SUM(amount) / COUNT(*) AS per_deal
FROM sales GROUP BY region ORDER BY 2 DESC;

-- Grouping on a computed key.
SELECT units % 2 AS parity, COUNT(*) FROM sales GROUP BY units % 2;

-- Scalar functions. All of them propagate NULL.
SELECT UPPER(rep), LENGTH(rep), ABS(-units), ROUND(amount, 1), COALESCE(amount, 0)
FROM sales WHERE region = 'west';

SELECT IFNULL(amount, 0.0) AS safe_amount, LOWER(region) FROM sales ORDER BY rep;
