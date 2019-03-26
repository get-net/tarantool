/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
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
#include "execute.h"

#include "bind.h"
#include "iproto_constants.h"
#include "sql/sqlInt.h"
#include "sql/sqlLimit.h"
#include "errcode.h"
#include "small/region.h"
#include "small/obuf.h"
#include "diag.h"
#include "sql.h"
#include "xrow.h"
#include "schema.h"
#include "port.h"
#include "tuple.h"
#include "sql/vdbe.h"

const char *sql_info_key_strs[] = {
	"row count",
};

/**
 * Serialize a single column of a result set row.
 * @param stmt Prepared and started statement. At least one
 *        sql_step must be called.
 * @param i Column number.
 * @param region Allocator for column value.
 *
 * @retval  0 Success.
 * @retval -1 Out of memory when resizing the output buffer.
 */
static inline int
sql_column_to_messagepack(struct sql_stmt *stmt, int i,
			  struct region *region)
{
	size_t size;
	int type = sql_column_type(stmt, i);
	switch (type) {
	case SQL_INTEGER: {
		int64_t n = sql_column_int64(stmt, i);
		if (n >= 0)
			size = mp_sizeof_uint(n);
		else
			size = mp_sizeof_int(n);
		char *pos = (char *) region_alloc(region, size);
		if (pos == NULL)
			goto oom;
		if (n >= 0)
			mp_encode_uint(pos, n);
		else
			mp_encode_int(pos, n);
		break;
	}
	case SQL_FLOAT: {
		double d = sql_column_double(stmt, i);
		size = mp_sizeof_double(d);
		char *pos = (char *) region_alloc(region, size);
		if (pos == NULL)
			goto oom;
		mp_encode_double(pos, d);
		break;
	}
	case SQL_TEXT: {
		uint32_t len = sql_column_bytes(stmt, i);
		size = mp_sizeof_str(len);
		char *pos = (char *) region_alloc(region, size);
		if (pos == NULL)
			goto oom;
		const char *s;
		s = (const char *)sql_column_text(stmt, i);
		mp_encode_str(pos, s, len);
		break;
	}
	case SQL_BLOB: {
		uint32_t len = sql_column_bytes(stmt, i);
		const char *s =
			(const char *)sql_column_blob(stmt, i);
		if (sql_column_subtype(stmt, i) == SQL_SUBTYPE_MSGPACK) {
			size = len;
			char *pos = (char *)region_alloc(region, size);
			if (pos == NULL)
				goto oom;
			memcpy(pos, s, len);
		} else {
			size = mp_sizeof_bin(len);
			char *pos = (char *)region_alloc(region, size);
			if (pos == NULL)
				goto oom;
			mp_encode_bin(pos, s, len);
		}
		break;
	}
	case SQL_NULL: {
		size = mp_sizeof_nil();
		char *pos = (char *) region_alloc(region, size);
		if (pos == NULL)
			goto oom;
		mp_encode_nil(pos);
		break;
	}
	default:
		unreachable();
	}
	return 0;
oom:
	diag_set(OutOfMemory, size, "region_alloc", "SQL value");
	return -1;
}

/**
 * Convert sql row into a tuple and append to a port.
 * @param stmt Started prepared statement. At least one
 *        sql_step must be done.
 * @param column_count Statement's column count.
 * @param region Runtime allocator for temporary objects.
 * @param port Port to store tuples.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
static inline int
sql_row_to_port(struct sql_stmt *stmt, int column_count,
		struct region *region, struct port *port)
{
	assert(column_count > 0);
	size_t size = mp_sizeof_array(column_count);
	size_t svp = region_used(region);
	char *pos = (char *) region_alloc(region, size);
	if (pos == NULL) {
		diag_set(OutOfMemory, size, "region_alloc", "SQL row");
		return -1;
	}
	mp_encode_array(pos, column_count);

	for (int i = 0; i < column_count; ++i) {
		if (sql_column_to_messagepack(stmt, i, region) != 0)
			goto error;
	}
	size = region_used(region) - svp;
	pos = (char *) region_join(region, size);
	if (pos == NULL) {
		diag_set(OutOfMemory, size, "region_join", "pos");
		goto error;
	}
	struct tuple *tuple =
		tuple_new(box_tuple_format_default(), pos, pos + size);
	if (tuple == NULL)
		goto error;
	region_truncate(region, svp);
	return port_tuple_add(port, tuple);

error:
	region_truncate(region, svp);
	return -1;
}

/**
 * Serialize a description of the prepared statement.
 * @param stmt Prepared statement.
 * @param out Out buffer.
 * @param column_count Statement's column count.
 *
 * @retval  0 Success.
 * @retval -1 Client or memory error.
 */
