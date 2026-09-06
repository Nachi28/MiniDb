-- Tables, rows, expressions, and how MiniDB treats NULL.
--   minidb --db play.db examples/01_basics.sql

CREATE TABLE players (id INTEGER, name TEXT, score REAL, team TEXT);

INSERT INTO players VALUES (1, 'ada',    90.5, 'red'),
                           (2, 'linus',  72.0, 'blue'),
                           (3, 'grace',  NULL, 'red'),
                           (4, 'alan',   88.25,'blue'),
                           (5, 'hopper', 75.0, NULL);

SELECT * FROM players;

-- Expressions and aliases. An unaliased column is named after its shape.
SELECT name, score * 2 AS doubled, score + 10 FROM players WHERE score > 80;

-- Filtering, sorting, limiting. NULLs sort first.
SELECT name, score FROM players ORDER BY score DESC LIMIT 3;
SELECT name, score FROM players ORDER BY score;

-- NULL is unknown, so comparing with it is never true. This returns nothing.
SELECT name FROM players WHERE score = NULL;

-- IS NULL is the null-safe test, and it finds grace.
SELECT name FROM players WHERE score IS NULL;

-- Three-valued logic: grace's unknown score can't satisfy either side.
SELECT name FROM players WHERE score > 80 AND team = 'red';
SELECT name FROM players WHERE NOT score > 80;

-- Text, concatenation, and the operators.
SELECT name || ' (' || team || ')' AS label FROM players WHERE team IS NOT NULL;
SELECT 7 / 2 AS int_div, 7.0 / 2 AS real_div, 7 % 2 AS modulo;

-- No FROM clause needed for constants.
SELECT 1 + 2 * 3, 'mini' || 'db', -5 < 0;
