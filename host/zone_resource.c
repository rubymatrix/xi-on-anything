#include "zone_resource.h"
#include <string.h>

static uint32_t word(const uint8_t* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int range(size_t size, uint64_t at, uint64_t bytes)
{
    return at <= size && bytes <= size - at;
}

/* f_1016fcd0:1016fcf7-1016fd45 and f_10170e80. Keep unsigned wrap and swap timing. */
int xi_zone_decode_mmb(uint8_t* data, size_t size, const uint8_t xor_table[256], const uint8_t swap_table[256])
{
    if (!data || !xor_table || !swap_table || size < 8)
        return 0;
    uint32_t n = word(data) & 0xffffff, index = data[5] ^ 0xf0;
    if (n < 8 || n > size)
        return 0;
    if (data[3] >= 5)
    {
        uint32_t key = xor_table[index], counter = 0;
        for (uint32_t i = 8; i < n; ++i)
        {
            uint32_t old = key & 255;
            key += ++counter;
            data[i] ^= (uint8_t)((old | old << 8) >> (key & 7));
            key += ++counter;
        }
    }
    if ((word(data + 4) & 0xffff0000) == 0xffff0000)
    {
        uint32_t half = ((n - 8) & ~15u) / 2;
        uint32_t key = swap_table[index], modifier = index;
        for (uint32_t at = 0; at < half; at += 8)
        {
            int swap = key & 1;
            modifier += 9;
            key += modifier;
            if (swap)
            {
                uint8_t temp[8];
                memcpy(temp, data + 8 + at, 8);
                memcpy(data + 8 + at, data + 8 + half + at, 8);
                memcpy(data + 8 + half + at, temp, 8);
            }
        }
    }
    return 1;
}

/* f_10179430:10179445-101794e1, then 1017986e-1017988a and record stride at 1017a23f. */
int xi_zone_decode_mzb(uint8_t* data, size_t size, const uint8_t invert_table[256])
{
    if (!data || !invert_table || size < 32)
        return 0;
    uint32_t n = word(data) & 0xffffff, count = word(data + 4) & 0xffffff;
    uint32_t version = data[3];
    /* First supported source profile has 100-byte placement records. */
    if (version < 0x1b || n < 32 || n > size || !range(n, 32, (uint64_t)count * 100))
        return 0;
    uint32_t key = invert_table[255 - data[7]], counter = 0, at = 0;
    while (at < n - 8)
    {
        uint32_t piece = ((key & 255) >> 4 & 7) + 16;
        int invert = key & 1;
        key += ++counter;
        if (invert && at + piece < n - 8)
            for (uint32_t j = 0; j < piece; ++j)
                data[8 + at + j] ^= 255;
        at += piece;
    }
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < 16; ++j)
            data[32 + i * 100 + j] ^= 0x55;
    return 1;
}

/* f_1016fcd0: part offsets +0x3c, part header +0x20, vertex flags at +0x10,
 * 36/48-byte source vertices, uint16 indices, next submesh aligned to four bytes. */
int xi_zone_mmb_batch(const uint8_t* data, size_t size, uint32_t part, uint32_t submesh, XiZoneBatch* out)
{
    if (!data || !out || size < 60)
        return -1;
    uint32_t n = word(data) & 0xffffff, kind = data[4];
    uint32_t count = data[3] < 2 ? 9 : word(data + 32);
    if (n > size || n < 60 || kind > 3 || !range(n, 60, (uint64_t)count * 4))
        return -1;
    if (part >= count)
        return 0;
    uint32_t at = word(data + 60 + part * 4);
    if (!at)
        return 0;
    if (!range(n, at, 32))
        return -1;
    uint32_t subcount = word(data + at), part_offset = at, part_flags = word(data + at + 28);
    if (submesh >= subcount)
        return 0;
    at += 32;
    for (uint32_t i = 0; i <= submesh; ++i)
    {
        if (!range(n, at, 20))
            return -1;
        uint32_t flags = word(data + at + 16), vc = flags & 0x0fffffff;
        uint32_t stride = kind == 2 ? 48 : 36;
        uint64_t index_header = (uint64_t)at + 20 + (uint64_t)vc * stride;
        if (!range(n, index_header, 4))
            return -1;
        uint32_t ic = word(data + index_header);
        uint64_t next = index_header + 4 + ((uint64_t)ic + 1 & ~(uint64_t)1) * 2;
        if (!range(n, index_header + 4, (uint64_t)ic * 2) || next > n)
            return -1;
        if (i == submesh)
        {
            out->part_offset = part_offset;
            out->part_flags = part_flags;
            out->submesh_offset = at;
            out->flags = flags;
            out->vertex_offset = at + 20;
            out->vertex_count = vc;
            out->vertex_stride = stride;
            out->index_offset = (uint32_t)index_header + 4;
            out->index_count = ic;
            out->primitive = kind == 1 ? 5 : 4;
            out->next_offset = (uint32_t)next;
            return 1;
        }
        at = (uint32_t)next;
    }
    return 0;
}
