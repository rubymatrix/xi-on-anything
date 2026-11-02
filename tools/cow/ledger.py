#!/usr/bin/env python3
"""Build ledger/functions.csv for the cow_kuluu_xi_anything conversion project.

One row per function in the matched client build's metadata, plus a static call graph
taken from the generated C, so conversion can proceed leaves-first.

Rerunnable and idempotent: it always regenerates the whole CSV from meta/ + generated/.
Status/name/file/verified columns are reset to their initial values, so anything you have
recorded by hand belongs in a separate file, not here.

Usage:
    python tools/cow/ledger.py [--repo PATH] [--build YYYY-MM-DD] [--generated DIR]

Sources of truth (all confirmed by reading the code, not guessed):
  meta/builds.json                      build date -> meta file
  generated/build.json                  written by tools/prepare.py: {"build": ..., "game": ...}
  meta/FFXiMain.<build>.meta.json       {"functions": [{"entry": int, "thunk": bool,
                                        "ranges": [[start, end], ...]}, ...]}
                                        ranges are [start, end) virtual addresses.
  generated/all/funcs_*.c               recomp.py output. One function per
                                        `void f_<8 hex VA>(Guest* g)` definition; a call to
                                        another FFXiMain function is emitted as `f_<hex>(g)`
                                        (plain calls: `REGS_STORE; f_x(g); REGS_LOAD;`,
                                        tail jumps/thunks: `REGS_STORE; f_x(g); return;`).
                                        `goto L_<hex>` stays inside one function, and
                                        instruction comments print bare `0x<hex>`, so neither
                                        is mistaken for a call. Dynamic calls go through
                                        rt_call_indirect() and cannot be resolved statically.

Column definitions:
  entry        function entry VA, hex
  end          end of the last range (exclusive), hex
  size         end - entry, bytes (span; discontinuous functions count their gaps)
  thunk        true for the 5-byte `jmp rel32` stubs
  status       "thunk" for thunks, otherwise "todo"
  name         empty until a human names it from the oracle
  file         empty until our own C file is written
  oracle_refs  empty until recorded
  verified     false until diffed against the original
  callees      count of distinct FFXiMain functions called, thunks resolved to their target
  leaf_rank    0 = calls no FFXiMain function; else 1 + highest rank among callees.
               Members of a call cycle share the cycle's rank (1 + highest rank of the
               callees outside the cycle, at least 1) and get cycle=true.
  cycle        true when the function sits in a call cycle (self-recursion counts)

Thunks are transparent in the graph: a call to a thunk is counted as a call to what the
thunk jumps at, and thunk rows themselves get callees=0, leaf_rank=0, cycle=false, because
they are never converted work.
"""

import argparse
import csv
import json
import os
import re
import sys
from collections import defaultdict

FUNC_DEF = re.compile(r"^void f_([0-9a-f]{8})\(Guest\* g\)$")
CALL_RE = re.compile(r"\bf_([0-9a-f]{8})\(g\)")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def resolve_build(repo, build_arg):
    if build_arg:
        return build_arg
    bj = os.path.join(repo, "generated", "build.json")
    if os.path.exists(bj):
        with open(bj, encoding="utf-8") as fh:
            return json.load(fh)["build"]
    sys.exit("error: pass --build YYYY-MM-DD (no generated/build.json; run tools/prepare.py first)")


def load_meta(repo, build):
    builds = json.load(open(os.path.join(repo, "meta", "builds.json"), encoding="utf-8"))["builds"]
    if build not in builds:
        sys.exit("error: build %r not in meta/builds.json (have: %s)"
                 % (build, ", ".join(sorted(builds))))
    meta_name = builds[build]["FFXiMain.dll"]["meta"]
    path = os.path.join(repo, "meta", meta_name)
    if not os.path.exists(path):
        sys.exit("error: missing meta file %s" % path)
    data = json.load(open(path, encoding="utf-8"))
    funcs = {}
    for rec in data["functions"]:
        entry = int(rec["entry"])
        ranges = [[int(a), int(b)] for a, b in (r for r in rec.get("ranges") or [])]
        if not ranges:
            ranges = [[entry, entry]]
        end = max(r[1] for r in ranges)
        funcs[entry] = {
            "entry": entry,
            "end": end,
            "size": end - entry,
            "thunk": bool(rec.get("thunk")),
            "n_ranges": len(ranges),
        }
    return funcs, data, meta_name


