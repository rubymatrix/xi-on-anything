"""Find what host/frame.c needs to run FFXiMain past 60 fps in this build, and record it in meta/builds.json.

  python3 tools/frame_sites.py [--dry-run] [--list]

FFXiMain steps its world by a frame delta in sixtieths of a second. The frame pacer (once a frame)
measures the frame, averages the last four, truncates the average to whole sixtieths and keeps it
between the frame-rate divisor and 20 at +0x28 of the game object; two getters hand it out, never
below 1. So past 60 fps every frame counts as a whole sixtieth and the game runs fast. host/frame.c
gives the delta's users the real one instead, and needs:

  frame_pace          (hooks) the pacer, just after it stores the delta, before it takes 60 over it
  frame_dt, frame_dt2 (wraps) the two getters (fld [obj+0x28], at least 1.0)
  frame_count         (wraps) the frame counter's getter ([obj+0x34], one more each frame): a few
                      callers turn it into time (an angle 16 degrees a frame, every 4th frame), so
                      past 60 fps it counts sixtieths instead
  frame_yield         (wraps) the lobby coroutine's wait for the next frame: the lobby counts frames
                      (a fade, the network bar kept up for so many frames) made for 30 fps, so past
                      30 fps one wait lasts a whole thirtieth - but only in the loops that count them
  frame_wait_sites    the lobby yield's return addresses inside a counted loop (dec; jne back, or
                      inc; cmp n; jl back). Its other callers poll the network's state for a value
                      that may last one frame, and keep waiting a frame at a time.
  frame_ease          (hooks) the scene camera's (title, character select, cutscenes) ease toward its
                      next pose, a fraction of the way each frame (a byte of the scene / 255): the
                      host makes the fraction one for the frame's length, so the camera keeps the
                      pace it has at 30 fps in the menus, 60 in the world
  frame_cam1..4       (hooks) the third-person camera's distance ease, a fraction of the way each frame:
                      past 100 back by half the excess, past 6 by 0.012 of it, under 3 out by 0.125
                      of the shortfall (two places). The host scales each step to the frame's length
                      (the character select's opening move is the under-3 one)
  frame_net           (wraps) the zone connection's tick (once a frame: what arrived handled, what is due
                      sent): the host runs it 30 times a second past 30 fps, as the game shipped
  frame_net_clock     the global holding its clock (+8 the milliseconds since the last frame, set once a
                      frame): the host adds the skipped frames' to the next tick's
  frame_net_wait      (wraps) the connection's wait for the server (signing in, a zone change): a step a
                      call, one call a frame, timed out at 3600 steps (FFXI-3001) - a minute at 60 fps,
                      a second on a loading screen past 3000. At divisor 0 the host steps it once a
                      sixtieth of the wall clock. Optional: not found, the rest is still written (and one
                      already there, matched some other way, kept)
  frame_cam_once      the third-person camera's two calls that count its steps this frame (n = (int)dt,
                      then "step < n" after each): past 60 fps the host has it step once every frame,
                      not once a whole sixtieth, so it moves as often as the character it follows
  frame_cam_tick, frame_cam_swing (hooks) and that step's own clocks, so a step is as long as the frame:
                      a counter down by 1 a step (made the frame's delta), and a swing behind the
                      player by a fixed amount a step (multiplied by it)
  frame_cam_wait      (hooks) and the step's wait after the player moves (4 steps before it follows again,
                      dec a step): counted in whole sixtieths, not steps, now that a step is a frame
  frame_look_once     the third-person camera's two eases toward the player (n = (int)dt steps of a
                      quarter / an eighth of the way): past 60 fps one step every frame
  frame_look1, frame_look2 (hooks) and their fractions (pushed for the scale), made one for the frame's length
  frame_int_sites     the getters' return addresses whose caller truncates the delta (or a value
                      made from it: t = (int)(t - dt), n = (int)dt, i += (int)(dt * k)) into
                      integer state. A fraction there would round every frame's step to the same
                      whole number (a countdown by 1 every frame, a count that never moves), so
                      host/frame.c gives these whole sixtieths, carrying the rest to the next frame.
                      Every other caller keeps the delta as a float (positions, angles, animation
                      time) and gets it as it is.

A caller is told apart from the x87 stack after the call, followed until the delta (or a value
made from it) is stored as a float, compared, or truncated by the CRT's _ftol.
"""
import argparse
import json
import os
import re
import struct
import sys

