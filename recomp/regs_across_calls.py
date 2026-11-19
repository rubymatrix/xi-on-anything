"""Which registers cross a call, so a call stores and reloads, and a return stores, only those (runtime/guest.h,
REGS_PUT and REGS_GET). Every call stored all eight registers and loaded them back, every return stored all eight,
though a function reads few at its entry and leaves most as it found them.

For every function f:
  - W(f), what it may leave changed for its caller: the registers it (or anything it calls) may write, less those it
    gives back as it found them (P: saved by a push and restored by the matching pop on every way out, MSVC's
    callee-saved esi, edi, ebx, ebp mostly; an abstract interpretation of the stack, below);
  - U(f), what it may use of what its caller left: the registers read on some path from its entry before they are
    written (a liveness, a call using what its callee may use), and W(f) too: a register written on one path and not
    another goes back (REGS_PUT at the return) as the caller left it on the other.
A direct call to t stores U(t) and reloads W(t). A return stores W(f). esp is always in both; an indirect call or
jump, a tail jump's store, a hook, cpuid and rdtsc keep storing and loading everything. Both sets are least
fixpoints over the call graph. A register outside W(t) after a call is the caller's own local, its value then.

The stack interpretation: esp's offset from the entry, ebp's when it was set from esp, the slots holding a register's
entry value (a push of one still holding it), and the registers that hold theirs. A pop from such a slot gives the
register its entry value back. A call to t moves esp by t's ret n (its rets agreeing), an indirect call by what isn't
known, until `mov esp, ebp`, `lea esp, [ebp+d]` or `leave` set it from ebp again. Paths meet by keeping what they agree
on. Only ebx, esi, edi and ebp are saved by a push (MSVC's `push ecx` reserves a local); a store at a known esp or
ebp offset drops the slots it overlaps.
"""
from capstone import x86_const as X

REGS = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
BIT = {r: 1 << i for i, r in enumerate(REGS)}
ALL = 0xFF
ESP = BIT['esp']
FLAGS_BIT = 0x100  # REGS_PUT/REGS_GET's mask: the arithmetic flags (flags_across_calls.py decides those)

SAVED4 = ("ebx", "esi", "edi", "ebp")
SUB = {}
for r in REGS:
    SUB[r] = (r, 32)
for r16, r in (('ax', 'eax'), ('cx', 'ecx'), ('dx', 'edx'), ('bx', 'ebx'), ('sp', 'esp'), ('bp', 'ebp'),
               ('si', 'esi'), ('di', 'edi')):
    SUB[r16] = (r, 16)
for r8, r in (('al', 'eax'), ('ah', 'eax'), ('cl', 'ecx'), ('ch', 'ecx'), ('dl', 'edx'), ('dh', 'edx'),
              ('bl', 'ebx'), ('bh', 'ebx')):
    SUB[r8] = (r, 8)

EXIT_MN = ('ret', 'retn', 'int3', 'hlt', 'ud2')


def access(ins):
    """(uses, must-writes, may-writes) of the eight registers for an ordinary instruction; ALL when unknown"""
    try:
        rd, wr = ins.regs_access()
    except Exception:
        return ALL, 0, ALL
    uses = kill = may = 0
    for r in rd:
        s = SUB.get(ins.reg_name(r))
        if s:
            uses |= BIT[s[0]]
    for r in wr:
        s = SUB.get(ins.reg_name(r))
        if s:
            may |= BIT[s[0]]
            if s[1] == 32:
                kill |= BIT[s[0]]
            else:
                uses |= BIT[s[0]]  # a partial write keeps the rest: the old value is used
    return uses, kill, may


class Fn:
    __slots__ = ('entry', 'insns', 'by', 'succ', 'calls', 'tails', 'popn', 'own_may', 'opaque', 'xtail', 'lost')


