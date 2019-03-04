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

#include "box/merger.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define HEAP_FORWARD_DECLARATION
#include "salad/heap.h"

#include "trivia/util.h"      /* unlikely() */
#include "diag.h"             /* diag_set() */
#include "say.h"              /* panic() */
#include "box/tuple.h"        /* box_tuple_unref() */
#include "box/tuple_format.h" /* box_tuple_format_*(),
				 tuple_format_id() */
#include "box/key_def.h"      /* box_key_def_*(),
				 box_tuple_compare() */

/* {{{ Create, delete, ref, unref a source and a context */

enum { MERGER_SOURCE_REF_MAX = INT_MAX };
enum { MERGER_CONTEXT_REF_MAX = INT_MAX };

void
merger_source_ref(struct merger_source *source)
{
	if (unlikely(source->refs >= MERGER_SOURCE_REF_MAX))
		panic("Merger source reference counter overflow");
	++source->refs;
}

void
merger_source_unref(struct merger_source *source)
{
	assert(source->refs - 1 >= 0);
	if (--source->refs == 0)
		source->vtab->delete(source);
}

void
merger_source_new(struct merger_source *source, struct merger_source_vtab *vtab)
{
	source->vtab = vtab;
	source->refs = 0;
}

void
merger_context_delete(struct merger_context *ctx)
{
	box_key_def_delete(ctx->key_def);
	box_tuple_format_unref(ctx->format);
	free(ctx);
}

void
merger_context_ref(struct merger_context *ctx)
{
	if (unlikely(ctx->refs >= MERGER_CONTEXT_REF_MAX))
		panic("Merger context reference counter overflow");
	++ctx->refs;
}

void
merger_context_unref(struct merger_context *ctx)
{
	assert(ctx->refs - 1 >= 0);
	if (--ctx->refs == 0)
		merger_context_delete(ctx);
}

struct merger_context *
merger_context_new(const struct key_def *key_def)
{
	struct merger_context *ctx = (struct merger_context *) malloc(
		sizeof(struct merger_context));
	if (ctx == NULL) {
		diag_set(OutOfMemory, sizeof(struct merger_context), "malloc",
			 "merger_context");
		return NULL;
	}

	/*
	 * We need to copy key_def, because a key_def from the
	 * parameter can be collected before merger_context end
	 * of life (say, by LuaJIT GC if the key_def comes from
	 * Lua).
	 */
	ctx->key_def = key_def_dup(key_def);
	if (ctx->key_def == NULL) {
		free(ctx);
		return NULL;
	}

	ctx->format = box_tuple_format_new(&ctx->key_def, 1);
	if (ctx->format == NULL) {
		box_key_def_delete(ctx->key_def);
		free(ctx);
		return NULL;
	}

	ctx->refs = 0;

	return ctx;
}

/* }}} */

/* {{{ Merger */

/**
 * Holds a source to fetch next tuples and a last fetched tuple to
 * compare the node against other nodes.
 *
 * The main reason why this structure is separated from a merger
 * source is that a heap node can not be a member of several
 * heaps.
 *
 * The second reason is that it allows to incapsulate all heap
 * related logic inside this compilation unit, without any trails
 * in externally visible structures.
 */
struct merger_heap_node {
	/* A source of tuples. */
	struct merger_source *source;
	/*
	 * A last fetched (refcounted) tuple to compare against
	 * other nodes.
	 */
	struct tuple *tuple;
	/* An anchor to make the structure a merger heap node. */
	struct heap_node heap_node_anchor;
};

static bool
merger_source_less(const heap_t *heap, const struct merger_heap_node *left,
		   const struct merger_heap_node *right);
#define HEAP_NAME merger_heap
#define HEAP_LESS merger_source_less
#define heap_value_t struct merger_heap_node
#define heap_value_attr heap_node_anchor
#include "salad/heap.h"
#undef HEAP_NAME
#undef HEAP_LESS
#undef heap_value_t
#undef heap_value_attr

