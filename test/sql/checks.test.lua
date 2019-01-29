env = require('test_run')
test_run = env.new()
test_run:cmd("push filter ".."'\\.lua.*:[0-9]+: ' to '.lua...\"]:<line>: '")
engine = test_run:get_cfg('engine')
box.sql.execute('pragma sql_default_engine=\''..engine..'\'')

--
-- gh-3272: Move SQL CHECK into server
--

-- Legacy data in _space (insertion on bootrap) test.
opts = {checks = {{expr = 'X>5'}}}
format = {{name = 'X', type = 'unsigned'}}
t = {513, 1, 'test', 'memtx', 0, opts, format}
s = box.space._space:insert(t)
box.space.test:create_index('pk')

-- Invalid expression test.
box.space._ck_constraint:insert({'CK_CONSTRAINT_01', 513, 'X><5'})
-- Unexistent space test.
box.space._ck_constraint:insert({'CK_CONSTRAINT_01', 550, 'X<5'})
-- Field type test.
box.space._ck_constraint:insert({'CK_CONSTRAINT_01', 550, 666})

-- Check constraints LUA creation test.
box.space._ck_constraint:insert({'CK_CONSTRAINT_01', 513, 'X<5'})
box.space._ck_constraint:count({})

box.sql.execute("INSERT INTO \"test\" VALUES(5);")
box.space.test:insert({5})
box.space._ck_constraint:replace({'CK_CONSTRAINT_01', 513, 'X<=5'})
box.sql.execute("INSERT INTO \"test\" VALUES(5);")
box.sql.execute("INSERT INTO \"test\" VALUES(6);")
box.space.test:insert({6})
-- Can't drop table with check constraints.
box.space.test:delete({5})
box.space.test.index.pk:drop()
box.space._space:delete({513})
box.space._ck_constraint:delete({'CK_CONSTRAINT_01', 513})
box.space.test:drop()

-- Create table with checks in sql.
box.sql.execute("CREATE TABLE t1(x INTEGER CONSTRAINT ONE CHECK( x<5 ), y REAL CONSTRAINT TWO CHECK( y>x ), z INTEGER PRIMARY KEY);")
box.space._ck_constraint:count()
box.sql.execute("INSERT INTO t1 VALUES (7, 1, 1)")
box.space.T1:insert({7, 1, 1})
box.sql.execute("INSERT INTO t1 VALUES (2, 1, 1)")
box.space.T1:insert({2, 1, 1})
box.sql.execute("INSERT INTO t1 VALUES (2, 4, 1)")
box.space.T1:update({1}, {{'+', 1, 5}})
box.sql.execute("DROP TABLE t1")

--
-- gh-3611: Segfault on table creation with check referencing this table
--
box.sql.execute("CREATE TABLE w2 (s1 INT PRIMARY KEY, CHECK ((SELECT COUNT(*) FROM w2) = 0));")
box.sql.execute("DROP TABLE w2;")

--
-- gh-3653: Dissallow bindings for DDL
--
box.sql.execute("CREATE TABLE t5(x INT PRIMARY KEY, y INT, CHECK( x*y < ? ));")

test_run:cmd("clear filter")
