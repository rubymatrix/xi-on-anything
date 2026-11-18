"""Which flags cross a call, in either direction, so that a call or return can leave the arithmetic flags
(CF PF AF ZF SF OF) out of what it stores and loads (runtime/guest.h, REGS_STORE_CALL) wherever nothing reads
them on the other side.

  - entry readers: functions that read a flag, on some path from their entry, before writing it (MSVC's
    _RTC_CheckEsp: `jne fail; ret`, the caller's `cmp esi, esp` its ZF). A call to one stores them all.
  - return readers: functions some caller reads a flag of after the call returns, before writing it. They
    store them all at their returns.

A scan follows straight-line code and unconditional direct jumps from an address, and ends:
  - clean: every tracked flag written before any is read
  - reads: one read first, or something it can't see past (an indirect jump, a conditional branch before all are
    written, its budget)
  - at a return with flags still live: they go back to the caller as they came (the caller's own when nothing
    stored or loaded them): from an entry, nothing read; after a call site, read if the function is a return
    reader (its caller reads them)
  - at a call with flags still live: read if the callee is an entry reader (an indirect call: read)
Both sets are fixpoints over those. Used by recomp.py when translating (Program.flag_entry, Program.flag_return);
run on its own it prints the counts:

  python recomp/flags_across_calls.py <meta.json> <image.dll>
"""
from capstone import x86_const as X

FLAGS = {
    'cf': (X.X86_EFLAGS_TEST_CF, X.X86_EFLAGS_MODIFY_CF | X.X86_EFLAGS_RESET_CF | X.X86_EFLAGS_SET_CF |
           X.X86_EFLAGS_UNDEFINED_CF),
    'pf': (X.X86_EFLAGS_TEST_PF, X.X86_EFLAGS_MODIFY_PF | X.X86_EFLAGS_RESET_PF | X.X86_EFLAGS_SET_PF |
           X.X86_EFLAGS_UNDEFINED_PF),
    'zf': (X.X86_EFLAGS_TEST_ZF, X.X86_EFLAGS_MODIFY_ZF | X.X86_EFLAGS_RESET_ZF | X.X86_EFLAGS_SET_ZF |
           X.X86_EFLAGS_UNDEFINED_ZF),
    'sf': (X.X86_EFLAGS_TEST_SF, X.X86_EFLAGS_MODIFY_SF | X.X86_EFLAGS_RESET_SF | X.X86_EFLAGS_SET_SF |
           X.X86_EFLAGS_UNDEFINED_SF),
    'of': (X.X86_EFLAGS_TEST_OF, X.X86_EFLAGS_MODIFY_OF | X.X86_EFLAGS_RESET_OF | X.X86_EFLAGS_SET_OF |
           X.X86_EFLAGS_UNDEFINED_OF),
}
# Reads by mnemonic, not by capstone's flag bits: it reports none at all for some forms (test r/m8, r8), and a read
# missed would drop flags something needs. Which flags a conditional reads: its condition's (CC_READS). AF is read only
# by the BCD adjustments and pushf/lahf (READS_ALL); test, and, or and the shifts leave it undefined, so it isn't
# tracked.
CC_READS = {
    'o': 'of', 'no': 'of', 'b': 'cf', 'c': 'cf', 'nae': 'cf', 'ae': 'cf', 'nb': 'cf', 'nc': 'cf', 'e': 'zf', 'z': 'zf',
    'ne': 'zf', 'nz': 'zf', 'be': 'cf zf', 'na': 'cf zf', 'a': 'cf zf', 'nbe': 'cf zf', 's': 'sf', 'ns': 'sf',
    'p': 'pf', 'pe': 'pf', 'np': 'pf', 'po': 'pf', 'l': 'sf of', 'nge': 'sf of', 'ge': 'sf of', 'nl': 'sf of',
    'le': 'zf sf of', 'ng': 'zf sf of', 'g': 'zf sf of', 'nle': 'zf sf of',
}
READS_ALL = {'pushf', 'pushfd', 'lahf', 'into', 'daa', 'das', 'aaa', 'aas', 'salc'}
READS_CF = {'adc', 'sbb', 'rcl', 'rcr', 'cmc', 'fcmovb', 'fcmovnb', 'fcmovbe', 'fcmovnbe', 'fcmove', 'fcmovne',
            'fcmovu', 'fcmovnu'}
# writers by mnemonic (capstone's bits for the rest): all five, or all but CF
WRITE_ALL = {'cmp', 'test', 'add', 'sub', 'and', 'or', 'xor', 'neg', 'adc', 'sbb', 'cmpxchg', 'xadd', 'sahf',
             'popf', 'popfd', 'bsf', 'bsr', 'comiss', 'comisd', 'ucomiss', 'ucomisd', 'fcomi', 'fcomip', 'fucomi',
             'fucomip', 'cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw', 'scasd'}
WRITE_NO_CF = {'inc', 'dec'}
CLEAN, READS, RET = 'clean', 'reads', 'ret'


def reads_of(ins):
    """The tracked flags ins reads (READS_ALL: all of them)"""
    mn = ins.mnemonic
    if mn in READS_ALL:
        return set(FLAGS)
    if mn in READS_CF:
        return {'cf', 'zf', 'pf'} if mn.startswith('fcmov') else {'cf'}
    for pre in ('set', 'cmov'):
        if mn.startswith(pre) and mn[len(pre):] in CC_READS:
            return set(CC_READS[mn[len(pre):]].split())
    if mn.startswith('j') and mn[1:] in CC_READS:
        return set(CC_READS[mn[1:]].split())
    if mn in ('loope', 'loopz', 'loopne', 'loopnz'):
        return {'zf'}
    if mn.startswith('rep') and ('cmps' in ins.op_str or 'scas' in ins.op_str or mn.startswith(('repe', 'repne'))):
        return set()  # repe/repne: its own compares' ZF
    return set()


