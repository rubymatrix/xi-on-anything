# iPhone and iPad: developer notes

The iOS build is the macOS host on UIKit: the same recompiled client, platform layer and Metal back
end, with SDL3's UIKit video, audio and GameController back ends in place of Cocoa's. No emulator,
no JIT: LuaJIT runs as its interpreter, because iOS forbids writable code.

Status: playable. It builds for the simulator and devices, signs in to LandSandBoat, and runs the
game in landscape, sized to the screen, with touch controls. What is left is under
[Not yet done](#differences-from-macos). The player's guide is [platforms/ios.md](platforms/ios.md).

## Build on a Mac

Requirements: Xcode 15 or later (`install` uses `xcrun devicectl`) with the iOS SDK (deployment
target 17.0 or later), and Python 3.12+ with `capstone` and `pefile`. The simulator also needs an
iOS simulator runtime (`xcodebuild -downloadPlatform iOS`).

```sh
python3 tools/build_posix.py prepare --game /path/to/FINAL-FANTASY-XI
python3 tools/build_ios.py app                       # build/ios-iphonesimulator/XI on Anything.app
python3 tools/build_ios.py run -- --game /path/to/FINAL-FANTASY-XI --server 127.0.0.1
```

`run` installs the build on the booted simulator, or boots the newest iPhone, and starts the app
with its log in the terminal. `--device <simulator name>` picks the simulator. Arguments after `--`
go to host64. The simulator reads the Mac's files, so `--game` can point at the install on the Mac.

Objects go to `build/ios-<sdk>/obj`; the desktop objects in `build/` are left alone.
`third_party/sdl3-ios` is SDL 3.4.16's UIKit tree, vendored by `tools/vendor.py sdl3-ios` (as
`tools/vendor.py sdl3` vendors the Cocoa tree).

## Device builds

```sh
python3 tools/build_ios.py install --bundle-id com.you.xionanything
```

`install` builds for the device, signs it and installs it on a connected iPhone or iPad
(`--device` picks one). It signs with the first Apple Development identity in the keychain and the
newest unexpired profile Xcode made for the bundle id; `--sign-identity` and `--profile` pick
others. `app --sdk iphoneos` builds and signs without installing.

**A free Apple account (a Personal Team)** works, with these limits:
- **App id:** it must be your own. A free team can't register one another team owns.
- **Signature:** it lasts 7 days, so install again weekly.
- **Set-up, once:**
  - Sign in to Xcode (Settings > Accounts).
  - Turn on Developer Mode on the device (Settings > Privacy & Security). It restarts and asks
    you to confirm.
  - Have Xcode make the certificate and a profile for the app id: build any small app with that id
    and automatic signing to the connected device, so Xcode registers the device to the team. A
    profile made before that won't install on it.

The app's icon is `assets/icon-ios.png` (made by `tools/make_ios_icon.swift`), compiled into the
bundle with `actool`.

On iOS the guest's memory is a 2 GB reserved window (`runtime/portable/gwin.c`), a 32-bit process's
address space; the desktop reserves 4 GB. The game is told it has 2 GB (`GetSystemInfo`), nothing
is handed out above it, and the thunks above it are call targets, never memory. The aim is a window
an app can reserve without the **Extended Virtual Addressing** entitlement, which may need a paid
account. That is untested on a device. The build still asks for that entitlement and **Increased
Memory Limit**, signs with the ones the profile grants, and drops the rest. The log shows the guest's memory every
ten seconds when it grows ("guest memory: ... top ...").

## Game files and arguments

The app shares its Documents folder through the Files app and Finder (`UIFileSharingEnabled`). Copy
the `FINAL FANTASY XI` folder, with its viewer folder beside it, into Documents. Without `--game`,
the app uses `Documents/FINAL FANTASY XI`.

