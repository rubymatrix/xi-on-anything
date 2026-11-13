/* Status icons drawn from vectors (vicons.c): the buff and debuff icons the game shows as 32x32
 * images, redrawn at any size from templates of our own (an orb, a sword, an arrow, a word), so
 * they stay sharp however large the interface is drawn. The game's icons are recognised as it
 * uploads them; those without a template are left as they are. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Reads a file of the install by its path in the install ("FTABLE.DAT", "ROM/119/57.DAT"): the
 * whole file, malloc'd, or NULL. */
typedef uint8_t* (*ViconsRead)(const char* rel, size_t* size);

/* With NULL, or never called, no icon is replaced. The install is read at the first icon. */
void vicons_init(ViconsRead read);

/* A d3d8 texture source (d3d8_set_texture_source): when level0 (w x h, D3DFORMAT format, rows
 * pitch bytes apart) is one of the game's status icons that has a template, its vector drawing,
 * malloc'd as A8R8G8B8 (B G R A in memory), *size x *size; else NULL. */
uint8_t* vicons_texture(uint32_t w, uint32_t h, uint32_t format, const uint8_t* level0, uint32_t pitch, uint32_t* size);

/* For tests and tools: the SVG document of status id's template into buf (0 when it has none), and
 * an SVG document drawn at size x size, RGBA with straight alpha, malloc'd (NULL on failure). */
int vicons_svg(uint32_t id, char* buf, size_t n);
uint8_t* vicons_rasterize(const char* svg, int size);
