/* The orbit camera (camera.c): the mouse turns the camera round the character, as in FINAL FANTASY
 * XIV. Dragging with either button held orbits the camera; both buttons held run forward; the wheel
 * over the world (not the interface) zooms. A press that does not move is a click, handed to the game
 * as it was. host64 --camera orbit|game; the settings file's camera (1 orbit, 2 the game's) overrides
 * it live, camera_speed scales how far a drag turns. */
#pragma once

/* on: 1 orbit (the default), 0 the game's own mouse. Takes the mouse (user32_mouse_hook). */
void camera_init(int on);
/* From the Present hook, on the game's thread: the drags and wheel turns since the last frame, applied
 * to the game's camera. */
void camera_frame(void);
