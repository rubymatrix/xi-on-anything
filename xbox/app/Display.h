// The resolution the game renders at: the screen's by default, or xbox.cfg's Resolution.
#pragma once

#include <string>
#include <utility>

using Resolution = std::pair<int, int>; // width, height in pixels

// The screen's resolution: on an Xbox, the TV's output mode (HdmiDisplayInformation: 1080p, 1440p,
// 4K...); elsewhere, the app's view in pixels. Call on the UI thread.
Resolution ScreenResolution();

// "1920x1080" -> {1920, 1080}; {0, 0} for anything else (and for "", which means the screen's).
Resolution ParseResolution(std::string const& text);

// Sets the game's resolution in a settings.reg (REGEDIT4): the window ("0001"/"0002"), the
// interface ("0037"/"0038": half size from 1080 lines up, so text stays legible; full size below)
// and the background ("0003"/"0004": the window's width rounded up to a power of two, from 1024 to
// 2048 - the game's own 4096 is more than an Xbox One's GPU has).
// Everything else in the file - the game's saved settings - is left as it is.
bool SetGameResolution(std::wstring const& reg_path, Resolution r);
