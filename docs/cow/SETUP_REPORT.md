# Setup report — cow_kuluu_xi_anything

Machine and repo preparation for converting every function of `FFXiMain.dll` into
human-readable C, one function at a time, leaves first. This file records what was verified,
what was skipped, and how to reproduce it. No conversion work has been started.

Generated during setup on 2026-11-02. Live conversion state lives in
[STATUS.md](STATUS.md) (`python tools/cow/status.py`).

## 1. Host and tools

| component | version / location | notes |
| --- | --- | --- |
| Shell | PowerShell **7.6.9** (`GOOSE_SHELL=pwsh`) | all setup commands run under pwsh 7 |
| git | 2.49.0.windows.1 | |
| gh CLI | 2.96.0, authenticated as **cowrevenge** | scopes `repo, read:org, workflow, gist` |
| Python | 3.13.3 — `C:\PycharmProjects\pycuda\.venv\Scripts\python.exe` | `python` and `python3` both resolve to this venv; `build.py` uses `sys.executable`, so the recompiler runs there |
| capstone | 5.0.7 | imports OK |
| pefile | 2024.8.26 | imports OK |
| Compiler used | **VS 2022 BuildTools** `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` | found by `build.py`'s vswhere query |
| Compiler also present | VS 2022 Community `C:\Program Files\Microsoft Visual Studio\2022\Community` | complete C++ install |
| cmake | 4.0.2 | not required by the documented Windows path |
| clang | 23.1.4 at `C:\Program Files\LLVM\bin` (not on PATH) | **unused** — the docs' Windows path is MSVC via vswhere |
| Free space C: | 684.2 GB of 1,861.9 GB | |

### Environment fix applied during setup

`tools/build.py` locates the compiler with `vswhere -latest -products * -property
installationPath` — with **no `-requires` filter**. It therefore picked the 2022 BuildTools
install, which at setup time was a hollow MSBuild-only shell with no `VC` directory, so
`vcvarsall.bat` did not exist and every build target died immediately.

Fixed by adding the documented workload to that install (VS Installer, elevated):

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify `
  --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" `
  --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --quiet --norestart
```

Note `setup.exe modify` has **no `--wait`** option (it is bootstrapper-only); passing it makes
setup.exe print its help and exit without doing anything. Without `--wait` it hands off to the
installer service, so poll for completion instead.

## 2. Game install (read-only)

`C:\PhoenixXI\SquareEnix\FINAL FANTASY XI`

| item | value |
| --- | --- |
| `FFXiMain.dll` | 2,901,584 bytes |
| SHA-256 | `042884888EEA4FADAC183E520F2AE9F8C4FA267860E685048E1880EFD358E872` |
| `FFXi.dll` SHA-256 | `760f8245173c85976ddf2b0828f90a11db6a2987e96982efc58df812dff5b2a4` |
| ROM | `ROM/` 49,375 files / 8.25 GB, plus `ROM2`–`ROM9` |

The install was only ever read. Nothing was written into it; `tools/install.py install`
(which drops a stand-in `FFXiMain.dll` into the game folder) was deliberately never run.

## 3. Repository

| item | value |
| --- | --- |
| Fork | `cowrevenge/cow_kuluu_xi_anything` (public, forked from `rubymatrix/xi-on-anything`) |
| Working branch | `main` |
| Fork base commit | `db2a1e2968ff211b2b5b51aacf366e5da17b6dd3` |
| `origin` | `https://github.com/cowrevenge/cow_kuluu_xi_anything.git` |
| `upstream` | `https://github.com/rubymatrix/xi-on-anything` |

## 4. Matched client build

**`2026-10-05`**, client version `30261006_0` — matched by `FFXiMain.dll` hash in
`meta/builds.json`, cross-checked against `FFXi.dll`. Metadata:
`meta/FFXiMain.2026-10-05.meta.json`, format `ffxi-recomp-meta/1`, image base `0x10000000`,
**21,761 functions**, 943 switch tables, 2,165 tail jumps.

