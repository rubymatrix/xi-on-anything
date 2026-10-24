#!/usr/bin/env python3
"""Install, update, and inspect XI on Xbox on an Xbox in Developer Mode through Device Portal, from any
computer on its network (written on a Mac). Standard library only. docs/xbox-install.md says why each
step is the way it is.

  XBOX=<console address> python3 tools/xbox.py <command> ...

The app runs as a loose app: its files, and the game beside them, in D:\\DevelopmentFiles\\LooseApps\\XIonXbox
on the console. An update replaces the exe and its catalog there; the 14 GB of game data stays put.

  status                          the XIonXbox package on the console, how it's installed, storage folders
  setup <app.msix> <game folder>  first time: the app as a loose app, then the game beside it
                                  (<game folder> is the "FINAL FANTASY XI" folder)
  game <game folder>              upload (or finish uploading) the game beside the loose app; resumes
  deploy <app.msix>               update the loose app from a newly built package, then launch it
  logs <local dir>                copy LocalState's logs and samples.bin to <local dir>
  launch | stop                   start or stop the app
  install <package.msix> [deps]   install a signed package the ordinary way (NOT for the loose app)
  uninstall [--no-backup]         remove the XIonXbox package; backs LocalState up to build/ first

Lower level (<folder> is dev:<path> for DevelopmentFiles, local: for the app's LocalState):
  ls <folder> | pull <folder> <dir> | push <dir> <folder> [--all] | rm <folder> <name> | rmdir <folder> <name>
  register <LooseApps subfolder>

Environment: XBOX (required; remembered in build/xbox-address after the first use), and XBOX_USER /
XBOX_PASS if Device Portal's authentication is on.
"""
import base64
import concurrent.futures
import http.client
import json
import os
import re
import shutil
import ssl
import sys
import threading
import time
import urllib.parse
import uuid
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')
STAGE = os.path.join(BUILD, 'xbox-loose')  # the loose app's files, as last sent to the console
NAME = 'XIonXbox'  # the manifest's Identity Name
LOOSE = 'XIonXbox'  # its folder under DevelopmentFiles\LooseApps
GAME_DIR = '\\LooseApps\\%s\\SquareEnix\\FINAL FANTASY XI' % LOOSE
PORT = 11443
CTX = ssl._create_unverified_context()  # Device Portal's certificate is the console's own, self-signed
PACKAGE_SKIP = ('AppxBlockMap.xml', 'AppxSignature.p7x', '[Content_Types].xml')
LOGS = ('host64.log', 'host64.out.log', 'app.log', 'samples.bin', 'settings.reg')

_local = threading.local()
_token = None


def xbox():
    saved = os.path.join(BUILD, 'xbox-address')
    addr = os.environ.get('XBOX')
    if addr:
        os.makedirs(BUILD, exist_ok=True)
        with open(saved, 'w') as f:
            f.write(addr)
        return addr
    if os.path.exists(saved):
        return open(saved).read().strip()
    sys.exit('Set XBOX=<the console\'s address> (Dev Home shows it).')


def auth_header():
    u, p = os.environ.get('XBOX_USER'), os.environ.get('XBOX_PASS')
    return {'Authorization': 'Basic ' + base64.b64encode(('%s:%s' % (u, p)).encode()).decode()} if u else {}


def conn():
    c = getattr(_local, 'c', None)
    if c is None:
        c = _local.c = http.client.HTTPSConnection(xbox(), PORT, context=CTX, timeout=600)
    return c


def csrf():
    """Device Portal wants every write to echo the CSRF-Token cookie from an earlier response."""
    global _token
    if _token is None:
        c = http.client.HTTPSConnection(xbox(), PORT, context=CTX, timeout=15)
        try:
            c.request('GET', '/api/os/info', headers=auth_header())
            r = c.getresponse()
        except OSError as e:
            sys.exit('no Device Portal at https://%s:%d (%s). Is the console on and in Dev Mode, with Device '
                     'Portal enabled (Dev Home > Remote Access Settings)?' % (xbox(), PORT, e))
        r.read()
        if r.status == 401:
            sys.exit('Device Portal wants a user name and password: set XBOX_USER and XBOX_PASS.')
        cookies = r.getheader('Set-Cookie') or ''
        m = re.search(r'CSRF-Token=([^;]+)', cookies)
        _token = m.group(1) if m else ''
    return _token


