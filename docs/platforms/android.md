# Android (experimental)

The recompiled game as a native ARM64 Android app, drawn with Vulkan. No emulator, Wine or DXVK.
Developer notes are in [../android.md](../android.md).

> [!IMPORTANT]
> **Status**: experimental, tested on a Pixel Fold. The APK is a debug build. The scene effects
> are not available, and addons are off by default.

## What you need

- An ARM64 phone or tablet with Vulkan (the app requires it). Android 10 or later for a launcher
  install; a build from source without `--native-tls` runs on Android 9. The game runs in landscape
  only.
- About 14 GB free on the phone for the game.
- A macOS, Linux or Windows computer, and USB or wireless debugging turned on on the phone.
- Your `FINAL FANTASY XI` folder and a LandSandBoat server ([README.md](README.md)).

## Get it

**From the launcher:** open Phone > Android. It fetches the Android tools, lists the phones `adb`
sees, then builds the app, signs it and installs it. It makes a signing key for you: back it up.
Android accepts an update only when it is signed with the same key. Updates keep the app's files.

**From source:** see [Build from source](../android.md#build-from-source).

## The game on the phone

The launcher copies the game to the phone and, after a game update, only the files that changed.
Its Game Location picks where:

- the app's own folder, `/sdcard/Android/data/com.rubymatrix.xionanything/files/FINAL FANTASY XI`
  (removed with the app), or
- `XI on Anything` on internal storage or an SD card, which outlive the app. The app then asks for
  All files access before the game starts.

The app opens on the sign-in screen. Pick your server there, or send an account from the launcher;
the phone still waits for you to tap Sign in.

## Controls

- **Touch:** a gamepad over the game, from the sign-in screen on. The left stick starts where your
  thumb lands, a drag elsewhere turns the camera, and a quick tap is a click. The lock beside the
  gamepad button lets you drag the controls where you want them. Pad opens Config > Gamepad.
- **A controller** hides the touch controls; a touch brings them back. Config > Gamepad's Sticks
  rows fix a controller whose sticks read wrong.
- **Keyboard:** the on-screen keyboard shows on the sign-in fields and while you type in the chat
  line.
- **Back** is Escape. It never closes the app.

## Settings and logs

Config > Advanced has Display, Interface and Gamepad. Graphics, Shadows, Lighting, Atmosphere and
Water are not in the Android build, and Addons shows only with `--android-addons 1` in `run.args`.

The log and settings are in `/sdcard/Android/data/com.rubymatrix.xionanything/files/data`:

| File | What it is |
| --- | --- |
| `host64.log` | The log |
| `settings.reg` | Your settings |
| `USER/` | The game's `USER` folder |
| `cache/` | Shader and effect files |
| `gamepads.txt` | Controller mappings from Config > Gamepad |

Beside `data`, in `files`, you can add `run.args` (one argument per line) and `wallpaper.png` or
`wallpaper.jpg` to fill the bars beside the game. Your touch layout is kept there too, in
`touch-layout.txt`. `run.args` replaces the app's whole command line, so copy the defaults from
[the developer notes](../android.md#build-from-source) and keep `--data-dir`.

## Troubleshooting

- **The phone isn't listed:** turn on USB or wireless debugging and accept the phone's prompt.
- **The update won't install:** it was signed with a different key. Use the key you backed up.
- **The app closes at once with no log:** `run.args` has no `--data-dir`. Fix or remove it.
- **The game doesn't start from a folder outside the app:** allow All files access for XI on
  Anything in Android's settings.
- **Anything else:** read `data/host64.log`.