import capstone
import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import buildinfo  # noqa: E402

IMAGE = os.path.join(buildinfo.ROOT, 'generated', 'FFXiMain.unpacked.dll')

# the getter: mov ecx,[game]; fld [ecx+0x28]; fcomp [1.0]; fnstsw ax; test ah,5; jp +7;
#             fld [1.0]; ret; fld [ecx+0x28]; ret
GETTER = re.compile(re.escape(bytes.fromhex('8b0d')) + b'(....)' + re.escape(bytes.fromhex('d94128d81d')) + b'(....)' +
                    re.escape(bytes.fromhex('dfe0f6c4057a07d905')) + b'....' + re.escape(bytes.fromhex('c3d94128c3')), re.S)
# the pacer's end: fstp [esi+0x2c]; fld [60.0]; fdiv [esi+0x28]  (hooked at the fld)
# the frame counter's getter: mov eax,[game]; mov eax,[eax+0x34]; ret
COUNT = b'\xa1%s\x8b\x40\x34\xc3'
# the lobby's yield: push esi; mov esi,[esp+8]; call <switch>; test esi,esi; jne +9; cmp al,1; jne +0x1c; or eax,-1
YIELD = re.compile(re.escape(bytes.fromhex('568b742408e8')) + b'....' + re.escape(bytes.fromhex('85f675093c01751c83c8ff5ec3')), re.S)
# the scene camera's ease: fild [esp+0xc]; fmul [1/255]; fst [esp+0xc]; fcomp [0.0]  (hooked at the fcomp)
EASE = re.compile(re.escape(bytes.fromhex('db44240cd80d')) + b'....' + re.escape(bytes.fromhex('d954240cd81d')), re.S)
# the third-person camera's distance ease: (fsub [100]; fmul [0.5]), (fsub [6]; push ecx; fmul [0.012]),
# (fsub [esp+0x10]; push ecx; lea eax,[esp+0x2c]; fmul [-0.125]), (fmul st(1); push edx; push ebp; push eax;
# fmul [0.125]) - hooked at each fmul
CAM_EASES = [  # (pattern, offset of the fmul, the constant before the fmul, the constant multiplied by)
    (re.compile(re.escape(bytes.fromhex('d825')) + b'(....)' + re.escape(bytes.fromhex('d80d')) + b'(....)', re.S), 6, 100.0, 0.5),
    (re.compile(re.escape(bytes.fromhex('d825')) + b'(....)' + re.escape(bytes.fromhex('51d80d')) + b'(....)', re.S), 7, 6.0, 0.012),
    (re.compile(re.escape(bytes.fromhex('d8642410518d44242cd80d')) + b'()(....)', re.S), 9, None, -0.125),
    (re.compile(re.escape(bytes.fromhex('d8c9525550d80d')) + b'()(....)', re.S), 5, None, 0.125),
]
# the zone connection's tick: sub esp,0x10c; push ebx; push esi; push edi; xor bl,bl; call <now>;
# mov esi,[esp+0x11c]; mov edi,eax; test esi,esi
NET = re.compile(re.escape(bytes.fromhex('81ec0c01000053565732dbe8')) + b'....' + re.escape(bytes.fromhex('8bb4241c0100008bf885f6')), re.S)
# its clock's update, once a frame: mov eax,[clock]; mov ecx,[eax+0x10]; mov edx,[eax+4]; push ecx; push edx;
# push 1000; call <elapsed>; mov ecx,[clock]; add esp,0xc; mov [ecx+8],eax
NET_CLOCK = re.compile(re.escape(bytes.fromhex('a1')) + b'(....)' + re.escape(bytes.fromhex('8b48108b5004515268e8030000e8')) +
                       b'....' + re.escape(bytes.fromhex('8b0d')) + rb'\1' + re.escape(bytes.fromhex('83c40c894108')), re.S)
