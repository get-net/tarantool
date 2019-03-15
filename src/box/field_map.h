#ifndef TARANTOOL_BOX_FIELD_MAP_H_INCLUDED
#define TARANTOOL_BOX_FIELD_MAP_H_INCLUDED
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
#include <stdint.h>

struct region;
struct field_map_builder_slot;

/**
 * A field map is a special area is reserved before tuple's
 * MessagePack data. It is a sequence of the 32-bit unsigned
 * offsets of tuple's indexed fields.
 *
 * These slots are numbered with negative indices called
 * offset_slot(s) starting with -1 (this is necessary to organize
 * the inheritance of tuples). Allocation and assignment of
 * offset_slot(s) is performed on tuple_format creation on index
 * create or alter (see tuple_format_create()).
 *
 *        4b   4b      4b          4b       MessagePack data.
 *       +-----------+------+----+------+------------------------+
 *tuple: |cnt|off1|..| offN | .. | off1 | header ..|key1|..|keyN||
 *       +-----+-----+--+---+----+--+---+------------------------+
 * ext1  ^     |        |   ...     |                 ^       ^
 *       +-----|--------+           |                 |       |
 * indirection |                    +-----------------+       |
 *             +----------------------------------------------+
 *             (offset_slot = N, extent_slot = 1) --> offset
 *
 * This field_map_builder class is used for tuple field_map
 * construction. It encapsulates field_map build logic and size
 * estimation implementation-specific details.
 *
 * Each field offset is a positive number, except the case when
 * a field is not in the tuple. In this case offset is 0.
 *
 * Some slots may store an offset of the field_map_ext structure,
 * which contains an additional sequence of offsets of size
 * defined above(see field_map_ext layout). The caller needs to
 * be aware of when the slot is an offset of the data and when
 * it is the offset of the field_map_ext.
 *
 * Now these extents are used to organize a multikey index.
 * The count of keys in the multikey index imposes the count of
 * items in extent while the i-th extent's slot contains the
 * offset of the i-th key field.
 */
struct field_map_builder {
	/**
	 * The pointer to the end of field_map_builder_slot(s)
	 * allocation.
	 * Its elements are accessible by negative indexes
	 * that coinciding with offset_slot(s).
	 */
	struct field_map_builder_slot *slots;
	/**
	 * The count of slots in field_map_builder::slots
	 * allocation.
	 */
	uint32_t slot_count;
	/**
	 * Total size of memory is allocated for field_map
	 * extentions.
	 */
	uint32_t extents_size;
	/**
	 * Region to use to perform memory allocations.
	 */
	struct region *region;
};

/**
 * Internal stucture representing field_map extent.
 * (see field_map_builder description).
 */
struct field_map_ext {
	/** Count of field_map_ext::offset[] elements. */
	uint32_t items;
	/** Data offset in tuple array. */
	uint32_t offset[0];
};

/**
 * Internal function to get or allocate field map extent
 * by offset_slot, and count of items.
 */
struct field_map_ext *
field_map_builder_ext_get(struct field_map_builder *builder,
			  int32_t offset_slot, uint32_t extent_items);

/**
 * Instead of using uint32_t offset slots directly the
 * field_map_builder uses this structure as a storage atom.
 * When there is a need to initialize an extent, the
 * field_map_builder allocates a new memory chunk and sets
 * field_map_builder_slot::pointer (instead of real field_map
 * reallocation).
 *
 * On field_map_build, all of the extents are dumped to the same
 * memory chunk that the regular field_map slots and corresponding
 * slots represent relative field_map_ext offset instead of
 * field data offset.
 *
 * The allocated memory is accounted for in extents_size.
 */
struct field_map_builder_slot {
	/**
	 * True when this slot must be interpret as
	 * extention pointer.
	 */
	bool has_extent;
	union {
		/** Data offset in tuple. */
		uint32_t offset;
		/** Pointer to field_map_ext extention. */
		struct field_map_ext *extent;
	};
};

/**
 * Get offset of the field in tuple data MessagePack using
 * tuple's field_map and required field's offset_slot.
 *
 * When a field is not in the data tuple, its offset is 0.
 */
static inline uint32_t
field_map_get_offset(const uint32_t *field_map, int32_t offset_slot,
		     int multikey_idx)
{
	uint32_t offset;
	if (multikey_idx >= 0) {
		assert(field_map[offset_slot] != 0);
		struct field_map_ext *extent =
			(struct field_map_ext *)((char *)field_map -
						 field_map[offset_slot]);
		if ((uint32_t)multikey_idx >= extent->items)
			return 0;
		offset = extent->offset[multikey_idx];
	} else {
		offset = field_map[offset_slot];
	}
	return offset;
}

/**
 * Initialize field_map_builder.
 *
 * The field_map_size argument is a size of the minimal field_map
 * allocation where each indexed field has own offset slot.
 *
 * Routine uses region to perform memory allocation for internal
 * structures.
 *
 * Returns 0 on success. In case of memory allocation error sets
 * diag message and returns -1.
 */
int
field_map_builder_create(struct field_map_builder *builder,
			 uint32_t minimal_field_map_size,
			 struct region *region);

/**
 * Set data offset for a field identified by unique offset_slot.
 *
 * The offset_slot argument must be negative and offset must be
 * positive (by definition).
 */
static inline void
field_map_builder_set_slot(struct field_map_builder *builder,
			   int32_t offset_slot, uint32_t offset)
{
	assert(offset_slot < 0);
	assert((uint32_t)-offset_slot <= builder->slot_count);
	assert(offset > 0);
	builder->slots[offset_slot].offset = offset;
}

/**
 * Set data offset in field map extent (by given offset_slot,
 * extent_slot and extent_items) for a field identified by
 * unique offset_slot.
 *
 * The offset_slot argument must be negative and offset must be
 * positive (by definition).
 */
static inline int
field_map_builder_set_extent_slot(struct field_map_builder *builder,
				  int32_t offset_slot, int32_t extent_slot,
				  uint32_t extent_items, uint32_t offset)
{
	assert(offset_slot < 0);
	assert(offset > 0);
	assert(extent_slot >= 0 && extent_items > 0);
	struct field_map_ext *extent =
		field_map_builder_ext_get(builder, offset_slot, extent_items);
	if (extent == NULL)
		return -1;
	assert(extent->items == extent_items);
	extent->offset[extent_slot] = offset;
	return 0;
}

/**
 * Calculate the size of tuple field_map to be built.
 */
static inline uint32_t
field_map_build_size(struct field_map_builder *builder)
{
	return builder->slot_count * sizeof(uint32_t) +
	       builder->extents_size;
}

/**
 * Write constructed field_map to the destination buffer field_map.
 *
 * The buffer must have at least field_map_build_size(builder) bytes.
 */
void
field_map_build(struct field_map_builder *builder, char *buffer);

#endif /* TARANTOOL_BOX_FIELD_MAP_H_INCLUDED */
