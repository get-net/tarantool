#ifndef INCLUDES_TARANTOOL_VCLOCK_H
#define INCLUDES_TARANTOOL_VCLOCK_H
/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
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
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <assert.h>

#define RB_COMPACT 1
#include <small/rb.h>

#include "bit/bit.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

enum {
	/**
	 * The maximum number of components in vclock, should be power of two.
	 */
	VCLOCK_MAX = 32,

	/**
	 * The maximum length of string representation of vclock.
	 *
	 * vclock formatted as {<pair>, ..., <pair>} where
	 *    <pair> is <replica_id>: <lsn>,
	 *    <replica_id> is 0..VCLOCK_MAX (2 chars),
	 *    <lsn> is int64_t (20 chars).
	 *
	 * @sa vclock_from_string()
	 * @sa vclock_to_string()
	 */
	VCLOCK_STR_LEN_MAX = 1 + VCLOCK_MAX * (2 + 2 + 20 + 2) + 1
};

/** Predefined replication group identifiers. */
enum {
	/**
	 * Default replication group: changes made to the space
	 * are replicated throughout the entire cluster.
	 */
	GROUP_DEFAULT = 0,
	/**
	 * Replica local space: changes made to the space are
	 * not replicated.
	 */
	GROUP_LOCAL = 1,
};

/** Cluster vector clock */
struct vclock {
	/** Map of used components in lsn array */
	unsigned int map;
	/** Sum of all components of vclock. */
	int64_t signature;
	int64_t lsn[VCLOCK_MAX];
	/** To order binary logs by vector clock. */
	rb_node(struct vclock) link;
};

/* Replica id, coordinate */
struct vclock_c {
	uint32_t id;
	int64_t lsn;
};

struct vclock_iterator
{
	struct bit_iterator it;
	const struct vclock *vclock;
};

static inline void
vclock_iterator_init(struct vclock_iterator *it, const struct vclock *vclock)
{
	it->vclock = vclock;
	bit_iterator_init(&it->it, &vclock->map, sizeof(vclock->map), true);
}

static inline struct vclock_c
vclock_iterator_next(struct vclock_iterator *it)
{
	struct vclock_c c = { 0, 0 };
	size_t id = bit_iterator_next(&it->it);
	c.id = id == SIZE_MAX ? (int) VCLOCK_MAX : id;
	if (c.id < VCLOCK_MAX)
		c.lsn = it->vclock->lsn[c.id];
	return c;
}


#define vclock_foreach(it, var) \
	for (struct vclock_c var = vclock_iterator_next(it); \
	     (var).id < VCLOCK_MAX; (var) = vclock_iterator_next(it))

static inline void
vclock_create(struct vclock *vclock)
{
	vclock->signature = 0;
	vclock->map = 0;
	vclock->lsn[0] = 0;
}

/**
 * Reset a vclock object. After this function is called,
 * vclock_is_set() will return false.
 */
static inline void
vclock_clear(struct vclock *vclock)
{
	vclock->signature = -1;
	vclock->map = 0;
	vclock->lsn[0] = 0;
}

/**
 * Returns false if the vclock was cleared with vclock_clear(),
 * true otherwise.
 */
static inline bool
vclock_is_set(const struct vclock *vclock)
{
	return vclock->signature >= 0;
}

static inline int64_t
vclock_get(const struct vclock *vclock, uint32_t replica_id)
{
	/*
	 * If replica_id does not fit in VCLOCK_MAX - 1 then
	 * let result be undefined.
	 */
	replica_id &= VCLOCK_MAX - 1;
	/* Evaluate a bitmask to avoid branching. */
	int64_t mask = 0ULL - ((vclock->map >> replica_id) & 0x1);
	return mask & vclock->lsn[replica_id];
}

static inline int64_t
vclock_inc(struct vclock *vclock, uint32_t replica_id)
{
	/* Easier add each time than check. */
	if (((vclock->map >> replica_id) & 0x01) == 0) {
		vclock->lsn[replica_id] = 0;
		vclock->map |= 1 << replica_id;
	}
	vclock->signature++;
	return ++vclock->lsn[replica_id];
}

static inline void
vclock_copy(struct vclock *dst, const struct vclock *src)
{
	/*
	 * Set the last bit of map because bit_clz_u32 returns
	 * undefined result if zero passed.
	 */
	unsigned int max_pos = VCLOCK_MAX - bit_clz_u32(src->map | 0x01);
	memcpy(dst, src, offsetof(struct vclock, lsn) +
			 sizeof(*dst->lsn) * max_pos);
}

