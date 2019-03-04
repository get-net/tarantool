#ifndef TARANTOOL_BOX_MERGER_H_INCLUDED
#define TARANTOOL_BOX_MERGER_H_INCLUDED
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
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdbool.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/* {{{ Structures */

struct tuple;
struct key_def;
struct tuple_format;
typedef struct tuple_format box_tuple_format_t;

struct merger_source;
struct merger_context;

struct merger_source_vtab {
	/**
	 * Free a merger source.
	 *
	 * Don't call it directly, use merger_source_unref()
	 * instead.
	 */
	void (*delete)(struct merger_source *base);
	/**
	 * Get a next tuple (refcounted) from a source.
	 *
	 * When format is NULL it means that it does not matter
	 * for a caller in which format a tuple will be.
	 *
	 * Return 0 when successfully fetched a tuple or NULL. In
	 * case of an error set a diag and return -1.
	 */
	int (*next)(struct merger_source *base, box_tuple_format_t *format,
		    struct tuple **out);
};

/**
 * Base (abstract) structure to represent a merger source.
 *
 * The structure does not hold any resources.
 */
struct merger_source {
	/* Source-specific methods. */
	struct merger_source_vtab *vtab;
	/* Reference counter. */
	int refs;
};

/**
 * Holds immutable parameters of a merger.
 */
struct merger_context {
	struct key_def *key_def;
	box_tuple_format_t *format;
	/* Reference counter. */
	int refs;
};

/* }}} */

/* {{{ Create, delete, ref, unref a source and a context */

/**
 * Increment a merger source reference counter.
 */
void
merger_source_ref(struct merger_source *source);

/**
 * Decrement a merger source reference counter. When it has
 * reached zero, free the source (call delete() virtual method).
 */
void
merger_source_unref(struct merger_source *source);

/**
 * Initialize a base merger source structure.
 */
void
merger_source_new(struct merger_source *source,
		  struct merger_source_vtab *vtab);

/**
 * Free a merger context.
 */
void
merger_context_delete(struct merger_context *ctx);

/**
 * Increment a merger context reference counter.
 */
void
merger_context_ref(struct merger_context *ctx);

/**
 * Decrement a merger context reference counter. When it has
 * reached zero, free the context.
 */
void
merger_context_unref(struct merger_context *ctx);

/**
 * Create a new merger context.
 *
 * A returned merger context is NOT reference counted.
 *
 * Return NULL and set a diag in case of an error.
 */
struct merger_context *
merger_context_new(const struct key_def *key_def);

/* }}} */

/* {{{ Merger */

/**
 * Create a new merger w/o sources.
 *
 * Return NULL and set a diag in case of an error.
 */
struct merger_source *
merger_new(struct merger_context *ctx);

/**
 * Set sources for a merger.
 *
 * Return 0 at success. Return -1 at an error and set a diag.
 */
int
merger_set_sources(struct merger_source *base, struct merger_source **sources,
		   uint32_t sources_count);

/**
 * Set reverse flag for a merger.
 */
void
merger_set_reverse(struct merger_source *base, bool reverse);

/* }}} */

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_BOX_MERGER_H_INCLUDED */