def parse_generated(gen_dir):
    """Return {addr: set(callee_addrs)} straight from recomp's C."""
    if not os.path.isdir(gen_dir):
        sys.exit("error: %s not found - run `python tools\\build.py boot64` (or host) first" % gen_dir)
    bodies = {}
    names = sorted(f for f in os.listdir(gen_dir) if f.startswith("funcs_") and f.endswith(".c"))
    if not names:
        sys.exit("error: no funcs_*.c in %s" % gen_dir)
    for fname in names:
        cur = None
        with open(os.path.join(gen_dir, fname), encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = FUNC_DEF.match(line.rstrip("\n"))
                if m:
                    cur = int(m.group(1), 16)
                    bodies.setdefault(cur, set())
                    continue
                if cur is not None:
                    for cm in CALL_RE.finditer(line):
                        bodies[cur].add(int(cm.group(1), 16))
    return bodies, len(names)


def sccs(nodes, graph):
    """Tarjan's SCC, iterative. Returns list of components (lists of node ids)."""
    index = {}
    low = {}
    on_stack = set()
    stack = []
    out = []
    counter = [0]
    for root in nodes:
        if root in index:
            continue
        work = [(root, iter(graph.get(root, ())))]
        index[root] = low[root] = counter[0]
        counter[0] += 1
        stack.append(root)
        on_stack.add(root)
        while work:
            v, it = work[-1]
            advanced = False
            for w in it:
                if w not in nodes:
                    continue
                if w not in index:
                    index[w] = low[w] = counter[0]
                    counter[0] += 1
                    stack.append(w)
                    on_stack.add(w)
                    work.append((w, iter(graph.get(w, ()))))
                    advanced = True
                    break
                elif w in on_stack:
                    low[v] = min(low[v], index[w])
            if advanced:
                continue
            work.pop()
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[v])
            if low[v] == index[v]:
                comp = []
                while True:
                    w = stack.pop()
                    on_stack.discard(w)
                    comp.append(w)
                    if w == v:
                        break
                out.append(comp)
    return out


