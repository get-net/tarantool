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
#include <assert.h>
#include "bind.h"
#include "ck_constraint.h"
#include "errcode.h"
#include "session.h"
#include "schema.h"
#include "small/rlist.h"
#include "tuple.h"
#include "sql.h"
#include "sql/sqlInt.h"
#include "sql/vdbeInt.h"

/**
 * Resolve space_def references for check constraint via AST
 * tree traversal.
 * @param expr Check constraint expression AST to resolve column
 *             references.
 * @param ck_constraint_name Check constraint name to raise error.
 * @param space_def Space definition to use.
 * @retval 0 on success.
 * @retval -1 on error.
 */
static int
ck_constraint_resolve_ref(struct Expr *expr, struct space_def *space_def)
{
	struct Parse parser;
	sql_parser_create(&parser, sql_get());
	parser.parse_only = true;
	sql_resolve_self_reference(&parser, space_def, NC_IsCheck, expr, NULL);
	int rc = parser.is_aborted ? -1 : 0;
	if (parser.is_aborted && parser.zErrMsg != NULL)
		diag_set(ClientError, ER_UNKNOWN, parser.zErrMsg);
	sql_parser_destroy(&parser);
	return rc;
}

uint32_t
ck_constraint_def_sizeof(uint32_t name_len, uint32_t expr_str_len,
			 uint32_t *name_offset, uint32_t *expr_str_offset)
{
	*name_offset = sizeof(struct ck_constraint_def);
	*expr_str_offset = *name_offset + (name_len != 0 ? name_len + 1 : 0);
	return *expr_str_offset + (expr_str_len != 0 ? expr_str_len + 1 : 0);
}

void
ck_constraint_def_create(struct ck_constraint_def *ck_constraint_def,
			 const char *name, uint32_t name_len,
			 const char *expr_str, uint32_t expr_str_len)
{
	uint32_t name_offset, expr_str_offset;
	(void)ck_constraint_def_sizeof(name_len, expr_str_len, &name_offset,
				       &expr_str_offset);
	ck_constraint_def->name = (char *)ck_constraint_def + name_offset;
	sprintf(ck_constraint_def->name, "%.*s", name_len, name);
	ck_constraint_def->expr_str =
		(char *)ck_constraint_def + expr_str_offset;
	sprintf(ck_constraint_def->expr_str, "%.*s", expr_str_len, expr_str);
	rlist_create(&ck_constraint_def->link);
}

/**
 * Compile constraint check subroutine.
 * @param ck_constraint Check constraint to compile.
 * @param expr Check constraint expression AST is built for
 *             ck_constraint->def.
 * @param space_def The space definition of the space this check
 *                  constraint is constructed for.
 * @retval not NULL sql_stmt program pointer on success.
 * @retval NULL otherwise.
 */
int
ck_constraint_test_compile(struct ck_constraint *ck_constraint,
			   struct Expr *expr, const struct space_def *space_def)
{
	int rc = -1;
	assert(ck_constraint->space_id == space_def->id);
	struct Parse parser;
	sql_parser_create(&parser, sql_get());
	struct Vdbe *v = sqlGetVdbe(&parser);
	if (v == NULL) {
		diag_set(OutOfMemory, sizeof(struct Vdbe),
			 "sqlGetVdbe", "vdbe");
		goto end;
	}

	/* Compile VDBE with default sql parameters. */
	struct session *user_session = current_session();
	uint32_t sql_flags = user_session->sql_flags;
	user_session->sql_flags = default_flags;

	/*
	 * Generate a prologue code to bind variable new_tuple_var
	 * to new_tuple_reg.
	 */
	uint32_t field_count = space_def->field_count;
	int new_tuple_reg = sqlGetTempRange(&parser, field_count);
	struct Expr bind = {.op = TK_VARIABLE, .u.zToken = "?"};
	ck_constraint->new_tuple_var = parser.nVar + 1;
	for (uint32_t i = 0; i < field_count; i++) {
		sqlExprAssignVarNumber(&parser, &bind, 1);
		sqlExprCodeTarget(&parser, &bind, new_tuple_reg + i);
	}
	vdbe_emit_ck_constraint(&parser, expr, ck_constraint->def->name,
				new_tuple_reg);
	sql_finish_coding(&parser);
	if (parser.is_aborted) {
		diag_set(ClientError, ER_CREATE_CK_CONSTRAINT,
			 ck_constraint->def->name,
			 "can not compile expression");
		goto end;
	}
	sql_parser_destroy(&parser);

	/* Restore original sql flags for user_session.  */
	user_session->sql_flags = sql_flags;
	ck_constraint->stmt = (struct sql_stmt *)v;
	rc = 0;
end:
	return rc;
}

/**
 * Perform ck constraint checks with new tuple data new_tuple_raw
 * before insert or replace in space space_def.
 * @param ck_constraint Check constraint to test.
 * @param space_def The space definition of the space this check
 *                  constraint is constructed for.
 * @param new_tuple_raw The tuple to be inserted in space.
 * @retval 0 if check constraint test is passed, -1 otherwise.
 */
