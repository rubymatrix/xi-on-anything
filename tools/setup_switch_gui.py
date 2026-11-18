"""A window for tools/setup_switch.py: the game folder and the SD card picked with folder pickers, the server
filled in from the player's launcher, one button to build and install. switch/setup-switch.bat (with no options)
and switch/setup-switch.sh (with a display) open it.

  python tools/setup_switch_gui.py

It checks the PC when it opens and installs what is missing on a click (Python's packages with pip; on
Windows devkitPro's installer, then its packages with its pacman). The build itself is setup_switch.py,
run with the window's answers; its lines go to the log at the bottom, its steps and progress to the bar.
"""
import os
import queue
import re
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import setup_switch as ss  # noqa: E402

TITLE = 'XI on Anything: Switch setup'
STEP = re.compile(r'^\[(\d+)/(\d+)\] (.*)')
BAR = re.compile(r'^\s*\[[#.]+\]\s+(\d+)%\s*(.*)')
COPY = re.compile(r'^\s*(\d+)/(\d+) files copied')


def missing_python_packages():
    return [p for p in ss.PACKAGES if subprocess.run([sys.executable, '-c', 'import ' + p], capture_output=True,
                                                     creationflags=NO_WINDOW).returncode]


NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)  # no console window flashing up on Windows


