-- Transactions and durability.
--   minidb --db ledger.db examples/04_transactions.sql
--
-- Outside a transaction every statement commits on its own, so a crash loses at
-- most the statement that was running. Inside one, nothing is durable until
-- COMMIT, and ROLLBACK puts the database back exactly as it was.

CREATE TABLE ledger (entry TEXT, amount REAL);
INSERT INTO ledger VALUES ('opening', 100.0);

-- Everything inside this block is discarded.
BEGIN;
INSERT INTO ledger VALUES ('mistake', -999.0), ('another mistake', -999.0);
SELECT COUNT(*) AS during_transaction FROM ledger;
ROLLBACK;

SELECT COUNT(*) AS after_rollback, SUM(amount) AS balance FROM ledger;

-- This one sticks, and is fsynced before COMMIT returns.
BEGIN;
INSERT INTO ledger VALUES ('deposit', 50.0);
COMMIT;

-- A table created inside an aborted transaction stops existing.
BEGIN;
CREATE TABLE scratch (a INTEGER);
INSERT INTO scratch VALUES (1);
ROLLBACK;
-- `scratch` is gone. Querying it now would be an error, and in the
-- interactive shell `.tables` shows only `ledger`.

SELECT entry, amount FROM ledger ORDER BY amount DESC;
