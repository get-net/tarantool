test_run = require('test_run').new()
engine = test_run:get_cfg('engine')

--
-- gh-1260: Multikey indexes
--
s = box.schema.space.create('withdata', {engine = 'vinyl'})
pk = s:create_index('pk')
-- Vinyl's space can't be multikey (yet).
_ = s:create_index('idx', {parts = {{3, 'str', path = '[*].fname'}, {3, 'str', path = '[*].sname'}}})
s:drop()

s = box.schema.space.create('withdata', {engine = 'memtx'})
-- Primary index must be unique so it can't be multikey.
_ = s:create_index('idx', {parts = {{3, 'str', path = '[*].fname'}}})
pk = s:create_index('pk')
-- Only tree index type may be mutlikey.
_ = s:create_index('idx', {type = 'hash', unique = true, parts = {{3, 'str', path = '[*].fname'}}})
_ = s:create_index('idx', {type = 'bitset', unique = false, parts = {{3, 'str', path = '[*].fname'}}})
_ = s:create_index('idx', {type = 'rtree', unique = false, parts = {{3, 'array', path = '[*].fname'}}})
-- Test incompatible multikey index parts.
_ = s:create_index('idx3', {parts = {{3, 'str', path = '[*].fname'}, {3, 'str', path = '["data"][*].sname'}}})
_ = s:create_index('idx2', {parts = {{3, 'str', path = '[*].fname'}, {3, 'str', path = '[*].sname[*].a'}}})
idx0 = s:create_index('idx0', {parts = {{3, 'str', path = '[1].fname'}}})
_ = s:create_index('idx', {parts = {{3, 'str', path = '[*].fname'}, {3, 'str', path = '[*].sname'}}})
idx0:drop()
-- Unique multikey index.
idx = s:create_index('idx', {unique = true, parts = {{3, 'str', path = '[*].fname'}, {3, 'str', path = '[*].sname'}}})
_ = s:create_index('idx2', {parts = {{3, 'str', path = '[1].fname'}, {3, 'str', path = '[1].sname'}}})
s:insert({1, {1, 2, 3}, {{fname='James', sname='Bond'}, {fname='Vasya', sname='Pupkin'}}})
s:insert({2, {3, 4, 5}, {{fname='Ivan', sname='Ivanych'}}})
_ = s:create_index('arr_idx', {unique = true, parts = {{2, 'unsigned', path = '[*]'}}})
-- Non-unique multikey index; two multikey indexes per space.
arr_idx = s:create_index('arr_idx', {unique = false, parts = {{2, 'unsigned', path = '[*]'}}})
arr_idx:select()
idx:get({'James', 'Bond'})
idx:get({'Ivan', 'Ivanych'})
idx:get({'Vasya', 'Pupkin'})
idx:select()
s:insert({3, {1, 2}, {{fname='Vasya', sname='Pupkin'}}})
s:insert({4, {1}, {{fname='James', sname='Bond'}}})
idx:select()
-- Duplicates in multikey parts.
s:insert({5, {1, 1, 1}, {{fname='A', sname='B'}, {fname='C', sname='D'}, {fname='A', sname='B'}}})
arr_idx:select({1})
s:delete(5)
-- Check that there is no garbage in index
arr_idx:select({1})
idx:get({'A', 'B'})
idx:get({'C', 'D'})
idx:delete({'Vasya', 'Pupkin'})
s:insert({6, {1, 2}, {{fname='Vasya', sname='Pupkin'}}})
s:insert({7, {1}, {{fname='James', sname='Bond'}}})
arr_idx:select({1})
idx:select()
-- Snapshot & recovery.
box.snapshot()
test_run:cmd("restart server default")
s = box.space["withdata"]
idx = s.index["idx"]
arr_idx = s.index["arr_idx"]
s:select()
idx:select()
arr_idx:select()
s:drop()

-- Assymetric multikey index paths.
s = box.schema.space.create('withdata')
pk = s:create_index('pk')
idx = s:create_index('idx', {parts = {{3, 'str', path = '[*].fname'}, {3, 'str', path = '[*].extra.sname', is_nullable = true}}})
s:insert({1, 1, {{fname='A1', extra={sname='A2'}}, {fname='B1'}, {fname='C1', extra={sname='C2'}}}})
s:drop()

-- Unique multikey index peculiar properties
s = box.schema.space.create('withdata')
pk = s:create_index('pk')
idx0 = s:create_index('idx0', {parts = {{2, 'int', path = '[*]'}}})
s:insert({1, {1, 1, 1}})
s:insert({2, {2, 2}})
s:insert({3, {3, 3, 2, 2, 1, 1}})
idx0:get(2)
idx0:get(1)
idx0:get(3)
idx0:select()
idx0:delete(2)
idx0:get(2)
idx0:select()
s:drop()