## 5. Build result — PASS

```
python tools\prepare.py --game "C:\PhoenixXI\SquareEnix\FINAL FANTASY XI"   # identifies build, unpacks
python tools\build.py boot64                                                 # translates all, compiles, links
```

* `prepare.py` → `generated/FFXiMain.unpacked.dll` (6,209,536 B), `generated/FFXi.unpacked.dll`,
  `generated/build.json` (`{"build": "2026-10-05", ...}`).
* `recomp.py --all` → **56 files** in `generated/all/` containing **21,891** function definitions
  (every one of the 21,761 meta functions has a body).
* MSVC compiled **77/77** objects; `build/boot64.exe` linked.
* Nothing generated is tracked: `.gitignore` covers `generated/`, `build/`, `out/`, `*.dll`,
  `*.exe`, `*.dat`, `*.DAT`, `*.unpacked.*`.

## 6. Test results

| test | result | detail |
| --- | --- | --- |
| Differential (`difftest`, `tests/difftest.c`) | **PASS** | 15 CRT-slice functions translated, 0 unimplemented instructions; **133,681 calls, 0 mismatches**; exit 0 |
| Boot (`boot64`, `tests/boot64.c`) | **FAIL — exit 1, environment precondition** | see below |

`boot64` printed only `guest window at …` and `registry: FinalFantasyXI\0001 missing 0`.
Cause: `build.py` exports this machine's `HKLM\SOFTWARE\WOW6432Node\PlayOnlineUS` into
`build\ffxi.reg` and `boot64.c` requires a DWORD `0001` under
`…\SquareEnix\FinalFantasyXI`, returning 1 if absent. This machine's PlayOnline registration
came from a **HorizonXI** install (`InstallFolder "0001"="C:\HorizonXI\Game\…"`) and has no such
value. The check runs **before `pe_load()` and before any guest code executes**, so it says
nothing about the translated functions. Making it pass would need registry writes to HKLM, which
was not done. Re-run it on a machine whose PlayOnline registry keys match a real install.

## 7. Vendors (read-only reference, pinned)

Added as submodules **without** initialising their own nested submodules (no `--recursive`,
no nested `submodule update --init`). None built, none modified.

| path | upstream | pinned commit |
| --- | --- | --- |
| `vendor/kuluu` | `cowrevenge/kuluu-ffxi` | `a7752b3a6a9c570b5af0011e89af6268615e8c14` (`v0.2.0-1670`) — oracle: names and meaning |
| `vendor/xi-on-anything` | `rubymatrix/xi-on-anything` | `db2a1e2968ff211b2b5b51aacf366e5da17b6dd3` — pristine reference, same commit our fork started from |
| `vendor/xi-tools` | `vekien/xi-tools` | `954617dfcf6603c59d4e62d81a512397f06918dd` (`v1.15.0-11`) — DAT tools, third priority |

`vendor/kuluu` declares 10 nested submodules (`vendor/server`, `vendor/POLUtils`,
`research/XiEvents`, …); all ten directories are **empty**, confirming they were not initialised.

## 8. Ledger

`python tools/cow/ledger.py` writes `ledger/functions.csv` — one row per function:
`entry, end, size, thunk, status, name, file, oracle_refs, verified, callees, leaf_rank, cycle`.

| metric | value |
| --- | --- |
| rows | **21,761** (exactly the meta function count) |
| thunks (`status=thunk`) | 313 |
| todo | 21,448 |
| leaf_rank 0 (convert first) | **7,497** |
| leaf_rank 1 | 2,738 |
| leaf_rank 2 | 1,353 |
| leaf_rank 3+ | 9,860 |
| in call cycles (`cycle=true`) | 118 |
| self-recursive | 78 |
| deepest leaf_rank | 46 |

### How the call graph is derived (verified, not guessed)

