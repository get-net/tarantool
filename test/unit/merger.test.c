#include "unit.h"              /* plan, header, footer, is, ok */
#include "memory.h"            /* memory_init() */
#include "fiber.h"             /* fiber_init() */
#include "box/tuple.h"         /* tuple_init(), box_tuple_*(),
				  tuple_*() */
#include "box/tuple_format.h"  /* box_tuple_format_default(),
				  tuple_format_id() */
#include "box/key_def.h"       /* key_def_new(),
				  key_def_delete() */
#include "box/merger.h"        /* merger_*() */

/* {{{ Array merger source */

struct merger_source_array {
	struct merger_source base;
	uint32_t tuples_count;
	struct tuple **tuples;
	uint32_t cur;
};

/* Virtual methods declarations */

static void
merger_source_array_delete(struct merger_source *base);
static int
merger_source_array_next(struct merger_source *base, box_tuple_format_t *format,
			 struct tuple **out);

/* Non-virtual methods */

static struct merger_source *
merger_source_array_new(bool even)
{
	static struct merger_source_vtab merger_source_array_vtab = {
		.delete = merger_source_array_delete,
		.next = merger_source_array_next,
	};

	struct merger_source_array *source =
		(struct merger_source_array *) malloc(
		sizeof(struct merger_source_array));
	assert(source != NULL);

	merger_source_new(&source->base, &merger_source_array_vtab);

	uint32_t tuple_size = 2;
	const uint32_t tuples_count = 2;
	/* {1}, {3} */
	static const char *data_odd[] = {"\x91\x01", "\x91\x03"};
	/* {2}, {4} */
	static const char *data_even[] = {"\x91\x02", "\x91\x04"};
	const char **data = even ? data_even : data_odd;
	source->tuples = (struct tuple **) malloc(
		tuples_count * sizeof(struct tuple *));
	assert(source->tuples != NULL);
	box_tuple_format_t *format = box_tuple_format_default();
	for (uint32_t i = 0; i < tuples_count; ++i) {
		const char *end = data[i] + tuple_size;
		source->tuples[i] = box_tuple_new(format, data[i], end);
		box_tuple_ref(source->tuples[i]);
	}
	source->tuples_count = tuples_count;
	source->cur = 0;

	return &source->base;
}

/* Virtual methods */

static void
merger_source_array_delete(struct merger_source *base)
{
	struct merger_source_array *source = container_of(base,
		struct merger_source_array, base);

	for (uint32_t i = 0; i < source->tuples_count; ++i)
		box_tuple_unref(source->tuples[i]);

	free(source->tuples);
	free(source);
}

static int
merger_source_array_next(struct merger_source *base, box_tuple_format_t *format,
			 struct tuple **out)
{
	struct merger_source_array *source = container_of(base,
		struct merger_source_array, base);

	if (source->cur == source->tuples_count) {
		*out = NULL;
		return 0;
	}

	struct tuple *tuple = source->tuples[source->cur];

	box_tuple_format_t *default_format = box_tuple_format_default();
	uint32_t id_stored = tuple_format_id(default_format);
	assert(tuple->format_id == id_stored);
	if (format == NULL)
		format = default_format;
	uint32_t id_requested = tuple_format_id(format);
	if (id_stored != id_requested) {
		const char *tuple_beg = tuple_data(tuple);
		const char *tuple_end = tuple_beg + tuple->bsize;
		tuple = box_tuple_new(format, tuple_beg, tuple_end);
		assert(tuple != NULL);
	}

	assert(tuple != NULL);
	box_tuple_ref(tuple);
	*out = tuple;
	++source->cur;
	return 0;
}

/* }}} */

static struct key_part_def key_part_unsigned = {
	.fieldno = 0,
	.type = FIELD_TYPE_UNSIGNED,
	.coll_id = COLL_NONE,
	.is_nullable = false,
	.nullable_action = ON_CONFLICT_ACTION_DEFAULT,
	.sort_order = SORT_ORDER_ASC,
	.path = NULL,
};

static struct key_part_def key_part_integer = {
	.fieldno = 0,
	.type = FIELD_TYPE_INTEGER,
	.coll_id = COLL_NONE,
	.is_nullable = false,
	.nullable_action = ON_CONFLICT_ACTION_DEFAULT,
	.sort_order = SORT_ORDER_ASC,
	.path = NULL,
};

uint32_t
min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

