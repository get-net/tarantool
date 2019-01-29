#ifndef INCLUDES_BOX_CK_CONSTRAINT_H
#define INCLUDES_BOX_CK_CONSTRAINT_H
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

#include <stdint.h>
#include "trigger.h"
#include "small/rlist.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct space;
struct space_def;
struct sql_stmt;
struct Expr;

/**
 * Definition of check constraint.
 * The memory of size calculated with ck_constraint_def_sizeof
 * must be allocated manually and must be initialized with routine
 * ck_constraint_def_create.
 */
struct ck_constraint_def {
	/**
	 * The name of the check constraint is used for error
	 * reporting. Must be unique for a given space.
	 */
	char *name;
	/**
	 * The string describing an check constraint expression.
	 */
	char *expr_str;
	/**
	 * Organize check_def structs into linked list with
	 * Parse::new_ck_constraint.
	 */
	struct rlist link;
};

/* Structure representing check constraint object. */
struct ck_constraint {
	/**
	 * The check constraint definition.
	 */
	struct ck_constraint_def *def;
	/**
	 * Precompiled reusable VDBE program for proceeding ck
	 * constraint checks and setting bad exitcode and error
	 * message when ck condition unsatisfied.
	 * Program rely on new_tuple_var parameter to be binded
	 * in the VDBE memory before run.
	 */
	struct sql_stmt *stmt;
	/**
	 * The id of the space this check constraint is
	 * built for.
	 */
	uint32_t space_id;
	/**
	 * The first ck_constraint::stmt VDBE variable of the
	 * range space[ck_constraint::space_id]->def->field_count
	 * representing a new tuple to be inserted.
	 */
	int new_tuple_var;
	/**
	 * Trigger object executing check constraint on space
	 * insert and replace.
	 */
	struct trigger trigger;
	/**
	 * Organize check constraint structs into linked list
	 * with space::ck_constraint.
	 */
	struct rlist link;
};

/**
 * Calculate check constraint definition memory size and fields
 * offsets for given arguments.
 * Alongside with struct ck_constraint_def itself, we reserve
 * memory for string containing its name and expression string.
 *
 * Memory layout:
 * +-----------------------------+ <- Allocated memory starts here
 * |   struct ck_constraint_def  |
 * |-----------------------------|
 * |          name + \0          |
 * |-----------------------------|
 * |        expr_str + \0        |
 * +-----------------------------+
 * @param name_len The length of the name.
 * @param expr_str_len The length of the expr_str.
 * @param[out] name_offset The offset of the name string.
 * @param[out] expr_str_offset The offset of the expr_str string.
 */
uint32_t
ck_constraint_def_sizeof(uint32_t name_len, uint32_t expr_str_len,
			 uint32_t *name_offset, uint32_t *expr_str_offset);

/**
 * Initialize specified memory chunk ck_constraint_def of size
 * calculated with ck_constraint_def_sizeof for given arguments.
 * @param ck_constraint_def Check constraint definition to
 *                          initialize.
 * @param name The check constraint name.
 * @param name_len The length of the name.
 * @param expr_str The string describing check constraint
 *                 expression (optional).
 * @param expr_str_len The length of the expr_str.
 */
void
ck_constraint_def_create(struct ck_constraint_def *ck_constraint_def,
			 const char *name, uint32_t name_len,
			 const char *expr_str, uint32_t expr_str_len);

/**
 * Create a new object representing check constraint object
 * for given check constraint definition and space definition
 * this constraint is related to.
 * This routine manually allocates own space_def structure as
 * a part of new memory chunk.
 * @param ck_constraint_def The check constraint definition object
 *                          to use. Must be initialized with
 *                          ck_constraint_def_new.
 * @param space_def The space definition of the space this check
 *                  constraint is constructed for.
 * @retval not NULL Check constraint object on success,
 *         NULL otherwise.
*/
struct ck_constraint *
ck_constraint_new(const struct ck_constraint_def *ck_constraint_def,
		  struct space_def *space_def);

/**
 * Destroy check constraint memory, release acquired resources.
 * @param ck_constraint The check constraint object to destroy.
 */
void
ck_constraint_delete(struct ck_constraint *ck_constraint);

/**
 * Find check constraint object in space by given name and
 * name_len.
 * @param space The space to lookup check constarint.
 * @param name The check constraint name.
 * @param name_len The length of the name.
 * @retval not NULL Check constrain if exists, NULL otherwise.
 */
struct ck_constraint *
space_ck_constraint_by_name(struct space *space, const char *name,
			    uint32_t name_len);

#if defined(__cplusplus)
} /* extern "C" { */
#endif

#endif /* INCLUDES_BOX_CK_CONSTRAINT_H */