static inline int
sql_get_description(struct sql_stmt *stmt, struct obuf *out,
		    int column_count)
{
	assert(column_count > 0);
	int size = mp_sizeof_uint(IPROTO_METADATA) +
		   mp_sizeof_array(column_count);
	char *pos = (char *) obuf_alloc(out, size);
	if (pos == NULL) {
		diag_set(OutOfMemory, size, "obuf_alloc", "pos");
		return -1;
	}
	pos = mp_encode_uint(pos, IPROTO_METADATA);
	pos = mp_encode_array(pos, column_count);
	for (int i = 0; i < column_count; ++i) {
		size_t size = mp_sizeof_map(2) +
			      mp_sizeof_uint(IPROTO_FIELD_NAME) +
			      mp_sizeof_uint(IPROTO_FIELD_TYPE);
		const char *name = sql_column_name(stmt, i);
		const char *type = sql_column_datatype(stmt, i);
		/*
		 * Can not fail, since all column names and types
		 * are preallocated during prepare phase and the
		 * column_name simply returns them.
		 */
		assert(name != NULL);
		assert(type != NULL);
		size += mp_sizeof_str(strlen(name));
		size += mp_sizeof_str(strlen(type));
		char *pos = (char *) obuf_alloc(out, size);
		if (pos == NULL) {
			diag_set(OutOfMemory, size, "obuf_alloc", "pos");
			return -1;
		}
		pos = mp_encode_map(pos, 2);
		pos = mp_encode_uint(pos, IPROTO_FIELD_NAME);
		pos = mp_encode_str(pos, name, strlen(name));
		pos = mp_encode_uint(pos, IPROTO_FIELD_TYPE);
		pos = mp_encode_str(pos, type, strlen(type));
	}
	return 0;
}

static inline int
sql_execute(sql *db, struct sql_stmt *stmt, struct port *port,
	    struct region *region)
{
	int rc, column_count = sql_column_count(stmt);
	if (column_count > 0) {
		/* Either ROW or DONE or ERROR. */
		while ((rc = sql_step(stmt)) == SQL_ROW) {
			if (sql_row_to_port(stmt, column_count, region,
					    port) != 0)
				return -1;
		}
		assert(rc == SQL_DONE || rc != SQL_OK);
	} else {
		/* No rows. Either DONE or ERROR. */
		rc = sql_step(stmt);
		assert(rc != SQL_ROW && rc != SQL_OK);
	}
	if (rc != SQL_DONE) {
		diag_set(ClientError, ER_SQL_EXECUTE, sql_errmsg(db));
		return -1;
	}
	return 0;
}

int
sql_prepare_and_execute(const char *sql, int len, const struct sql_bind *bind,
			uint32_t bind_count, struct sql_response *response,
			struct region *region)
{
	struct sql_stmt *stmt;
	struct sql *db = sql_get();
	if (db == NULL) {
		diag_set(ClientError, ER_LOADING);
		return -1;
	}
	if (sql_prepare_v2(db, sql, len, &stmt, NULL) != SQL_OK) {
		diag_set(ClientError, ER_SQL_EXECUTE, sql_errmsg(db));
		return -1;
	}
	assert(stmt != NULL);
	port_tuple_create(&response->port);
	response->prep_stmt = stmt;
	if (sql_bind(stmt, bind, bind_count) == 0 &&
	    sql_execute(db, stmt, &response->port, region) == 0)
		return 0;
	port_destroy(&response->port);
	sql_finalize(stmt);
	return -1;
}

