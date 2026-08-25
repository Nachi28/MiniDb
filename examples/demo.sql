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
