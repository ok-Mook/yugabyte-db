CREATE TABLE test (h1 INT, h2 INT, a1 INT, v1 INT, PRIMARY KEY ((h1, h2) HASH, a1 ASC));
CREATE INDEX test_a1 ON test (a1 HASH);
INSERT INTO test (SELECT s, s, s, s FROM generate_series(1, 100000) s);

-- ANALYZE has not been called and actual number of rows in the table are not known.
SELECT reltuples FROM pg_class WHERE relname = 'test';

-- Filter on full key of unique index, expect 1 row.
EXPLAIN SELECT * FROM test WHERE h1 = 1 AND h2 = 1 AND a1 = 1;

-- Filter on full key of non-unique index, expect 10 rows.
EXPLAIN SELECT * FROM test WHERE a1 = 1;

-- Filter on only the hash columns in unique index, expect 100 rows
EXPLAIN SELECT * FROM test WHERE h1 = 1 AND h2 = 1;

-- Filter on only a partial key, expect sequential scan with 1000 rows
EXPLAIN SELECT * FROM test WHERE h1 = 1;

-- Filter on row with no index, expect sequential scan with 1000 rows
EXPLAIN SELECT * FROM test WHERE v1 = 1;

-- No flter, expect sequential scan with 1000 rows
EXPLAIN SELECT * FROM test;

-- ANALYZE produces a rough estimate of the number of rows. This can make
-- the test flaky, so to simulate ANALYZE we write to pg_class.reltuples manually.
SET yb_non_ddl_txn_for_sys_tables_allowed = on;
UPDATE pg_class SET reltuples=100000 WHERE relname='test';
UPDATE pg_yb_catalog_version SET current_version=current_version+1 WHERE db_oid=1;
-- Adding a sleep because changes to catalog need to be propogated asynchronously.
select pg_sleep(1);
SET yb_non_ddl_txn_for_sys_tables_allowed = off;
SELECT reltuples FROM pg_class where relname = 'test';

-- Filter on full key of unique index, expect 1 row.
EXPLAIN SELECT * FROM test WHERE h1 = 1 AND h2 = 1 AND a1 = 1;

-- Filter on full key of non-unique index, expect 1% rows ie. 1000 rows
EXPLAIN SELECT * FROM test WHERE a1 = 1;

-- Filter on only the hash columns in unique index, expect 10% rows ie. 10000 rows
EXPLAIN SELECT * FROM test WHERE h1 = 1 AND h2 = 1;

-- Filter on only a partial key, expect sequential scan with 100% rows
EXPLAIN SELECT * FROM test WHERE h1 = 1;

-- Filter on row with no index, expect sequential scan with 100% rows
EXPLAIN SELECT * FROM test WHERE v1 = 1;

-- No flter, expect sequential scan with 100% rows
EXPLAIN SELECT * FROM test;

DROP TABLE test cascade;

-- #13526 : Hard coded heuristic selectivity leads to poor query plan when
--          applied to actual statistics in the cost model.
CREATE TABLE t1 (h1 INT, a1 INT, v1 INT, PRIMARY KEY (h1 HASH, a1 ASC));
CREATE TABLE t2 (h1 INT, v1 INT, PRIMARY KEY (h1 HASH));
INSERT INTO t1 SELECT s, s, s FROM generate_series(1, 300000) s;
INSERT INTO t2 SELECT s, s FROM generate_series(1, 300000) s;

-- Without statistics, each table is assumed to have 1000 rows only.
-- Filter is propagated to t2.h1 = 123. h1 is the primary key in t2, and
-- the filter should produce 1 row.
EXPLAIN SELECT * FROM t1 LEFT JOIN t2 on t1.h1 = t2.h1 where t1.h1 = 123;

-- With statistics, before this change the filter on t2 was estimated to
-- produce 300 rows because hard coded selectvity of 0.001 was applied to
-- 300000 rows. After the fix, the index scan on t2 should produce 1 row.
-- ANALYZE produces a rough estimate of the number of rows. This can make
-- the test flaky, so to simulate ANALYZE we write to pg_class.reltuples manually.
SET yb_non_ddl_txn_for_sys_tables_allowed = on;
UPDATE pg_class SET reltuples=300000 WHERE relname='t1';
UPDATE pg_class SET reltuples=300000 WHERE relname='t2';
UPDATE pg_yb_catalog_version SET current_version=current_version+1 WHERE db_oid=1;
-- Adding a sleep because changes to catalog need to be propogated asynchronously.
select pg_sleep(1);
SET yb_non_ddl_txn_for_sys_tables_allowed = off;

EXPLAIN SELECT * FROM t1 LEFT JOIN t2 on t1.h1 = t2.h1 where t1.h1 = 123;

DROP TABLE t1, t2 cascade;