A build with the PlayOnline client signs in to PlayOnline and updates these files first
([building.md](building.md#playonline-sign-in-on-iphone-ipad-and-android)), so the folder must stay
writable.

`Documents/run.args` adds host64 arguments, one per line, with no shell expansion (as on Android).
It can't set environment variables. The log goes to `Documents/host64.log`, or to the terminal with
`run`.

## Display

`host/display_fit.c` sizes the game for the screen, by the web port's rule:
- **Resolution:** the window has the screen's real pixels (`SDL_WINDOW_HIGH_PIXEL_DENSITY`). The
  game draws at the safe area's pixels, scaled down to a budget: about 2.4 MP on a phone, 4 MP on
  an iPad. `FFXI_MAX_PIXELS` overrides the budget.
- **Interface:** about one point per interface pixel, within 640x480 to 1200 lines. A phone 402
  points high gets 480 lines. `FFXI_UI_POINTS` sets the points per interface pixel. The player's
  UI Scale (Config > Advanced > Display > [Scaling]) overrides this while the result is at least
  640x480.
- **When:** the size goes into the registry before the game starts, and the game follows later
  changes of size or safe area (`modern_window_size`).
- **Safe area:** the frame fills its width at the screen's full height. A phone's Dynamic Island
  gets bars at the sides, and the home indicator sits over the game's foot. The pointer is mapped
  through the same rectangle (`user32_set_view`).
- **Wallpaper:** a picture fills the bars. Put `wallpaper.png`, `.jpg` or `.jpeg` in Documents, or
  build one in with `build_ios.py app --wallpaper picture`. It is the player's own, never in the
  source, and is cropped to fill the screen and dimmed.
- **Defaults:**
  - the scene effects are off (`FFXI_FX=0`). `fx` is a fixed key, so no menu row or `fx.txt`
    turns them on, and a device can't set the variable.
  - the frame-rate gauge is off (`FFXI_FPS=0`). Display > [Performance] FPS Counter turns it on.
  - at most 60 frames a second are presented (`FFXI_MAX_FPS`, 0 for no cap), against a ProMotion
    phone's 120. Frame Rate choices above 60 stay capped. Only the simulator can set the variable
    (`SIMCTL_CHILD_FFXI_MAX_FPS`).

`tests/display_fit_test.c` checks the sizes for phones and iPads.

## Touch controls

`host/touch.c` draws a gamepad over the game, from the sign-in screen on. It is SDL's virtual
gamepad, so the game sees an Xbox pad and uses its own button mapping.
- **Left stick:** starts where the thumb lands in the lower left.
- **Camera:** a drag anywhere else turns it (the right stick).
- **Buttons:**
  - the face buttons, with PlayStation's shapes
  - the D-pad, L1/R1, L2/R2 (the macro palette), SELECT and START
  - Target, above △: the F8 key
- **Taps:** a quick tap off the controls is a click.
- **Layout:** a safe area over 600 points high (as on an iPad) gets the tall layout.
  - Phone: the top row along the top, the D-pad upper left, the stick and face buttons in the
    lower corners.
  - Tall: everything at the foot. The stick and face buttons sit 17.5% of the height up, the rest
    above them, and SELECT/START at the very foot.
- **Toggle:** the gamepad button in the left bar (an iPad: the top-left corner) hides or shows the
  controls.
- **Moving them:** the lock beside the toggle, or a long press on the toggle, unlocks the layout.
  - Drag a group (the stick, the D-pad, the face buttons with Target, L1/L2, R1/R2, SELECT/START)
    anywhere in its box.
  - The lock again, or Done, keeps the layout. Reset puts every group back.
  - Layouts are kept per window shape (phone, iPad) in `Documents/touch-layout.txt`.
- **Pad:** past the lock (under it in a phone's bar, right of it on an iPad), opens Config > Gamepad
  (`C_PADCFG`). It hides with the other controls.
- **Real controllers:** one in use hides the controls and becomes the game's pad. A touch brings
  the controls back.
- **Back:** a swipe in from the left edge is Escape.
- **Mouse:** the simulator's mouse and an iPad's trackpad work the controls like a finger.
- **Keyboard up:** the controls dim.

**Keyboard:** the on-screen keyboard shows on the sign-in fields, and in game only while typing in
the chat line. A tap outside the text hides it, and the next brings it back. An iPad's hide-keyboard
key works too. While it is up, the view slides up just enough to keep the focused field or chat line
visible. The game's own soft keyboard (the PS2 client's) is off: its pad button, the 26th value of
`padsin000`, is set to none.

## Differences from macOS

- **Textures:** most iPhone GPUs can't sample BC (DXT) textures. When
  `supportsBCTextureCompression` is false, the Metal back end decodes DXT1-5 to BGRA8 on upload.
  `FFXI_BC=0` tests that path on a Mac.
- **Frame pacing:** the Metal layer always waits for the display. iOS has no `displaySyncEnabled`.
- **Closing:** Escape on the sign-in screen does nothing, since iOS apps don't quit. When the game
  ends (Shutdown, or Exit at its main menu), the app exits; returning from `SDL_main` would leave
  it running with a black window.
- **Not yet done:**
  - pausing in the background
  - an iPad in the simulator: its system keyboard is drawn upright over the landscape game
  - performance work
  - the scene effects (`FFXI_FX` is forced off)
