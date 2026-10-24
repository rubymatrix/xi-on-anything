# iPhone and iPad (experimental, in progress)

The iOS build is the macOS host on UIKit: the same recompiled client, platform layer and Metal back
end, with SDL3's UIKit video, audio and GameController back ends in place of Cocoa's. It uses no
emulator or JIT. LuaJIT runs as its interpreter, because iOS forbids writable code.

Status: it compiles and links for the simulator and for devices. In the iOS 27 simulator (iPhone
18 Pro) it reaches the sign-in screen in landscape, and the on-screen keyboard types into it. It has
not signed in to a server or run on a device yet.

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

## Differences from macOS

- **Textures:** most iPhone GPUs cannot sample BC (DXT) textures. When
  `supportsBCTextureCompression` is false, the Metal back end decodes DXT1-5 to BGRA8 on upload.
  To test that path on a Mac, set `FFXI_BC=0`.
- **Frame pacing:** the Metal layer always waits for the display. iOS has no `displaySyncEnabled`.
- **Not yet done:**
  - touch controls (use a controller or a keyboard)
  - safe-area and notch layout
  - pausing in the background
  - keeping the sign-in fields above the on-screen keyboard, which covers the lower half
  - performance work
