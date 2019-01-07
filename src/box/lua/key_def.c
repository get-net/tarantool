/*
 * Copyright 2010-2019, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "box/coll_id_cache.h"
#include "box/lua/key_def.h"
#include "box/tuple.h"
#include "diag.h"
#include "fiber.h"
#include "lua/utils.h"
#include "tuple.h"

static uint32_t key_def_type_id = 0;

/**
 * Set key_part_def from a table on top of a Lua stack.
 *
 * When successful return 0, otherwise return -1 and set a diag.
 */
static int
luaT_key_def_set_part(struct lua_State *L, struct key_part_def *parts,
		      int part_idx, struct region *region)
{
	struct key_part_def *part = &parts[part_idx];
	*part = key_part_def_default;

	/* Set part->fieldno. */
	lua_pushstring(L, "fieldno");
	lua_gettable(L, -2);
	if (lua_isnil(L, -1)) {
		diag_set(IllegalParams, "fieldno must not be nil");
		return -1;
	}
	/*
	 * Transform one-based Lua fieldno to zero-based
	 * fieldno to use in key_def_new().
	 */
	part->fieldno = lua_tointeger(L, -1) - TUPLE_INDEX_BASE;
	lua_pop(L, 1);

	/* Set part->type. */
	lua_pushstring(L, "type");
	lua_gettable(L, -2);
	if (lua_isnil(L, -1)) {
		diag_set(IllegalParams, "type must not be nil");
		return -1;
	}
	size_t type_len;
	const char *type_name = lua_tolstring(L, -1, &type_len);
	lua_pop(L, 1);
	part->type = field_type_by_name(type_name, type_len);
	switch (part->type) {
	case FIELD_TYPE_ANY:
	case FIELD_TYPE_ARRAY:
	case FIELD_TYPE_MAP:
		/* Tuple comparators don't support these types. */
		diag_set(IllegalParams, "Unsupported field type: %s",
			 type_name);
		return -1;
	case field_type_MAX:
		diag_set(IllegalParams, "Unknown field type: %s", type_name);
		return -1;
	default:
		/* Pass though. */
		break;
	}

	/* Set part->is_nullable and part->nullable_action. */
	lua_pushstring(L, "is_nullable");
	lua_gettable(L, -2);
	if (!lua_isnil(L, -1) && lua_toboolean(L, -1) != 0) {
		part->is_nullable = true;
		part->nullable_action = ON_CONFLICT_ACTION_NONE;
	}
	lua_pop(L, 1);

	/*
	 * Set part->coll_id using collation_id.
	 *
	 * The value will be checked in key_def_new().
	 */
	lua_pushstring(L, "collation_id");
	lua_gettable(L, -2);
	if (!lua_isnil(L, -1))
		part->coll_id = lua_tointeger(L, -1);
	lua_pop(L, 1);

	/* Set part->coll_id using collation. */
	lua_pushstring(L, "collation");
	lua_gettable(L, -2);
	if (!lua_isnil(L, -1)) {
		/* Check for conflicting options. */
		if (part->coll_id != COLL_NONE) {
			diag_set(IllegalParams, "Conflicting options: "
				 "collation_id and collation");
			return -1;
		}

		size_t coll_name_len;
		const char *coll_name = lua_tolstring(L, -1, &coll_name_len);
		struct coll_id *coll_id = coll_by_name(coll_name,
						       coll_name_len);
		if (coll_id == NULL) {
			diag_set(IllegalParams, "Unknown collation: \"%s\"",
				 coll_name);
			return -1;
		}
		part->coll_id = coll_id->id;
	}
	lua_pop(L, 1);

	/* Set part->path (JSON path). */
	lua_pushstring(L, "path");
	lua_gettable(L, -2);
	if (!lua_isnil(L, -1)) {
		size_t path_len;
		const char *path = lua_tolstring(L, -1, &path_len);
		if (json_path_validate(path, path_len, TUPLE_INDEX_BASE) != 0) {
			diag_set(ClientError, ER_WRONG_INDEX_OPTIONS,
				 part_idx + TUPLE_INDEX_BASE, "invalid path");
			return -1;
		}
		char *tmp = region_alloc(region, path_len + 1);
		if (tmp == NULL) {
			diag_set(OutOfMemory, path_len + 1, "region", "path");
			return -1;
		}
		/*
		 * lua_tolstring() guarantees that a string have
		 * trailing '\0'.
		 */
		memcpy(tmp, path, path_len + 1);
		part->path = tmp;
	} else {
		part->path = NULL;
	}
	lua_pop(L, 1);
	return 0;
}

