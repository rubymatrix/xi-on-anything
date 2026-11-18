"""The x87 stack in fixed locals, by its depth (instead of x87_locals' eight rotating ones).

FFXiMain's x87 code is MSVC's, whose convention keeps the x87 stack empty at every call (a callee may use all eight
registers) and on return either empty or holding the result in ST0. A function that follows it - its stack depth,
counted from its entry, known and the same on every path to each instruction - needs none of the whole-stack copying
x87_locals does around every call and return (X87_STORE/X87_LOAD: eight doubles, the top and the control word each
way; in the Switch's profile, 31% of the hottest translated code), nor its shift of all eight locals at every push:

  - the value at depth k is the local x87_r<k>: ST(i) at depth d is x87_r<d-1-i>, a push at depth d writes
    x87_r<d>, a pop reads x87_r<d-1>;
  - a call (at depth 0) gives the callee the top and the control word, and reads back ST0 when it returned a float;
    whether it did is what the code after it does first - reads ST0 (fstp, fcomp, fadd...), or pushes / doesn't
    touch the stack - on every path (they must agree);
  - a return or a jump into another function (at depth 0 or 1) stores the top, the control word and ST0 at depth 1;
  - g->top at the entry is x87_t0, and everything else is relative to it.

A function this can't place - a depth that differs between paths, a read below its own entry (a value left by its
caller), a call with values on the stack, an exit deeper than 1, the whole-state helpers (fsave, frstor, fenv, fxam,
the top moved by hand), a host hook - keeps x87_locals' scheme: plan() returns None.

After every call RT_X87_CHECK(cond, addr) checks that the callee left the top where the plan expects (guest.h):
a wrong guess about a float return would otherwise go unseen.
"""
import re

WHOLE = ('fenv_load(', 'fenv_store(', 'fsave(', 'frstor(', 'fxam(', 'g->top', 'g->st', 'X87_', 'x87_')
TOKEN = re.compile(r'ST\(([0-7])\)|fpush\(g, |fpop\(g\)|fstsw\(g\)')


class NoPlan(Exception):
    pass


def calls_re(prefix):
    """a call, as x86c.py emits it: REGS_STORE(_CALL); <fn>(g) or rt_call_indirect(g, t); REGS_LOAD(_CALL);"""
    fn = r'(?:%s[0-9a-f]{8}\(g\)|rt_call_indirect\(g, t\))' % re.escape(prefix)
    return re.compile(r'(REGS_STORE(?:_CALL)?;) (%s;) (REGS_LOAD(?:_CALL)?;)' % fn)


def exits_re(prefix):
    """a way out of the function: a return, a tail call, an indirect jump"""
    fn = r'(?:%s[0-9a-f]{8}\(g\)|rt_call_indirect\(g, t\))' % re.escape(prefix)
    return re.compile(r'(REGS_STORE(?:_CALL)?;)( (?:%s; )?return;)' % fn)


def simulate(text, d):
    """The depth after text's stack operations from depth d, or NoPlan if one reads below the function's own pushes"""
    for m in TOKEN.finditer(text):
        t = m.group(0)
        if t.startswith('ST('):
            if int(m.group(1)) >= d:
                raise NoPlan('reads below its pushes')
        elif t.startswith('fpush'):
            d += 1
        elif t.startswith('fpop'):
            if d < 1:
                raise NoPlan('pops below its pushes')
            d -= 1
    if d > 8:
        raise NoPlan('deeper than 8')
    return d


def first_use(text):
    """'read' if text reads the stack before pushing on it (from depth 0), 'push' if it pushes first, None if neither"""
    for m in TOKEN.finditer(text):
        t = m.group(0)
        if t.startswith('ST(') or t.startswith('fpop'):
            return 'read'
        if t.startswith('fpush'):
            return 'push'
    return None


