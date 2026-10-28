# iPhone and iPad

The macOS host on UIKit: the same recompiled client and Metal back end, with SDL3's UIKit video,
audio and game controller support, and touch controls drawn over the game. No emulator and no JIT
(LuaJIT runs as its interpreter, since iOS forbids writable code). Developer notes, the display
sizing and every touch control are in [../ios.md](../ios.md).

## What you need

- A Mac with Xcode and the iOS SDK (deployment target iOS 17.0 or later).
- Python 3.12 or later with `capstone` and `pefile`.
- For the simulator: an iOS simulator runtime (`xcodebuild -downloadPlatform iOS`).
- For a device: an Apple account signed in to Xcode. A free one (a Personal Team) works; its
  signature lasts 7 days, so install again weekly.
- Your `FINAL FANTASY XI` folder, with the viewer folder beside it if your install has one.

## Simulator

```bash
python3 tools/build_posix.py prepare --game /path/to/"FINAL FANTASY XI"
python3 tools/build_ios.py app          # build/ios-iphonesimulator/XI on Anything.app
python3 tools/build_ios.py run -- --game /path/to/"FINAL FANTASY XI" --server <your server>
```

`run` installs the build on the booted simulator (or boots the newest iPhone) and starts it with its
log in the terminal. Arguments after `--` go to host64. The simulator reads the Mac's files, so
`--game` can name the install on the Mac.

## Device

1. **Once:** sign in to Xcode (Settings > Accounts), and turn on Developer Mode on the device
   (Settings > Privacy & Security; it restarts and asks you to confirm). Then get Xcode to make a
   certificate and profile for your app id, e.g. a device build of any small app with that id and
   automatic signing, with the device connected. A free team can only use an id of its own.
2. **Build, sign and install** with the device connected:

   ```bash
   python3 tools/build_ios.py install --bundle-id com.you.xionanything
   ```

   It signs with the first Apple Development identity in the keychain and the newest profile Xcode
   made for the id (`--sign-identity` and `--profile` name them instead; `--device` picks a device).

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

## Playing

- **Touch controls:** a gamepad over the game, from the sign-in screen on. The left stick starts
  where your thumb lands, a drag elsewhere turns the camera, and a quick tap is a click. The lock
  beside the gamepad button lets you drag the controls where you want them.
- **A controller** hides the touch controls and takes over; touching the screen brings them back.
- **The keyboard** shows on the sign-in fields and while you type in the chat line, with the view
  kept above it.
- **Defaults:** the scene effects and the frame-rate gauge are off, and the frame rate is capped at
  60. Config > Modern turns the effects on.
- **Wallpaper:** put `wallpaper.png` or `wallpaper.jpg` in Documents to fill the bars beside the
  game.