static int
ck_constraint_test(struct ck_constraint *ck_constraint,
		   struct space_def *space_def, const char *new_tuple_raw)
{
	assert(new_tuple_raw != NULL);
	/*
	 * Prepare parameters for checks->stmt execution:
	 * Unpacked new tuple fields mapped to Vdbe memory from
	 * variables from range:
	 * [new_tuple_var,new_tuple_var+field_count]
	 */
	mp_decode_array(&new_tuple_raw);
	/* Reset VDBE to make new bindings. */
	sql_reset(ck_constraint->stmt);
	for (uint32_t i = 0; i < space_def->field_count; i++) {
		struct sql_bind bind;
		if (sql_bind_decode(&bind, ck_constraint->new_tuple_var + i,
				    &new_tuple_raw) != 0)
			return -1;
		if (sql_bind_column(ck_constraint->stmt, &bind,
				    ck_constraint->new_tuple_var + i) != 0)
			return -1;
	}
	/* Checks VDBE can't expire, reset expired flag & Burn. */
	struct Vdbe *v = (struct Vdbe *)ck_constraint->stmt;
	v->expired = 0;
	int rc;
	while ((rc = sql_step(ck_constraint->stmt)) == SQL_ROW) {}
	if (v->rc != SQL_DONE && v->rc != SQL_TARANTOOL_ERROR)
		diag_set(ClientError, ER_SQL, v->zErrMsg);
	return rc == SQL_DONE ? 0 : -1;
}

/**
 * Trigger routine executing ck constraint check on space
 * insert and replace.
 */
static void
ck_constraint_space_trigger(struct trigger *trigger, void *event)
{
	struct ck_constraint *ck_constraint =
		(struct ck_constraint *)trigger->data;
	struct space *space = space_by_id(ck_constraint->space_id);
	assert(space != NULL);
	struct txn *txn = (struct txn *) event;
	struct txn_stmt *stmt = txn_current_stmt(txn);
	struct tuple *new_tuple = stmt->new_tuple;
	if (stmt == NULL || new_tuple == NULL)
		return;
	if (ck_constraint_test(ck_constraint, space->def,
			       tuple_data(new_tuple)) != 0)
		diag_raise();
}

struct ck_constraint *
ck_constraint_new(const struct ck_constraint_def *ck_constraint_def,
		  struct space_def *space_def)
{
	uint32_t ck_constraint_name_len = strlen(ck_constraint_def->name);
	uint32_t expr_str_len = strlen(ck_constraint_def->expr_str);
	uint32_t name_offset, expr_str_offset;
	uint32_t ck_constraint_def_sz =
		ck_constraint_def_sizeof(ck_constraint_name_len, expr_str_len,
					 &name_offset, &expr_str_offset);
	uint32_t ck_constraint_sz = sizeof(struct ck_constraint) +
				    ck_constraint_def_sz;
	struct ck_constraint *ck_constraint = calloc(1, ck_constraint_sz);
	if (ck_constraint == NULL) {
		diag_set(OutOfMemory, ck_constraint_sz, "malloc",
			 "ck_constraint");
		return NULL;
	}
	rlist_create(&ck_constraint->link);
	ck_constraint->space_id = space_def->id;
	ck_constraint->def =
		(struct ck_constraint_def *)((char *)ck_constraint +
					     sizeof(struct ck_constraint));
	ck_constraint_def_create(ck_constraint->def, ck_constraint_def->name,
				 ck_constraint_name_len,
				 ck_constraint_def->expr_str, expr_str_len);
	trigger_create(&ck_constraint->trigger, ck_constraint_space_trigger,
		       ck_constraint, NULL);
	struct Expr *expr =
		sql_expr_compile(sql_get(), ck_constraint_def->expr_str,
				 expr_str_len);
	if (expr == NULL ||
	    ck_constraint_resolve_ref(expr, space_def) != 0) {
		diag_set(ClientError, ER_CREATE_CK_CONSTRAINT,
			 ck_constraint->def->name,
			 box_error_message(box_error_last()));
		goto error;
	}
	if (ck_constraint_test_compile(ck_constraint, expr, space_def) != 0)
		goto error;

end:
	sql_expr_delete(sql_get(), expr, false);
	return ck_constraint;
error:
	ck_constraint_delete(ck_constraint);
	ck_constraint = NULL;
	goto end;
}

void
ck_constraint_delete(struct ck_constraint *ck_constraint)
{
	assert(rlist_empty(&ck_constraint->trigger.link));
	sql_finalize(ck_constraint->stmt);
	TRASH(ck_constraint);
	free(ck_constraint);
}

struct ck_constraint *
space_ck_constraint_by_name(struct space *space, const char *name,
			    uint32_t name_len)
{
	struct ck_constraint *ck_constraint = NULL;
	rlist_foreach_entry(ck_constraint, &space->ck_constraint, link) {
		if (strlen(ck_constraint->def->name) == name_len &&
		    memcmp(ck_constraint->def->name, name, name_len) == 0)
			return ck_constraint;
	}
	return NULL;
}