/**
 * Holds a heap, an immutable context, parameters of a merge
 * process and utility fields.
 */
struct merger {
	/* A merger is a source. */
	struct merger_source base;
	/*
	 * Whether a merge process started.
	 *
	 * The merger postpones charging of heap nodes until a
	 * first output tuple is acquired.
	 */
	bool started;
	/* A merger context. */
	struct merger_context *ctx;
	/*
	 * A heap of sources (of nodes that contains a source to
	 * be exact).
	 */
	heap_t heap;
	/* An array of heap nodes. */
	uint32_t nodes_count;
	struct merger_heap_node *nodes;
	/* Ascending (false) / descending (true) order. */
	bool reverse;
};

/* Helpers */

/**
 * Data comparing function to construct a heap of sources.
 */
static bool
merger_source_less(const heap_t *heap, const struct merger_heap_node *left,
		   const struct merger_heap_node *right)
{
	if (left->tuple == NULL && right->tuple == NULL)
		return false;
	if (left->tuple == NULL)
		return false;
	if (right->tuple == NULL)
		return true;
	struct merger *merger = container_of(heap, struct merger, heap);
	int cmp = box_tuple_compare(left->tuple, right->tuple,
				    merger->ctx->key_def);
	return merger->reverse ? cmp >= 0 : cmp < 0;
}

/**
 * How much more memory the heap will reserve at the next grow.
 *
 * See HEAP(reserve)() function in lib/salad/heap.h.
 */
static size_t
heap_next_grow_size(const heap_t *heap)
{
	heap_off_t heap_capacity_diff =	heap->capacity == 0 ?
		HEAP_INITIAL_CAPACITY : heap->capacity;
	return heap_capacity_diff * sizeof(struct heap_node *);
}

/**
 * Initialize a new merger heap node.
 */
static void
merger_heap_node_new(struct merger_heap_node *node,
		     struct merger_source *source)
{
	node->source = source;
	merger_source_ref(node->source);
	node->tuple = NULL;
	heap_node_create(&node->heap_node_anchor);
}

/**
 * Free a merger heap node.
 */
static void
merger_heap_node_delete(struct merger_heap_node *node)
{
	merger_source_unref(node->source);
	if (node->tuple != NULL)
		box_tuple_unref(node->tuple);
}

/**
 * The helper to add a new heap node to a merger heap.
 *
 * Return -1 at an error and set a diag.
 *
 * Otherwise store a next tuple in node->tuple, add the node to
 * merger->heap and return 0.
 */
static int
merger_add_heap_node(struct merger *merger, struct merger_heap_node *node)
{
	struct tuple *tuple = NULL;

	/* Acquire a next tuple. */
	struct merger_source *source = node->source;
	if (source->vtab->next(source, merger->ctx->format, &tuple) != 0)
		return -1;

	/* Don't add an empty source to a heap. */
	if (tuple == NULL)
		return 0;

	node->tuple = tuple;

	/* Add a node to a heap. */
	if (merger_heap_insert(&merger->heap, node)) {
		diag_set(OutOfMemory, heap_next_grow_size(&merger->heap),
			 "malloc", "merger->heap");
		return -1;
	}

	return 0;
}

/* Virtual methods declarations */

static void
merger_delete(struct merger_source *base);
static int
merger_next(struct merger_source *base, box_tuple_format_t *format,
	    struct tuple **out);

/* Non-virtual methods */

struct merger_source *
merger_new(struct merger_context *ctx)
{
	static struct merger_source_vtab merger_vtab = {
		.delete = merger_delete,
		.next = merger_next,
	};

	struct merger *merger = (struct merger *) malloc(sizeof(struct merger));
	if (merger == NULL) {
		diag_set(OutOfMemory, sizeof(struct merger), "malloc",
			 "merger");
		return NULL;
	}

	merger_source_new(&merger->base, &merger_vtab);

	merger->started = false;
	merger->ctx = ctx;
	merger_context_ref(merger->ctx);
	merger_heap_create(&merger->heap);
	merger->nodes_count = 0;
	merger->nodes = NULL;
	merger->reverse = false;

	return &merger->base;
}