def request(method, path, params=None, body=None, headers=None, tries=4):
    url = path + ('?' + urllib.parse.urlencode(params) if params else '')
    h = {'Cookie': 'CSRF-Token=' + csrf(), 'X-CSRF-Token': csrf()}
    h.update(auth_header())
    h.update(headers or {})
    if body is None and method in ('POST', 'DELETE'):
        h['Content-Length'] = '0'  # IIS answers 411 to a bodyless POST without it
    for attempt in range(tries):
        try:
            c = conn()
            c.request(method, url, body=body, headers=h)
            r = c.getresponse()
            data = r.read()
            if r.status >= 500 and attempt < tries - 1 and method == 'GET':
                time.sleep(2 * (attempt + 1))
                continue
            return r.status, data
        except (OSError, http.client.HTTPException):
            _local.c = None
            if attempt == tries - 1:
                raise
            time.sleep(2 * (attempt + 1))


def api_json(method, path, params=None):
    st, data = request(method, path, params)
    try:
        return st, json.loads(data.decode('utf-8-sig')) if data else {}
    except ValueError:
        return st, {'raw': data.decode('utf-8', 'replace')}


# --- the package ---------------------------------------------------------------------------------------------

def installed():
    """The XIonXbox package on the console (Device Portal's record), or None."""
    st, d = api_json('GET', '/api/app/packagemanager/packages')
    for p in d.get('InstalledPackages', []):
        if p.get('Name') == 'XI on Xbox' or p['PackageFullName'].startswith(NAME + '_'):
            return p
    return None


def need_installed():
    p = installed()
    if not p:
        sys.exit('XI on Xbox is not on the console. First time: tools/xbox.py setup <app.msix> <game folder>')
    return p


def version_of(p):
    v = p['Version']
    return '%d.%d.%d.%d' % (v['Major'], v['Minor'], v['Build'], v['Revision'])


def wait_deploy():
    """The package manager's last request: 204 while it runs, then its result."""
    while True:
        st, data = request('GET', '/api/app/packagemanager/state')
        if st == 204:
            time.sleep(2)
            continue
        d = json.loads(data.decode('utf-8-sig'))
        if d.get('Success'):
            return d
        raise SystemExit('the console refused it: %s\n  (%s, code %s; docs/xbox-install.md lists the usual ones)'
                         % (d.get('Reason'), d.get('CodeText', '').strip(), d.get('Code')))


def cmd_register(folder=LOOSE):
    st, data = request('POST', '/api/app/packagemanager/register',
                       {'folder': base64.b64encode(folder.encode()).decode()})
    if st != 202:
        sys.exit('register: HTTP %d %s' % (st, data[:300]))
    wait_deploy()
    print('registered LooseApps\\%s' % folder)


def cmd_launch():
    p = need_installed()
    st, data = request('POST', '/api/taskmanager/app', {
        'appid': base64.b64encode(p['PackageRelativeId'].encode()).decode(),
        'package': base64.b64encode(p['PackageFullName'].encode()).decode()})
    if st != 200:
        # 0x80070002 here was the missing CodeIntegrity.cat (the console showed "Please try again")
        sys.exit('launch: HTTP %d %s' % (st, data.decode('utf-8', 'replace')[:300]))
    print('launched %s' % p['PackageFullName'])


def cmd_stop(quiet=False):
    p = installed()
    if not p:
        return
    st, _ = request('DELETE', '/api/taskmanager/app', {
        'package': base64.b64encode(p['PackageFullName'].encode()).decode(), 'forcestop': 'true'})
    if not quiet:
        print('stopped' if st == 200 else 'not running')
    time.sleep(1.5)  # let it release the exe before it is replaced


