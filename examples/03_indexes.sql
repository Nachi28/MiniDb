-- Indexes, and watching the planner decide whether to use one.
--   minidb --db plan.db examples/03_indexes.sql

CREATE TABLE events (id INTEGER, kind TEXT, score INTEGER);

-- 120 rows: every score distinct, only four distinct kinds. Small, but
-- enough for the planner to reach opposite conclusions about the two
-- columns.
INSERT INTO events VALUES (1,'view',1), (2,'buy',2), (3,'signup',3),
  (4,'click',4), (5,'view',5), (6,'buy',6), (7,'signup',7), (8,'click',8),
  (9,'view',9), (10,'buy',10), (11,'signup',11), (12,'click',12),
  (13,'view',13), (14,'buy',14), (15,'signup',15), (16,'click',16),
  (17,'view',17), (18,'buy',18), (19,'signup',19), (20,'click',20),
  (21,'view',21), (22,'buy',22), (23,'signup',23), (24,'click',24),
  (25,'view',25), (26,'buy',26), (27,'signup',27), (28,'click',28),
  (29,'view',29), (30,'buy',30), (31,'signup',31), (32,'click',32),
  (33,'view',33), (34,'buy',34), (35,'signup',35), (36,'click',36),
  (37,'view',37), (38,'buy',38), (39,'signup',39), (40,'click',40),
  (41,'view',41), (42,'buy',42), (43,'signup',43), (44,'click',44),
  (45,'view',45), (46,'buy',46), (47,'signup',47), (48,'click',48),
  (49,'view',49), (50,'buy',50), (51,'signup',51), (52,'click',52),
  (53,'view',53), (54,'buy',54), (55,'signup',55), (56,'click',56),
  (57,'view',57), (58,'buy',58), (59,'signup',59), (60,'click',60),
  (61,'view',61), (62,'buy',62), (63,'signup',63), (64,'click',64),
  (65,'view',65), (66,'buy',66), (67,'signup',67), (68,'click',68),
  (69,'view',69), (70,'buy',70), (71,'signup',71), (72,'click',72),
  (73,'view',73), (74,'buy',74), (75,'signup',75), (76,'click',76),
  (77,'view',77), (78,'buy',78), (79,'signup',79), (80,'click',80),
  (81,'view',81), (82,'buy',82), (83,'signup',83), (84,'click',84),
  (85,'view',85), (86,'buy',86), (87,'signup',87), (88,'click',88),
  (89,'view',89), (90,'buy',90), (91,'signup',91), (92,'click',92),
  (93,'view',93), (94,'buy',94), (95,'signup',95), (96,'click',96),
  (97,'view',97), (98,'buy',98), (99,'signup',99), (100,'click',100),
  (101,'view',101), (102,'buy',102), (103,'signup',103), (104,'click',104),
  (105,'view',105), (106,'buy',106), (107,'signup',107), (108,'click',108),
  (109,'view',109), (110,'buy',110), (111,'signup',111), (112,'click',112),
  (113,'view',113), (114,'buy',114), (115,'signup',115), (116,'click',116),
  (117,'view',117), (118,'buy',118), (119,'signup',119), (120,'click',120);

CREATE INDEX idx_score ON events (score);
CREATE INDEX idx_kind  ON events (kind);

-- One row out of 120 matches, so a seek is far cheaper than reading the table.
EXPLAIN SELECT * FROM events WHERE score = 42;
SELECT id, kind FROM events WHERE score = 42;

-- A range, with both bounds folded into a single index scan.
EXPLAIN SELECT * FROM events WHERE score >= 10 AND score < 30;

-- There IS an index on kind, and the planner refuses it: only four distinct
-- kinds exist, so a quarter of the table matches and reaching all of it
-- through the index would cost more than just reading the table.
EXPLAIN SELECT * FROM events WHERE kind = 'click';

-- Under an OR, a row failing one side may still qualify through the other,
-- so no single index range covers the predicate.
EXPLAIN SELECT * FROM events WHERE score = 10 OR kind = 'signup';

-- EXPLAIN describes the whole pipeline, not just the access path.
EXPLAIN SELECT kind, COUNT(*) AS n FROM events WHERE score >= 100
  GROUP BY kind HAVING COUNT(*) > 1 ORDER BY n DESC LIMIT 2;

-- (In the interactive shell, `.indexes` lists them.)

-- EXPLAIN predicts. EXPLAIN ANALYZE runs the query and reports what actually
-- happened next to what was predicted, including how many rows the filter threw
-- away and how the buffer pool behaved. Postgres has this; SQLite does not.
EXPLAIN ANALYZE SELECT * FROM events WHERE score = 42;
EXPLAIN ANALYZE SELECT kind, COUNT(*) FROM events WHERE score >= 100 GROUP BY kind;