class Plan:
    def __init__(self, fn, insns, stmts, falls, succ_of, prefix, effect_of=lambda target: None):
        """insns: the function's instructions; stmts[a], falls[a]: what x86c.py emitted for the one at a and whether it
        falls through; succ_of(ins): its jump targets inside the function; effect_of(entry): what the function there
        does to the depth (net_effect), None when that isn't known"""
        self.prefix = prefix
        self.effect_of = effect_of
        self.target = re.compile(r'%s([0-9a-f]{8})\(g\)' % re.escape(prefix))
        self.call = calls_re(prefix)
        self.exit = exits_re(prefix)
        self.by = {i.address: i for i in insns}
        self.stmts = stmts
        self.falls = falls
        self.succ_of = succ_of
        self.text = {a: ' '.join(s) for a, s in stmts.items()}
        for a, t in self.text.items():
            if any(w in t for w in WHOLE):
                raise NoPlan('whole-state x87 at %08x' % a)
        self.calls = {}  # a call's address: (the depth before it, after it)
        self.depth = {}
        self._infer = {}
        self.analyze(fn)

    def is_call(self, a):
        return bool(self.call.search(self.text[a]))

    def call_depths(self, a, d):
        """(before, after) for the call at a made at depth d: by what the callee does to the depth when that is known
        (MSVC's helpers take their argument on the stack: __ftol2 pops it), else - from an empty stack - by what the code
        after it does first (reads ST0: a float came back)"""
        m = self.target.search(self.call.search(self.text[a]).group(0))
        eff = self.effect_of(int(m.group(1), 16)) if m else None
        nxt = self.by[a].address + self.by[a].size
        if eff is not None:
            after = d + eff
            if after < 0 or after > 8:
                raise NoPlan('a call at %08x leaves %d on the stack' % (a, after))
            return d, after  # (exact: the code after may push before it uses a float the call returned)
        if d != 0:
            raise NoPlan('a call at %08x with %d on the x87 stack' % (a, d))
        return 0, self.infer(nxt) or 0

    def infer(self, start, _unused=None):
        """1 if every path from start reads ST0 before pushing, 0 if none does (they push first, leave the function, make a
        call, or never touch the stack), None if no path says; NoPlan if they disagree"""
        found = set()
        seen = set()
        work = [start]
        while work:
            a = work.pop()
            if a in seen:
                continue
            seen.add(a)
            if a not in self.by:
                found.add(0)  # into another function: it starts with an empty stack
                continue
            t = self.text[a]
            use = first_use(t)
            if use == 'read':
                found.add(1)
            elif use == 'push' or self.exit.search(t) or self.is_call(a):
                found.add(0)
            else:
                if self.falls[a]:
                    work.append(a + self.by[a].size)
                work.extend(self.succ_of(self.by[a]))
        if len(found) > 1:
            raise NoPlan('a call before %08x returns a float on one path, not on another' % start)
        return found.pop() if found else None

    def analyze(self, fn):
        if fn not in self.by:
            raise NoPlan('no instruction at the entry')
        self.depth[fn] = 0
        work = [fn]
        while work:
            a = work.pop()
            d = self.depth[a]
            t = self.text[a]
            after = simulate(t, d)
            ins = self.by[a]
            nxt = []
            if self.is_call(a):
                self.calls[a] = self.call_depths(a, d)
                after = self.calls[a][1]
            for m in self.exit.finditer(t):
                if simulate(t[:m.start()], d) > 1:
                    raise NoPlan('an exit at %08x deeper than 1' % a)
            if self.falls[a]:
                nxt.append(ins.address + ins.size)
            nxt += self.succ_of(ins)
            for n in nxt:
                if n not in self.by:
                    if after > 1:
                        raise NoPlan('a jump out at %08x deeper than 1' % a)
                    continue
                if n in self.depth:
                    if self.depth[n] != after:
                        raise NoPlan('depths %d and %d meet at %08x' % (self.depth[n], after, n))
                else:
                    self.depth[n] = after
                    work.append(n)
        for a, t in self.text.items():
            if a not in self.depth:
                if TOKEN.search(t):
                    # not reached from the entry (another entry's code in this one's ranges): its depth is unknown
                    raise NoPlan('x87 code at %08x not reached from the entry' % a)
                if self.exit.search(t) or self.is_call(a):
                    self.depth[a] = 0

    # ---------------------------------------------------------------- the rewrite

    def out(self, d):
        """what an exit at depth d stores"""
        s = 'g->top = (x87_t0 - %du) & 7u; g->fcw = (uint16_t)x87_cw;' % d
        if d == 1:
            s += ' g->st[(x87_t0 - 1u) & 7u] = x87_r0;'
        return s

    def rewrite(self, a, stmts):
        """the statements x86c.py emitted for the instruction at a, its stack in the depth's locals"""
        d = [self.depth.get(a, 0)]
        before, after = self.calls.get(a, (0, 0))

        def sub(m):
            if m.group('call'):
                # the values on the stack go to g, where the callee takes them; what it leaves comes back
                give = ''.join(' g->st[(x87_t0 - %du) & 7u] = x87_r%d;' % (k + 1, k) for k in range(before))
                give += ' g->top = (x87_t0 - %du) & 7u; g->fcw = (uint16_t)x87_cw;' % before
                back = 'x87_cw = g->fcw;'
                back += ''.join(' x87_r%d = g->st[(x87_t0 - %du) & 7u];' % (k, k + 1) for k in range(after))
                back += ' RT_X87_CHECK(g->top == ((x87_t0 - %du) & 7u), 0x%08Xu);' % (after, a)
                return '%s%s %s %s %s' % (m.group('cs'), give, m.group('cf'), m.group('cl'), back)
            if m.group('es'):
                return '%s %s%s' % (m.group('es'), self.out(d[0]), m.group('er'))
            if m.group('st') is not None:
                return 'x87_r%d' % (d[0] - 1 - int(m.group('st')))
            t = m.group(0)
            if t.startswith('fpush'):
                d[0] += 1
                return 'x87_r%d = (' % (d[0] - 1)
            if t.startswith('fpop'):
                d[0] -= 1
                return '(x87_r%d)' % d[0]
            if t.startswith('fstsw'):
                return 'fstsw_top(g, (x87_t0 - %du) & 7u)' % d[0]
            return t

        fn = r'(?:%s[0-9a-f]{8}\(g\)|rt_call_indirect\(g, t\))' % re.escape(self.prefix)
        pat = re.compile(
            r'(?P<call>(?P<cs>REGS_STORE(?:_CALL)?;) (?P<cf>%s;) (?P<cl>REGS_LOAD(?:_CALL)?;))'
            r'|(?P<es>REGS_STORE(?:_CALL)?;)(?P<er> (?:%s; )?return;)'
            r'|ST\((?P<st>[0-7])\)|fpush\(g, |fpop\(g\)|fstsw\(g\)' % (fn, fn))
        out = []
        for line in stmts:  # in order: the depth goes on from one statement to the next
            # the control word is the local one (fldcw, fnstcw) before the stores this adds give it back to g
            s = pat.sub(sub, line.replace('g->fcw', 'x87_cw'))
            s = s.replace('fr(g, ', 'fr_cw(x87_cw, ')
            for f in ('frnd', 'fist16', 'fist32', 'fist64'):
                s = s.replace(f + '(g, ', f + '_cw(x87_cw, ')
            out.append(s)
        return out

    def tail(self, a_prev, line):
        """a jump into another function emitted between instructions (x86c.py's goto at a fall-through), after a_prev"""
        d = simulate(self.text[a_prev], self.depth.get(a_prev, 0)) if a_prev in self.text else 0
        if a_prev in self.calls:
            d = self.calls[a_prev][1]
        return re.sub(r'(REGS_STORE(?:_CALL)?;)( (?:\w+\(g(?:, t)?\); )?return;)',
                      lambda m: '%s %s%s' % (m.group(1), self.out(d), m.group(2)), line)


