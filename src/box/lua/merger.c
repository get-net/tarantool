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

#include "box/lua/merger.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>             /* lua_*() */
#include <lauxlib.h>         /* luaL_*() */

#include "fiber.h"           /* fiber() */
#include "diag.h"            /* diag_set() */

#include "box/tuple.h"       /* box_tuple_*() */

#include "lua/error.h"       /* luaT_error() */
#include "lua/utils.h"       /* luaL_pushcdata(),
				luaL_iterator_*() */

#include "box/lua/key_def.h" /* check_key_def() */
#include "box/lua/tuple.h"   /* luaT_tuple_new() */

#include "small/ibuf.h"      /* struct ibuf */
#include "msgpuck.h"         /* mp_*() */

#include "box/merger.h"      /* merger_*() */

static uint32_t merger_source_type_id = 0;
static uint32_t merger_context_type_id = 0;
static uint32_t ibuf_type_id = 0;

/**
 * A type of a function to create a source from a Lua iterator on
 * a Lua stack.
 *
 * Such function is to be passed to lbox_merger_source_new() as
 * a parameter.
 */
typedef struct merger_source *(*luaL_merger_source_new_f)(struct lua_State *L);

/* {{{ Helpers */

/**
 * Extract an ibuf object from the Lua stack.
 */
static struct ibuf *
luaT_check_ibuf(struct lua_State *L, int idx)
{
	if (lua_type(L, idx) != LUA_TCDATA)
		return NULL;

	uint32_t cdata_type;
	struct ibuf *ibuf_ptr = luaL_checkcdata(L, idx, &cdata_type);
	if (ibuf_ptr == NULL || cdata_type != ibuf_type_id)
		return NULL;
	return ibuf_ptr;
}

/**
 * Extract a merger source from the Lua stack.
 */
static struct merger_source *
luaT_check_merger_source(struct lua_State *L, int idx)
{
	uint32_t cdata_type;
	struct merger_source **source_ptr = luaL_checkcdata(L, idx,
							    &cdata_type);
	if (source_ptr == NULL || cdata_type != merger_source_type_id)
		return NULL;
	return *source_ptr;
}

/**
 * Extract a merger context from the Lua stack.
 */
static struct merger_context *
luaT_check_merger_context(struct lua_State *L, int idx)
{
	uint32_t cdata_type;
	struct merger_context **ctx_ptr = luaL_checkcdata(L, idx, &cdata_type);
	if (ctx_ptr == NULL || cdata_type != merger_context_type_id)
		return NULL;
	return *ctx_ptr;
}

/**
 * Skip an array around tuples and save its length.
 */
static int
decode_header(struct ibuf *buf, size_t *len_p)
{
	/* Check the buffer is correct. */
	if (buf->rpos > buf->wpos)
		return -1;

	/* Skip decoding if the buffer is empty. */
	if (ibuf_used(buf) == 0) {
		*len_p = 0;
		return 0;
	}

	/* Check and skip the array around tuples. */
	int ok = mp_typeof(*buf->rpos) == MP_ARRAY;
	if (ok)
		ok = mp_check_array(buf->rpos, buf->wpos) <= 0;
	if (ok)
		*len_p = mp_decode_array((const char **) &buf->rpos);
	return ok ? 0 : -1;
}

/**
 * Encode an array around tuples.
 */
static void
encode_header(struct ibuf *obuf, uint32_t result_len)
{
	ibuf_reserve(obuf, mp_sizeof_array(result_len));
	obuf->wpos = mp_encode_array(obuf->wpos, result_len);
}

/* }}} */

/* {{{ Create, delete structures from Lua */

/**
 * Free a merger source from a Lua code.
 */
static int
lbox_merger_source_gc(struct lua_State *L)
{
	struct merger_source *source;
	if ((source = luaT_check_merger_source(L, 1)) == NULL)
		return 0;
	merger_source_unref(source);
	return 0;
}

/**
 * Free a merger context from a Lua code.
 */
static int
lbox_merger_context_gc(struct lua_State *L)
{
	struct merger_context *ctx;
	if ((ctx = luaT_check_merger_context(L, 1)) == NULL)
		return 0;
	merger_context_unref(ctx);
	return 0;
}

