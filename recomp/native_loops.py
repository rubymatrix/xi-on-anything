"""Hot game loops with a native version in the runtime (runtime/native_skin.h), found by their bytes.

A loop is a run of the image's bytes ending where the loop exits, the address constants in it (the image's
relocations) masked: so a game version that moved the loop, or the globals it reads, still matches; one that
changed its code doesn't, and keeps the translation. Just after the run's first instruction (the last before the
loop's head) the translation calls the native version; when it ran the loop (it declines at the x87's 24-bit
precision), the function goes on at the exit with the registers it gives back. With FFXI_SKIN_CHECK=1 it only
computes, the translated loop runs, and the exit compares the two (runtime/native_skin.h).

XI_NATIVE_LOOPS=0 when translating leaves them all to the translation.
"""
import hashlib
import os

ON = os.environ.get('XI_NATIVE_LOOPS', '1') != '0'

# FFXiMain's two-bone skinning loop (0x1002f273-0x1002f7a1 in 2025-12-26, inside f_1002efe0): 4% of the game thread
# on the Switch in a crowd. The globals: the matrix table's pointer, and the index and count words.
LOOPS = [
    dict(fn='rt_skin_two_bone', length=1326,
         sha256='2b70b0314b5758d2e4da33b235bfc007754e3de411976a47c0342fccc9145c1c',
         relocs=(0xe0, 0x187, 0x22e, 0x2f0, 0x312, 0x3b5, 0x45a, 0x51a),
         globals=dict(mtx=0xe0, idx=0x2f0, cnt=0x51a),
         check='rt_skin_two_bone_check'),
]


def masked(ctx, at, loop):
    """the bytes at `at` with its relocations zeroed, or None when they aren't where the loop has them"""
    n = loop['length']
    code = bytearray(ctx.read(at, n))
    rel = sorted(a - at for a in ctx.relocs if at <= a < at + n)
    if tuple(rel) != loop['relocs']:
        return None
    for r in rel:
        code[r:r + 4] = b'\0\0\0\0'
    return bytes(code)


def sites(ctx):
    """{the address of a loop's first instruction: (the loop, its exit, its globals' static addresses)}, once per
    translation"""
    if getattr(ctx, 'native_loops', None) is not None:
        return ctx.native_loops
    found = {}
    if ON:
        image = bytes(ctx.image)
        for loop in LOOPS:
            n = loop['length']
            for fn, ranges in ctx.functions.items():
                for lo, hi in ranges:
                    # (a cheap first test: the loop's first instruction, mov [esp+0x3c], edx, in this range)
                    start = lo - ctx.base
                    i = image.find(b'\x89\x54\x24\x3c', start, hi - ctx.base)
                    while i >= 0:
                        at = ctx.base + i
                        if at + n <= hi:
                            code = masked(ctx, at, loop)
                            if code is not None and hashlib.sha256(code).hexdigest() == loop['sha256']:
                                g = {k: int.from_bytes(ctx.read(at + off, 4), 'little')
                                     for k, off in loop['globals'].items()}
                                found[at] = (loop, at + n, g)
                        i = image.find(b'\x89\x54\x24\x3c', i + 1, hi - ctx.base)
    ctx.native_loops = found
    return found


def call(loop, exit_, g):
    """the statements after the loop's first instruction"""
    return ['{ RtSkinOut o = %s(RT_GB, eax, ecx, edx, ebx, esp, ebp, esi, g->fcw, 0x%08Xu + RD, 0x%08Xu + RD, '
            '0x%08Xu + RD);' % (loop['fn'], g['mtx'], g['idx'], g['cnt']),
            '  if (o.ok) { eax = o.eax; ecx = o.ecx; edx = o.edx; ebx = o.ebx; ebp = o.ebp; esi = o.esi; '
            'cf = 0; zf = 1; sf = 0; of = 0; pf = 1; af = 0; goto L_%08x; } }' % exit_]


def at_exit(loop):
    """the statement before the instruction at the loop's exit"""
    return 'if (rt_native_check > 0) %s(RT_GB);' % loop['check']
