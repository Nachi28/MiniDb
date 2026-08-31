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

-- ---------------------------------------------------------------- phase 5 --
-- An index is not automatically a win: reaching a row through one costs a
-- random fetch, while a scan walks the leaf chain sequentially. The planner
-- probes the index to learn how many rows actually match, then picks.
CREATE TABLE events (id INTEGER, kind TEXT, score INTEGER);
INSERT INTO events VALUES (1,'view',1),(2,'purchase',2),(3,'signup',3),(4,'click',4),
       (5,'view',5),(6,'purchase',6),(7,'signup',7),(8,'click',8),(9,'view',9),
       (10,'purchase',10),(11,'signup',11),(12,'click',12),(13,'view',13),(14,'purchase',14),
       (15,'signup',15),(16,'click',16),(17,'view',17),(18,'purchase',18),(19,'signup',19),
       (20,'click',20),(21,'view',21),(22,'purchase',22),(23,'signup',23),(24,'click',24),
       (25,'view',25),(26,'purchase',26),(27,'signup',27),(28,'click',28),(29,'view',29),
       (30,'purchase',30),(31,'signup',31),(32,'click',32),(33,'view',33),(34,'purchase',34),
       (35,'signup',35),(36,'click',36),(37,'view',37),(38,'purchase',38),(39,'signup',39),
       (40,'click',40),(41,'view',41),(42,'purchase',42),(43,'signup',43),(44,'click',44),
       (45,'view',45),(46,'purchase',46),(47,'signup',47),(48,'click',48),(49,'view',49),
       (50,'purchase',50),(51,'signup',51),(52,'click',52),(53,'view',53),(54,'purchase',54),
       (55,'signup',55),(56,'click',56),(57,'view',57),(58,'purchase',58),(59,'signup',59),
       (60,'click',60),(61,'view',61),(62,'purchase',62),(63,'signup',63),(64,'click',64),
       (65,'view',65),(66,'purchase',66),(67,'signup',67),(68,'click',68),(69,'view',69),
       (70,'purchase',70),(71,'signup',71),(72,'click',72),(73,'view',73),(74,'purchase',74),
       (75,'signup',75),(76,'click',76),(77,'view',77),(78,'purchase',78),(79,'signup',79),
       (80,'click',80),(81,'view',81),(82,'purchase',82),(83,'signup',83),(84,'click',84),
       (85,'view',85),(86,'purchase',86),(87,'signup',87),(88,'click',88),(89,'view',89),
       (90,'purchase',90),(91,'signup',91),(92,'click',92),(93,'view',93),(94,'purchase',94),
       (95,'signup',95),(96,'click',96),(97,'view',97),(98,'purchase',98),(99,'signup',99),
       (100,'click',100),(101,'view',101),(102,'purchase',102),(103,'signup',103),
       (104,'click',104),(105,'view',105),(106,'purchase',106),(107,'signup',107),
       (108,'click',108),(109,'view',109),(110,'purchase',110),(111,'signup',111),
       (112,'click',112),(113,'view',113),(114,'purchase',114),(115,'signup',115),
       (116,'click',116),(117,'view',117),(118,'purchase',118),(119,'signup',119),
       (120,'click',120),(121,'view',121),(122,'purchase',122),(123,'signup',123),
       (124,'click',124),(125,'view',125),(126,'purchase',126),(127,'signup',127),
       (128,'click',128),(129,'view',129),(130,'purchase',130),(131,'signup',131),
       (132,'click',132),(133,'view',133),(134,'purchase',134),(135,'signup',135),
       (136,'click',136),(137,'view',137),(138,'purchase',138),(139,'signup',139),
       (140,'click',140),(141,'view',141),(142,'purchase',142),(143,'signup',143),
       (144,'click',144),(145,'view',145),(146,'purchase',146),(147,'signup',147),
       (148,'click',148),(149,'view',149),(150,'purchase',0),(151,'signup',1),(152,'click',2),
       (153,'view',3),(154,'purchase',4),(155,'signup',5),(156,'click',6),(157,'view',7),
       (158,'purchase',8),(159,'signup',9),(160,'click',10),(161,'view',11),
       (162,'purchase',12),(163,'signup',13),(164,'click',14),(165,'view',15),
       (166,'purchase',16),(167,'signup',17),(168,'click',18),(169,'view',19),
       (170,'purchase',20),(171,'signup',21),(172,'click',22),(173,'view',23),
       (174,'purchase',24),(175,'signup',25),(176,'click',26),(177,'view',27),
       (178,'purchase',28),(179,'signup',29),(180,'click',30),(181,'view',31),
       (182,'purchase',32),(183,'signup',33),(184,'click',34),(185,'view',35),
       (186,'purchase',36),(187,'signup',37),(188,'click',38),(189,'view',39),
       (190,'purchase',40),(191,'signup',41),(192,'click',42),(193,'view',43),
       (194,'purchase',44),(195,'signup',45),(196,'click',46),(197,'view',47),
       (198,'purchase',48),(199,'signup',49),(200,'click',50),(201,'view',51),
       (202,'purchase',52),(203,'signup',53),(204,'click',54),(205,'view',55),
       (206,'purchase',56),(207,'signup',57),(208,'click',58),(209,'view',59),
       (210,'purchase',60),(211,'signup',61),(212,'click',62),(213,'view',63),
       (214,'purchase',64),(215,'signup',65),(216,'click',66),(217,'view',67),
       (218,'purchase',68),(219,'signup',69),(220,'click',70),(221,'view',71),
       (222,'purchase',72),(223,'signup',73),(224,'click',74),(225,'view',75),
       (226,'purchase',76),(227,'signup',77),(228,'click',78),(229,'view',79),
       (230,'purchase',80),(231,'signup',81),(232,'click',82),(233,'view',83),
       (234,'purchase',84),(235,'signup',85),(236,'click',86),(237,'view',87),
       (238,'purchase',88),(239,'signup',89),(240,'click',90),(241,'view',91),
       (242,'purchase',92),(243,'signup',93),(244,'click',94),(245,'view',95),
       (246,'purchase',96),(247,'signup',97),(248,'click',98),(249,'view',99),
       (250,'purchase',100),(251,'signup',101),(252,'click',102),(253,'view',103),
       (254,'purchase',104),(255,'signup',105),(256,'click',106),(257,'view',107),
       (258,'purchase',108),(259,'signup',109),(260,'click',110),(261,'view',111),
       (262,'purchase',112),(263,'signup',113),(264,'click',114),(265,'view',115),
       (266,'purchase',116),(267,'signup',117),(268,'click',118),(269,'view',119),
       (270,'purchase',120),(271,'signup',121),(272,'click',122),(273,'view',123),
       (274,'purchase',124),(275,'signup',125),(276,'click',126),(277,'view',127),
       (278,'purchase',128),(279,'signup',129),(280,'click',130),(281,'view',131),
       (282,'purchase',132),(283,'signup',133),(284,'click',134),(285,'view',135),
       (286,'purchase',136),(287,'signup',137),(288,'click',138),(289,'view',139),
       (290,'purchase',140),(291,'signup',141),(292,'click',142),(293,'view',143),
       (294,'purchase',144),(295,'signup',145),(296,'click',146),(297,'view',147),
       (298,'purchase',148),(299,'signup',149),(300,'click',0),(301,'view',1),
       (302,'purchase',2),(303,'signup',3),(304,'click',4),(305,'view',5),(306,'purchase',6),
       (307,'signup',7),(308,'click',8),(309,'view',9),(310,'purchase',10),(311,'signup',11),
       (312,'click',12),(313,'view',13),(314,'purchase',14),(315,'signup',15),(316,'click',16),
       (317,'view',17),(318,'purchase',18),(319,'signup',19),(320,'click',20),(321,'view',21),
       (322,'purchase',22),(323,'signup',23),(324,'click',24),(325,'view',25),
       (326,'purchase',26),(327,'signup',27),(328,'click',28),(329,'view',29),
       (330,'purchase',30),(331,'signup',31),(332,'click',32),(333,'view',33),
       (334,'purchase',34),(335,'signup',35),(336,'click',36),(337,'view',37),
       (338,'purchase',38),(339,'signup',39),(340,'click',40),(341,'view',41),
       (342,'purchase',42),(343,'signup',43),(344,'click',44),(345,'view',45),
       (346,'purchase',46),(347,'signup',47),(348,'click',48),(349,'view',49),
       (350,'purchase',50),(351,'signup',51),(352,'click',52),(353,'view',53),
       (354,'purchase',54),(355,'signup',55),(356,'click',56),(357,'view',57),
       (358,'purchase',58),(359,'signup',59),(360,'click',60),(361,'view',61),
       (362,'purchase',62),(363,'signup',63),(364,'click',64),(365,'view',65),
       (366,'purchase',66),(367,'signup',67),(368,'click',68),(369,'view',69),
       (370,'purchase',70),(371,'signup',71),(372,'click',72),(373,'view',73),
       (374,'purchase',74),(375,'signup',75),(376,'click',76),(377,'view',77),
       (378,'purchase',78),(379,'signup',79),(380,'click',80),(381,'view',81),
       (382,'purchase',82),(383,'signup',83),(384,'click',84),(385,'view',85),
       (386,'purchase',86),(387,'signup',87),(388,'click',88),(389,'view',89),
       (390,'purchase',90),(391,'signup',91),(392,'click',92),(393,'view',93),
       (394,'purchase',94),(395,'signup',95),(396,'click',96),(397,'view',97),
       (398,'purchase',98),(399,'signup',99),(400,'click',100),(401,'view',101),
       (402,'purchase',102),(403,'signup',103),(404,'click',104),(405,'view',105),
       (406,'purchase',106),(407,'signup',107),(408,'click',108),(409,'view',109),
       (410,'purchase',110),(411,'signup',111),(412,'click',112),(413,'view',113),
       (414,'purchase',114),(415,'signup',115),(416,'click',116),(417,'view',117),
       (418,'purchase',118),(419,'signup',119),(420,'click',120),(421,'view',121),
       (422,'purchase',122),(423,'signup',123),(424,'click',124),(425,'view',125),
       (426,'purchase',126),(427,'signup',127),(428,'click',128),(429,'view',129),
       (430,'purchase',130),(431,'signup',131),(432,'click',132),(433,'view',133),
       (434,'purchase',134),(435,'signup',135),(436,'click',136),(437,'view',137),
       (438,'purchase',138),(439,'signup',139),(440,'click',140),(441,'view',141),
       (442,'purchase',142),(443,'signup',143),(444,'click',144),(445,'view',145),
       (446,'purchase',146),(447,'signup',147),(448,'click',148),(449,'view',149),
       (450,'purchase',0),(451,'signup',1),(452,'click',2),(453,'view',3),(454,'purchase',4),
       (455,'signup',5),(456,'click',6),(457,'view',7),(458,'purchase',8),(459,'signup',9),
       (460,'click',10),(461,'view',11),(462,'purchase',12),(463,'signup',13),(464,'click',14),
       (465,'view',15),(466,'purchase',16),(467,'signup',17),(468,'click',18),(469,'view',19),
       (470,'purchase',20),(471,'signup',21),(472,'click',22),(473,'view',23),
       (474,'purchase',24),(475,'signup',25),(476,'click',26),(477,'view',27),
       (478,'purchase',28),(479,'signup',29),(480,'click',30),(481,'view',31),
       (482,'purchase',32),(483,'signup',33),(484,'click',34),(485,'view',35),
       (486,'purchase',36),(487,'signup',37),(488,'click',38),(489,'view',39),
       (490,'purchase',40),(491,'signup',41),(492,'click',42),(493,'view',43),
       (494,'purchase',44),(495,'signup',45),(496,'click',46),(497,'view',47),
       (498,'purchase',48),(499,'signup',49),(500,'click',50),(501,'view',51),
       (502,'purchase',52),(503,'signup',53),(504,'click',54),(505,'view',55),
       (506,'purchase',56),(507,'signup',57),(508,'click',58),(509,'view',59),
       (510,'purchase',60),(511,'signup',61),(512,'click',62),(513,'view',63),
       (514,'purchase',64),(515,'signup',65),(516,'click',66),(517,'view',67),
       (518,'purchase',68),(519,'signup',69),(520,'click',70),(521,'view',71),
       (522,'purchase',72),(523,'signup',73),(524,'click',74),(525,'view',75),
       (526,'purchase',76),(527,'signup',77),(528,'click',78),(529,'view',79),
       (530,'purchase',80),(531,'signup',81),(532,'click',82),(533,'view',83),
       (534,'purchase',84),(535,'signup',85),(536,'click',86),(537,'view',87),
       (538,'purchase',88),(539,'signup',89),(540,'click',90),(541,'view',91),
       (542,'purchase',92),(543,'signup',93),(544,'click',94),(545,'view',95),
       (546,'purchase',96),(547,'signup',97),(548,'click',98),(549,'view',99),
       (550,'purchase',100),(551,'signup',101),(552,'click',102),(553,'view',103),
       (554,'purchase',104),(555,'signup',105),(556,'click',106),(557,'view',107),
       (558,'purchase',108),(559,'signup',109),(560,'click',110),(561,'view',111),
       (562,'purchase',112),(563,'signup',113),(564,'click',114),(565,'view',115),
       (566,'purchase',116),(567,'signup',117),(568,'click',118),(569,'view',119),
       (570,'purchase',120),(571,'signup',121),(572,'click',122),(573,'view',123),
       (574,'purchase',124),(575,'signup',125),(576,'click',126),(577,'view',127),
       (578,'purchase',128),(579,'signup',129),(580,'click',130),(581,'view',131),
       (582,'purchase',132),(583,'signup',133),(584,'click',134),(585,'view',135),
       (586,'purchase',136),(587,'signup',137),(588,'click',138),(589,'view',139),
       (590,'purchase',140),(591,'signup',141),(592,'click',142),(593,'view',143),
       (594,'purchase',144),(595,'signup',145),(596,'click',146),(597,'view',147),
       (598,'purchase',148),(599,'signup',149),(600,'click',0);

CREATE INDEX idx_score ON events (score);
CREATE INDEX idx_kind ON events (kind);

-- 600 rows, 150 distinct scores: four rows match, so a seek is far cheaper.
EXPLAIN SELECT * FROM events WHERE score = 40;
SELECT id, kind FROM events WHERE score = 40;

-- A narrow range, with both ends folded into one index scan.
EXPLAIN SELECT * FROM events WHERE score >= 20 AND score < 25;

-- Only four distinct kinds, so a quarter of the table matches. The index
-- would cost more than reading the table straight through, and is rejected
-- even though it exists.
EXPLAIN SELECT * FROM events WHERE kind = 'click';

-- Under an OR no single index range covers the predicate.
EXPLAIN SELECT * FROM events WHERE score = 10 OR kind = 'signup';

-- The plan describes the whole pipeline, not just the access path.
EXPLAIN SELECT kind, COUNT(*) AS n FROM events WHERE score >= 100
  GROUP BY kind HAVING COUNT(*) > 1 ORDER BY n DESC LIMIT 2;
