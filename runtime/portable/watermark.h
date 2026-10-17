/* Stack and guest-memory watermarks (watermark.c); no-ops unless built with -DRT_WATERMARK. */
#pragma once

#include "runtime.h"

#if defined(RT_WATERMARK)
void watermark_probe(Guest* g, uint32_t target);
void watermark_frame(void);
#else
#define watermark_probe(g, target) ((void)0)
#define watermark_frame() ((void)0)
#endif
