---
name: xbox-logs
description: Pull XI on Xbox's logs and profile (host64.log, app.log, samples.bin) off the Xbox, summarize the run, and optionally send them to another machine (e.g. over Taildrop) with a handoff prompt. Use for "grab the logs", "what happened on the Xbox", or sending a run's results to the Windows PC.
---

# Get the logs from the Xbox

## Pull

```bash
python3 xbox/tools/xbox.py logs build/xbox/xbox-logs/$(date +%Y%m%d-%H%M%S)
```

It copies `host64.log`, `host64.out.log`, `app.log`, `samples.bin` and `settings.reg` from the app's LocalState,
whichever of them exist. They are overwritten on each run, so pull them before the next launch. If the console
doesn't answer, ask the user to wake it. Don't copy them into a git-tracked folder: `build/` is ignored.

## Summarize

Read `host64.log` and report:
- **Where the game came from:** the `[app] starting the game: ...` line. It should be
  `D:\DevelopmentFiles\LooseApps\XIonXbox\SquareEnix\FINAL FANTASY XI`. Anything else means `game.txt` or a
  LocalState copy overrode it (see `docs/xbox-install.md`).
- **Sign-in:** the `[lsb]` line. Don't repeat credentials or account details beyond what the user already knows.
- **Frame rate:** the `[gfx] NN.N fps: frame ... = game code ... + API calls ...` lines. Give the count, min,
  median and max, and the last line's split between game code and API calls. The `[gfx]   API time (2 s)`
  lines name the costliest D3D8 calls.
- **How it ended:** the `GameStart returned ...; exit code N` line and `app.log`. Say plainly if you can't tell
  whether the exit was a normal quit.

## Profile

`samples.bin` is only readable with the linker map of **the same build** of `XIonXbox.exe`, which is on the
machine that built it (usually the Windows PC):
`python tools/sample_report.py samples.bin <exe.map> [--top 40]` (in xi-on-anything).
Don't analyze it against a different build's map. Every sample would be attributed to the wrong function.

## Send to another machine

If the user asks (e.g. "taildrop those to my Windows PC"):
1. `tailscale status` lists the devices. On a Mac the CLI may be `/Applications/Tailscale.app/Contents/MacOS/Tailscale`.
   Pick the one the user means; ask if it's ambiguous.
2. Write a `HANDOFF.md` next to the logs: the build (package name, manifest version, exe size and date), what
   the run showed (the summary above, with the key log lines quoted), and what the other side should do. Usually
   that means running `sample_report.py` with that build's map, explaining the exit code, and proposing fixes for the
   top costs. Add the console notes from `docs/xbox-install.md` that matter (loose app; updates are the exe plus
   `CodeIntegrity.cat`; don't install a full package; the game doesn't fit in LocalState).
3. `tailscale file cp HANDOFF.md host64.log app.log samples.bin <device>:` (leave out empty files).
4. Give the user a short paste-ready version of the handoff in the reply.
