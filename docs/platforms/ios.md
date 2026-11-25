# iPhone and iPad

The macOS host on UIKit: the same recompiled client and Metal back end, with SDL3 for video, audio
and controllers, and touch controls over the game. No emulator and no JIT: LuaJIT runs as its
interpreter, since iOS forbids writable code. Developer notes are in [ios-dev.md](ios-dev.md).

There are two ways to play on an iPhone or iPad:

- **The XI on Anything app** (below): the launcher and the game in one app, installed with
  [Sideloadly](https://sideloadly.io/). It manages your game installs and makes the game on the
  phone from your own copy of FINAL FANTASY XI. Nothing of Square Enix's is in the app.
- **A build of your own** on a Mac with Xcode ([What you need](#what-you-need) on): the game alone,
  your install's translation built into it.

## The XI on Anything app (Sideloadly)

1. **Install it.** Open `XI-on-Anything.ipa` in Sideloadly with the iPhone connected, sign in with
   your Apple Account (a free one works; its apps run for 7 days, then install again: your
   accounts, installs and the game the app made stay). On the iPhone, trust the account in
   Settings > General > VPN & Device Management, and turn on Developer Mode (Settings > Privacy &
   Security) if it asks.
2. **Add your game.** In the app's Installs tab: download it from Square Enix (about 8 GB, then
   updated from PlayOnline), get a private server's client from its mirror (a `.zip`, `.tar` or
   `.tar.gz` of the game folder, or a folder served as a listing), copy a FINAL FANTASY XI folder
   in from Files (iCloud Drive, a USB drive or card reader, On My iPhone), or put one in the app's
   own folder with Finder (your iPhone > Files > XI on Anything) and use it where it is.
3. **Add your signing certificate.** iOS runs only code signed by the Apple Account the app was
   installed with, so the app signs the game it makes with your development certificate: Settings >
   Game on this iPhone > Signing certificate, then pick its `.p12` in Files and give its password.
   - A paid Apple Developer account: Keychain Access > My Certificates > "Apple Development: …" >
     Export (a `.p12` with a password of your choosing), then AirDrop it or put it in iCloud Drive.
   - A free account: Sideloadly keeps its key on your computer, so export one from a tool that
     gives it to you (AltStore or SideStore's Export Certificate, or Impactor's Export P12), signed
     in with the same Apple Account.
   The certificate's team has to be the app's: the app says if it isn't.
4. **Play.** The first Play makes the game: it translates your FFXiMain.dll and FFXi.dll, compiles
   and links them with the app's own compiler, and signs the result (a few minutes; an iPhone 17 Pro
   takes under three). After that Play starts at once, until the game or the app is updated.

Settings > Game on this iPhone > Make the game again makes it again on the next Play. Without a
certificate there is one other way: build `XIGame.dylib` on a Mac (`python3 tools/build_ios.py kit
--sdk iphoneos`, from your own prepared install) and add it in Sideloadly's Advanced Options >
Inject dylibs/frameworks: the app plays it as it is.

## What you need

- A Mac with Xcode 15 or later and the iOS SDK (deployment target iOS 17.0 or later).
- Python 3.12 or later with `capstone` and `pefile`.
- For the simulator: an iOS simulator runtime (`xcodebuild -downloadPlatform iOS`).
- For a device: an Apple account signed in to Xcode. A free one (a Personal Team) works, but its
  signature lasts 7 days, so install again weekly.
- Your `FINAL FANTASY XI` folder, and the viewer folder beside it if you have one.

## Simulator

```bash
python3 tools/build_posix.py prepare --game /path/to/"FINAL FANTASY XI"
python3 tools/build_ios.py app          # build/ios-iphonesimulator/XI on Anything.app
python3 tools/build_ios.py run -- --game /path/to/"FINAL FANTASY XI" --server <your server>
```

`run` installs the build on the booted simulator, or boots the newest iPhone, and shows the log in
the terminal. `--device <simulator name>` picks the simulator. Arguments after `--` go to host64.
The simulator reads the Mac's files, so `--game` can point at the install on the Mac.

## Device

1. **Once:** sign in to Xcode (Settings > Accounts). Turn on Developer Mode on the device
   (Settings > Privacy & Security); it restarts and asks you to confirm. Then have Xcode make a
   certificate and profile for your app id: build any small app with that id and automatic
   signing to the connected device. A free team can only use an id of its own.
2. **Build, sign and install** with the device connected:

   ```bash
   python3 tools/build_ios.py install --bundle-id com.you.xionanything
   ```

   It signs with the first Apple Development identity in the keychain and the newest profile Xcode
   made for the id. `--sign-identity` and `--profile` pick others; `--device` picks a device.

## Game files and arguments

The app's Documents folder is shared through the Files app and Finder (`UIFileSharingEnabled`).

1. Connect the device, select it in Finder, open **Files**, and drag the `FINAL FANTASY XI` folder
   (and the viewer folder, if any) onto **XI on Anything**. It's about 14 GB.
2. Without `--game`, the app uses `Documents/FINAL FANTASY XI`.
3. For other arguments, put a `run.args` file in Documents, one argument per line, no shell
   expansion:

   ```
   --server
   play.example.net
   ```

The log goes to `Documents/host64.log`, or to the terminal with `run`.

## Playing

- **Touch controls:** a gamepad over the game, from the sign-in screen on. The left stick starts
  where your thumb lands, a drag elsewhere turns the camera, and a quick tap is a click. The lock
  beside the gamepad button lets you move the controls. The Pad button opens Config > Gamepad.
- **A controller** hides the touch controls and takes over. Touch the screen to bring them back.
- **The keyboard** shows on the sign-in fields and while you type in chat, with the view kept
  above it.
- **Scene effects:** off on iPhone and iPad for now. No setting turns them on.
- **Frame rate:** at most 60, even if Config > Advanced > Display > [Performance] Frame Rate is
  higher. FPS Counter, on the same page, shows the gauge.
- **Wallpaper:** put `wallpaper.png`, `wallpaper.jpg` or `wallpaper.jpeg` in Documents to fill the
  bars beside the game, or build one in with `build_ios.py app --wallpaper <picture>`.