void
lbox_push_key_part(struct lua_State *L, const struct key_part *part)
{
	lua_newtable(L);

	lua_pushstring(L, field_type_strs[part->type]);
	lua_setfield(L, -2, "type");

	lua_pushnumber(L, part->fieldno + TUPLE_INDEX_BASE);
	lua_setfield(L, -2, "fieldno");

	if (part->path != NULL) {
		lua_pushlstring(L, part->path, part->path_len);
		lua_setfield(L, -2, "path");
	}

	lua_pushboolean(L, key_part_is_nullable(part));
	lua_setfield(L, -2, "is_nullable");

	if (part->coll_id != COLL_NONE) {
		struct coll_id *coll_id = coll_by_id(part->coll_id);
		assert(coll_id != NULL);
		lua_pushstring(L, coll_id->name);
		lua_setfield(L, -2, "collation");
	}
}

struct key_def *
check_key_def(struct lua_State *L, int idx)
{
	if (lua_type(L, idx) != LUA_TCDATA)
		return NULL;

	uint32_t cdata_type;
	struct key_def **key_def_ptr = luaL_checkcdata(L, idx, &cdata_type);
	if (key_def_ptr == NULL || cdata_type != key_def_type_id)
		return NULL;
	return *key_def_ptr;
}

/**
 * Free a key_def from a Lua code.
 */
static int
lbox_key_def_gc(struct lua_State *L)
{
	struct key_def *key_def = check_key_def(L, 1);
	if (key_def == NULL)
		return 0;
	box_key_def_delete(key_def);
	return 0;
}

/**
 * Validate a tuple from a given index on a Lua stack against
 * a key def and return the tuple.
 *
 * If a table is passed instead of a tuple, create a new tuple
 * from it.
 *
 * Return a refcounted tuple (either provided one or a new one).
 */
static struct tuple *
lbox_key_def_check_tuple(struct lua_State *L, struct key_def *key_def, int idx)
{
	struct tuple *tuple = luaT_istuple(L, idx);
	if (tuple == NULL)
		tuple = luaT_tuple_new(L, idx, box_tuple_format_default());
	if (tuple == NULL)
		return NULL;
	/* Check that tuple match with the key definition. */
	uint32_t min_field_count =
		tuple_format_min_field_count(&key_def, 1, NULL, 0);
	uint32_t field_count = tuple_field_count(tuple);
	if (field_count < min_field_count) {
		diag_set(ClientError, ER_NO_SUCH_FIELD_NO, field_count + 1);
		return NULL;
	}
	for (uint32_t idx = 0; idx < key_def->part_count; idx++) {
		struct key_part *part = &key_def->parts[idx];
		const char *field = tuple_field_by_part(tuple, part);
		if (field == NULL) {
			assert(key_def->has_optional_parts);
			continue;
		}
		if (key_part_validate(part->type, field, idx,
				      key_part_is_nullable(part)) != 0)
			return NULL;
	}
	tuple_ref(tuple);
	return tuple;
}

static int
lbox_key_def_extract_key(struct lua_State *L)
{
	struct key_def *key_def;
	if (lua_gettop(L) != 2 || (key_def = check_key_def(L, 1)) == NULL)
		return luaL_error(L, "Usage: key_def:extract_key(tuple)");

	struct tuple *tuple;
	if ((tuple = lbox_key_def_check_tuple(L, key_def, 2)) == NULL)
		return luaT_error(L);

	uint32_t key_size;
	char *key = tuple_extract_key(tuple, key_def, &key_size);
	tuple_unref(tuple);
	if (key == NULL)
		return luaT_error(L);

	struct tuple *ret =
		box_tuple_new(box_tuple_format_default(), key, key + key_size);
	if (ret == NULL)
		return luaT_error(L);
	luaT_pushtuple(L, ret);
	return 1;
}

static int
lbox_key_def_compare(struct lua_State *L)
{
	struct key_def *key_def;
	if (lua_gettop(L) != 3 || (key_def = check_key_def(L, 1)) == NULL) {
		return luaL_error(L, "Usage: key_def:"
				     "compare(tuple_a, tuple_b)");
	}

	struct tuple *tuple_a, *tuple_b;
	if ((tuple_a = lbox_key_def_check_tuple(L, key_def, 2)) == NULL)
		return luaT_error(L);
	if ((tuple_b = lbox_key_def_check_tuple(L, key_def, 3)) == NULL) {
		tuple_unref(tuple_a);
		return luaT_error(L);
	}

	int rc = tuple_compare(tuple_a, tuple_b, key_def);
	tuple_unref(tuple_a);
	tuple_unref(tuple_b);
	lua_pushinteger(L, rc);
	return 1;
}

static int
lbox_key_def_compare_with_key(struct lua_State *L)
{
	struct key_def *key_def;
	if (lua_gettop(L) != 3 || (key_def = check_key_def(L, 1)) == NULL) {
		return luaL_error(L, "Usage: key_def:"
				     "compare_with_key(tuple, key)");
	}

	struct tuple *tuple, *key_tuple = NULL;
	struct tuple_format *format = box_tuple_format_default();
	if ((tuple = lbox_key_def_check_tuple(L, key_def, 2)) == NULL)
		return luaT_error(L);
	if ((key_tuple = luaT_tuple_new(L, 3, format)) == NULL) {
		tuple_unref(tuple);
		return luaT_error(L);
	}
	tuple_ref(key_tuple);

	const char *key = tuple_data(key_tuple);
	assert(mp_typeof(*key) == MP_ARRAY);
	uint32_t part_count = mp_decode_array(&key);
	if (key_validate_parts(key_def, key, part_count, true) != 0) {
		tuple_unref(tuple);
		tuple_unref(key_tuple);
		return luaT_error(L);
	}

	int rc = tuple_compare_with_key(tuple, key, part_count, key_def);
	tuple_unref(tuple);
	tuple_unref(key_tuple);
	lua_pushinteger(L, rc);
	return 1;
}

