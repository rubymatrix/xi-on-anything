/* The scene renderer's taps on the game (scene.c): the zone's instances as the game picks them, drawn by the
 * graphics back end (gfx_world_draw) in place of the game's own draws while the scene setting is on. */
#pragma once
#include <stdint.h>

/* Once, after the device exists: the setting, the back end's support and the build's addresses. */
void scene_setup(void);
/* From host64's Present hook: the frame is done; the next one starts empty. */
void scene_frame(void);
/* From host64's level-of-detail hook (1017cb9c, 2026-09-03), after its pick: the instance, the model the
 * game will draw, its light list and whether it rides the moving sub-map. Returns the model the game is
 * to draw: 0 when the scene renderer took the instance. */
uint32_t scene_zone_pick(uint32_t inst, uint32_t model, uint32_t lights, int moving);