int
merger_set_sources(struct merger_source *base, struct merger_source **sources,
		   uint32_t sources_count)
{
	struct merger *merger = container_of(base, struct merger, base);

	/* Ensure we don't leak old nodes. */
	assert(merger->nodes_count == 0);
	assert(merger->nodes == NULL);

	const size_t nodes_size =
		sources_count * sizeof(struct merger_heap_node);
	struct merger_heap_node *nodes = (struct merger_heap_node *) malloc(
		nodes_size);
	if (nodes == NULL) {
		diag_set(OutOfMemory, nodes_size, "malloc",
			 "merger heap nodes");
		return -1;
	}

	for (uint32_t i = 0; i < sources_count; ++i)
		merger_heap_node_new(&nodes[i], sources[i]);

	merger->nodes_count = sources_count;
	merger->nodes = nodes;
	return 0;
}

void
merger_set_reverse(struct merger_source *base, bool reverse)
{
	struct merger *merger = container_of(base, struct merger, base);

	merger->reverse = reverse;
}

/* Virtual methods */

static void
merger_delete(struct merger_source *base)
{
	struct merger *merger = container_of(base, struct merger, base);

	merger_context_unref(merger->ctx);
	merger_heap_destroy(&merger->heap);

	for (uint32_t i = 0; i < merger->nodes_count; ++i)
		merger_heap_node_delete(&merger->nodes[i]);

	if (merger->nodes != NULL)
		free(merger->nodes);

	free(merger);
}

static int
merger_next(struct merger_source *base, box_tuple_format_t *format,
	    struct tuple **out)
{
	struct merger *merger = container_of(base, struct merger, base);

	/*
	 * Fetch a first tuple for each source and add all heap
	 * nodes to a merger heap.
	 */
	if (!merger->started) {
		for (uint32_t i = 0; i < merger->nodes_count; ++i) {
			struct merger_heap_node *node = &merger->nodes[i];
			if (merger_add_heap_node(merger, node) != 0)
				return -1;
		}
		merger->started = true;
	}

	/* Get a next tuple. */
	struct merger_heap_node *node = merger_heap_top(&merger->heap);
	if (node == NULL) {
		*out = NULL;
		return 0;
	}
	struct tuple *tuple = node->tuple;
	assert(tuple != NULL);

	/*
	 * The tuples are stored in merger->ctx->format for
	 * fast comparisons, but we should return tuples in a
	 * requested format.
	 */
	uint32_t id_stored = tuple_format_id(merger->ctx->format);
	assert(tuple->format_id == id_stored);
	if (format == NULL)
		format = merger->ctx->format;
	uint32_t id_requested = tuple_format_id(format);
	if (id_stored != id_requested) {
		const char *tuple_beg = tuple_data(tuple);
		const char *tuple_end = tuple_beg + tuple->bsize;
		tuple = box_tuple_new(format, tuple_beg, tuple_end);
		if (tuple == NULL)
			return -1;
		box_tuple_ref(tuple);
		/*
		 * The node->tuple pointer will be rewritten below
		 * and in this branch it will not be returned. So
		 * we unreference it.
		 */
		box_tuple_unref(node->tuple);
	}

	/*
	 * Note: An old node->tuple pointer will be written to
	 * *out as refcounted tuple or is already unreferenced
	 * above, so we don't unreference it here.
	 */
	struct merger_source *source = node->source;
	if (source->vtab->next(source, merger->ctx->format, &node->tuple) != 0)
		return -1;

	/* Update a heap. */
	if (node->tuple == NULL)
		merger_heap_delete(&merger->heap, node);
	else
		merger_heap_update(&merger->heap, node);

	*out = tuple;
	return 0;
}

/* }}} */
