# iPhone and iPad (experimental, in progress)

The iOS build is the macOS host on UIKit: the same recompiled client, platform layer and Metal back
end, with SDL3's UIKit video, audio and GameController back ends in place of Cocoa's. It uses no
emulator or JIT. LuaJIT runs as its interpreter, because iOS forbids writable code.

Status: it compiles and links for the simulator and for devices. In the iOS 27 simulator (iPhone
18 Pro) it signs in to a local LandSandBoat server and runs the game in landscape, sized to the
screen, with touch controls. It has not run on a device yet. The design notes for the display and
the controls are in [ios-design.md](ios-design.md).

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
python3 tools/build_ios.py app --sdk iphoneos --sign-identity "Apple Development: ..." \
    --profile path/to/profile.mobileprovision
```

The guest's memory is a 4 GB reserved window (`runtime/guest.h`), more than an iOS app's default
address space allows. The provisioning profile's app ID needs the **Extended Virtual Addressing**
capability, and preferably **Increased Memory Limit** too. The build signs with the entitlements the
profile grants and drops the rest. If a profile lacks extended virtual addressing, the app can
install but will fail to reserve guest memory.

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
- **Safe area:** the frame is presented into it, with black bars by a phone's Dynamic Island. The
  pointer is mapped through the same rectangle (`user32_set_view`).

`tests/display_fit_test.c` checks the sizes for phones and iPads.

## Touch controls

`host/ios_touch.c` draws a gamepad over the game, from the sign-in screen on. It is SDL's virtual
gamepad, so the game sees an Xbox pad and uses its own button mapping.
- **Left stick:** it starts where the thumb lands in the lower left.
- **Camera:** a drag anywhere else on the game turns it (the right stick).
- **Buttons:**
  - the face buttons, with PlayStation's shapes
  - the D-pad, L1/R1, L2/R2 (the macro palette), SELECT and START
  - Target, above △: the F8 key
- **Taps:** a quick tap off the controls is a click, for the game's menus and targets.
- **Toggle:** the gamepad button in the left bar (an iPad: the top-left corner) hides the controls
  or brings them back.
- **Real controllers:** one in use hides the controls and becomes the game's pad. A touch on the
  screen brings the controls back.
- **Back:** a swipe in from the left edge is Escape.
- **Mouse:** the simulator's mouse, and an iPad's trackpad, work the controls like a finger.

**Keyboard:** the on-screen keyboard shows on the sign-in screen's fields, and in game only while
the chat line is typed in. A tap outside the text puts it away, and the next brings it back. An
iPad's key that hides it works too.

## Differences from macOS

- **Textures:** most iPhone GPUs cannot sample BC (DXT) textures. When
  `supportsBCTextureCompression` is false, the Metal back end decodes DXT1-5 to BGRA8 on upload.
  To test that path on a Mac, set `FFXI_BC=0`.
- **Frame pacing:** the Metal layer always waits for the display. iOS has no `displaySyncEnabled`.
- **Never closes itself:** Escape on the sign-in screen does nothing, as an iOS app does not quit.
- **Not yet done:**
  - pausing in the background
  - keeping the sign-in fields above the on-screen keyboard, which covers the lower half
  - an iPad in the simulator: its system keyboard is drawn upright over the landscape game
  - performance work
