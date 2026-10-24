// What the app reads before the game starts, from LocalState\xbox.cfg (key=value lines, # for notes;
// edited through Device Portal's file explorer, there being no screen for it):
//
//   Resolution=1920x1080   the game's resolution; left out, the screen's (Display.h)
//   Profile=1              FFXI_PROFILE and FFXI_SAMPLE: the frame profile and the game thread's samples
//   FFXI...=...            host64's app defaults (host/appdefaults.h): FFXIServer, FFXIDrawDistance...
//
// The sign-in screen keeps its own choices (signin.cfg) beside it.
#pragma once

#include <string>

// LocalState, where the app keeps everything.
std::wstring LocalFolder();

// xbox.cfg's value for key, or fallback.
std::string Option(char const* key, std::string const& fallback = "");
