# iPhone and iPad: developer notes

The iOS build is the macOS host on UIKit: the same recompiled client, platform layer and Metal back
end, with SDL3's UIKit video, audio and GameController back ends in place of Cocoa's. It uses no
emulator or JIT. LuaJIT runs as its interpreter, because iOS forbids writable code.

Status: done. It builds for the simulator and for devices, signs in to LandSandBoat and runs the
game in landscape, sized to the screen, with touch controls. The step-by-step guide is
[platforms/ios.md](platforms/ios.md); these are the notes for working on it.

## Build on a Mac

Requirements: Xcode with the iOS SDK (17.0 or later as the deployment target), and Python 3.12+
with `capstone` and `pefile`. For the simulator you also need an iOS simulator runtime
(`xcodebuild -downloadPlatform iOS`).

```sh
python3 tools/build_posix.py prepare --game /path/to/FINAL-FANTASY-XI
python3 tools/build_ios.py app                       # build/ios-iphonesimulator/XI on Anything.app
python3 tools/build_ios.py run -- --game /path/to/FINAL-FANTASY-XI --server 127.0.0.1
```

`run` installs the simulator build on the booted simulator, or boots the newest iPhone if none is
running. It then starts the app with its log in the terminal. Arguments after `--` go to host64.
The simulator can read the Mac's files, so `--game` can name the install on the Mac.

Objects go to `build/ios-<sdk>/obj`. The desktop objects in `build/` are left alone.
`third_party/sdl3-ios` is SDL 3.4.16's UIKit tree. `tools/vendor.py sdl3-ios` vendors it, as
`tools/vendor.py sdl3` does for the Cocoa tree.

## Device builds

```sh
python3 tools/build_ios.py install --bundle-id com.you.xionanything
```

`install` builds for the device, signs it and installs it on a connected iPhone or iPad
(`--device` picks one). It signs with the first Apple Development identity in the keychain and the
newest unexpired profile Xcode made for the bundle id. `--sign-identity` and `--profile` name them
instead; `app --sdk iphoneos` builds and signs without installing.

**A free Apple account (a Personal Team)** works, with these limits:
- **App id:** it must be one of your own: a free team can't register one another team owns.
- **Signature:** it lasts 7 days, so install again weekly.
- **Set-up, once:**
  - Sign in to Xcode (Settings > Accounts).
  - Turn on Developer Mode on the device (Settings > Privacy & Security). It restarts and asks
    you to confirm.
  - Get Xcode to make the certificate and a profile for the app id. One way is a device build of
    any small app with that id and automatic signing, with the device connected, so Xcode registers
    it to the team. A profile made before then won't install on that device.

The app's icon is `assets/icon-ios.png` (made by `tools/make_ios_icon.swift`), compiled into the
bundle with `actool`.

On iOS the guest's memory is a 2 GB reserved window (`runtime/portable/gwin.c`), the address space of
a 32-bit process, where the desktop reserves 4 GB. The game is told it has 2 GB (`GetSystemInfo`),
nothing is handed out above it, and the thunks above it are call targets, never memory. The aim is a
window an app can reserve without the **Extended Virtual Addressing** entitlement, which may need a
paid account; that has not been tried on a device. The build still asks for that entitlement and
**Increased Memory Limit**, signs with the ones the profile grants, and drops the rest. The log shows
the guest's memory every ten seconds when it grows ("guest memory: ... top ...").

## Game files and arguments

The app shares its Documents folder through the Files app and Finder (`UIFileSharingEnabled`). Copy
the `FINAL FANTASY XI` folder, with its viewer folder beside it as on desktop, into the app's
Documents. Without `--game`, the app uses `Documents/FINAL FANTASY XI`.

`Documents/run.args` adds arguments, one per line, with no shell expansion (as on Android). The log
goes to `Documents/host64.log` unless the app runs with a terminal (`run`).

## Display

`host/display_fit.c` sizes the game for the screen, by the web port's rule:
- **Resolution:** the window has the screen's real pixels (`SDL_WINDOW_HIGH_PIXEL_DENSITY`). The
  game draws at the safe area's pixels, scaled down to a pixel budget: about 2.4 MP on a phone and
  4 MP on an iPad. `FFXI_MAX_PIXELS` overrides the budget.