void
check_tuple(const struct tuple *tuple, box_tuple_format_t *format,
	    const char *exp_data, uint32_t exp_data_len, const char *case_name)
{
	uint32_t size;
	const char *data = tuple_data_range(tuple, &size);

	ok(tuple != NULL, "%s: tuple != NULL", case_name);
	if (format == NULL) {
		ok(true, "%s: skip tuple format id check", case_name);
	} else {
		is(tuple->format_id, tuple_format_id(format),
		   "%s: check tuple format id", case_name);
	}
	is(size, exp_data_len, "%s: check tuple size", case_name);
	ok(!strncmp(data, exp_data, min_u32(size, exp_data_len)),
	   "%s: check tuple data", case_name);
}

/**
 * Check array source itself (just in case).
 */
int
test_array_source(box_tuple_format_t *format)
{
	plan(9);
	header();

	/* {1}, {3} */
	const uint32_t exp_tuple_size = 2;
	const uint32_t exp_tuples_count = 2;
	static const char *exp_tuples_data[] = {"\x91\x01", "\x91\x03"};

	struct merger_source *source = merger_source_array_new(false);
	assert(source != NULL);
	merger_source_ref(source);

	struct tuple *tuple = NULL;
	const char *msg = format == NULL ?
		"array source next() (any format)" :
		"array source next() (user's format)";
	for (uint32_t i = 0; i < exp_tuples_count; ++i) {
		int rc = source->vtab->next(source, format, &tuple);
		(void) rc;
		assert(rc == 0);
		check_tuple(tuple, format, exp_tuples_data[i], exp_tuple_size,
			    msg);
		box_tuple_unref(tuple);
	}
	int rc = source->vtab->next(source, format, &tuple);
	(void) rc;
	assert(rc == 0);
	is(tuple, NULL, format == NULL ?
	   "array source is empty (any format)" :
	   "array source is empty (user's format)");

	merger_source_unref(source);

	footer();
	return check_plan();
}

int
test_merger(box_tuple_format_t *format)
{
	plan(17);
	header();

	/* {1}, {2}, {3}, {4} */
	const uint32_t exp_tuple_size = 2;
	const uint32_t exp_tuples_count = 4;
	static const char *exp_tuples_data[] = {
		"\x91\x01", "\x91\x02", "\x91\x03", "\x91\x04",
	};

	const uint32_t sources_count = 2;
	struct merger_source *sources[] = {
		merger_source_array_new(false),
		merger_source_array_new(true),
	};
	merger_source_ref(sources[0]);
	merger_source_ref(sources[1]);

	struct key_def *key_def = key_def_new(&key_part_unsigned, 1);
	struct merger_context *ctx = merger_context_new(key_def);
	merger_context_ref(ctx);
	key_def_delete(key_def);
	struct merger_source *merger = merger_new(ctx);
	merger_set_sources(merger, sources, sources_count);
	merger_set_reverse(merger, false);
	merger_source_ref(merger);

	struct tuple *tuple = NULL;
	const char *msg = format == NULL ?
		"merger next() (any format)" :
		"merger next() (user's format)";
	for (uint32_t i = 0; i < exp_tuples_count; ++i) {
		int rc = merger->vtab->next(merger, format, &tuple);
		(void) rc;
		assert(rc == 0);
		check_tuple(tuple, format, exp_tuples_data[i], exp_tuple_size,
			    msg);
		box_tuple_unref(tuple);
	}
	int rc = merger->vtab->next(merger, format, &tuple);
	(void) rc;
	assert(rc == 0);
	is(tuple, NULL, format == NULL ?
	   "merger is empty (any format)" :
	   "merger is empty (user's format)");

	merger_source_unref(merger);
	merger_context_unref(ctx);
	merger_source_unref(sources[0]);
	merger_source_unref(sources[1]);

	footer();
	return check_plan();
}

int
test_basic()
{
	plan(4);
	header();

	struct key_def *key_def = key_def_new(&key_part_integer, 1);
	box_tuple_format_t *format = box_tuple_format_new(&key_def, 1);
	assert(format != NULL);

	test_array_source(NULL);
	test_array_source(format);
	test_merger(NULL);
	test_merger(format);

	key_def_delete(key_def);
	box_tuple_format_unref(format);

	footer();
	return check_plan();
}

int
main()
{
	memory_init();
	fiber_init(fiber_c_invoke);
	tuple_init(NULL);

	int rc = test_basic();

	tuple_free();
	fiber_free();
	memory_free();

	return rc;
}