def main():
    ap = argparse.ArgumentParser(description="build ledger/functions.csv")
    ap.add_argument("--repo", default=repo_root())
    ap.add_argument("--build")
    ap.add_argument("--generated", default=None, help="dir of generated C (default <repo>/generated/all)")
    ap.add_argument("--out", default=None, help="csv to write (default <repo>/ledger/functions.csv)")
    args = ap.parse_args()

    build = resolve_build(args.repo, args.build)
    funcs, meta, meta_name = load_meta(args.repo, build)
    gen_dir = args.generated or os.path.join(args.repo, "generated", "all")
    bodies, n_files = parse_generated(gen_dir)

    known = set(funcs)
    thunk_of = {a for a, f in funcs.items() if f["thunk"]}

    # thunk -> target (its body's single call). Chains followed with a guard.
    def resolve(addr, seen=None):
        seen = seen or set()
        cur = addr
        while cur in thunk_of and cur not in seen and cur in bodies:
            seen.add(cur)
            nxt = [c for c in bodies[cur] if c in known]
            if len(nxt) != 1:
                break
            cur = nxt[0]
        return cur

    # static call graph over non-thunk functions, thunks resolved away
    graph = {}
    outside = set()
    for a, f in funcs.items():
        if f["thunk"]:
            graph[a] = set()
            continue
        raw = bodies.get(a, set())
        res = set()
        for c in raw:
            if c == a:
                continue  # handled as a self-edge below
            if c in known:
                t = resolve(c)
                if t != a and t in known:
                    res.add(t)
            else:
                outside.add(c)
        graph[a] = res

    self_loop = {a for a in funcs if a in bodies.get(a, set()) and not funcs[a]["thunk"]}

    # rank by condensation: leaves 0, everything else 1 + max(callee rank)
    nodes = set(funcs) - thunk_of
    comps = sccs(nodes, graph)
    comp_of = {}
    for i, c in enumerate(comps):
        for v in c:
            comp_of[v] = i
    edges = defaultdict(set)  # component -> component
    for a in nodes:
        for b in graph[a]:
            if b in comp_of and comp_of[a] != comp_of[b]:
                edges[comp_of[a]].add(comp_of[b])
    in_cycle = {i for i, c in enumerate(comps) if len(c) > 1}
    for a in self_loop:
        if a in comp_of:
            in_cycle.add(comp_of[a])

    rank = {}

    def comp_rank(i, visiting=None):
        if i in rank:
            return rank[i]
        visiting = visiting or set()
        if i in visiting:
            return 0  # defensive: should not happen on a condensation DAG
        visiting.add(i)
        best = 0
        for j in edges[i]:
            best = max(best, comp_rank(j, visiting) + 1)
        if i in in_cycle:
            best = max(best, 1)
        visiting.discard(i)
        rank[i] = best
        return best

    for i in range(len(comps)):
        comp_rank(i)

    out_path = args.out or os.path.join(args.repo, "ledger", "functions.csv")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    cols = ["entry", "end", "size", "thunk", "status", "name", "file", "oracle_refs",
            "verified", "callees", "leaf_rank", "cycle"]
    rows = []
    for a in sorted(funcs):
        f = funcs[a]
        is_thunk = f["thunk"]
        cyc = (not is_thunk) and (comp_of.get(a) in in_cycle)
        rows.append({
            "entry": "0x%08x" % a,
            "end": "0x%08x" % f["end"],
            "size": f["size"],
            "thunk": "true" if is_thunk else "false",
            "status": "thunk" if is_thunk else "todo",
            "name": "",
            "file": "",
            "oracle_refs": "",
            "verified": "false",
            "callees": 0 if is_thunk else len(graph[a]),
            "leaf_rank": 0 if is_thunk else rank.get(comp_of.get(a), 0),
            "cycle": "false" if is_thunk else ("true" if cyc else "false"),
        })
    with open(out_path, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)

    # ---- totals -----------------------------------------------------------
    n = len(rows)
    th = sum(1 for r in rows if r["status"] == "thunk")
    todo = n - th
    rc = defaultdict(int)
    for r in rows:
        if r["status"] != "thunk":
            rc[r["leaf_rank"]] += 1
    cyc = sum(1 for r in rows if r["cycle"] == "true")
    deep = sum(v for k, v in rc.items() if k >= 3)
    no_body = sum(1 for a in funcs if a not in bodies)

    print("build            : %s (%s)" % (build, meta_name))
    print("generated C      : %s (%d funcs_*.c)" % (gen_dir, n_files))
    print("rows             : %d" % n)
    print("thunks           : %d" % th)
    print("todo             : %d" % todo)
    print("leaf_rank 0      : %d" % rc[0])
    print("leaf_rank 1      : %d" % rc[1])
    print("leaf_rank 2      : %d" % rc[2])
    print("leaf_rank 3+     : %d" % deep)
    print("in call cycles   : %d" % cyc)
    print("self-recursive   : %d" % len(self_loop))
    print("max leaf_rank    : %s" % (max(rc) if rc else 0))
    print("meta fn with no generated body : %d" % no_body)
    print("generated callees outside meta : %d" % len(outside))
    print("wrote            : %s" % out_path)


if __name__ == "__main__":
    main()