# its wait for the server: mov eax,[n]; xor ecx,ecx; inc eax; cmp eax,3600; mov [n],eax; setge cl; mov eax,ecx; ret
NET_WAIT = re.compile(re.escape(bytes.fromhex('a1')) + b'(....)' + re.escape(bytes.fromhex('33c9403d100e0000a3')) + rb'\1' +
                      re.escape(bytes.fromhex('0f9dc18bc1c3')), re.S)
# the third-person camera's step count: mov [esp+0x18],ebx; call getter; call ftol; test eax,eax; jle; mov ebx,10
CAM_ONCE = [re.compile(re.escape(bytes.fromhex('895c2418e8')) + b'....\xe8....' + re.escape(bytes.fromhex('85c00f8e')) + b'....' +
                       re.escape(bytes.fromhex('bb0a000000')), re.S),
            # and its loop test: mov [esp+0x18],ebp; call getter; call ftol; cmp ebp,eax; jl
            re.compile(re.escape(bytes.fromhex('896c2418e8')) + b'....\xe8....' + re.escape(bytes.fromhex('3be80f8c')), re.S)]
# its counter: fld [g]; fsub [1.0]; fst [g]  (hooked at the fsub)
CAM_TICK = re.compile(re.escape(bytes.fromhex('d905')) + b'(....)' + re.escape(bytes.fromhex('d825')) + b'(....)' +
                      re.escape(bytes.fromhex('d915')) + b'(....)', re.S)
# its swing: fild [n]; add esp,0xc; lea eax,[esp+0x50]; lea ecx,[esp+0x50]; fmul [0.01]; fmul [0.05]  (hooked at the second)
CAM_SWING = re.compile(re.escape(bytes.fromhex('db05')) + b'....' + re.escape(bytes.fromhex('83c40c8d4424508d4c2450d80d')) +
                       b'(....)' + re.escape(bytes.fromhex('d80d')) + b'(....)', re.S)
# its wait: mov eax,[g]; dec eax; mov [g],eax; jns +0xc  (hooked at the store)
CAM_WAIT = re.compile(re.escape(bytes.fromhex('a1')) + b'(....)' + re.escape(bytes.fromhex('48a3')) + b'(....)' +
                      re.escape(bytes.fromhex('790c')), re.S)
# the camera's eases toward the player: call getter; call ftol; test eax,eax; jle; mov ebx|ebp,eax; ...
# push 0x3e800000|0x3e000000 (0.25, 0.125); push reg; call <scale>  (hooked at the call)
LOOK_ONCE = [re.compile(re.escape(bytes.fromhex('e8')) + b'....\xe8....' + re.escape(bytes.fromhex('85c07e')) + b'.' +
                        re.escape(bytes.fromhex(t)), re.S) for t in ('8bd8', '8be8')]
LOOK_K = [re.compile(re.escape(bytes.fromhex('680000803e52e8')), re.S), re.compile(re.escape(bytes.fromhex('680000003e51e8')), re.S)]
PACE = re.compile(re.escape(bytes.fromhex('d95e2cd905')) + b'(....)' + re.escape(bytes.fromhex('d87628')), re.S)


def die(msg):
    raise SystemExit('frame_sites: ' + msg)


