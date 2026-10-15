/* Offline, bounded counterparts of the 2026-09-03 client's zone resource loaders.
 * No guest execution, graphics device, culling or network dependency. See docs/zone-resource-export.md. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Decode in a caller-owned copy. Tables are read from the user's matching client image. */
int xi_zone_decode_mmb(uint8_t* data, size_t size, const uint8_t xor_table[256], const uint8_t swap_table[256]);
int xi_zone_decode_mzb(uint8_t* data, size_t size, const uint8_t invert_table[256]);

typedef struct XiZoneBatch
{
    uint32_t part_offset, part_flags, submesh_offset, flags;
    uint32_t vertex_offset, vertex_count, vertex_stride;
    uint32_t index_offset, index_count, primitive, next_offset;
} XiZoneBatch;

/* 1: found; 0: absent slot/end of submesh list; -1: malformed/unsupported resource. */
int xi_zone_mmb_batch(const uint8_t* data, size_t size, uint32_t part, uint32_t submesh, XiZoneBatch* out);