def describe(ctx, entry, translator):
    """the function's instructions and edges, from x86c's decoding"""
    t = translator(ctx, entry, ctx.functions[entry])
    t.decode()
    f = Fn()
    f.entry = entry
    f.insns = t.insns
    f.by = {i.address: i for i in t.insns}
    f.succ = {}
    f.calls = set()
    f.tails = set()
    f.xtail = {}  # a conditional jump's targets in other functions (the translation's `f_x(g); return;`)
    f.lost = False  # a jump to an address in no function
    f.opaque = entry in getattr(ctx, 'wraps', {})
    pops = set()
    for ins in t.insns:
        mn = ins.mnemonic
        nxt = ins.address + ins.size
        s = [x for x in t.jump_targets(ins) if x in t.addrs]
        falls = not (mn in EXIT_MN or mn.startswith('ret') or mn == 'jmp')
        if falls and nxt in t.addrs:
            s.append(nxt)
        f.succ[ins.address] = s
        if mn == 'call' and ins.operands and ins.operands[0].type == X.X86_OP_IMM:
            tg = ins.operands[0].imm & 0xFFFFFFFF
            if tg != nxt and tg in ctx.entries:
                f.calls.add(tg)
        if mn != 'call' and ins.address not in ctx.hooks:
            out = [x for x in t.jump_targets(ins) if x not in t.addrs]
            for tg in out:
                if tg in ctx.entries:
                    f.tails.add(tg)
                else:
                    f.lost = True
            if out and mn != 'jmp':
                f.xtail[ins.address] = out
        if mn.startswith('ret'):
            pops.add(ins.operands[0].imm if ins.operands else 0)
        if ins.address in ctx.hooks:
            f.opaque = True
    f.popn = pops.pop() if len(pops) == 1 else None
    return f


def kind(ctx, f, ins):
    """('call', t) | ('icall',) | ('tail', t) | ('ijmp',) | ('full',) | ('ret',) | ('stop',) | None (ordinary)"""
    mn = ins.mnemonic
    nxt = ins.address + ins.size
    if ins.address in ctx.hooks:
        return ('full',)
    if mn == 'call':
        op = ins.operands[0]
        if op.type == X.X86_OP_IMM:
            tg = op.imm & 0xFFFFFFFF
            if tg == nxt:
                return None  # call $+5: a push
            return ('call', tg) if tg in ctx.entries else ('stop',)
        return ('icall',)
    if mn == 'jmp':
        op = ins.operands[0]
        if op.type == X.X86_OP_IMM:
            tg = op.imm & 0xFFFFFFFF
            if tg in f.by:
                return None
            return ('tail', tg) if tg in ctx.entries else ('stop',)
        return None if ins.address in ctx.switches else ('ijmp',)
    if mn in ('cpuid', 'rdtsc'):
        return ('full',)
    if mn.startswith('ret'):
        return ('ret',)
    if mn in EXIT_MN:
        return ('stop',)
    return None