static inline uint32_t
vclock_size(const struct vclock *vclock)
{
	return __builtin_popcount(vclock->map);
}

static inline int64_t
vclock_calc_sum(const struct vclock *vclock)
{
	int64_t sum = 0;
	struct vclock_iterator it;
	vclock_iterator_init(&it, vclock);
	vclock_foreach(&it, replica)
		sum += replica.lsn;
	return sum;
}

static inline int64_t
vclock_sum(const struct vclock *vclock)
{
	return vclock->signature;
}

/**
 * Update vclock with the next LSN value for given replica id.
 *
 * @param vclock Vector clock.
 * @param replica_id Replica identifier.
 * @param lsn Next lsn.
 * @return previous lsn value.
 */
int64_t
vclock_follow(struct vclock *vclock, uint32_t replica_id, int64_t lsn);

/**
 * Merge all diff changes into the destination
 * vclock and after reset the diff.
 */
static inline void
vclock_merge(struct vclock *dst, struct vclock *diff)
{
	struct vclock_iterator it;
	vclock_iterator_init(&it, diff);
	vclock_foreach(&it, item)
		vclock_follow(dst, item.id, vclock_get(dst, item.id) + item.lsn);
	vclock_create(diff);
}

/**
 * \brief Format vclock to YAML-compatible string representation:
 * { replica_id: lsn, replica_id:lsn })
 * \param vclock vclock
 * \return fomatted string, stored in a static buffer.
 */
const char *
vclock_to_string(const struct vclock *vclock);

/**
 * \brief Fill vclock from string representation.
 * \param vclock vclock
 * \param str string to parse
 * \retval 0 on sucess
 * \retval error offset on error (indexed from 1)
 * \sa vclock_to_string()
 */
size_t
vclock_from_string(struct vclock *vclock, const char *str);

enum { VCLOCK_ORDER_UNDEFINED = INT_MAX };

/**
 * \brief Compare vclocks
 * \param a vclock
 * \param b vclock
 * \retval 1 if \a vclock is ordered after \a other
 * \retval -1 if \a vclock is ordered before than \a other
 * \retval 0 if vclocks are equal
 * \retval VCLOCK_ORDER_UNDEFINED if vclocks are concurrent
 */
static inline int
vclock_compare(const struct vclock *a, const struct vclock *b)
{
	bool le = true, ge = true;
	unsigned int map = a->map | b->map;
	struct bit_iterator it;
	bit_iterator_init(&it, &map, sizeof(map), true);

	for (size_t replica_id = bit_iterator_next(&it); replica_id < VCLOCK_MAX;
	     replica_id = bit_iterator_next(&it)) {

		int64_t lsn_a = vclock_get(a, replica_id);
		int64_t lsn_b = vclock_get(b, replica_id);
		le = le && lsn_a <= lsn_b;
		ge = ge && lsn_a >= lsn_b;
		if (!ge && !le)
			return VCLOCK_ORDER_UNDEFINED;
	}
	if (ge && !le)
		return 1;
	if (le && !ge)
		return -1;
	return 0;
}

/**
 * @brief vclockset - a set of vclocks
 */
typedef rb_tree(struct vclock) vclockset_t;
rb_proto(, vclockset_, vclockset_t, struct vclock);

/**
 * A proximity search in a set of vclock objects.
 *
 * The set is normally the index of vclocks in the binary
 * log files of the current directory. The task of the search is
 * to find the first log,
 *
 * @return a vclock that <= than \a key
 */
static inline struct vclock *
vclockset_match(vclockset_t *set, struct vclock *key)
{
	struct vclock *match = vclockset_psearch(set, key);
	/**
	 * vclockset comparator returns 0 for
	 * incomparable keys, rendering them equal.
	 * So the match, even when found, is not necessarily
	 * strictly preceding the search key, it may be
	 * incomparable. If this is the case, unwind until we get
	 * to a key which is strictly below the search pattern.
	 */
	while (match != NULL) {
		if (vclock_compare(match, key) <= 0)
			return match;
		/* The order is undefined, try the previous vclock. */
		match = vclockset_prev(set, match);
	}
	/*
	 * There is no xlog which is strictly less than the search
	 * pattern. Return the first log - it is either
	 * strictly greater, or incomparable with the key.
	 */
	return vclockset_first(set);
}

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_VCLOCK_H */
