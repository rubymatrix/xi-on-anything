# Installing on the Xbox, and getting the game onto it

What works, what doesn't, and why, from getting XI on Xbox running on an Xbox One X in Developer
Mode (September 2026, console OS 26100 `xb_flt_2608ge`). The commands are `xbox/tools/xbox.py`, which
runs on any computer on the console's network (it was written on a Mac) and talks to Device Portal.

## The setup that works

The app is registered as a **loose app** from the console's development drive, and the game sits
next to it:

```
D:\DevelopmentFiles\LooseApps\XIonXbox\
    AppxManifest.xml, XIonXbox.exe, resources.pri, Assets\
    AppxMetadata\CodeIntegrity.cat          <- the exe's catalog; the console won't start it without one
    SquareEnix\FINAL FANTASY XI\            <- the player's install, 65k files, about 14 GB
```

The shell looks for the game beside the app (`PackagedGameFolder()` in `shell/GameCopy.cpp`), so no
setting is needed. The game is uploaded once. An update replaces the exe and its catalog, which is
about 45 MB, and never touches the game.

```
XBOX=<console address> python3 xbox/tools/xbox.py setup build/xbox/XIonXbox-xbox.msix "<...>/FINAL FANTASY XI"
python3 xbox/tools/xbox.py deploy build/xbox/XIonXbox-xbox.msix      # every build after that
python3 xbox/tools/xbox.py logs <folder>                          # host64.log, app.log, samples.bin
python3 xbox/tools/xbox.py status                                 # what's on the console
```

`deploy` and `setup` take the **app-only** package (`deploy-xbox.ps1 -PackageOnly`), not
`XIonXbox-with-game.msix`.

## The console's two drives