class Analysis:
    def __init__(self, ctx, translator):
        self.ctx = ctx
        self.fns = {}
        for e in ctx.functions:
            try:
                self.fns[e] = describe(ctx, e, translator)
            except Exception:
                pass
        self.W = {e: 0 for e in self.fns}
        self.U = {e: 0 for e in self.fns}
        self.save_push = {}
        self.kinds = {e: {i.address: kind(ctx, f, i) for i in f.insns} for e, f in self.fns.items()}
        self.acc = {e: {i.address: access(i) for i in f.insns} for e, f in self.fns.items()}
        for e, f in self.fns.items():
            f.own_may = 0
            for a, k in self.kinds[e].items():
                f.own_may |= self.acc[e][a][2] if k is None or k[0] in ('call', 'tail', 'ret') else ALL
            if f.opaque:
                self.W[e] = self.U[e] = ALL

    def w_of(self, t):
        return self.W.get(t, ALL)

    def u_of(self, t):
        return self.U.get(t, ALL)

    # ---------------------------------------------------------------- may-write, less the preserved
    def preserved(self, e):
        """the registers f gives back as it found them on every way out (P)"""
        f = self.fns[e]
        ks = self.kinds[e]
        # state: (sp, ebp_off, slots (frozenset of (offset, reg)), entry-valued regs mask); None: unreached
        st = {f.entry: (0, None, frozenset(), ALL, 0)}
        work = [f.entry]
        out = ALL
        seen_exit = False
        steps = 0
        saves = set()  # pushes of a register still holding its entry value (uses: not a use when it is preserved)
        while work:
            steps += 1
            if steps > 200000:
                return 0
            a = work.pop()
            ins = f.by[a]
            sp, bo, slots, ent, saved = st[a]
            k = ks[a]
            mn = ins.mnemonic
            ops = ins.operands
            nsp, nbo, nslots, nent, nsaved = sp, bo, set(slots), ent, saved
            if k is not None and k[0] == 'ret':
                seen_exit = True
                out &= ent if sp == 0 or sp is None else 0
                continue
            if k is not None and k[0] == 'tail':
                seen_exit = True
                out &= (ent & ~self.w_of(k[1])) if sp == 0 or sp is None else 0
                continue
            if k is not None and k[0] in ('ijmp',):
                seen_exit = True
                out = 0
                continue
            if k is not None and k[0] == 'stop':
                continue
            for t in f.xtail.get(a, ()):
                seen_exit = True
                out &= (ent & ~self.w_of(t)) if sp == 0 or sp is None else 0
            if k is not None and k[0] == 'full':
                nent = 0
            elif k is not None and k[0] == 'call':
                pn = self.fns[k[1]].popn if k[1] in self.fns else None
                nsp = None if sp is None or pn is None else sp + pn
                nent &= ~self.w_of(k[1])
            elif k is not None and k[0] == 'icall':
                # an indirect callee keeps the calling convention's callee-saved registers (any compiled function, the
                # runtime's shims); how much of the stack it pops isn't known
                nsp = None
                nent &= ~(BIT['eax'] | BIT['ecx'] | BIT['edx'])
            else:
                # the stack's own instructions
                reg0 = ins.reg_name(ops[0].reg) if ops and ops[0].type == X.X86_OP_REG else None
                if mn == 'push':
                    nsp = None if sp is None else sp - 4
                    if nsp is not None:
                        nslots = {s for s in nslots if s[0] != nsp}
                        # only the callee-saved four: MSVC's `push ecx` (eax, edx) reserves a local, written before
                        # its pop (the console check: 32 calls whose callee changed ecx through one)
                        if reg0 in SAVED4 and ent & BIT[reg0]:
                            nslots.add((nsp, reg0))
                            nsaved |= BIT[reg0]
                            saves.add(a)
                elif mn == 'pop' and reg0 in BIT and reg0 != 'esp':
                    if sp is not None and (sp, reg0) in slots:
                        nent |= BIT[reg0]
                    elif sp is None and saved & BIT[reg0] and reg0 in ('ebx', 'esi', 'edi', 'ebp'):
                        nent |= BIT[reg0]  # the stack lost to an indirect call: the epilogue's pop of what the prologue saved
                    else:
                        nent &= ~BIT[reg0]
                    nsp = None if sp is None else sp + 4
                    if sp is not None:
                        nslots = {s for s in nslots if s[0] != sp}
                elif mn == 'pop':
                    nsp = None if sp is None else sp + 4
                    if not (reg0 in (None,) or reg0 in BIT):
                        pass
                    if reg0 == 'esp':
                        nsp = None
                elif mn == 'leave':
                    nsp = None if bo is None else bo + 4
                    if bo is not None and (bo, 'ebp') in slots:
                        nent |= BIT['ebp']
                    elif saved & BIT['ebp'] and bo is not None:
                        nent |= BIT['ebp']  # ebp was pushed at bo (mov ebp, esp right after it)
                    else:
                        nent &= ~BIT['ebp']
                else:
                    uses, kill, may = self.acc[e][a]
                    if may & ESP:
                        nsp = None
                        if mn in ('add', 'sub') and reg0 == 'esp' and len(ops) == 2 and ops[1].type == X.X86_OP_IMM \
                                and sp is not None:
                            d = ops[1].imm & 0xFFFFFFFF
                            d = d - (1 << 32) if d & 0x80000000 else d
                            nsp = sp + d if mn == 'add' else sp - d
                        elif mn == 'mov' and reg0 == 'esp' and ops[1].type == X.X86_OP_REG and \
                                ins.reg_name(ops[1].reg) == 'ebp':
                            nsp = bo
                        elif mn == 'lea' and reg0 == 'esp' and ops[1].type == X.X86_OP_MEM and \
                                ops[1].mem.index == 0 and ops[1].mem.segment == 0:
                            base = ins.reg_name(ops[1].mem.base) if ops[1].mem.base else None
                            if base == 'ebp' and bo is not None:
                                nsp = bo + ops[1].mem.disp
                            elif base == 'esp' and sp is not None:
                                nsp = sp + ops[1].mem.disp
                    if may & BIT['ebp']:
                        nbo = None
                        if mn == 'mov' and reg0 == 'ebp' and ops[1].type == X.X86_OP_REG and \
                                ins.reg_name(ops[1].reg) == 'esp':
                            nbo = sp
                    nent &= ~may
                    # a store at a known offset into a slot: that slot no longer holds the saved register
                    if ops and ops[0].type == X.X86_OP_MEM and mn not in ('cmp', 'test') and ops[0].mem.index == 0:
                        base = ins.reg_name(ops[0].mem.base) if ops[0].mem.base else None
                        at = (sp + ops[0].mem.disp if base == 'esp' and sp is not None else
                              bo + ops[0].mem.disp if base == 'ebp' and bo is not None else None)
                        if at is not None:
                            n = max(ops[0].size, 1)
                            nslots = {s for s in nslots if not (s[0] < at + n and at < s[0] + 4)}
                        elif base in ('esp', 'ebp'):
                            nslots = set()
            if k is None and mn in ('push', 'pop', 'leave'):
                if mn == 'leave':
                    nbo = None
                # (push/pop's other effects: none on the eight but esp, and pop's register handled above)
            if nsp is None:
                nslots = set()
            s2 = (nsp, nbo, frozenset(nslots), nent, nsaved)
            for n in f.succ[a]:
                old = st.get(n)
                if old is None:
                    st[n] = s2
                    work.append(n)
                else:
                    m = (old[0] if old[0] == s2[0] else None, old[1] if old[1] == s2[1] else None,
                         old[2] & s2[2] if old[0] == s2[0] else frozenset(), old[3] & s2[3], old[4] & s2[4])
                    if m != old:
                        st[n] = m
                        work.append(n)
        p = out if seen_exit else ALL
        self.save_push[e] = {a for a in saves if BIT.get(self.fns[e].by[a].reg_name(self.fns[e].by[a].operands[0].reg), 0) & p
                             & ~ESP}
        return p

    def may_write(self, e):
        f = self.fns[e]
        m = f.own_may
        for t in f.calls:
            m |= self.w_of(t)
        for t in f.tails:
            m |= self.w_of(t)
        if f.lost:
            m |= ALL
        for a, k in self.kinds[e].items():
            if k is not None and k[0] in ('icall', 'ijmp', 'full'):
                m |= ALL
        return (m & ~self.preserved(e)) | ESP

    # ---------------------------------------------------------------- uses at the entry
    def uses(self, e):
        f = self.fns[e]
        ks = self.kinds[e]
        saves = self.save_push.get(e, set())
        live = {a: 0 for a in f.by}
        order = [i.address for i in reversed(f.insns)]
        changed = True
        rounds = 0
        while changed and rounds < 50:
            changed = False
            rounds += 1
            for a in order:
                k = ks[a]
                out = 0
                for n in f.succ[a]:
                    out |= live[n]
                if k is None:
                    u, kill, _ = self.acc[e][a]
                    if a in saves:
                        # saving a register it gives back: the caller's value isn't needed (a stale one goes back
                        # unstored, and the caller keeps its own)
                        u &= ~BIT[f.by[a].reg_name(f.by[a].operands[0].reg)]
                    li = u | (out & ~kill)
                    if a in f.xtail:
                        li = ALL  # leaving by a conditional tail jump, which stores everything
                elif k[0] == 'call':
                    li = self.u_of(k[1]) | self.w_of(k[1]) | out | ESP
                elif k[0] in ('icall', 'ijmp', 'full', 'tail'):
                    li = ALL
                elif k[0] == 'ret':
                    li = ESP
                else:
                    li = 0
                if li != live[a]:
                    live[a] = li
                    changed = True
        return live.get(f.entry, ALL) | ESP

    # ---------------------------------------------------------------- the fixpoint
    def run(self):
        callers = {e: set() for e in self.fns}
        for e, f in self.fns.items():
            for t in f.calls | f.tails:
                if t in callers:
                    callers[t].add(e)
        work = set(self.fns)
        n = 0
        while work:
            e = work.pop()
            n += 1
            if self.fns[e].opaque:
                continue
            w, u = self.may_write(e), self.uses(e)
            w |= self.W[e]
            u |= w | self.U[e]
            if w != self.W[e] or u != self.U[e]:  # the joins, so a pass giving less can't loop forever
                self.W[e], self.U[e] = w, u
                work |= callers[e]
                work.add(e)
        return n


    def dirtied(self):
        """D(f): the registers f, or anything it calls, may store into the guest's (a call's REGS_PUT, a REGS_STORE)
        while holding a value it gives back changed: one saved by a push, put in for a call, then restored by the pop.
        A return stores them too (their entry values then), so the guest's registers are right after every return,
        for an indirect caller or a host callback that loads them all (the console: the CRT's initializer loop, `call
        eax`, got an initializer's inner esi back)."""
        D = {e: 0 for e in self.fns}
        callers = {e: set() for e in self.fns}
        for e, f in self.fns.items():
            for t in f.calls:
                if t in callers:
                    callers[t].add(e)
        own = {}
        for e, f in self.fns.items():
            m = 0
            if f.opaque:
                m = ALL
            for t in f.calls:
                m |= self.w_of(t) | self.u_of(t)
            for k in self.kinds[e].values():
                if k is not None and k[0] in ('icall', 'full', 'stop'):
                    m = ALL
            own[e] = m & ALL
        work = set(self.fns)
        while work:
            e = work.pop()
            m = own[e]
            for t in self.fns[e].calls:
                m |= D.get(t, ALL)
            m |= D[e]
            if m != D[e]:
                D[e] = m
                work |= callers[e]
        return D


def analyze(ctx, translator):
    """ctx.regs_w, ctx.regs_u, ctx.regs_d: {entry: mask}"""
    a = Analysis(ctx, translator)
    a.run()
    ctx.regs_w, ctx.regs_u = a.W, a.U
    ctx.regs_d = a.D = a.dirtied()
    return a


if __name__ == '__main__':
    import json
    import sys
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import recomp
    import x86c
    ctx = recomp.Program(json.load(open(sys.argv[1])), sys.argv[2])
    a = analyze(ctx, x86c.FunctionTranslator)
    pc = lambda m: bin(m & 0xFF).count('1')
    ws = [pc(w) for w in a.W.values()]
    us = [pc(u) for u in a.U.values()]
    print('%d functions; registers a call reloads (W): %.2f on average (of 8); stores (U): %.2f'
          % (len(ws), sum(ws) / len(ws), sum(us) / len(us)))