/**
 * Create a new source from a Lua iterator and push it onto the
 * Lua stack.
 *
 * It is the helper for lbox_merger_new_buffer_source(),
 * lbox_merger_new_table_source() and
 * lbox_merger_new_tuple_source().
 */
static int
lbox_merger_source_new(struct lua_State *L, const char *func_name,
		       luaL_merger_source_new_f luaL_merger_source_new)
{
	int top = lua_gettop(L);
	if (top < 1 || top > 3 || !luaL_iscallable(L, 1))
		return luaL_error(L, "Usage: %s(gen, param, state)", func_name);

	/*
	 * luaL_merger_source_new() reads exactly three top
	 * values.
	 */
	while (lua_gettop(L) < 3)
		lua_pushnil(L);

	struct merger_source *source = luaL_merger_source_new(L);
	if (source == NULL)
		return luaT_error(L);
	merger_source_ref(source);
	*(struct merger_source **) luaL_pushcdata(L, merger_source_type_id) =
		source;
	lua_pushcfunction(L, lbox_merger_source_gc);
	luaL_setcdatagc(L, -2);

	return 1;
}

/**
 * Create a new merger context and push it to a Lua stack.
 *
 * Expect cdata<struct key_def> on a Lua stack.
 */
static int
lbox_merger_context_new(struct lua_State *L)
{
	struct key_def *key_def;
	if (lua_gettop(L) != 1 || (key_def = check_key_def(L, 1)) == NULL)
		return luaL_error(L, "Usage: merger.context.new(key_def)");

	struct merger_context *ctx = merger_context_new(key_def);
	if (ctx == NULL)
		return luaT_error(L);

	merger_context_ref(ctx);
	*(struct merger_context **) luaL_pushcdata(L, merger_context_type_id) =
		ctx;
	lua_pushcfunction(L, lbox_merger_context_gc);
	luaL_setcdatagc(L, -2);

	return 1;
}

/**
 * Raise a Lua error with merger.new() usage info.
 */
static int
lbox_merger_new_usage(struct lua_State *L, const char *param_name)
{
	static const char *usage = "merger.new(merger_context, "
				   "{source, source, ...}[, {"
				   "reverse = <boolean> or <nil>}])";
	if (param_name == NULL)
		return luaL_error(L, "Bad params, use: %s", usage);
	else
		return luaL_error(L, "Bad param \"%s\", use: %s", param_name,
				  usage);
}

/**
 * Parse a second parameter of merger.new() into an array of
 * sources.
 *
 * Return an array of pointers to sources and set @a
 * sources_count_ptr. In case of an error set a diag and return
 * NULL.
 *
 * It is the helper for lbox_merger_new().
 */
static struct merger_source **
luaT_merger_new_parse_sources(struct lua_State *L, int idx,
			      uint32_t *sources_count_ptr)
{
	/* Allocate sources array. */
	uint32_t sources_count = lua_objlen(L, idx);
	const ssize_t sources_size =
		sources_count * sizeof(struct merger_source *);
	struct merger_source **sources =
		(struct merger_source **) malloc(sources_size);
	if (sources == NULL) {
		diag_set(OutOfMemory, sources_size, "malloc", "sources");
		return NULL;
	}

	/* Save all sources. */
	for (uint32_t i = 0; i < sources_count; ++i) {
		lua_pushinteger(L, i + 1);
		lua_gettable(L, idx);

		/* Extract a source from a Lua stack. */
		struct merger_source *source = luaT_check_merger_source(L, -1);
		if (source == NULL) {
			free(sources);
			diag_set(IllegalParams,
				 "Unknown source type at index %d", i + 1);
			return NULL;
		}
		sources[i] = source;
	}
	lua_pop(L, sources_count);

	*sources_count_ptr = sources_count;
	return sources;
}

/**
 * Create a new merger and push it to a Lua stack as a merger
 * source.
 *
 * Expect cdata<struct context>, a table of sources and
 * (optionally) a table of options on a Lua stack.
 */