static int
lbox_key_def_merge(struct lua_State *L)
{
	struct key_def *key_def_a, *key_def_b;
	if (lua_gettop(L) != 2 || (key_def_a = check_key_def(L, 1)) == NULL ||
	   (key_def_b = check_key_def(L, 2)) == NULL)
		return luaL_error(L, "Usage: key_def:merge(second_key_def)");

	struct key_def *new_key_def = key_def_merge(key_def_a, key_def_b);
	if (new_key_def == NULL)
		return luaT_error(L);

	*(struct key_def **) luaL_pushcdata(L, key_def_type_id) = new_key_def;
	lua_pushcfunction(L, lbox_key_def_gc);
	luaL_setcdatagc(L, -2);
	return 1;
}

static int
lbox_key_def_to_table(struct lua_State *L)
{
	struct key_def *key_def;
	if (lua_gettop(L) != 1 || (key_def = check_key_def(L, 1)) == NULL)
		return luaL_error(L, "Usage: key_def:totable()");

	lua_createtable(L, key_def->part_count, 0);
	for (uint32_t i = 0; i < key_def->part_count; ++i) {
		lbox_push_key_part(L, &key_def->parts[i]);
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

/**
 * Create a new key_def from a Lua table.
 *
 * Expected a table of key parts on the Lua stack. The format is
 * the same as box.space.<...>.index.<...>.parts or corresponding
 * net.box's one.
 *
 * Push the new key_def as cdata to a Lua stack.
 */
static int
lbox_key_def_new(struct lua_State *L)
{
	if (lua_gettop(L) != 1 || lua_istable(L, 1) != 1)
		return luaL_error(L, "Bad params, use: key_def.new({"
				  "{fieldno = fieldno, type = type"
				  "[, is_nullable = <boolean>]"
				  "[, path = <string>]"
				  "[, collation_id = <number>]"
				  "[, collation = <string>]}, ...}");

	uint32_t part_count = lua_objlen(L, 1);
	const ssize_t parts_size = sizeof(struct key_part_def) * part_count;

	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	struct key_part_def *parts = region_alloc(region, parts_size);
	if (parts == NULL) {
		diag_set(OutOfMemory, parts_size, "region", "parts");
		return luaT_error(L);
	}

	for (uint32_t i = 0; i < part_count; ++i) {
		lua_pushinteger(L, i + 1);
		lua_gettable(L, 1);
		if (luaT_key_def_set_part(L, parts, i, region) != 0) {
			region_truncate(region, region_svp);
			return luaT_error(L);
		}
	}

	struct key_def *key_def = key_def_new(parts, part_count);
	region_truncate(region, region_svp);
	if (key_def == NULL)
		return luaT_error(L);

	/*
	 * Calculate minimal field count of tuples with specified
	 * key and update key_def optionality to use correct
	 * compare/extract functions.
	 */
	uint32_t min_field_count =
		tuple_format_min_field_count(&key_def, 1, NULL, 0);
	key_def_update_optionality(key_def, min_field_count);

	*(struct key_def **) luaL_pushcdata(L, key_def_type_id) = key_def;
	lua_pushcfunction(L, lbox_key_def_gc);
	luaL_setcdatagc(L, -2);

	return 1;
}

LUA_API int
luaopen_key_def(struct lua_State *L)
{
	luaL_cdef(L, "struct key_def;");
	key_def_type_id = luaL_ctypeid(L, "struct key_def&");

	/* Export C functions to Lua. */
	static const struct luaL_Reg meta[] = {
		{"new", lbox_key_def_new},
		{NULL, NULL}
	};
	luaL_register_module(L, "key_def", meta);

	lua_newtable(L); /* key_def.internal */
	lua_pushcfunction(L, lbox_key_def_extract_key);
	lua_setfield(L, -2, "extract_key");
	lua_pushcfunction(L, lbox_key_def_compare);
	lua_setfield(L, -2, "compare");
	lua_pushcfunction(L, lbox_key_def_compare_with_key);
	lua_setfield(L, -2, "compare_with_key");
	lua_pushcfunction(L, lbox_key_def_merge);
	lua_setfield(L, -2, "merge");
	lua_pushcfunction(L, lbox_key_def_to_table);
	lua_setfield(L, -2, "totable");
	lua_setfield(L, -2, "internal");

	return 1;
}