def is_loose(p):
    """A package installed from an .msix has its own folder under DevelopmentFiles\\WindowsApps; a loose
    registration runs from LooseApps and has none."""
    return p['PackageFullName'] not in listing(dev(), '\\WindowsApps')


def cmd_status():
    p = installed()
    if not p:
        print('XI on Xbox: not installed')
    else:
        print('XI on Xbox: %s (%s)' % (p['PackageFullName'], 'loose, from LooseApps\\' + LOOSE if is_loose(p)
                                       else 'installed from a package'))
        game = listing(dev(), GAME_DIR)
        print('game beside the app: %s' % ('yes (%d entries in FINAL FANTASY XI)' % len(game) if game else 'no'))
        ls = listing(localstate(p), '\\LocalState')
        print('LocalState: %s' % (', '.join(sorted(ls)) or 'empty, or not made yet (the app makes it on first run)'))
        if 'game.txt' in ls:
            print('  game.txt is there: it overrides the game beside the app')
        if 'SquareEnix' in ls:
            print('  LocalState\\SquareEnix is there: a copy made by the app (uses the small user drive)')
    old = [n for n in listing(dev(), '\\WindowsApps') if n.startswith(NAME + '_')]
    if old:
        print('install folders of packaged versions under DevelopmentFiles\\WindowsApps: ' + ', '.join(old))


def install_package(path, deps=()):
    """Device Portal's Add: the package (and dependencies) in one multipart upload, streamed from disk."""
    files = [path] + list(deps)
    b = uuid.uuid4().hex
    parts = []
    for f in files:
        name = os.path.basename(f)
        parts.append((('--%s\r\nContent-Disposition: form-data; name="%s"; filename="%s"\r\n'
                       'Content-Type: application/octet-stream\r\n\r\n' % (b, name, name)).encode(), f))
    tail = ('--%s--\r\n' % b).encode()
    total = sum(len(h) + os.path.getsize(f) + 2 for h, f in parts) + len(tail)
    c = http.client.HTTPSConnection(xbox(), PORT, context=CTX, timeout=3600)
    h = {'Cookie': 'CSRF-Token=' + csrf(), 'X-CSRF-Token': csrf(), 'Content-Length': str(total),
         'Content-Type': 'multipart/form-data; boundary=' + b}
    h.update(auth_header())
    c.putrequest('POST', '/api/app/packagemanager/package?' +
                 urllib.parse.urlencode({'package': os.path.basename(path)}))
    for k, v in h.items():
        c.putheader(k, v)
    c.endheaders()
    sent, t0 = 0, time.time()
    for head, f in parts:
        c.send(head)
        with open(f, 'rb') as fh:
            while True:
                chunk = fh.read(1 << 20)
                if not chunk:
                    break
                c.send(chunk)
                sent += len(chunk)
                if sent % (256 << 20) < (1 << 20):
                    print('  %.2f / %.2f GB' % (sent / 1e9, total / 1e9), flush=True)
        c.send(b'\r\n')
    c.send(tail)
    r = c.getresponse()
    data = r.read()
    if r.status != 202:
        sys.exit('upload: HTTP %d %s' % (r.status, data[:300]))
    print('uploaded %.2f GB in %.0f s; installing (a package with the game takes 30-60 min)'
          % (total / 1e9, time.time() - t0), flush=True)
    wait_deploy()


def cmd_install(path, *deps):
    p = installed()
    if p and is_loose(p):
        sys.exit('XI on Xbox is a loose app here; a package cannot replace it ("An unpackaged version cannot '
                 'replace this"). Use deploy for updates, or uninstall first.')
    install_package(path, deps)
    print('installed. In Dev Home, set its type to Game (View details).')


def cmd_uninstall(*flags):
    p = need_installed()
    if '--no-backup' not in flags:
        dest = os.path.join(BUILD, 'localstate-backup-' + time.strftime('%Y%m%d-%H%M%S'))
        n = pull(localstate(p), '\\LocalState', dest, skip=('SquareEnix',))
        print('backed up LocalState (%d files) to %s' % (n, dest))
    cmd_stop(quiet=True)
    st, data = request('DELETE', '/api/app/packagemanager/package', {'package': p['PackageFullName']})
    if st != 200:
        sys.exit('uninstall: HTTP %d %s' % (st, data[:300]))
    for _ in range(30):
        if not installed():
            break
        time.sleep(2)
    print('uninstalled %s (its LocalState stays on the console)' % p['PackageFullName'])