static int
lbox_merger_new(struct lua_State *L)
{
	struct merger_context *ctx;
	int top = lua_gettop(L);
	bool ok = (top == 2 || top == 3) &&
		/* Merger context. */
		(ctx = luaT_check_merger_context(L, 1)) != NULL &&
		/* Sources. */
		lua_istable(L, 2) == 1 &&
		/* Opts. */
		(lua_isnoneornil(L, 3) == 1 || lua_istable(L, 3) == 1);
	if (!ok)
		return lbox_merger_new_usage(L, NULL);

	/* Options. */
	bool reverse = false;

	/* Parse options. */
	if (!lua_isnoneornil(L, 3)) {
		/* Parse reverse. */
		lua_pushstring(L, "reverse");
		lua_gettable(L, 3);
		if (!lua_isnil(L, -1)) {
			if (lua_isboolean(L, -1))
				reverse = lua_toboolean(L, -1);
			else
				return lbox_merger_new_usage(L, "reverse");
		}
		lua_pop(L, 1);
	}

	struct merger_source *merger = merger_new(ctx);
	if (merger == NULL)
		return luaT_error(L);

	uint32_t sources_count = 0;
	struct merger_source **sources = luaT_merger_new_parse_sources(L, 2,
		&sources_count);
	if (sources == NULL) {
		merger->vtab->delete(merger);
		return luaT_error(L);
	}

	if (merger_set_sources(merger, sources, sources_count) != 0) {
		free(sources);
		merger->vtab->delete(merger);
		return luaT_error(L);
	}
	free(sources);
	merger_set_reverse(merger, reverse);

	merger_source_ref(merger);
	*(struct merger_source **)
		luaL_pushcdata(L, merger_source_type_id) = merger;
	lua_pushcfunction(L, lbox_merger_source_gc);
	luaL_setcdatagc(L, -2);

	return 1;
}

/* }}} */

/* {{{ Buffer merger source */

struct merger_source_buffer {
	struct merger_source base;
	/*
	 * A reference to a Lua iterator to fetch a next chunk of
	 * tuples.
	 */
	struct luaL_iterator *fetch_it;
	/*
	 * A reference a buffer with a current chunk of tuples.
	 * It is needed to prevent LuaJIT from collecting the
	 * buffer while the source consider it as the current
	 * one.
	 */
	int ref;
	/*
	 * A buffer with a current chunk of tuples.
	 */
	struct ibuf *buf;
	/*
	 * A merger stops before end of a buffer when it is not
	 * the last merger in the chain.
	 */
	size_t remaining_tuples_cnt;
};

/* Virtual methods declarations */

static void
luaL_merger_source_buffer_delete(struct merger_source *base);
static int
luaL_merger_source_buffer_next(struct merger_source *base,
			       box_tuple_format_t *format,
			       struct tuple **out);

/* Non-virtual methods */

/**
 * Create a new merger source of the buffer type.
 *
 * Reads gen, param, state from the top of a Lua stack.
 *
 * In case of an error it returns NULL and sets a diag.
 */
static struct merger_source *
luaL_merger_source_buffer_new(struct lua_State *L)
{
	static struct merger_source_vtab merger_source_buffer_vtab = {
		.delete = luaL_merger_source_buffer_delete,
		.next = luaL_merger_source_buffer_next,
	};

	struct merger_source_buffer *source = (struct merger_source_buffer *)
		malloc(sizeof(struct merger_source_buffer));
	if (source == NULL) {
		diag_set(OutOfMemory, sizeof(struct merger_source_buffer),
			 "malloc", "merger_source_buffer");
		return NULL;
	}

	merger_source_new(&source->base, &merger_source_buffer_vtab);

	source->fetch_it = luaL_iterator_new(L, 0);
	source->ref = 0;
	source->buf = NULL;
	source->remaining_tuples_cnt = 0;

	return &source->base;
}

/**
 * Call a user provided function to get a next data chunk (a
 * buffer).
 *
 * Return 1 when a new buffer is received, 0 when a buffers
 * iterator ends and -1 at error and set a diag.
 */
