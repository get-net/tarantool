#ifndef TARANTOOL_SWIM_TEST_UTILS_H_INCLUDED
#define TARANTOOL_SWIM_TEST_UTILS_H_INCLUDED
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
#include <stdbool.h>
#include "swim/swim.h"

struct swim_cluster;

/**
 * Create a new cluster of SWIM instances. Instances are assigned
 * URIs like '127.0.0.1:[1 - size]' and UUIDs like
 * '00...00[1 - size]'. Instances can be got by their ordinal
 * numbers equal to their port and to last part of UUID.
 */
struct swim_cluster *
swim_cluster_new(int size);

/** Change ACK timeout of all the instances in the cluster. */
void
swim_cluster_set_ack_timeout(struct swim_cluster *cluster, double ack_timeout);

/**
 * Change number of unacknowledged pings to delete a dead member
 * of all the instances in the cluster.
 */
void
swim_cluster_set_gc(struct swim_cluster *cluster, enum swim_gc_mode gc_mode);

/** Delete all the SWIM instances, and the cluster itself. */
void
swim_cluster_delete(struct swim_cluster *cluster);

/** Check that an error in diag contains @a msg. */
bool
swim_error_check_match(const char *msg);

/** Get a SWIM instance by its ordinal number. */
struct swim *
swim_cluster_node(struct swim_cluster *cluster, int i);

/** Drop and create again a SWIM instance with id @a i. */
void
swim_cluster_restart_node(struct swim_cluster *cluster, int i);

/** Block IO on a SWIM instance with id @a i. */
void
swim_cluster_block_io(struct swim_cluster *cluster, int i);

/** Unblock IO on a SWIM instance with id @a i. */
void
swim_cluster_unblock_io(struct swim_cluster *cluster, int i);

void
swim_cluster_set_drop(struct swim_cluster *cluster, int i, bool value);

/**
 * Explicitly add a member of id @a from_id to a member of id
 * @a to_id.
 */
int
swim_cluster_add_link(struct swim_cluster *cluster, int to_id, int from_id);

enum swim_member_status
swim_cluster_member_status(struct swim_cluster *cluster, int node_id,
			   int member_id);

uint64_t
swim_cluster_member_incarnation(struct swim_cluster *cluster, int node_id,
				int member_id);

/**
 * Check if in the cluster every instance knowns the about other
 * instances.
 */
bool
swim_cluster_is_fullmesh(struct swim_cluster *cluster);

/** Wait for fullmesh at most @a timeout fake seconds. */
int
swim_cluster_wait_fullmesh(struct swim_cluster *cluster, double timeout);

/**
 * Wait until a member with id @a member_id is seen with @a status
 * in the membership table of a member with id @a node_id. At most
 * @a timeout seconds.
 */
int
swim_cluster_wait_status(struct swim_cluster *cluster, int node_id,
			 int member_id, enum swim_member_status status,
			 double timeout);

/**
 * Wait until a member with id @a member_id is seen with @a
 * incarnation in the membership table of a member with id @a
 * node_id. At most @a timeout seconds.
 */
int
swim_cluster_wait_incarnation(struct swim_cluster *cluster, int node_id,
			      int member_id, uint64_t incarnation,
			      double timeout);

/** Process SWIM events for @a duration fake seconds. */
void
swim_run_for(double duration);

#define swim_start_test(n) { \
	header(); \
	say_verbose("-------- SWIM start test %s --------", __func__); \
	plan(n); \
}

#define swim_finish_test() { \
	say_verbose("-------- SWIM end test %s --------", __func__); \
	swim_test_ev_reset(); \
	check_plan(); \
	footer(); \
}

#endif /* TARANTOOL_SWIM_TEST_UTILS_H_INCLUDED */
