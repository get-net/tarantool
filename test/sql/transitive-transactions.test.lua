test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
box.execute("pragma sql_default_engine=\'"..engine.."\'")
test_run:cmd("setopt delimiter ';'")

-- These tests are aimed at checking transitive transactions
-- between SQL and Lua. In particular, make sure that deferred foreign keys
-- violations are passed correctly.
--

box.begin() box.execute('COMMIT');
box.begin() box.execute('ROLLBACK');
box.execute('START TRANSACTION;') box.commit();
box.execute('START TRANSACTION;') box.rollback();

box.execute('CREATE TABLE parent(id INT PRIMARY KEY, y INT UNIQUE);');
box.execute('CREATE TABLE child(id INT PRIMARY KEY, x INT REFERENCES parent(y) DEFERRABLE INITIALLY DEFERRED);');

fk_violation_1 = function()
    box.begin()
    box.execute('INSERT INTO child VALUES (1, 1);')
    box.execute('COMMIT;')
end;
fk_violation_1();
box.space.CHILD:select();

fk_violation_2 = function()
    box.execute('START TRANSACTION;')
    box.execute('INSERT INTO child VALUES (1, 1);')
    box.commit()
end;
fk_violation_2();
box.space.CHILD:select();

fk_violation_3 = function()
    box.begin()
    box.execute('INSERT INTO child VALUES (1, 1);')
    box.execute('INSERT INTO parent VALUES (1, 1);')
    box.commit()
end;
fk_violation_3();
box.space.CHILD:select();
box.space.PARENT:select();

-- Make sure that 'PRAGMA defer_foreign_keys' works.
--
box.execute('DROP TABLE child;')
box.execute('CREATE TABLE child(id INT PRIMARY KEY, x INT REFERENCES parent(y))')

fk_defer = function()
    box.begin()
    box.execute('INSERT INTO child VALUES (1, 2);')
    box.execute('INSERT INTO parent VALUES (2, 2);')
    box.commit()
end;
fk_defer();
box.space.CHILD:select();
box.space.PARENT:select();
box.execute('PRAGMA defer_foreign_keys = 1;')
box.rollback()
fk_defer();
box.space.CHILD:select();
box.space.PARENT:select();

box.execute('PRAGMA defer_foreign_keys = 0;')

-- Cleanup
box.execute('DROP TABLE child;');
box.execute('DROP TABLE parent;');