static int
luaL_merger_source_buffer_fetch(struct merger_source_buffer *source)
{
	struct lua_State *L = fiber()->storage.lua.stack;
	int nresult = luaL_iterator_next(L, source->fetch_it);

	/* Handle a Lua error in a gen function. */
	if (nresult == -1)
		return -1;

	/* No more data: do nothing. */
	if (nresult == 0)
		return 0;

	/* Handle incorrect results count. */
	if (nresult != 2) {
		diag_set(IllegalParams, "Expected <state>, <buffer>, got %d "
			 "return values", nresult);
		return -1;
	}

	/* Set a new buffer as the current chunk. */
	if (source->ref > 0)
		luaL_unref(L, LUA_REGISTRYINDEX, source->ref);
	lua_pushvalue(L, -nresult + 1); /* Popped by luaL_ref(). */
	source->ref = luaL_ref(L, LUA_REGISTRYINDEX);
	source->buf = luaT_check_ibuf(L, -1);
	assert(source->buf != NULL);
	lua_pop(L, nresult);

	/* Update remaining_tuples_cnt and skip the header. */
	if (decode_header(source->buf, &source->remaining_tuples_cnt) != 0) {
		diag_set(IllegalParams, "Invalid merger source %p",
			 &source->base);
		return -1;
	}
	return 1;
}

/* Virtual methods */

static void
luaL_merger_source_buffer_delete(struct merger_source *base)
{
	struct merger_source_buffer *source = container_of(base,
		struct merger_source_buffer, base);

	assert(source->fetch_it != NULL);
	luaL_iterator_delete(source->fetch_it);
	if (source->ref > 0)
		luaL_unref(tarantool_L, LUA_REGISTRYINDEX, source->ref);

	free(source);
}

static int
luaL_merger_source_buffer_next(struct merger_source *base,
			       box_tuple_format_t *format,
			       struct tuple **out)
{
	struct merger_source_buffer *source = container_of(base,
		struct merger_source_buffer, base);

	/*
	 * Handle the case when all data were processed: ask a
	 * next chunk until a non-empty chunk is received or a
	 * chunks iterator ends.
	 */
	while (source->remaining_tuples_cnt == 0) {
		int rc = luaL_merger_source_buffer_fetch(source);
		if (rc < 0)
			return -1;
		if (rc == 0) {
			*out = NULL;
			return 0;
		}
	}
	if (ibuf_used(source->buf) == 0) {
		diag_set(IllegalParams, "Unexpected msgpack buffer end");
		return -1;
	}
	const char *tuple_beg = source->buf->rpos;
	const char *tuple_end = tuple_beg;
	/*
	 * mp_next() is faster then mp_check(), but can	read bytes
	 * outside of the buffer and so can cause segmentation
	 * faults or an incorrect result.
	 *
	 * We check buffer boundaries after the mp_next() call and
	 * throw an error when the boundaries are violated, but it
	 * does not save us from possible segmentation faults.
	 *
	 * It is in a user responsibility to provide valid
	 * msgpack.
	 */
	mp_next(&tuple_end);
	--source->remaining_tuples_cnt;
	if (tuple_end > source->buf->wpos) {
		diag_set(IllegalParams, "Unexpected msgpack buffer end");
		return -1;
	}
	source->buf->rpos = (char *) tuple_end;
	if (format == NULL)
		format = box_tuple_format_default();
	struct tuple *tuple = box_tuple_new(format, tuple_beg, tuple_end);
	if (tuple == NULL)
		return -1;

	box_tuple_ref(tuple);
	*out = tuple;
	return 0;
}

/* Lua functions */

/**
 * Create a new buffer source and push it onto the Lua stack.
 */
static int
lbox_merger_new_buffer_source(struct lua_State *L)
{
	return lbox_merger_source_new(L, "merger.new_buffer_source",
				      luaL_merger_source_buffer_new);
}

/* }}} */

/* {{{ Table merger source */

struct merger_source_table {
	struct merger_source base;
	/*
	 * A reference to a Lua iterator to fetch a next chunk of
	 * tuples.
	 */
	struct luaL_iterator *fetch_it;
	/*
	 * A reference to a table with a current chunk of tuples.
	 */
	int ref;
	/* An index of current tuples within a current chunk. */
	int next_idx;
};

/* Virtual methods declarations */

