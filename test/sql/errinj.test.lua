remote = require('net.box')
test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
box.sql.execute('pragma sql_default_engine=\''..engine..'\'')
errinj = box.error.injection
fiber = require('fiber')

-- gh-3924 Check that tuple_formats of ephemeral spaces are
-- reused.
box.sql.execute("CREATE TABLE t4 (id INTEGER PRIMARY KEY, a INTEGER);")
box.sql.execute("INSERT INTO t4 VALUES (1,1)")
box.sql.execute("INSERT INTO t4 VALUES (2,1)")
box.sql.execute("INSERT INTO t4 VALUES (3,2)")
errinj.set('ERRINJ_TUPLE_FORMAT_COUNT', 200)
errinj.set('ERRINJ_MEMTX_DELAY_GC', true)
for i = 1, 201 do box.sql.execute("SELECT DISTINCT a FROM t4") end
errinj.set('ERRINJ_MEMTX_DELAY_GC', false)
errinj.set('ERRINJ_TUPLE_FORMAT_COUNT', -1)
box.sql.execute('DROP TABLE t4')

box.sql.execute('create table test (id int primary key, a float, b text)')
box.schema.user.grant('guest','read,write,execute', 'universe')
cn = remote.connect(box.cfg.listen)
cn:ping()

-- gh-2601 iproto messages are corrupted
errinj = box.error.injection
fiber = require('fiber')
errinj.set("ERRINJ_WAL_DELAY", true)
insert_res = nil
select_res = nil
function execute_yield() insert_res = cn:execute("insert into test values (100, 1, '1')") end
function execute_notyield() select_res = cn:execute('select 1') end
f1 = fiber.create(execute_yield)
while f1:status() ~= 'suspended' do fiber.sleep(0) end
f2 = fiber.create(execute_notyield)
while f2:status() ~= 'dead' do fiber.sleep(0) end
errinj.set("ERRINJ_WAL_DELAY", false)
while f1:status() ~= 'dead' do fiber.sleep(0) end
insert_res
select_res

cn:close()
box.sql.execute('drop table test')

--
-- gh-3326: after the iproto start using new buffers rotation
-- policy, SQL responses could be corrupted, when DDL/DML is mixed
-- with DQL. Same as gh-3255.
--
box.sql.execute('CREATE TABLE test (id integer primary key)')
cn = remote.connect(box.cfg.listen)

ch = fiber.channel(200)
errinj.set("ERRINJ_IPROTO_TX_DELAY", true)
for i = 1, 100 do fiber.create(function() for j = 1, 10 do cn:execute('REPLACE INTO test VALUES (1)') end ch:put(true) end) end
for i = 1, 100 do fiber.create(function() for j = 1, 10 do cn.space.TEST:get{1} end ch:put(true) end) end
for i = 1, 200 do ch:get() end
errinj.set("ERRINJ_IPROTO_TX_DELAY", false)

box.sql.execute('DROP TABLE test')
box.schema.user.revoke('guest', 'read,write,execute', 'universe')

----
---- gh-3273: Move SQL TRIGGERs into server.
----
box.sql.execute("CREATE TABLE t1(id INTEGER PRIMARY KEY, a INTEGER);");
box.sql.execute("CREATE TABLE t2(id INTEGER PRIMARY KEY, a INTEGER);");
box.error.injection.set("ERRINJ_WAL_IO", true)
box.sql.execute("CREATE TRIGGER t1t INSERT ON t1 BEGIN INSERT INTO t2 VALUES (1, 1); END;")
box.sql.execute("CREATE INDEX t1a ON t1(a);")
box.error.injection.set("ERRINJ_WAL_IO", false)
box.sql.execute("CREATE TRIGGER t1t INSERT ON t1 BEGIN INSERT INTO t2 VALUES (1, 1); END;")
box.sql.execute("INSERT INTO t1 VALUES (3, 3);")
box.sql.execute("SELECT * from t1");
box.sql.execute("SELECT * from t2");
box.error.injection.set("ERRINJ_WAL_IO", true)
t = box.space._trigger:get('T1T')
t_new = t:totable()
t_new[3]['sql'] = 'CREATE TRIGGER t1t INSERT ON t1 BEGIN INSERT INTO t2 VALUES (2, 2); END;'
_ = box.space._trigger:replace(t, t_new)
box.error.injection.set("ERRINJ_WAL_IO", false)
_ = box.space._trigger:replace(t, t_new)
box.error.injection.set("ERRINJ_WAL_IO", true)
box.sql.execute("DROP TRIGGER t1t;")
box.error.injection.set("ERRINJ_WAL_IO", false)
box.sql.execute("DELETE FROM t1;")
box.sql.execute("DELETE FROM t2;")
box.sql.execute("INSERT INTO t1 VALUES (3, 3);")
box.sql.execute("SELECT * from t1");
box.sql.execute("SELECT * from t2");
box.sql.execute("DROP TABLE t1;")
box.sql.execute("DROP TABLE t2;")