| drive | what | size |
|---|---|---|
| `U:` | user data: every app's `LocalState` (`U:\Users\UserMgr1\AppData\Local\Packages\XIonXbox_...`) | **about 3.2 GB free** |
| `D:\DevelopmentFiles` | dev-mode apps: installed packages (`WindowsApps\`) and loose apps (`LooseApps\`) | large; holds the game |

**The game does not fit in LocalState.** The first XI on Xbox could copy the game from a computer
into LocalState. On this console that showed about 2.9 GB free of 3.2 GB and couldn't finish, so the
app no longer offers it (a finished copy there is still used).

## What didn't work

| approach | result |
|---|---|
| The package with the game (`package-with-game.ps1`, 15 GB), through Device Portal's **Add page in a browser** | The upload failed every time. |
| The same package, **streamed** (`xbox/tools/xbox.py install`) | Works: the upload takes minutes, then the console installs for **30–60 min** (it checks all 66k files at about 8 MB/s). Every update is another 15 GB, and a lower-version or loose app can't replace it later. |
| **Register from a network share** (Dev Home / Device Portal, `\\<mac>\share`) | `0x80073CF0` "Failed to register package", with both macOS File Sharing and Samba. The console connected, signed in, read `AppxManifest.xml` in full and closed it without an error, then gave up. It never opened the exe. The cause is unknown. The same small layout registers fine from `LooseApps`. |
| The app **reading the game straight off an SMB share** (`game.txt` = `\\host\share\...`) | The app shows "cannot open that FINAL FANTASY XI folder (error 65)" (`ERROR_NETWORK_ACCESS_DENIED`). No connection reached the server: the app sandbox refuses before sending anything. The `enterpriseAuthentication` capability and a guest-readable share didn't change that. |
| The game in **LocalState** (the in-app copy, or Device Portal uploads) | Doesn't fit on `U:` (above). |

## Quirks, and what `xbox/tools/xbox.py` does about them

**Launching**
- **The loose folder needs `AppxMetadata\CodeIntegrity.cat`, and it must be the one built with that exe.**
  Without it the app registers fine but won't start: Dev Home says **"Please try again"**, and Device
  Portal's launch returns `0x80070002`. After replacing the catalog, **register again**. `deploy` does both.
  Windows desktop doesn't enforce this, so `deploy.ps1`'s loose layout isn't a guide here.
- After registering, check that the app's type is **Game** in Dev Home (View details). As an App it gets
  about 1 GB of RAM. It hasn't been checked whether re-registering resets it.

**Registering**
- **A loose app can't replace an installed package**, or the reverse: `0x80073CFB` "An unpackaged version
  cannot replace this" (and the reverse message). Uninstall first (`xbox/tools/xbox.py uninstall`).
- **Uninstalling left LocalState in place on this console** (settings, `USER`, shader cache, even an old
  `game.txt`). `uninstall` still backs it up to `build/` first.
- Registering the same version again works, and a lower one might not. `deploy` writes the **registered**
  version into the manifest, since builds from `deploy-xbox.ps1` may carry a lower one.
- Loose registration is `POST /api/app/packagemanager/register?folder=<base64 of the LooseApps subfolder>`,
  then poll `/api/app/packagemanager/state`: it returns 204 while working, then a JSON `Success` / `Reason`.

**Device Portal's file API** (`/api/filesystem/apps/...`, known folders `DevelopmentFiles` and `LocalAppData`)
- Every write needs the `CSRF-Token` cookie from an earlier response echoed as an `X-CSRF-Token` header.
- A POST with no body needs `Content-Length: 0`, or IIS answers 411.
- **Uploads don't create folders.** Uploading into a missing folder returns 500 "The system cannot find the
  path specified" / "File move failed". Make each folder first (`POST .../folder?path=<parent>&newfoldername=<name>`).
- **Listing a folder that doesn't exist returns an empty list, not an error.**
- **LocalState doesn't exist until the app's first run.** Before that, uploads to it fail with a 500 and
  `mkdir` returns 200 without making anything. Launch the app once.
- Deleting a folder, with everything in it: `DELETE .../folder?path=<parent>&filename=<folder name>`.
  With `path=<the folder itself>` it answers 400 "Missing fileName".
- A file is fetched with `GET .../file?path=<folder>&filename=<name>`.
- **A rebuilt file can keep its size.** `CodeIntegrity.cat` is 1764 bytes build after build, so a push
  that skips same-size files skips it. `deploy` sends every app file.
- LAN speed was about 10–30 MB/s for large files and much slower for small ones. The first game upload took
  about 40 minutes. `game` resumes: run it again and it sends only what's missing.
- **Leave the local game folder where it is until an upload finishes.** Moving it mid-push makes every
  remaining file fail ("No such file"). Running it again from the new place resumes.
- `FINAL FANTASY XI\TEMP` holds logs the game writes on the computer. `game` doesn't send it.

**Which game folder the app uses** (`xbox/app/GameFolder.cpp`): `LocalState\game.txt` if present, else the game
beside the app, else a LocalState copy. **A leftover `game.txt`
overrides the game beside the app**; `status` flags it, and `xbox/tools/xbox.py rm local:\LocalState game.txt`
removes it. A leftover `LocalState\SquareEnix` from an unfinished in-app copy uses up the small drive;
remove it with `xbox/tools/xbox.py rmdir local:\LocalState SquareEnix`.

**Space**
- Updating an installed package left the old version's folder (with its 14 GB) under
  `D:\DevelopmentFiles\WindowsApps\XIonXbox_0.2.0.0_...`. `status` lists these. Removing them through Device
  Portal hasn't been tried.

## Error codes seen

| code | where | meaning here |
|---|---|---|
| `0x80073CF0` | registering from a network share | The package couldn't be opened. See above; use `LooseApps` instead. |
| `0x80073CFB` | registering a loose app over a package (or the reverse) | Uninstall the other one first. |
| `0x80070002` | launching through Device Portal | Here: no `CodeIntegrity.cat` in the loose folder. |
| "Please try again" | launching from Dev Home | Same as above. |
| error 65 (`ERROR_NETWORK_ACCESS_DENIED`) | the app opening a `\\server\share` game folder | The sandbox blocks it; no fix found. |
| HTTP 411 | Device Portal POST without a body | Send `Content-Length: 0`. |
| HTTP 500 "File move failed" | Device Portal upload | The target folder doesn't exist (or LocalState isn't made yet). |

## Logs

The app writes to its LocalState: `host64.log` (every `[app]`, `[signin]`, `[lsb]`, `[recomp]` and `[gfx]` line),
`app.log` (the shell), and, with profiling on, `samples.bin`. `xbox/tools/xbox.py logs <folder>` copies them.
`samples.bin` has to be read against the linker map of **the same build** of `XIonXbox.exe`:
`tools/sample_report.py samples.bin <exe.map>`. That map is on the machine that built the exe.