static void
luaL_merger_source_table_delete(struct merger_source *base);
static int
luaL_merger_source_table_next(struct merger_source *base,
			      box_tuple_format_t *format,
			      struct tuple **out);

/* Non-virtual methods */

/**
 * Create a new merger source of the table type.
 *
 * In case of an error it returns NULL and set a diag.
 */
static struct merger_source *
luaL_merger_source_table_new(struct lua_State *L)
{
	static struct merger_source_vtab merger_source_table_vtab = {
		.delete = luaL_merger_source_table_delete,
		.next = luaL_merger_source_table_next,
	};

	struct merger_source_table *source = (struct merger_source_table *)
		malloc(sizeof(struct merger_source_table));
	if (source == NULL) {
		diag_set(OutOfMemory, sizeof(struct merger_source_table),
			 "malloc", "merger_source_table");
		return NULL;
	}

	merger_source_new(&source->base, &merger_source_table_vtab);

	source->fetch_it = luaL_iterator_new(L, 0);
	source->ref = 0;
	source->next_idx = 1;

	return &source->base;
}

/**
 * Call a user provided function to fill the source.
 *
 * Return 0 when a tables iterator ends, 1 when a new table is
 * received and -1 at an error (set a diag).
 */
static int
luaL_merger_source_table_fetch(struct merger_source_table *source)
{
	struct lua_State *L = fiber()->storage.lua.stack;
	int nresult = luaL_iterator_next(L, source->fetch_it);

	/* Handle a Lua error in a gen function. */
	if (nresult == -1)
		return -1;

	/* No more data: do nothing. */
	if (nresult == 0)
		return 0;

	/* Handle incorrect results count. */
	if (nresult != 2) {
		diag_set(IllegalParams, "Expected <state>, <table>, got %d "
			 "return values", nresult);
		return -1;
	}

	/* Set a new table as the current chunk. */
	if (source->ref > 0)
		luaL_unref(L, LUA_REGISTRYINDEX, source->ref);
	lua_pushvalue(L, -nresult + 1); /* Popped by luaL_ref(). */
	source->ref = luaL_ref(L, LUA_REGISTRYINDEX);
	source->next_idx = 1;
	lua_pop(L, nresult);

	return 1;
}

/* Virtual methods */

static void
luaL_merger_source_table_delete(struct merger_source *base)
{
	struct merger_source_table *source = container_of(base,
		struct merger_source_table, base);

	assert(source->fetch_it != NULL);
	luaL_iterator_delete(source->fetch_it);
	if (source->ref > 0)
		luaL_unref(tarantool_L, LUA_REGISTRYINDEX, source->ref);

	free(source);
}

static int
luaL_merger_source_table_next(struct merger_source *base,
			      box_tuple_format_t *format,
			      struct tuple **out)
{
	struct lua_State *L = fiber()->storage.lua.stack;
	struct merger_source_table *source = container_of(base,
		struct merger_source_table, base);

	if (source->ref > 0) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, source->ref);
		lua_pushinteger(L, source->next_idx);
		lua_gettable(L, -2);
	}
	/*
	 * If all data were processed, try to fetch more.
	 */
	while (source->ref == 0 || lua_isnil(L, -1)) {
		if (source->ref > 0)
			lua_pop(L, 2);
		int rc = luaL_merger_source_table_fetch(source);
		if (rc < 0)
			return -1;
		if (rc == 0) {
			*out = NULL;
			return 0;
		}
		/*
		 * Retry tuple extracting when a next table is
		 * received.
		 */
		lua_rawgeti(L, LUA_REGISTRYINDEX, source->ref);
		lua_pushinteger(L, source->next_idx);
		lua_gettable(L, -2);
	}
	if (format == NULL)
		format = box_tuple_format_default();
	struct tuple *tuple = luaT_tuple_new(L, -1, format);
	if (tuple == NULL)
		return -1;
	++source->next_idx;
	lua_pop(L, 2);

	box_tuple_ref(tuple);
	*out = tuple;
	return 0;
}

/* Lua functions */

/**
 * Create a new table source and push it onto the Lua stack.
 */
static int
lbox_merger_new_table_source(struct lua_State *L)
{
	return lbox_merger_source_new(L, "merger.new_table_source",
				      luaL_merger_source_table_new);
}