last_reason = None  # why the last plan() returned None (for recomp.py's --stats and tests)


def net_effect(fn, insns, stmts, falls, succ_of, prefix):
    """What the function does to the stack's depth from entry to return (-1 for one that pops its argument, +1 for a
    float return, 0), or None: it makes calls, uses the whole state, or returns at different depths"""
    by = {i.address: i for i in insns}
    text = {a: ' '.join(s) for a, s in stmts.items()}
    call, exit_ = calls_re(prefix), exits_re(prefix)
    if fn not in by or any(any(w in t for w in WHOLE) or call.search(t) for t in text.values()):
        return None
    depth = {fn: 0}
    work = [fn]
    ends = set()
    while work:
        a = work.pop()
        d = depth[a]
        for m in TOKEN.finditer(text[a]):
            tok = m.group(0)
            d += 1 if tok.startswith('fpush') else -1 if tok.startswith('fpop') else 0
        if exit_.search(text[a]):
            ends.add(d)
        nxt = ([a + by[a].size] if falls[a] else []) + succ_of(by[a])
        for n in nxt:
            if n not in by:
                ends.add(d)
            elif n not in depth:
                depth[n] = d
                work.append(n)
            elif depth[n] != d:
                return None
    return ends.pop() if len(ends) == 1 else None


def plan(fn, insns, stmts, falls, succ_of, prefix, effect_of=lambda target: None):
    """A Plan for the function, or None when it must keep x87_locals' scheme"""
    global last_reason
    try:
        return Plan(fn, insns, stmts, falls, succ_of, prefix, effect_of)
    except NoPlan as e:
        last_reason = str(e)
        return None