# --- the loose app -------------------------------------------------------------------------------------------

def stage(msix, registered_version=None):
    """Unpack the app's files from a built package into build/xbox-loose: everything but the package's own
    signature, block map, and content types. Its catalog (AppxMetadata/CodeIntegrity.cat) stays: without it the
    console won't start the exe. The manifest keeps the registered version, so a rebuild re-registers in place
    (the build's own version may be lower than one registered before)."""
    z = zipfile.ZipFile(msix)
    names = z.namelist()
    if any(n.startswith('SquareEnix/') for n in names):
        sys.exit('%s holds the game: deploy wants the app-only package (deploy-xbox.ps1 -PackageOnly)' % msix)
    if 'AppxMetadata/CodeIntegrity.cat' not in names:
        sys.exit('%s has no AppxMetadata/CodeIntegrity.cat; the console will not launch it without one' % msix)
    manifest = z.read('AppxManifest.xml').decode('utf-8-sig')
    m = re.search(r'<Identity\b[^>]*\bName="([^"]+)"[^>]*\bPublisher="([^"]+)"', manifest)
    if not m or m.group(1) != NAME:
        sys.exit('%s is not XI on Xbox (Identity Name %r)' % (msix, m and m.group(1)))
    if registered_version:
        manifest = re.sub(r'(<Identity\b[^>]*\bVersion=")[\d.]+(")', r'\g<1>%s\2' % registered_version, manifest, 1)
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    for n in names:
        if n in PACKAGE_SKIP or n.endswith('/'):
            continue
        out = os.path.join(STAGE, *n.split('/'))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(manifest.encode('utf-8') if n == 'AppxManifest.xml' else z.read(n))
    return STAGE


def cmd_deploy(msix):
    p = need_installed()
    if not is_loose(p):
        sys.exit('XI on Xbox is installed from a package, not as a loose app. Set it up once with '
                 'tools/xbox.py setup <app.msix> <game folder> (docs/xbox-install.md).')
    stage(msix, version_of(p))
    cmd_stop(quiet=True)
    # every file, whatever its size: a rebuilt CodeIntegrity.cat is the same size as the old one
    if push(STAGE, dev(), '\\LooseApps\\' + LOOSE, send_all=True):
        sys.exit('some files did not reach the console; run deploy again')
    cmd_register(LOOSE)  # picks up the new catalog
    cmd_launch()


def cmd_setup(msix, game):
    if not os.path.isfile(os.path.join(game, 'FFXiMain.dll')):
        sys.exit('%s is not a FINAL FANTASY XI folder (no FFXiMain.dll)' % game)
    p = installed()
    if p and not is_loose(p):
        sys.exit('XI on Xbox %s is installed from a package. A loose app cannot replace it: run '
                 'tools/xbox.py uninstall first (it backs LocalState up; the console keeps LocalState anyway).'
                 % version_of(p))
    stage(msix, version_of(p) if p else None)
    if push(STAGE, dev(), '\\LooseApps\\' + LOOSE, send_all=True):
        sys.exit('some app files did not reach the console; run setup again')
    cmd_register(LOOSE)
    print('app registered; now the game (resumable: run "tools/xbox.py game <folder>" again if it stops)')
    cmd_game(game)


def cmd_game(game):
    if not os.path.isfile(os.path.join(game, 'FFXiMain.dll')):
        sys.exit('%s is not a FINAL FANTASY XI folder (no FFXiMain.dll)' % game)
    failed = push(game, dev(), GAME_DIR, skip_dirs=('TEMP',))
    if failed:
        sys.exit('%d files failed; run the same command again to send just those' % failed)
    p = installed()
    if p and 'game.txt' in listing(localstate(p), '\\LocalState'):
        print('note: LocalState\\game.txt overrides the game beside the app; remove it with\n'
              '  tools/xbox.py rm local:\\LocalState game.txt')
    print('the game is beside the app. Launch XI on Xbox.')