* `recomp.py` emits one definition per function: `void f_<8-hex-VA>(Guest* g)`.
* A call to another FFXiMain function is emitted as `f_<hex>(g)` — plain calls as
  `REGS_STORE; f_x(g); REGS_LOAD;`, tail jumps as `REGS_STORE; f_x(g); return;`.
* `goto L_<hex>` stays inside one function, and instruction comments print bare `0x<hex>`, so
  neither is mistaken for a call. Dynamic calls go through `rt_call_indirect()` and cannot be
  resolved statically.
* `ranges` in the meta is a list of `[start, end)` pairs; `end` is the last range's end and
  `size` is the span `end - entry` (185 functions are discontinuous and so count their gaps).

**Verification:** the ledger's Tarjan/condensation ranking was re-checked with a completely
different algorithm (Kosaraju + Kahn, iterative) — **0 leaf_rank mismatches and 0 cycle
mismatches across all 21,448 non-thunk rows**, and the `callees` counts matched exactly.

### Known nuances (documented, not silently accepted)

* **Thunks are transparent.** A call to one of the 313 five-byte `jmp rel32` stubs is counted as
  a call to the stub's target. Thunk rows themselves get `callees=0, leaf_rank=0, cycle=false`,
  because they are never conversion work.
* **66 functions** have at least one callee that is a thunk whose jump target is not a meta
  function, so it cannot be resolved; it is counted as a callee (the stub itself) but excluded
  from ranking. Of those, **27** sit at `leaf_rank 0` purely for that reason — harmless for
  scheduling (they depend on no ledger function), worth knowing when a conversion looks
  "leaf" but jumps somewhere.
* The generated C contains **21,891** definitions vs 21,761 meta entries; 63 of those extras
  appear as call targets. They are outside the ledger universe and excluded from the graph.
* A self-recursive function with no other calls has `callees=0`, `cycle=true`, `leaf_rank=1`.

## 9. Skipped, on purpose

| item | why |
| --- | --- |
| **SDL3** (`SDL3-devel-3.4.16-VC.zip` → `C:\Dev\SDL3`) | Needed only by the `host64` target — the playable game, which this project must not launch. It is a manual GitHub zip, outside the authorised `winget`/`pip` install methods. Without it `build.py host64` will fail with "SDL3 not found". |
| **`dxc`** (Windows SDK, optional) | Docs: its absence only leaves ray-traced effects off at runtime. |
| `build.py host` (x86 stand-in DLL) | Not needed; and its whole purpose is to be installed into the game folder, which is forbidden here. |
| `tools/install.py install` | Writes into `C:\PhoenixXI` — forbidden. |
| Launching the game | Forbidden; in-game testing is the user's. |
| Docker / LSB server | Untouched. |
| Any function conversion | Out of scope for setup. |

## 10. Reproduce

```powershell
# 0. one-time environment fix (elevated): give BuildTools the C++ workload
& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify `
  --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" `
  --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --quiet --norestart

# 1. tools
pip install capstone pefile

# 2. identify the build and unpack it into generated\
python tools\prepare.py --game "C:\PhoenixXI\SquareEnix\FINAL FANTASY XI"

# 3. translate every function into generated\all\*.c, compile, link  (also runs the boot test)
python tools\build.py boot64

# 4. the differential test (expect: PASS, 0 mismatches)
python tools\build.py difftest

# 5. rebuild the ledger, then the status page
python tools\cow\ledger.py
python tools\cow\status.py
```

`ledger.py` flags: `--build YYYY-MM-DD`, `--generated DIR`, `--out FILE`, `--repo PATH`.
`status.py` flags: `--ledger FILE`, `--out FILE`, `--repo PATH`. Both are rerunnable and never
hand-edited. **Run `status.py` before every commit that will be pushed, and commit
`docs/cow/STATUS.md` with it.**

To add more client builds later, see `docs/new-client-version.md` (a separate task; not done here).