class App:
    def __init__(self, root):
        self.root = root
        self.q = queue.Queue()
        self.saved = ss.load_saved()
        self.busy = False
        self.server_auto = ''  # the server last filled in from the launcher (replaced when the game changes)
        root.title(TITLE)
        root.minsize(640, 520)
        pad = {'padx': 10, 'pady': 4}

        frm = ttk.Frame(root, padding=12)
        frm.pack(fill='both', expand=True)
        frm.columnconfigure(1, weight=1)

        ttk.Label(frm, text='Build XI on Anything for your Switch and put it on its SD card.',
                  font=('', 11, 'bold')).grid(row=0, column=0, columnspan=3, sticky='w', pady=(0, 8))

        # this PC
        ttk.Label(frm, text='This PC').grid(row=1, column=0, sticky='nw', **pad)
        self.pc = ttk.Label(frm, text='checking...', wraplength=440, justify='left')
        self.pc.grid(row=1, column=1, sticky='w', **pad)
        self.fix = ttk.Button(frm, text='Install', command=self.install_missing)
        self.fix.grid(row=1, column=2, sticky='e', **pad)
        self.fix.grid_remove()

        # the game
        ttk.Label(frm, text='Game folder').grid(row=2, column=0, sticky='w', **pad)
        self.game = tk.StringVar(value=self.saved.get('game') or ss.registry_game() or '')
        ttk.Entry(frm, textvariable=self.game).grid(row=2, column=1, sticky='ew', **pad)
        ttk.Button(frm, text='Browse...', command=self.pick_game).grid(row=2, column=2, **pad)
        self.game_info = ttk.Label(frm, text='the FINAL FANTASY XI folder, with FFXiMain.dll and ROM in it',
                                   foreground='gray')
        self.game_info.grid(row=3, column=1, columnspan=2, sticky='w', padx=10)
        self.game.trace_add('write', lambda *_: self.root.after(400, self.game_changed, self.game.get()))

        # the card
        ttk.Label(frm, text='SD card').grid(row=4, column=0, sticky='w', **pad)
        self.card = tk.StringVar()
        self.cards = ttk.Combobox(frm, textvariable=self.card)
        self.cards.grid(row=4, column=1, sticky='ew', **pad)
        ttk.Button(frm, text='Browse...', command=self.pick_card).grid(row=4, column=2, **pad)
        ttk.Label(frm, text="its top folder (e.g. E:\\); a card with Atmosphère on it is found by itself",
                  foreground='gray').grid(row=5, column=1, columnspan=2, sticky='w', padx=10)

        # the server
        ttk.Label(frm, text='Server').grid(row=6, column=0, sticky='w', **pad)
        self.server = tk.StringVar(value=self.saved.get('server', ''))
        ttk.Entry(frm, textvariable=self.server).grid(row=6, column=1, columnspan=2, sticky='ew', **pad)
        self.server_info = ttk.Label(frm, text="its address, as your launcher uses it (blank: the Switch asks)",
                                     foreground='gray')
        self.server_info.grid(row=7, column=1, columnspan=2, sticky='w', padx=10)

        # options
        opts = ttk.Frame(frm)
        opts.grid(row=8, column=1, columnspan=2, sticky='w', **pad)
        self.layout = tk.StringVar(value=self.saved.get('layout') or 'nintendo')
        ttk.Label(opts, text='Buttons:').pack(side='left')
        ttk.Radiobutton(opts, text='Nintendo (A confirms)', value='nintendo', variable=self.layout).pack(side='left', padx=4)
        ttk.Radiobutton(opts, text='Xbox (B confirms)', value='xbox', variable=self.layout).pack(side='left', padx=4)
        self.copy_game = tk.BooleanVar(value=True)
        ttk.Checkbutton(frm, text='Copy the game to the card (about 14 GB the first time, then only what changed)',
                        variable=self.copy_game).grid(row=9, column=1, columnspan=2, sticky='w', padx=10)
        self.icon = tk.BooleanVar(value=True)
        ttk.Checkbutton(frm, text='Home screen icon: put Sphaira on the card, which makes it on the Switch',
                        variable=self.icon).grid(row=10, column=1, columnspan=2, sticky='w', padx=10)

        # go
        self.go = ttk.Button(frm, text='Build and install', command=self.start)
        self.go.grid(row=11, column=0, columnspan=3, sticky='ew', pady=(12, 4), padx=10, ipady=6)
        self.step = ttk.Label(frm, text='')
        self.step.grid(row=12, column=0, columnspan=3, sticky='w', padx=10)
        self.bar = ttk.Progressbar(frm, maximum=100)
        self.bar.grid(row=13, column=0, columnspan=3, sticky='ew', padx=10, pady=4)

        logf = ttk.Frame(frm)
        logf.grid(row=14, column=0, columnspan=3, sticky='nsew', padx=10, pady=(4, 0))
        frm.rowconfigure(14, weight=1)
        self.log = tk.Text(logf, height=10, wrap='word', state='disabled', font=('Consolas', 9) if os.name == 'nt' else None)
        sb = ttk.Scrollbar(logf, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set)
        self.log.pack(side='left', fill='both', expand=True)
        sb.pack(side='right', fill='y')

        self.root.after(100, self.pump)
        self.find_cards()
        self.check_pc()
        if self.game.get():
            self.game_changed(self.game.get())

    # ---- the background: work in threads, the window updated from pump() ---------------------------

    def background(self, fn, *args):
        threading.Thread(target=fn, args=args, daemon=True).start()

    def post(self, fn, *args):
        self.q.put((fn, args))

    def pump(self):
        try:
            while True:
                fn, args = self.q.get_nowait()
                fn(*args)
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def write(self, line):
        self.log.configure(state='normal')
        self.log.insert('end', line if line.endswith('\n') else line + '\n')
        self.log.see('end')
        self.log.configure(state='disabled')

    # ---- this PC ---------------------------------------------------------------------------------

    def check_pc(self):
        self.pc.configure(text='checking...', foreground='')
        self.fix.grid_remove()
        self.background(self._check_pc)

    def _check_pc(self):
        try:
            self.__check_pc()
        except Exception as e:  # noqa: BLE001 - said on the window, not a "checking..." that never ends
            self.post(self.pc.configure, {'text': f'could not check: {e}', 'foreground': '#b91c1c'})

    def __check_pc(self):
        problems = []
        if sys.version_info < (3, 12):
            problems.append('Python 3.12 or newer (this is %d.%d)' % sys.version_info[:2])
        py = missing_python_packages()
        tc = ss.toolchain()
        dkp = ss.absent_packages(tc)
        pacman = ss.find_pacman(tc)
        if py:
            problems.append("Python's " + ', '.join(py))
        if dkp and not tc.DKP and not pacman:
            problems.append('devkitPro (the free Switch compiler)')
        elif dkp:
            problems.append("devkitPro's " + ', '.join(dkp))
        self.post(self.pc_checked, problems, py, dkp, pacman, tc.DKP)

    def pc_checked(self, problems, py, dkp, pacman, dkp_root):
        self.missing = (py, dkp, pacman)
        if not problems:
            self.pc.configure(text=f'ready (devkitPro: {dkp_root})', foreground='green')
            self.fix.grid_remove()
        else:
            self.pc.configure(text='missing: ' + '; '.join(problems), foreground='#b45309')
            # (Linux and macOS: pacman wants sudo, which wants a terminal)
            if py or (dkp and sys.platform == 'win32'):
                self.fix.grid()
            elif dkp:
                self.pc.configure(text=self.pc.cget('text') + '. Install devkitPro\'s pacman '
                                  '(devkitpro.org/wiki/devkitPro_pacman), then in a terminal: sudo dkp-pacman -S '
                                  + ' '.join(dkp))

    def install_missing(self):
        if self.busy:
            return
        py, dkp, pacman = self.missing
        self.busy = True
        self.fix.configure(state='disabled')
        self.background(self._install, py, dkp, pacman)

    def _install(self, py, dkp, pacman):
        try:
            if py:
                self.post(self.write, "installing Python's " + ' '.join(py) + ' with pip...')
                self.stream([sys.executable, '-m', 'pip', 'install', '--disable-pip-version-check'] + py)
            if dkp and not pacman and sys.platform == 'win32':
                self.post(self.write, "downloading devkitPro's installer...")
                exe = ss.get_installer()
                subprocess.Popen([str(exe)])
                done = threading.Event()
                self.post(self.wait_installer, done)
                done.wait()
                tc = ss.toolchain()
                dkp, pacman = ss.absent_packages(tc), ss.find_pacman(tc)
            if dkp and pacman and sys.platform == 'win32':
                self.post(self.write, "installing devkitPro's " + ' '.join(dkp) + ' (a few minutes)...')
                self.stream(pacman + ['pacman -S --needed --noconfirm ' + ' '.join(dkp)])
        except Exception as e:  # noqa: BLE001 - shown to the player, who can try again
            self.post(self.write, f'could not install: {e}')
        finally:
            self.post(self.installed)

    def wait_installer(self, done):
        messagebox.showinfo(TITLE, "devkitPro's installer is open.\n\nKeep its folder (C:\\devkitPro) and tick "
                            '"Switch Development" when it asks which parts to install.\n\nClick OK here when it has finished.')
        done.set()

    def installed(self):
        self.busy = False
        self.fix.configure(state='normal')
        self.check_pc()

    def stream(self, cmd):
        """cmd's output into the log, a line at a time; raises when it fails."""
        p = subprocess.Popen([str(c) for c in cmd], cwd=ss.ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, text=True, errors='replace', creationflags=NO_WINDOW)
        for line in p.stdout:
            self.post(self.line, line.rstrip('\n'))
        if p.wait():
            raise RuntimeError(f'{Path(str(cmd[0])).name} stopped with code {p.returncode}')

    # ---- the game, the card, the server ---------------------------------------------------------------

    def pick_game(self):
        d = filedialog.askdirectory(title='The FINAL FANTASY XI folder (with FFXiMain.dll and ROM in it)',
                                    initialdir=self.game.get() or None, mustexist=True)
        if d:
            self.game.set(os.path.normpath(d))

    def game_changed(self, folder):
        if folder != self.game.get():  # typed on since: the later call does it
            return
        self.background(self._identify, folder)

    def _identify(self, folder):
        if not folder:
            return
        if not ss.is_game(folder):
            self.post(self.game_identified, folder, None, 'not a FINAL FANTASY XI folder: it needs FFXiMain.dll, FFXi.dll and ROM', [])
            return
        try:
            label = ss.identify(folder)
            text, ok = f'game version {label}: supported', label
        except ss.Failure:
            digest = ss.buildinfo.sha256(os.path.join(folder, 'FFXiMain.dll'))
            text, ok = (f'a game version XI on Anything does not know yet (SHA-256 {digest[:16]}...): it needs its '
                        'metadata first (docs/new-client-version.md)'), None
        self.post(self.game_identified, folder, ok, text, ss.server_hints(folder))

    def game_identified(self, folder, ok, text, hints):
        if folder != self.game.get():
            return
        self.game_info.configure(text=text, foreground='green' if ok else '#b91c1c')
        if hints and (not self.server.get() or self.server.get() == self.server_auto):
            self.server_auto = hints[0][0]
            self.server.set(self.server_auto)
            self.server_info.configure(text=f'found in your launcher: {hints[0][1]}', foreground='green')

    def find_cards(self):
        found = ss.card_candidates()
        self.cards['values'] = found
        if not self.card.get():
            self.card.set(found[0] if len(found) == 1 else (self.saved.get('sd') if self.saved.get('sd') != 'emu' else '') or '')

    def pick_card(self):
        d = filedialog.askdirectory(title="The SD card's top folder", initialdir=self.card.get() or None, mustexist=True)
        if d:
            self.card.set(os.path.normpath(d))

    # ---- build and install --------------------------------------------------------------------------

    def start(self):
        if self.busy:
            return
        game, card, server = self.game.get().strip(), self.card.get().strip(), self.server.get().strip()
        if not ss.is_game(game):
            messagebox.showerror(TITLE, 'Pick the FINAL FANTASY XI folder first (the one with FFXiMain.dll and ROM in it).')
            return
        if not card or (card != 'emu' and not Path(card).is_dir()):
            messagebox.showerror(TITLE, "Pick the SD card's folder first (put the card in the PC, or the console's over "
                                 "USB with Hekate's USB Mass Storage).")
            return
        cmd = [sys.executable, '-u', HERE / 'setup_switch.py', '--game', game, '--sd', card, '--server', server,
               '--layout', self.layout.get()]
        if not self.copy_game.get():
            cmd.append('--no-copy-game')
        if not self.icon.get():
            cmd.append('--no-icon')
        self.icon_wanted = self.icon.get() and card != 'emu'
        self.busy = True
        self.go.configure(state='disabled', text='Working... (the first build takes a long time)')
        self.bar.configure(value=0)
        self.background(self._run, cmd)

    def _run(self, cmd):
        try:
            self.stream(cmd)
            self.post(self.finished, None)
        except Exception as e:  # noqa: BLE001
            self.post(self.finished, str(e))

    def line(self, text):
        m = STEP.match(text)
        if m:
            self.step.configure(text=f'Step {m.group(1)} of {m.group(2)}: {m.group(3)}')
            self.bar.configure(value=0)
        elif BAR.match(text):
            pct, detail = BAR.match(text).groups()
            self.bar.configure(value=int(pct))
            self.step.configure(text=self.step.cget('text').split(' - ')[0] + (f' - {detail.strip()}' if detail.strip() else ''))
            return  # (the bar shows it: not every percent in the log)
        elif COPY.match(text):
            n, total = map(int, COPY.match(text).groups())
            self.bar.configure(value=100 * n / max(total, 1))
        self.write(text)

    def finished(self, error):
        self.busy = False
        self.go.configure(state='normal', text='Build and install')
        if error:
            self.step.configure(text='Stopped: see the log below (and build/switch/setup.log)')
            messagebox.showerror(TITLE, 'Setup stopped. The log in the window says why.\n\nFix that and click '
                                 '"Build and install" again: it carries on from where it was.')
        else:
            self.bar.configure(value=100)
            self.step.configure(text='Done')
            icon = ('\n\n' + ss.ICON_STEPS.replace('\n', ' ') + ' Then XI on Anything starts from its own icon.'
                    if getattr(self, 'icon_wanted', False) else '')
            messagebox.showinfo(TITLE, 'Done.\n\nOn the Switch: hold R while starting any game to open the homebrew menu '
                                'with full memory, then pick XI on Anything.' + icon + '\n\nAfter a game update, open '
                                'this again and click "Build and install": it does only what changed.')


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use('vista' if os.name == 'nt' else 'clam')
    except tk.TclError:
        pass
    App(root)
    root.mainloop()


if __name__ == '__main__':
    main()