- **Interface:** about one point an interface pixel, within 640x480 to 1200 lines. A phone's 402
  points high gives 480 lines. `FFXI_UI_POINTS` sets the points an interface pixel.
- **When:** the size goes into the registry before the game starts, and the game follows a later
  change of size or safe area (`modern_window_size`).
- **Safe area:** the frame is presented into its width at the screen's full height. A phone's
  Dynamic Island gets bars at the sides, and the home indicator is drawn over the game's foot. The
  pointer is mapped through the same rectangle (`user32_set_view`).
- **Wallpaper:** a picture fills the bars. Put `wallpaper.png` or `wallpaper.jpg` in the app's
  Documents, or build one in with `build_ios.py app --wallpaper picture`. It is the player's own,
  never in the source. It is cropped to fill the screen and dimmed.
- **Defaults:**
  - the scene effects are off (`FFXI_FX=0`), and so is the frame-rate gauge (`FFXI_FPS=0`). Config >
    Modern turns them on.
  - at most 60 frames a second are presented (`FFXI_MAX_FPS`, 0 for no cap), against a ProMotion
    phone's 120

`tests/display_fit_test.c` checks the sizes for phones and iPads.

## Touch controls

`host/touch.c` draws a gamepad over the game, from the sign-in screen on. It is SDL's virtual
gamepad, so the game sees an Xbox pad and uses its own button mapping.
- **Left stick:** it starts where the thumb lands in the lower left.
- **Camera:** a drag anywhere else on the game turns it (the right stick).
- **Buttons:**
  - the face buttons, with PlayStation's shapes
  - the D-pad, L1/R1, L2/R2 (the macro palette), SELECT and START
  - Target, above △: the F8 key
- **Taps:** a quick tap off the controls is a click, for the game's menus and targets.
- **Layout:**
  - A phone's has the top row along the top, the D-pad upper left, and the stick and face buttons
    in the lower corners.
  - An iPad's gathers everything at the foot. The stick and the face buttons sit 17.5% of the
    height up, with the rest above them.
- **Toggle:** the gamepad button in the left bar (an iPad: the top-left corner) hides the controls
  or brings them back.
- **Moving them:** the lock beside the toggle unlocks the layout; a long press on the toggle does too.
  - Drag a group (the stick, the D-pad, the face buttons with Target, L1/L2, R1/R2, SELECT/START)
    anywhere in its box.
  - The lock again, or Done, keeps the layout. Reset puts every group back.
  - Layouts are kept per window shape (a phone's, an iPad's) in `Documents/touch-layout.txt`.
- **Real controllers:** one in use hides the controls and becomes the game's pad. A touch on the
  screen brings the controls back.
- **Back:** a swipe in from the left edge is Escape.
- **Mouse:** the simulator's mouse, and an iPad's trackpad, work the controls like a finger.
- **Keyboard up:** the controls dim while iOS's keyboard is up.

**Keyboard:** the on-screen keyboard shows on the sign-in screen's fields, and in game only while
the chat line is typed in. A tap outside the text puts it away, and the next brings it back. An
iPad's key that hides it works too. While it is up, the view slides up just far enough to keep the
focused field, or the chat line, above it. The game's own soft keyboard (the PS2 client's) is off:
the pad button that opens it, the 26th value of `padsin000`, is set to none.

## Differences from macOS

- **Textures:** most iPhone GPUs cannot sample BC (DXT) textures. When
  `supportsBCTextureCompression` is false, the Metal back end decodes DXT1-5 to BGRA8 on upload.
  To test that path on a Mac, set `FFXI_BC=0`.
- **Frame pacing:** the Metal layer always waits for the display. iOS has no `displaySyncEnabled`.
- **Closing:** Escape on the sign-in screen does nothing, as an iOS app does not quit. When the game
  ends (Shutdown, or Exit at its main menu), the app exits: returning from `SDL_main` would leave it
  running with a black window.
- **Not yet done:**
  - pausing in the background
  - an iPad in the simulator: its system keyboard is drawn upright over the landscape game
  - performance work
