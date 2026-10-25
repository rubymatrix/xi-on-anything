# iPhone and iPad (experimental, in progress)

The macOS host on UIKit: the same recompiled client and Metal back end, with SDL3's UIKit video,
audio and game controller support. No emulator and no JIT (LuaJIT runs as its interpreter, since iOS
forbids writable code).

Branch: `port/iphone` (`git checkout port/iphone`). Developer notes: `docs/ios.md` on that branch.

**Status**: builds for the simulator and for devices. In the iOS 27 simulator (iPhone 18 Pro) it
reaches the sign-in screen in landscape and the on-screen keyboard types into it. It has not signed
in to a server or run on a device yet. Expect the steps for devices below to need adjusting.

## What you need

- A Mac with Xcode and the iOS SDK (deployment target iOS 17.0 or later).
- Python 3.12 or later with `capstone` and `pefile`.
- For the simulator: an iOS simulator runtime (`xcodebuild -downloadPlatform iOS`).
- For a device: an Apple Developer account whose App ID can have the **Extended Virtual Addressing**
  capability (and, preferably, **Increased Memory Limit**). A free personal team can't add these
  capabilities as far as we know; that is unverified.
- Your `FINAL FANTASY XI` folder, with the viewer folder beside it if your install has one.
- A controller or keyboard: touch controls aren't done yet.

## Simulator

```bash
git checkout port/iphone
python3 tools/build_posix.py prepare --game /path/to/"FINAL FANTASY XI"
python3 tools/build_ios.py app          # build/ios-iphonesimulator/XI on Anything.app
python3 tools/build_ios.py run -- --game /path/to/"FINAL FANTASY XI" --server <your server>
```

`run` installs the build on the booted simulator (or boots the newest iPhone; `--device <name>` picks
one) and starts it with its log in the terminal. Arguments after `--` go to host64. The simulator
reads the Mac's files, so `--game` can name the install on the Mac.

## Device

1. **Make an App ID and profile.** In the Apple Developer site (Certificates, Identifiers &
   Profiles): an App ID for `com.rubymatrix.xionanything` (or your own), with **Extended Virtual
   Addressing** and **Increased Memory Limit** ticked; then a development provisioning profile for it
   that includes your device. Download the `.mobileprovision`.
2. **Build and sign:**

   ```bash
   python3 tools/build_ios.py app --sdk iphoneos \
       --sign-identity "Apple Development: <you> (<team>)" \
       --profile path/to/profile.mobileprovision
   ```

   `security find-identity -v -p codesigning` lists your identities. The build signs with the
   entitlements the profile grants and drops the rest.
3. **Install** with the device connected (or paired over Wi-Fi), using Xcode's tools:

   ```bash
   xcrun devicectl list devices
   xcrun devicectl device install app --device <device id> "build/ios-iphoneos/XI on Anything.app"
   ```

   Or drag the `.app` onto the device in Xcode's Devices and Simulators window. The first launch may
   need Settings, General, VPN & Device Management, trust your developer certificate, and Developer
   Mode turned on (Settings, Privacy & Security).

The guest's memory is a 4 GB reserved window, more than an iOS app may reserve by default. Without
Extended Virtual Addressing in the profile the app installs but fails to reserve guest memory at
start.

## Game files and arguments

The app's Documents folder is shared through the Files app and Finder (`UIFileSharingEnabled`).

1. Connect the device to the Mac, select it in Finder, open **Files**, and drag the
   `FINAL FANTASY XI` folder (and the viewer folder, if any) onto **XI on Anything**. It's about
   14 GB.
2. Without `--game`, the app uses `Documents/FINAL FANTASY XI`.
3. To pass other arguments, put a `run.args` file in Documents: one argument per line, no shell
   expansion. For example:

   ```
   --server
   play.example.net
   ```

The log goes to `Documents/host64.log` unless the app was started with a terminal (`run`).

## Differences from macOS

- Most iPhone GPUs can't sample BC (DXT) textures; the back end decodes them to BGRA8 on upload.
  `FFXI_BC=0` tests that path on a Mac.
- The frame rate always follows the display's refresh.
- Not done yet: touch controls, safe-area and notch layout, pausing in the background, keeping the
  sign-in fields above the on-screen keyboard (it covers the lower half), performance work.