def classify(md, read, ret, ftol, getters):
    """'int' when the caller truncates the delta into an integer, 'float' otherwise, and why."""
    stack = ['dt']  # x87 stack, top first: 'dt' for the delta or a value made from it
    at, steps = ret, 0
    while steps < 40:
        insn = next(md.disasm(read(at, 16), at), None)
        if insn is None:
            return 'float', 'undecodable at %#x' % at
        steps += 1
        m, ops = insn.mnemonic, insn.op_str
        nxt = at + insn.size
        if m == 'call':
            if ops == hex(ftol):
                return ('int', 'ftol at %#x' % at) if stack and stack[0] == 'dt' else ('float', 'ftol of another value')
            if ops in getters:  # another delta: the top was used up as a float before
                return 'float', 'call at %#x' % at
            return 'float', 'call at %#x' % at
        if m == 'jmp' and ops.startswith('0x'):
            at = int(ops, 16)
            continue
        if m.startswith('j') or m == 'ret':
            return 'float', '%s at %#x' % (m, at)
        if not m.startswith('f'):
            at = nxt  # integer work in between (mov, lea, push, movsx...)
            continue
        two = [o.strip() for o in ops.split(',')] if ops else []
        mem = any('ptr' in o for o in two)
        if m in ('fld', 'fild', 'fldz', 'fld1'):
            src = 'other'
            if m == 'fld' and two and two[0].startswith('st('):
                i = int(two[0][3])
                src = stack[i] if i < len(stack) else 'other'
            stack.insert(0, src)
        elif m in ('fst', 'fstp', 'fist', 'fistp'):
            if two and two[0].startswith('st('):  # fstp st(i): st(i) = st0, pop
                i = int(two[0][3])
                if i < len(stack):
                    stack[i] = stack[0]
                if m == 'fstp':
                    stack.pop(0)
            else:
                if stack and stack[0] == 'dt':
                    return 'float', '%s at %#x' % (m, at)
                if m in ('fstp', 'fistp'):
                    stack.pop(0)
        elif m == 'fxch':
            i = int(two[0][3]) if two else 1
            if i < len(stack):
                stack[0], stack[i] = stack[i], stack[0]
        elif m in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp', 'ftst', 'fcomi', 'fcomip', 'fucomi', 'fucomip'):
            return 'float', '%s at %#x' % (m, at)
        elif m in ('fchs', 'fabs', 'frndint', 'fsqrt', 'fsin', 'fcos'):
            pass
        elif re.match(r'^fi?(add|sub|subr|mul|div|divr)p?$', m):
            if mem or not two:  # st0 op= mem, or the p form with no operands: st1 op= st0, pop
                if not two:
                    if len(stack) >= 2:
                        stack[1] = 'dt' if 'dt' in stack[:2] else 'other'
                    stack.pop(0)
            else:
                regs = [int(o[3]) if o.startswith('st(') else 0 for o in two]
                if len(regs) == 2:
                    d, s = regs
                else:  # one operand: st0 op= st(i), or the p form's st(i) op= st0, pop
                    d, s = (regs[0], 0) if m.endswith('p') else (0, regs[0])
                if max(d, s) < len(stack):
                    stack[d] = 'dt' if 'dt' in (stack[d], stack[s]) else 'other'
                if m.endswith('p'):
                    stack.pop(0)
        elif m == 'fnstsw' or m == 'fstsw':
            pass
        else:
            return 'float', 'unhandled %s at %#x' % (m, at)
        if 'dt' not in stack:
            return 'float', 'delta used up at %#x' % at
        at = nxt
    return 'float', 'too long'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dry-run', action='store_true', help='print, do not write meta/builds.json')
    ap.add_argument('--list', action='store_true', help='print every caller and how it was classified')
    args = ap.parse_args()
    build = buildinfo.current()
    pe = pefile.PE(IMAGE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    lo, hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
    ftol = int(build['crt']['ftol'], 16)

    def read(va, n):
        return img[va - base:va - base + n]

    getters = [base + lo + m.start() for m in GETTER.finditer(img[lo:hi])]
    if len(getters) != 2:
        die('delta getters: %d matches, want 2' % len(getters))
    games = {read(g + 2, 4) for g in getters}
    if len(games) != 1:
        die('the getters read different globals')
    game = games.pop()
    counts_at = [base + lo + m.start() for m in re.finditer(re.escape(COUNT % game), img[lo:hi])]
    if len(counts_at) != 1:
        die('frame counter getter: %d matches, want 1' % len(counts_at))
    yields = [base + lo + m.start() for m in YIELD.finditer(img[lo:hi])]
    if len(yields) != 1:
        die('lobby yield: %d matches, want 1' % len(yields))
    eases = [base + lo + m.start() + 14 for m in EASE.finditer(img[lo:hi])]
    if len(eases) != 1:
        die('scene camera ease: %d matches, want 1' % len(eases))
    def f32(va):
        return struct.unpack('<f', read(va, 4))[0]
    cams = []
    for rx, off, before, k in CAM_EASES:
        hits = []
        for m in rx.finditer(img[lo:hi]):
            if before is not None and abs(f32(struct.unpack('<I', m.group(1))[0]) - before) > 1e-6:
                continue
            if abs(f32(struct.unpack('<I', m.group(2))[0]) - k) > 1e-6:
                continue
            hits.append(base + lo + m.start() + off)
        if len(hits) != 1:
            die('camera distance ease (%g): %d matches, want 1' % (k, len(hits)))
        cams.append(hits[0])
    once = []
    for rx in CAM_ONCE:
        hits = []
        for m in rx.finditer(img[lo:hi]):
            at = base + lo + m.start()
            call1 = at + 9 + struct.unpack('<i', read(at + 5, 4))[0]
            call2 = at + 14 + struct.unpack('<i', read(at + 10, 4))[0]
            if call1 in getters and call2 == ftol and (not once or 0 < at - once[0] < 0x600):
                hits.append(at + 9)
        if len(hits) != 1:
            die('third-person camera step count: %d matches, want 1' % len(hits))
        once.append(hits[0])
    ticks = [base + lo + m.start() + 6 for m in CAM_TICK.finditer(img[lo:hi])
             if m.group(1) == m.group(3) and abs(f32(struct.unpack('<I', m.group(2))[0]) - 1.0) < 1e-9 and
             base + lo + m.start() + 6 - once[0] in range(0, 0x40)]
    if len(ticks) != 1:
        die('third-person camera counter: %d matches, want 1' % len(ticks))
    swings = [base + lo + m.start() + 0x17 for m in CAM_SWING.finditer(img[lo:hi])
              if abs(f32(struct.unpack('<I', m.group(1))[0]) - 0.01) < 1e-9 and abs(f32(struct.unpack('<I', m.group(2))[0]) - 0.05) < 1e-9]
    if len(swings) != 1:
        die('third-person camera swing: %d matches, want 1' % len(swings))
    waits_cam = [base + lo + m.start() + 6 for m in CAM_WAIT.finditer(img[lo:hi])
                 if m.group(1) == m.group(2) and 0 < base + lo + m.start() - once[0] < 0x400]
    if len(waits_cam) != 1:
        die('third-person camera wait: %d matches, want 1' % len(waits_cam))
    look_once, look_k = [], []
    for rx, kx in zip(LOOK_ONCE, LOOK_K):
        hits = []
        for m in rx.finditer(img[lo:hi]):
            at = base + lo + m.start()
            call1 = at + 5 + struct.unpack('<i', read(at + 1, 4))[0]
            call2 = at + 10 + struct.unpack('<i', read(at + 6, 4))[0]
            if call1 in getters and call2 == ftol and 0x1000 > abs(at - once[0]):
                hits.append(at + 5)
        ks = [base + lo + m.start() + 6 for m in kx.finditer(img[lo:hi]) if hits and 0 < base + lo + m.start() - hits[0] < 0x60]
        if len(hits) != 1 or len(ks) != 1:
            die('camera ease toward the player: %d/%d matches, want 1/1' % (len(hits), len(ks)))
        look_once.append(hits[0]), look_k.append(ks[0])
    nets = [base + lo + m.start() for m in NET.finditer(img[lo:hi])]
    if len(nets) != 1:
        die('zone connection tick: %d matches, want 1' % len(nets))
    clocks = [struct.unpack('<I', m.group(1))[0] for m in NET_CLOCK.finditer(img[lo:hi])]
    if len(clocks) != 1:
        die('zone connection clock: %d matches, want 1' % len(clocks))
    # (a function of its own: padding or a ret before it). Optional: without it frame.c says an uncapped
    # zone change can time out, and the rest still runs
    net_waits = [base + lo + m.start() for m in NET_WAIT.finditer(img[lo:hi]) if img[lo + m.start() - 1] in (0x90, 0xcc, 0xc3)]
    if len(net_waits) != 1:
        print('frame_sites: wait for the server: %d matches, want 1; frame_net_wait not written (one already in '
              'meta/builds.json is kept: check it by hand)' % len(net_waits))
    paces = [base + lo + m.start() + 3 for m in PACE.finditer(img[lo:hi])]
    if len(paces) != 1:
        die('frame pacer: %d matches, want 1' % len(paces))
    if struct.unpack('<f', read(struct.unpack('<I', read(paces[0] + 2, 4))[0], 4))[0] != 60.0:
        die('frame pacer at %#x: not 60 over the delta' % paces[0])

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    names = {hex(g) for g in getters}
    sites, counts, tails = [], {'int': 0, 'float': 0}, 0
    md.skipdata = True  # a linear sweep over .text: data in it is skipped, not the end
    for insn in md.disasm(bytes(img[lo:hi]), base + lo):
        if insn.mnemonic not in ('call', 'jmp') or insn.op_str not in names:
            continue
        if insn.mnemonic == 'jmp':
            tails += 1  # a tail call: its caller's caller gets the delta; left as a float
            continue
        ret = insn.address + insn.size
        kind, why = classify(md, read, ret, ftol, names)
        counts[kind] += 1
        if kind == 'int':
            sites.append(ret)
        if args.list:
            print('%08x %-5s %s' % (ret, kind, why))
    waits = []
    for insn in md.disasm(bytes(img[lo:hi]), base + lo):
        if insn.mnemonic != 'call' or insn.op_str != hex(yields[0]):
            continue
        ret = insn.address + insn.size
        body = list(md.disasm(read(ret, 48), ret))[:8]
        for j, b in enumerate(body):
            if b.mnemonic == 'call':
                break
            if b.mnemonic in ('jne', 'jl', 'jb') and b.op_str.startswith('0x') and insn.address - 8 <= int(b.op_str, 16) <= insn.address:
                if any(x.mnemonic in ('dec', 'inc') for x in body[:j]):
                    waits.append(ret)
                break
    if not waits:
        die('lobby yield: no counted waits')
    print('frame_sites: getters %s, pacer hook %#x; %d callers truncate the delta, %d keep it as a float, %d tail calls'
          % (', '.join('%#x' % g for g in getters), paces[0], counts['int'], counts['float'], tails))
    out = {
        'hooks': dict({'frame_pace': '0x%08x' % paces[0], 'frame_ease': '0x%08x' % eases[0]},
                      **{'frame_cam%d' % (i + 1): '0x%08x' % a for i, a in enumerate(cams)},
                      frame_cam_tick='0x%08x' % ticks[0], frame_cam_swing='0x%08x' % swings[0],
                      frame_cam_wait='0x%08x' % waits_cam[0],
                      frame_look1='0x%08x' % look_k[0], frame_look2='0x%08x' % look_k[1]),
        'frame_look_once': ['0x%08x' % a for a in look_once],
        'frame_cam_once': ['0x%08x' % a for a in once],
        'wraps': {'frame_dt': '0x%08x' % getters[0], 'frame_dt2': '0x%08x' % getters[1],
                  'frame_count': '0x%08x' % counts_at[0], 'frame_yield': '0x%08x' % yields[0],
                  'frame_net': '0x%08x' % nets[0]},
        'frame_int_sites': ['0x%08x' % s for s in sites],
        'frame_wait_sites': ['0x%08x' % s for s in waits],
        'frame_net_clock': '0x%08x' % clocks[0],
    }
    if len(net_waits) == 1:
        out['wraps']['frame_net_wait'] = '0x%08x' % net_waits[0]
    if args.dry_run:
        print(json.dumps(out, indent=2))
        return
    with open(buildinfo.BUILDS) as f:
        meta = json.load(f)
    b = meta['builds'][build['build']]
    for section, values in out.items():
        if isinstance(values, dict):
            b.setdefault(section, {}).update(values)
        else:
            b[section] = values
    with open(buildinfo.BUILDS, 'w') as f:
        json.dump(meta, f, indent=2)
        f.write('\n')
    print('frame_sites: wrote build %s in %s' % (build['build'], os.path.relpath(buildinfo.BUILDS, buildinfo.ROOT)))


if __name__ == '__main__':
    main()
