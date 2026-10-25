# Client versions

## Supported builds

Every supported build is listed in `meta/builds.json`, keyed by the SHA-256 of its retail
`FFXiMain.dll` and `FFXi.dll`. `tools/prepare.py` identifies the install's build and records it in
`generated/build.json`; the build tools read it from there.

| build | `FFXiMain.dll` SHA-256 | client version | taken from |
| --- | --- | --- | --- |
| 2026-08-22 | `6f8844eb…3c3b` | `30260805_0` | a retail install |
| 2026-09-03 | `f2245d1c…23e4` | `30260903_0` | a private-server install (no `patch.ver`) |
| 2026-10-05 | `04288488…e872` | `30261006_0` | a private-server install's `FFXiMain.dll`, `FFXi.dll` and `patch.ver` |
| 2025-12-26 | `f5ed4c3b…a7f6` | `30251226_0` | loose `FFXiMain.dll`/`FFXi.dll`, an older client than 2026-08-22 (no `patch.ver`) |
| 2025-11-12 | `bda769e2…0d9a` | `30251101_2` | a private-server install (`patch.ver` present) |

New labels are the date of the PE timestamp (2026-08-22 predates that rule).

## Supporting a new client version

A game update replaces `FFXiMain.dll` and often `FFXi.dll`. The metadata holds absolute addresses,
so every build needs its own entry before it can be translated. `prepare` refuses a build it
does not know:

```
.../FFXiMain.dll is build <sha256>, which meta/builds.json does not know
```

In Claude Code, the `game-version-update` skill (`.claude/skills/`) runs all of this: hand it the
install folder. By hand:

1. **Identify and unpack.**

   ```
   python tools/newbuild.py identify --game "<FINAL FANTASY XI>"   # hashes, label, version
   python tools/newbuild.py unpack   --game "<FINAL FANTASY XI>"   # into generated/images/<label>/
   ```

   The label is the date of `FFXiMain.dll`'s PE timestamp. The version is decrypted from the
   install's `patch.ver`; an install with none gets `30` + the timestamp's YYMMDD
   (2026-09-03 → `30260903_0`), which the lobby compares with the server's `CLIENT_VER` and
   `host64` writes into the `patch.ver` it makes. `unpack` also says whether each DLL's `.text`
   is byte-identical to a known build's (the previous build's images must be in
   `generated/images/`: `prepare` keeps one per build).

2. **Carry the previous build over.**

   ```
   python tools/newbuild.py carry --from <previous> --to <label> [--write]
   ```

   Maps every address the previous build's entry names onto the new image: `chars_ptr` (the
   global the character list hangs from, read by gamecore), `present_site` (the return address of
   the game's `IDirect3DDevice8::Present` call, used on Windows), the CRT functions the
   differential test compares, host/modern.c's menu addresses (the `modern` section; a build
   without all of them builds with Config > Modern and Config > Menus off), the skinning kernels'
   functions and globals (the `geometry` section, all or none, without its layout; see step 4),
   and the manual verdicts in `discovery/verdicts.py`. `--only modern --write` or
   `--only geometry --write` carries just that section into a build `meta/builds.json` already
   has. `--write` adds the build to `meta/builds.json` and `discovery/verdicts.py`. Addresses it
   cannot map come with a hint (how their neighbours moved); check each with
   `python tools/newbuild.py dis --label <build> --at <addr>` in both builds and fill it in.

3. **Metadata.** A DLL whose `.text` is identical to the previous build's carries its metadata
   over (`python tools/newbuild.py meta --from <previous> --to <label> --module FFXi.dll`). Otherwise:

   ```
   python tools/discover.py --label <label> --module FFXiMain.dll   # Ghidra headless; minutes
   ```

   It stops before exporting if a decode conflict, a function that looks like data, or an
   unaudited switch remains; the report says which. Each needs a manual verdict in
   `discovery/verdicts.py` (see the entries there and `discovery/notes/`); then rerun with
   `--reuse`.

4. **Prepare and build.**

   ```
   python3 tools/build_posix.py prepare --game "<FINAL FANTASY XI>"   # should report the new build
   python3 tools/build_posix.py boot64  --game "<FINAL FANTASY XI>"   # expect BOOT64 OK
   python3 tools/build_posix.py host64  --game "<FINAL FANTASY XI>"
   ```

   On Windows also run `python tools\build.py difftest` (expect 0 mismatches) and
   `python tools\build.py host`.

   If `carry` wrote a `geometry` section, run `python3 tests/geometry_replay_test.py` (needs
   `pefile` and `unicorn`). It compares `FFXI_NATIVE_GEOMETRY`'s kernels with the new build's own
   SSE code; when it passes, set `"layout": 1` in the section and rebuild. Without a layout the
   build keeps the translated skinning.

5. **Play it.** Sign in, zone in, fight, and zone again. A crash like

   ```
   [recomp] FATAL at 100542e0: indirect call/jump to an address with no translation
   ```

   means the game reached code the metadata does not list as a function. The recompiler already
   makes an entry of every code address the image's data points at (vtables, callbacks,
   exception handlers), including small functions the discovery pass folded into a neighbour, so
   what remains is a real gap: add the function to the metadata and rebuild.

6. **Record it.** Add the build to the [Supported builds](#supported-builds) table above, write
   its discovery notes in `discovery/notes/`, and commit the metadata, `builds.json` and
   `discovery/verdicts.py` together. Never commit anything from `generated/`.