def cmd_logs(dest):
    p = need_installed()
    have = listing(localstate(p), '\\LocalState')
    os.makedirs(dest, exist_ok=True)
    for name in LOGS:
        if name not in have:
            continue
        st, data = request('GET', '/api/filesystem/apps/file',
                           dict(localstate(p), path='\\LocalState', filename=name))
        if st == 200:
            with open(os.path.join(dest, name), 'wb') as f:
                f.write(data)
            print('  %-16s %9d bytes' % (name, len(data)))
    print('in %s' % dest)


# --- files ---------------------------------------------------------------------------------------------------

def dev():
    return {'knownfolderid': 'DevelopmentFiles'}


def localstate(p=None):
    p = p or need_installed()
    return {'knownfolderid': 'LocalAppData', 'packagefullname': p['PackageFullName']}


def parse_folder(spec):
    kind, _, path = spec.partition(':')
    if kind == 'dev':
        return dev(), path or '\\'
    if kind == 'local':
        return localstate(), path or '\\LocalState'
    sys.exit('a folder is dev:<path> or local:<path>, e.g. dev:\\LooseApps\\XIonXbox or local:\\LocalState')


def join(a, b):
    return a.rstrip('\\') + '\\' + b


def listing(base, path):
    """{name: (is_dir, size)}. Beware: a folder that doesn't exist lists as empty, not as an error."""
    st, d = api_json('GET', '/api/filesystem/apps/files', dict(base, path=path))
    if st != 200:
        return {}
    return {i['Name']: (bool(i['Type'] & 16), i.get('FileSize') or 0) for i in d.get('Items', [])}


def mkdir(base, parent, name):
    st, data = request('POST', '/api/filesystem/apps/folder', dict(base, path=parent, newfoldername=name))
    if st != 200:
        raise RuntimeError('mkdir %s\\%s: HTTP %d %s' % (parent, name, st, data[:200]))


def upload(base, folder, local):
    name = os.path.basename(local)
    b = uuid.uuid4().hex
    with open(local, 'rb') as f:
        payload = f.read()
    head = ('--%s\r\nContent-Disposition: form-data; name="%s"; filename="%s"\r\n'
            'Content-Type: application/octet-stream\r\n\r\n' % (b, name, name)).encode('utf-8')
    body = head + payload + ('\r\n--%s--\r\n' % b).encode()
    st, data = request('POST', '/api/filesystem/apps/file', dict(base, path=folder), body,
                       {'Content-Type': 'multipart/form-data; boundary=' + b, 'Content-Length': str(len(body))})
    if st != 200:
        # 500 "path specified" / "File move failed": the folder isn't there. LocalState only exists after the
        # app's first run; any other folder has to be made first (uploads don't make folders).
        raise RuntimeError('HTTP %d %s' % (st, data[:200].decode('utf-8', 'replace')))


