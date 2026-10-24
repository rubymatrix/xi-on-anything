// The game's sound: sdl_uwp.c's audio thread pulls DirectSound's mix every 10 ms (480 frames of
// 48 kHz stereo float) and hands it here, to an XAudio2 source voice.
#pragma once

// Opens XAudio2 and makes it the game's audio sink (uwp_set_audio_sink). 0 if there is no audio
// device; the game then runs silent, as before.
bool StartAudio();