-- Tests which are aimed at verifying work of commit/rollback
-- triggers on _fk_constraint space.
--
box.sql.execute("CREATE TABLE t3 (id FLOAT PRIMARY KEY, a INT REFERENCES t3, b INT UNIQUE);")
t = box.space._fk_constraint:select{}[1]:totable()
errinj = box.error.injection
errinj.set("ERRINJ_WAL_IO", true)
-- Make constraint reference B field instead of id.
t[9] = {2}
box.space._fk_constraint:replace(t)
errinj.set("ERRINJ_WAL_IO", false)
box.sql.execute("INSERT INTO t3 VALUES (1, 2, 2);")
errinj.set("ERRINJ_WAL_IO", true)
box.sql.execute("ALTER TABLE t3 ADD CONSTRAINT fk1 FOREIGN KEY (b) REFERENCES t3;")
errinj.set("ERRINJ_WAL_IO", false)
box.sql.execute("INSERT INTO t3 VALUES(1, 1, 3);")
box.sql.execute("DELETE FROM t3;")
box.snapshot()
box.sql.execute("ALTER TABLE t3 ADD CONSTRAINT fk1 FOREIGN KEY (b) REFERENCES t3;")
box.sql.execute("INSERT INTO t3 VALUES(1, 1, 3);")
errinj.set("ERRINJ_WAL_IO", true)
box.sql.execute("ALTER TABLE t3 DROP CONSTRAINT fk1;")
box.sql.execute("INSERT INTO t3 VALUES(1, 1, 3);")
errinj.set("ERRINJ_WAL_IO", false)
box.sql.execute("DROP TABLE t3;")

-- gh-3780: space without PK raises error if
-- it is used in SQL queries.
--
errinj = box.error.injection
fiber = require('fiber')
box.sql.execute("CREATE TABLE t (id INT PRIMARY KEY);")
box.sql.execute("INSERT INTO t VALUES (1);")
errinj.set("ERRINJ_WAL_DELAY", true)
-- DROP TABLE consists of several steps: firstly indexes
-- are deleted, then space itself. Lets make sure that if
-- first part of drop is successfully finished, but resulted
-- in yield, all operations on space will be blocked due to
-- absence of primary key.
--
function drop_table_yield() box.sql.execute("DROP TABLE t;") end
f = fiber.create(drop_table_yield)
box.sql.execute("SELECT * FROM t;")
box.sql.execute("INSERT INTO t VALUES (2);")
box.sql.execute("UPDATE t SET id = 2;")
-- Finish drop space.
errinj.set("ERRINJ_WAL_DELAY", false)

--
-- Tests which are aimed at verifying work of commit/rollback
-- triggers on _ck_constraint space.
--
errinj = box.error.injection
s = box.schema.space.create('test', {format = {{name = 'X', type = 'unsigned'}}})
pk = box.space.test:create_index('pk')

errinj.set("ERRINJ_WAL_IO", true)
_ = box.space._ck_constraint:insert({'CK_CONSTRAINT_01', s.id, 'X<5'})
errinj.set("ERRINJ_WAL_IO", false)
_ = box.space._ck_constraint:insert({'CK_CONSTRAINT_01', s.id, 'X<5'})
box.sql.execute("INSERT INTO \"test\" VALUES(5);")
errinj.set("ERRINJ_WAL_IO", true)
_ = box.space._ck_constraint:replace({'CK_CONSTRAINT_01', s.id, 'X<=5'})
errinj.set("ERRINJ_WAL_IO", false)
_ = box.space._ck_constraint:replace({'CK_CONSTRAINT_01', s.id, 'X<=5'})
box.sql.execute("INSERT INTO \"test\" VALUES(5);")
s:drop()