/* }}} */

/* {{{ Tuple merger source */

struct merger_source_tuple {
	struct merger_source base;
	/*
	 * A reference to a Lua iterator to fetch a next chunk of
	 * tuples.
	 */
	struct luaL_iterator *fetch_it;
};

/* Virtual methods declarations */

static void
luaL_merger_source_tuple_delete(struct merger_source *base);
static int
luaL_merger_source_tuple_next(struct merger_source *base,
			      box_tuple_format_t *format,
			      struct tuple **out);

/* Non-virtual methods */

/**
 * Create a new merger source of the tuple type.
 *
 * In case of an error it returns NULL and set a diag.
 */
static struct merger_source *
luaL_merger_source_tuple_new(struct lua_State *L)
{
	static struct merger_source_vtab merger_source_tuple_vtab = {
		.delete = luaL_merger_source_tuple_delete,
		.next = luaL_merger_source_tuple_next,
	};

	struct merger_source_tuple *source =
		(struct merger_source_tuple *) malloc(
		sizeof(struct merger_source_tuple));
	if (source == NULL) {
		diag_set(OutOfMemory, sizeof(struct merger_source_tuple),
			 "malloc", "merger_source_tuple");
		return NULL;
	}

	merger_source_new(&source->base, &merger_source_tuple_vtab);

	source->fetch_it = luaL_iterator_new(L, 0);

	return &source->base;
}

/**
 * Call a user provided function to fill the source.
 *
 * Return 1 at success and push a resulting tuple to a the Lua
 * stack.
 * Return 0 when no more data.
 * Return -1 at error (set a diag).
 */
static int
luaL_merger_source_tuple_fetch(struct merger_source_tuple *source,
			       struct lua_State *L)
{
	int nresult = luaL_iterator_next(L, source->fetch_it);

	/* Handle a Lua error in a gen function. */
	if (nresult == -1)
		return -1;

	/* No more data: do nothing. */
	if (nresult == 0)
		return 0;

	/* Handle incorrect results count. */
	if (nresult != 2) {
		diag_set(IllegalParams, "Expected <state>, <tuple> got %d "
			 "return values", nresult);
		return -1;
	}

	/* Set a new tuple as the current chunk. */
	lua_insert(L, -2); /* Swap state and tuple. */
	lua_pop(L, 1); /* Pop state. */

	return 1;
}

/* Virtual methods */

static void
luaL_merger_source_tuple_delete(struct merger_source *base)
{
	struct merger_source_tuple *source = container_of(base,
		struct merger_source_tuple, base);

	assert(source->fetch_it != NULL);
	luaL_iterator_delete(source->fetch_it);

	free(source);
}

static int
luaL_merger_source_tuple_next(struct merger_source *base,
				 box_tuple_format_t *format,
				 struct tuple **out)
{
	struct lua_State *L = fiber()->storage.lua.stack;
	struct merger_source_tuple *source = container_of(base,
		struct merger_source_tuple, base);

	int rc = luaL_merger_source_tuple_fetch(source, L);
	if (rc < 0)
		return -1;
	/*
	 * Check whether a tuple appears after the fetch.
	 */
	if (rc == 0) {
		*out = NULL;
		return 0;
	}

	if (format == NULL)
		format = box_tuple_format_default();
	struct tuple *tuple = luaT_tuple_new(L, -1, format);
	if (tuple == NULL)
		return -1;
	lua_pop(L, 1);

	box_tuple_ref(tuple);
	*out = tuple;
	return 0;
}

/* Lua functions */

/**
 * Create a new tuple source and push it onto the Lua stack.
 */
static int
lbox_merger_new_tuple_source(struct lua_State *L)
{
	return lbox_merger_source_new(L, "merger.new_tuple_source",
				      luaL_merger_source_tuple_new);
}

/* }}} */

/* {{{ Merger source Lua methods */

/**
 * Iterator gen function to traverse source results.
 *
 * Expected a nil as the first parameter (param) and a
 * merger_source as the second parameter (state) on a Lua stack.
 *
 * Push the original merger_source (as a new state) and a next
 * tuple.
 */
