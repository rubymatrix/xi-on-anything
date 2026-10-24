---
name: xbox-deploy
description: Deploy a newly built XI on Xbox package to the Xbox (Dev Mode) as an in-place update of the loose app, without re-sending the 14 GB of game data. Use when the user has a new .msix (often "new package in Downloads") and wants it on the console.
---

# Deploy a new build to the Xbox

The console runs XI on Xbox as a **loose app** from `D:\DevelopmentFiles\LooseApps\XIonXbox`, with the game
beside it. An update replaces the app's files there and re-registers. **Never install the .msix itself**: a
package can't replace the loose app, and the with-game package means a 15 GB upload plus an hour of installing.
Background: `docs/xbox-install.md`.

## Steps

1. **Find the package.** Usually the newest `XIonXbox-xbox*.msix` in `~/Downloads` (browsers add ` 1`, ` 2`),
   or `build/xbox/XIonXbox-xbox.msix` on the Windows machine. Check with `ls -lt ~/Downloads/*.msix | head`.
   It must be the **app-only** package (about 17 MB). If it's about 15 GB, it's the with-game package: stop and ask.
2. **Know the console's address.** `xbox/tools/xbox.py` remembers it in `build/xbox/xbox-address`. If that file doesn't
   exist, ask the user (Dev Home shows it) and pass `XBOX=<address>`.
3. **Check the console's state:** `python3 xbox/tools/xbox.py status`.
   - "installed from a package", or "not installed": this isn't an update. Use the `xbox-setup` skill instead.
   - "game.txt is there": it overrides the game beside the app. Say so and ask before removing it
     (`python3 xbox/tools/xbox.py rm local:\LocalState game.txt`).
   - "no Device Portal": the console is off, asleep, or not in Dev Mode. Ask the user to wake it.
4. **Say what changed** before deploying. Compare the new package with `build/xbox/xbox-loose` (the last deploy):
   ```bash
   python3 - "<msix>" <<'EOF'
   import zipfile,hashlib,os,sys,re
   z=zipfile.ZipFile(sys.argv[1]); s='build/xbox/xbox-loose/'
   for n in z.namelist():
       if n in ('AppxBlockMap.xml','AppxSignature.p7x','[Content_Types].xml') or n.endswith('/'): continue
       a=z.read(n); p=s+n
       if n=='AppxManifest.xml':
           strip=lambda t:re.sub(r'(<Identity\b[^>]*?)Version="[\d.]+"',r'\1',t.decode('utf-8-sig'))
           same=os.path.exists(p) and strip(a)==strip(open(p,'rb').read())
       else: same=os.path.exists(p) and hashlib.md5(a).digest()==hashlib.md5(open(p,'rb').read()).digest()
       print(('same    ' if same else 'CHANGED ')+n, len(a))
   EOF
   ```
   Usually only `XIonXbox.exe` and `AppxMetadata/CodeIntegrity.cat` change. If the manifest changed (not just
   its version), tell the user. `deploy` re-registers anyway, so a manifest change still takes effect.
5. **Deploy:** `python3 xbox/tools/xbox.py deploy "<msix>"`. It stops the app, stages the files in `build/xbox/xbox-loose`
   with the **registered** version written into the manifest, sends every app file (the catalog keeps its size
   between builds, so a same-size skip would miss it), re-registers, and launches.
6. **Report** the new exe's size, that it registered and launched, and offer to pull logs after a run
   (`xbox-logs` skill).

## If something fails

- **Launch fails with `0x80070002`, or Dev Home says "Please try again":** the catalog on the console doesn't
  match the exe. Check that `build/xbox/xbox-loose/AppxMetadata/CodeIntegrity.cat` exists, then run `deploy` again.
- **`0x80073CFB` "An unpackaged version cannot replace this":** a packaged install is in the way. Use `xbox-setup`.
- **Upload failures:** run `deploy` again; it's safe to repeat.
- Other codes: the table in `docs/xbox-install.md`.
