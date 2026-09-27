/* DirectInput 8 over SDL3 input (dinput.c, input.c). */
#pragma once

/* Registers the shims (before the images are mapped). */
void dinput_init(void);
/* Builds the COM vtables in guest memory (once the guest heap is up). */
void dinput_setup(void);