def push(local_root, base, root, send_all=False, skip_dirs=(), workers=4):
    """Sync a local folder up. Files already there with the same size are skipped, so an interrupted push
    resumes, unless send_all. Returns the number of files that failed."""
    local_root = os.path.abspath(local_root)
    cur = '\\'
    for part in [p for p in root.split('\\') if p]:
        if part not in listing(base, cur):
            mkdir(base, cur, part)
        cur = join(cur, part)

    todo, total, skipped = [], 0, 0
    for dirpath, dirnames, filenames in os.walk(local_root):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith('.') and d not in skip_dirs)
        rel = os.path.relpath(dirpath, local_root)
        remote = root if rel == '.' else join(root, rel.replace(os.sep, '\\'))
        have = listing(base, remote)
        for d in dirnames:
            if d not in have:
                mkdir(base, remote, d)
        for f in sorted(filenames):
            if f.startswith('.'):
                continue
            full = os.path.join(dirpath, f)
            size = os.path.getsize(full)
            if not send_all and f in have and not have[f][0] and have[f][1] == size:
                skipped += 1
                continue
            todo.append((remote, full, size))
            total += size
    print('%d files to send (%.2f GB), %d already there' % (len(todo), total / 1e9, skipped), flush=True)

    sent, done, failed, t0 = 0, 0, [], time.time()

    def one(job):
        try:
            upload(base, job[0], job[1])
            return job, None
        except Exception as e:  # noqa: BLE001 - report it and keep going
            return job, e

    with concurrent.futures.ThreadPoolExecutor(workers) as ex:
        for job, err in ex.map(one, todo):
            done += 1
            if err:
                failed.append((job[1], err))
            else:
                sent += job[2]
            if done % 500 == 0 or done == len(todo):
                el = time.time() - t0
                rate = sent / el / 1e6 if el else 0
                print('  %d/%d files, %.2f/%.2f GB, %.1f MB/s' % (done, len(todo), sent / 1e9, total / 1e9, rate),
                      flush=True)
    for full, err in failed[:20]:
        print('  FAILED %s: %s' % (full, err))
    if len(failed) > 20:
        print('  ... and %d more' % (len(failed) - 20))
    if failed and all(isinstance(e, FileNotFoundError) for _, e in failed):
        print('  (the local folder moved or changed while sending: leave it in place until a push finishes)')
    return len(failed)


def pull(base, path, dest, skip=()):
    n = 0
    for name, (d, _) in listing(base, path).items():
        if name in skip:
            continue
        out = os.path.join(dest, name)
        if d:
            n += pull(base, join(path, name), out)
            continue
        os.makedirs(dest, exist_ok=True)
        st, data = request('GET', '/api/filesystem/apps/file', dict(base, path=path, filename=name))
        if st == 200:
            with open(out, 'wb') as f:
                f.write(data)
            n += 1
        else:
            print('  FAILED %s: HTTP %d' % (join(path, name), st))
    return n


def cmd_ls(spec):
    base, path = parse_folder(spec)
    for name, (d, size) in sorted(listing(base, path).items()):
        print('%s  %12s  %s' % ('d' if d else '-', '' if d else size, name))


def cmd_pull(spec, dest):
    base, path = parse_folder(spec)
    print('%d files' % pull(base, path, dest))


def cmd_push(local, spec, *flags):
    base, path = parse_folder(spec)
    return 1 if push(local, base, path, send_all='--all' in flags) else 0


def cmd_rm(spec, name):
    base, path = parse_folder(spec)
    st, data = request('DELETE', '/api/filesystem/apps/file', dict(base, path=path, filename=name))
    print('HTTP %d %s' % (st, data.decode('utf-8', 'replace')[:200]))


def cmd_rmdir(spec, name):
    """Deletes a folder and everything in it. The API names the folder as filename=, beside its parent's path."""
    base, path = parse_folder(spec)
    st, data = request('DELETE', '/api/filesystem/apps/folder', dict(base, path=path, filename=name))
    print('HTTP %d %s' % (st, data.decode('utf-8', 'replace')[:200]))


COMMANDS = {
    'status': (cmd_status, 0, 0), 'setup': (cmd_setup, 2, 2), 'game': (cmd_game, 1, 1),
    'deploy': (cmd_deploy, 1, 1), 'logs': (cmd_logs, 1, 1), 'launch': (cmd_launch, 0, 0),
    'stop': (cmd_stop, 0, 0), 'install': (cmd_install, 1, 9), 'uninstall': (cmd_uninstall, 0, 1),
    'ls': (cmd_ls, 1, 1), 'pull': (cmd_pull, 2, 2), 'push': (cmd_push, 2, 3), 'rm': (cmd_rm, 2, 2),
    'rmdir': (cmd_rmdir, 2, 2), 'register': (cmd_register, 1, 1),
}


def main():
    a = sys.argv[1:]
    if not a or a[0] not in COMMANDS or not COMMANDS[a[0]][1] <= len(a) - 1 <= COMMANDS[a[0]][2]:
        sys.exit(__doc__)
    sys.exit(COMMANDS[a[0]][0](*a[1:]) or 0)


if __name__ == '__main__':
    main()