int
sql_response_dump(struct sql_response *response, struct obuf *out)
{
	sql *db = sql_get();
	struct sql_stmt *stmt = (struct sql_stmt *) response->prep_stmt;
	struct port_tuple *port_tuple = (struct port_tuple *) &response->port;
	int rc = 0, column_count = sql_column_count(stmt);
	if (column_count > 0) {
		int keys = 2;
		int size = mp_sizeof_map(keys);
		char *pos = (char *) obuf_alloc(out, size);
		if (pos == NULL) {
			diag_set(OutOfMemory, size, "obuf_alloc", "pos");
			goto err;
		}
		pos = mp_encode_map(pos, keys);
		if (sql_get_description(stmt, out, column_count) != 0) {
err:
			rc = -1;
			goto finish;
		}
		size = mp_sizeof_uint(IPROTO_DATA) +
		       mp_sizeof_array(port_tuple->size);
		pos = (char *) obuf_alloc(out, size);
		if (pos == NULL) {
			diag_set(OutOfMemory, size, "obuf_alloc", "pos");
			goto err;
		}
		pos = mp_encode_uint(pos, IPROTO_DATA);
		pos = mp_encode_array(pos, port_tuple->size);
		/*
		 * Just like SELECT, SQL uses output format compatible
		 * with Tarantool 1.6
		 */
		if (port_dump_msgpack_16(&response->port, out) < 0) {
			/* Failed port dump destroyes the port. */
			goto err;
		}
	} else {
		int keys = 1;
		assert(port_tuple->size == 0);
		struct stailq *autoinc_id_list =
			vdbe_autoinc_id_list((struct Vdbe *)stmt);
		uint32_t map_size = stailq_empty(autoinc_id_list) ? 1 : 2;
		int size = mp_sizeof_map(keys) +
			   mp_sizeof_uint(IPROTO_SQL_INFO) +
			   mp_sizeof_map(map_size);
		char *pos = (char *) obuf_alloc(out, size);
		if (pos == NULL) {
			diag_set(OutOfMemory, size, "obuf_alloc", "pos");
			goto err;
		}
		pos = mp_encode_map(pos, keys);
		pos = mp_encode_uint(pos, IPROTO_SQL_INFO);
		pos = mp_encode_map(pos, map_size);
		uint64_t id_count = 0;
		int changes = db->nChange;
		size = mp_sizeof_uint(SQL_INFO_ROW_COUNT) +
		       mp_sizeof_uint(changes);
		if (!stailq_empty(autoinc_id_list)) {
			struct autoinc_id_entry *id_entry;
			stailq_foreach_entry(id_entry, autoinc_id_list, link) {
				size += id_entry->id >= 0 ?
					mp_sizeof_uint(id_entry->id) :
					mp_sizeof_int(id_entry->id);
				id_count++;
			}
			size += mp_sizeof_uint(SQL_INFO_AUTOINCREMENT_IDS) +
				mp_sizeof_array(id_count);
		}
		char *buf = obuf_alloc(out, size);
		if (buf == NULL) {
			diag_set(OutOfMemory, size, "obuf_alloc", "buf");
			goto err;
		}
		buf = mp_encode_uint(buf, SQL_INFO_ROW_COUNT);
		buf = mp_encode_uint(buf, changes);
		if (!stailq_empty(autoinc_id_list)) {
			buf = mp_encode_uint(buf, SQL_INFO_AUTOINCREMENT_IDS);
			buf = mp_encode_array(buf, id_count);
			struct autoinc_id_entry *id_entry;
			stailq_foreach_entry(id_entry, autoinc_id_list, link) {
				buf = id_entry->id >= 0 ?
				      mp_encode_uint(buf, id_entry->id) :
				      mp_encode_int(buf, id_entry->id);
			}
		}
	}
finish:
	port_destroy(&response->port);
	sql_finalize(stmt);
	return rc;
}
