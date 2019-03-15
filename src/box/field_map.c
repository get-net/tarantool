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
#include "diag.h"
#include "field_map.h"
#include "small/region.h"

int
field_map_builder_create(struct field_map_builder *builder,
			 uint32_t minimal_field_map_size,
			 struct region *region)
{
	builder->region = region;
	builder->extents_size = 0;
	builder->slot_count = minimal_field_map_size / sizeof(uint32_t);
	if (minimal_field_map_size == 0) {
		builder->slots = NULL;
		return 0;
	}
	uint32_t sz = builder->slot_count * sizeof(builder->slots[0]);
	builder->slots = region_alloc(region, sz);
	if (builder->slots == NULL) {
		diag_set(OutOfMemory, sz, "region_alloc", "field_map");
		return -1;
	}
	memset((char *)builder->slots, 0, sz);
	builder->slots = builder->slots + builder->slot_count;
	return 0;
}

/**
 * Get size of extention (in bytes) by count of items it
 * must contain.
 */
static uint32_t
field_map_ext_size(uint32_t items)
{
	return sizeof(struct field_map_ext) + items * sizeof(uint32_t);
}

struct field_map_ext *
field_map_builder_ext_get(struct field_map_builder *builder,
			  int32_t offset_slot, uint32_t extent_items)
{
	struct field_map_ext *extent;
	if (builder->slots[offset_slot].has_extent) {
		extent = builder->slots[offset_slot].extent;
		assert(extent != NULL);
		assert(extent->items == extent_items);
		return extent;
	}
	uint32_t sz = field_map_ext_size(extent_items);
	extent = region_alloc(builder->region, sz);
	if (extent == NULL) {
		diag_set(OutOfMemory, sz, "region", "extent");
		return NULL;
	}
	memset(extent, 0, sz);
	extent->items = extent_items;
	builder->slots[offset_slot].extent = extent;
	builder->slots[offset_slot].has_extent = true;
	builder->extents_size += sz;
	return extent;
}

void
field_map_build(struct field_map_builder *builder, char *buffer)
{
	/*
	 * To initialize the field map and its extents, prepare
	 * the following memory layout with pointers:
	 *
	 *                      offset
	 *            +---------------------+
	 *            |                     |
	 * [extentK]..[extent1][[slotN]..[slot2][slot1]]
	 *            |        |extent_wptr            |field_map
	 *            <-       <-                     <-
	 *
	 * The buffer size is assumed to be sufficient to write
	 * field_map_build_size(builder) bytes there.
	 */
	uint32_t *field_map =
		(uint32_t *)(buffer + field_map_build_size(builder));
	char *extent_wptr = buffer + builder->extents_size;
	for (int32_t i = -1; i >= -(int32_t)builder->slot_count; i--) {
		if (!builder->slots[i].has_extent) {
			field_map[i] = builder->slots[i].offset;
			continue;
		}
		struct field_map_ext *extent = builder->slots[i].extent;
		/** Retrive memory for the extent. */
		uint32_t sz = field_map_ext_size(extent->items);
		extent_wptr -= sz;
		field_map[i] = (char *)field_map - (char *)extent_wptr;
		memcpy(extent_wptr, extent, sz);
	}
}