static int
lbox_merger_source_gen(struct lua_State *L)
{
	struct merger_source *source;
	bool ok = lua_gettop(L) == 2 && lua_isnil(L, 1) &&
		(source = luaT_check_merger_source(L, 2)) != NULL;
	if (!ok)
		return luaL_error(L, "Bad params, use: lbox_merger_source_gen("
				  "nil, merger_source)");

	struct tuple *tuple;
	if (source->vtab->next(source, NULL, &tuple) != 0)
		return luaT_error(L);
	if (tuple == NULL) {
		lua_pushnil(L);
		lua_pushnil(L);
		return 2;
	}

	/* Push merger_source, tuple. */
	*(struct merger_source **)
		luaL_pushcdata(L, merger_source_type_id) = source;
	luaT_pushtuple(L, tuple);

	/*
	 * luaT_pushtuple() references the tuple, so we
	 * unreference it on merger's side.
	 */
	box_tuple_unref(tuple);

	return 2;
}

/**
 * Iterate over merger source results from Lua.
 *
 * Push three values to the Lua stack:
 *
 * 1. gen (lbox_merger_source_gen wrapped by fun.wrap());
 * 2. param (nil);
 * 3. state (merger_source).
 */
static int
lbox_merger_source_ipairs(struct lua_State *L)
{
	struct merger_source *source;
	bool ok = lua_gettop(L) == 1 &&
		(source = luaT_check_merger_source(L, 1)) != NULL;
	if (!ok)
		return luaL_error(L, "Usage: merger_source:ipairs()");
	/* Stack: merger_source. */

	luaL_loadstring(L, "return require('fun').wrap");
	lua_call(L, 0, 1);
	lua_insert(L, -2); /* Swap merger_source and wrap. */
	/* Stack: wrap, merger_source. */

	lua_pushcfunction(L, lbox_merger_source_gen);
	lua_insert(L, -2); /* Swap merger_source and gen. */
	/* Stack: wrap, gen, merger_source. */

	/*
	 * Push nil as an iterator param, because all needed state
	 * is in a merger source.
	 */
	lua_pushnil(L);
	/* Stack: wrap, gen, merger_source, nil. */

	lua_insert(L, -2); /* Swap merger_source and nil. */
	/* Stack: wrap, gen, nil, merger_source. */

	/* Call fun.wrap(gen, nil, merger_source). */
	lua_call(L, 3, 3);
	return 3;
}

/**
 * Write source results into ibuf.
 *
 * It is the helper for lbox_merger_source_select().
 */
static int
encode_result_buffer(struct lua_State *L, struct merger_source *source,
		     struct ibuf *obuf, uint32_t limit)
{
	uint32_t result_len = 0;
	uint32_t result_len_offset = 4;

	/*
	 * Reserve maximum size for the array around resulting
	 * tuples to set it later.
	 */
	encode_header(obuf, UINT32_MAX);

	/* Fetch, merge and copy tuples to the buffer. */
	struct tuple *tuple;
	int rc = 0;
	while (result_len < limit && (rc =
	       source->vtab->next(source, NULL, &tuple)) == 0 &&
	       tuple != NULL) {
		uint32_t bsize = tuple->bsize;
		ibuf_reserve(obuf, bsize);
		memcpy(obuf->wpos, tuple_data(tuple), bsize);
		obuf->wpos += bsize;
		result_len_offset += bsize;
		++result_len;

		/* The received tuple is not more needed. */
		box_tuple_unref(tuple);
	}

	if (rc != 0)
		return luaT_error(L);

	/* Write the real array size. */
	mp_store_u32(obuf->wpos - result_len_offset, result_len);

	return 0;
}

/**
 * Write source results into a new Lua table.
 *
 * It is the helper for lbox_merger_source_select().
 */
static int
create_result_table(struct lua_State *L, struct merger_source *source,
		    uint32_t limit)
{
	/* Create result table. */
	lua_newtable(L);

	uint32_t cur = 1;

	/* Fetch, merge and save tuples to the table. */
	struct tuple *tuple;
	int rc = 0;
	while (cur - 1 < limit && (rc =
	       source->vtab->next(source, NULL, &tuple)) == 0 &&
	       tuple != NULL) {
		luaT_pushtuple(L, tuple);
		lua_rawseti(L, -2, cur);
		++cur;

		/*
		 * luaT_pushtuple() references the tuple, so we
		 * unreference it on merger's side.
		 */
		box_tuple_unref(tuple);
	}

	if (rc != 0)
		return luaT_error(L);

	return 1;
}