def writes_of(ins):
    mn = ins.mnemonic.split()[-1] if ins.mnemonic.startswith('rep') else ins.mnemonic
    if mn in WRITE_ALL:
        return set(FLAGS)
    if mn in WRITE_NO_CF:
        return set(FLAGS) - {'cf'}
    if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'shld', 'shrd'):
        if ins.op_str.endswith('cl'):
            return set()  # a count of 0 changes nothing
    try:
        ef = ins.eflags
    except Exception:
        return set()
    return set(f for f, (_, w) in FLAGS.items() if ef & w)


def scan(prog, va, budget=64):
    """What code from va does with the flags it finds: CLEAN, READS, or a set of the ways it leaves with some still
    unwritten - RET, ('call', target), target None for an indirect call - when every path ends that way. Both sides
    of a conditional branch are followed."""
    ends = set()
    work = [(va, frozenset(FLAGS))]
    seen = set()
    steps = 0
    while work:
        pc, live = work.pop()
        while True:
            steps += 1
            if steps > budget or (pc, live) in seen:
                return READS
            seen.add((pc, live))
            try:
                found = list(prog.md.disasm(prog.read(pc, 16), pc, count=1))
            except Exception:
                return READS
            if not found:
                return READS
            ins = found[0]
            mn = ins.mnemonic
            if reads_of(ins) & live:
                return READS
            live = live - writes_of(ins)
            if not live:
                break  # this path is clean
            op = ins.operands[0] if ins.operands else None
            direct = op is not None and op.type == X.X86_OP_IMM
            nxt = ins.address + ins.size
            if mn == 'jmp':
                if not direct:
                    return READS
                pc = op.imm & 0xFFFFFFFF
                continue
            if mn.startswith('j') or mn.startswith('loop') or mn == 'jecxz':
                if not direct:
                    return READS
                work.append((op.imm & 0xFFFFFFFF, live))
                pc = nxt
                continue
            if mn == 'call':
                ends.add(('call', op.imm & 0xFFFFFFFF if direct else None))
                break
            if mn in ('ret', 'retn'):
                ends.add(RET)
                break
            if mn in ('int3', 'hlt', 'ud2'):
                break  # never returns here
            pc = nxt
    return frozenset(ends) if ends else CLEAN


def entry_readers(prog):
    """The functions that read a flag at entry, as a fixpoint: a scan that reads, or that reaches an indirect call or a
    call to one of them with flags unwritten."""
    entry_scan = {e: scan(prog, e) for e in prog.entries}
    entry = set()
    changed = True
    while changed:
        changed = False
        for e, v in entry_scan.items():
            if e not in entry and (v == READS or (v != CLEAN and any(
                    x != RET and (x[1] is None or x[1] in entry) for x in v))):
                entry.add(e)
                changed = True
    return entry


def analyze(prog):
    """(entry readers, return readers), or (entry readers, None) when every return must store the flags (a caller
    reads them after an indirect call, whose callee can't be known)."""
    entry_scan = {e: scan(prog, e) for e in prog.entries}
    sites = []  # (function, its call's target or None, the scan after the call)
    tails = {}
    for e, ranges in prog.functions.items():
        for lo, hi in ranges:
            for ins in prog.md.disasm(prog.read(lo, hi - lo), lo):
                op = ins.operands[0] if ins.operands else None
                direct = op is not None and op.type == X.X86_OP_IMM
                if ins.mnemonic == 'call':
                    after = scan(prog, ins.address + ins.size)
                    if after != CLEAN:
                        sites.append((e, op.imm & 0xFFFFFFFF if direct else None, after))
                elif ins.mnemonic == 'jmp' and direct:
                    t = op.imm & 0xFFFFFFFF
                    if t in prog.entries and t != e:
                        tails.setdefault(e, set()).add(t)

    entry = set()
    changed = True
    while changed:
        changed = False
        for e, v in entry_scan.items():
            if e in entry:
                continue
            if v == READS or (v != CLEAN and any(x != RET and (x[1] is None or x[1] in entry) for x in v)):
                entry.add(e)
                changed = True

    ret = set()
    indirect = False
    changed = True
    while changed:
        changed = False
        for e, callee, after in sites:
            reads = after == READS or (after != CLEAN and any(
                (x == RET and e in ret) or (x != RET and (x[1] is None or x[1] in entry)) for x in after))
            if not reads:
                continue
            if callee is None:
                indirect = True
            elif callee not in ret:
                ret.add(callee)
                changed = True
        for e in list(ret):  # a return reader's tail jumps return for it
            for t in tails.get(e, ()):
                if t not in ret:
                    ret.add(t)
                    changed = True
    return entry, (None if indirect else ret)


if __name__ == '__main__':
    import json
    import os
    import sys
    sys.path.insert(0, os.path.dirname(__file__))
    import recomp
    prog = recomp.Program(json.load(open(sys.argv[1])), sys.argv[2])
    ent, ret = analyze(prog)
    print('%d functions; %d read a flag at entry; %s store them at their returns' % (
        len(prog.entries), len(ent), 'all (a caller reads them after an indirect call)' if ret is None else len(ret)))
    print('entry readers:', ' '.join('%x' % e for e in sorted(ent)[:30]))
    if ret:
        print('return readers:', ' '.join('%x' % e for e in sorted(ret)[:30]))