/**
 * Raise a Lua error with merger_inst:select() usage info.
 */
static int
lbox_merger_source_select_usage(struct lua_State *L, const char *param_name)
{
	static const char *usage = "merger_source:select([{"
				   "buffer = <cdata<struct ibuf>> or <nil>, "
				   "limit = <number> or <nil>}])";
	if (param_name == NULL)
		return luaL_error(L, "Bad params, use: %s", usage);
	else
		return luaL_error(L, "Bad param \"%s\", use: %s", param_name,
				  usage);
}

/**
 * Pull results of a merger source to a Lua stack.
 *
 * Write results into a buffer or a Lua table depending on
 * options.
 *
 * Expected a merger source and options (optional) on a Lua stack.
 *
 * Return a Lua table or nothing when a 'buffer' option is
 * provided.
 */
static int
lbox_merger_source_select(struct lua_State *L)
{
	struct merger_source *source;
	int top = lua_gettop(L);
	bool ok = (top == 1 || top == 2) &&
		/* Merger source. */
		(source = luaT_check_merger_source(L, 1)) != NULL &&
		/* Opts. */
		(lua_isnoneornil(L, 2) == 1 || lua_istable(L, 2) == 1);
	if (!ok)
		return lbox_merger_source_select_usage(L, NULL);

	uint32_t limit = 0xFFFFFFFF;
	struct ibuf *obuf = NULL;

	/* Parse options. */
	if (!lua_isnoneornil(L, 2)) {
		/* Parse buffer. */
		lua_pushstring(L, "buffer");
		lua_gettable(L, 2);
		if (!lua_isnil(L, -1)) {
			if ((obuf = luaT_check_ibuf(L, -1)) == NULL)
				return lbox_merger_source_select_usage(L,
					"buffer");
		}
		lua_pop(L, 1);

		/* Parse limit. */
		lua_pushstring(L, "limit");
		lua_gettable(L, 2);
		if (!lua_isnil(L, -1)) {
			if (lua_isnumber(L, -1))
				limit = lua_tointeger(L, -1);
			else
				return lbox_merger_source_select_usage(L,
					"limit");
		}
		lua_pop(L, 1);
	}

	if (obuf == NULL)
		return create_result_table(L, source, limit);
	else
		return encode_result_buffer(L, source, obuf, limit);
}

/* }}} */

/**
 * Register the module.
 */
LUA_API int
luaopen_merger(struct lua_State *L)
{
	luaL_cdef(L, "struct merger_source;");
	luaL_cdef(L, "struct merger_context;");
	luaL_cdef(L, "struct ibuf;");

	merger_source_type_id = luaL_ctypeid(L, "struct merger_source&");
	merger_context_type_id = luaL_ctypeid(L, "struct merger_context&");
	ibuf_type_id = luaL_ctypeid(L, "struct ibuf");

	/* Export C functions to Lua. */
	static const struct luaL_Reg meta[] = {
		{"new_buffer_source", lbox_merger_new_buffer_source},
		{"new_table_source", lbox_merger_new_table_source},
		{"new_tuple_source", lbox_merger_new_tuple_source},
		{"new", lbox_merger_new},
		{NULL, NULL}
	};
	luaL_register_module(L, "merger", meta);

	/* Add context.new(). */
	lua_newtable(L); /* merger.context */
	lua_pushcfunction(L, lbox_merger_context_new);
	lua_setfield(L, -2, "new");
	lua_setfield(L, -2, "context");

	/* Add internal.{select,ipairs}(). */
	lua_newtable(L); /* merger.internal */
	lua_pushcfunction(L, lbox_merger_source_select);
	lua_setfield(L, -2, "select");
	lua_pushcfunction(L, lbox_merger_source_ipairs);
	lua_setfield(L, -2, "ipairs");
	lua_setfield(L, -2, "internal");

	return 1;
}
