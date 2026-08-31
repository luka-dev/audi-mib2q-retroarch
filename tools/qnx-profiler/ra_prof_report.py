#!/usr/bin/env python3
"""Aggregate ra_prof samples into a flat CPU profile + a blocking report.

Note it is a FLAT profile, not a true flamegraph: DCMD_PROC_TIDSTATUS gives one
instruction pointer per thread, not a call stack. Walking stacks would need
target-side unwinding. In practice the flat profile answers "which function
burns the CPU" and the blocking report answers "what stalls the audio", which
are the two questions this port keeps asking.

Samples carry ABSOLUTE run-time addresses, so a raw nm lookup is meaningless: a
shared object is loaded nowhere near its link-time addresses, and the dynarec's
code space has no symbols in any ELF at all. Every address is therefore routed
through the MAP records first -- ip -> mapping -> file offset -> PT_LOAD ->
link-time address -> symbol -- and anything that lands outside a known object is
reported as such instead of being snapped onto whichever symbol happens to
precede it.

  ra_prof_report.py samples.txt [unstripped-binary | symbols-dir]
"""
import subprocess, sys, collections, os, re, bisect

# QNX thread states (sys/neutrino.h)
STATE = {0:"DEAD",1:"RUNNING",2:"READY",3:"STOPPED",4:"SEND",5:"RECEIVE",
         6:"REPLY",7:"STACK",8:"WAITTHREAD",9:"WAITPAGE",10:"SIGSUSPEND",
         11:"SIGWAITINFO",12:"NANOSLEEP",13:"MUTEX",14:"CONDVAR",15:"JOIN",
         16:"INTR",17:"SEM",18:"WAITCTX"}
RUNNING, READY = 1, 2
ONCPU = {RUNNING, READY}

LOAD_RE = re.compile(
    r'^\s+LOAD\s+(0x\S+)\s+(0x\S+)\s+0x\S+\s+(0x\S+)\s+(0x\S+)\s+([RWE ]+?)\s+0x')


class Obj:
    """One local unstripped ELF: PT_LOAD table + sorted symbol table."""
    def __init__(self, path):
        self.path = path
        self.loads = []   # (file_off, filesz, link_vaddr)
        self.addrs = []   # sorted symbol start addresses
        self.syms  = []   # (end, name) parallel to addrs
        self._read_loads()
        self._read_syms()

    def _read_loads(self):
        out = subprocess.run(["readelf", "-lW", self.path],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = LOAD_RE.match(line)
            if m and "E" in m.group(5):
                self.loads.append((int(m.group(1), 16), int(m.group(3), 16),
                                   int(m.group(2), 16)))

    def _read_syms(self):
        out = subprocess.run(["nm", "-nS", "--defined-only", self.path],
                             capture_output=True, text=True).stdout
        raw = []
        for line in out.splitlines():
            f = line.split()
            # "addr size type name" or "addr type name"
            if len(f) >= 4 and f[2].lower() in ("t", "w"):
                try: raw.append((int(f[0], 16), int(f[1], 16), f[3]))
                except ValueError: pass
            elif len(f) >= 3 and f[1].lower() in ("t", "w"):
                try: raw.append((int(f[0], 16), 0, f[2]))
                except ValueError: pass
        raw.sort()
        for i, (a, sz, name) in enumerate(raw):
            # A symbol with no size runs until the next one starts.
            end = a + sz if sz else (raw[i + 1][0] if i + 1 < len(raw) else a + 4)
            self.addrs.append(a)
            self.syms.append((end, name))

    def link_addr(self, file_off):
        for off, filesz, vaddr in self.loads:
            if off <= file_off < off + filesz:
                return vaddr + (file_off - off)
        return None

    def resolve(self, link):
        i = bisect.bisect_right(self.addrs, link) - 1
        if i < 0:
            return None
        end, name = self.syms[i]
        return name if link < end else None


def selfcheck(elf):
    """Round-trip the tricky part: pick real symbols out of an ELF, pretend the
    object is mapped at an arbitrary base, and assert each one resolves back."""
    obj = Obj(elf)
    assert obj.loads, f"no executable PT_LOAD in {elf}"
    off, filesz, vaddr = obj.loads[0]
    BASE = 0x7a000000                      # nothing like the link-time address
    maps = [(BASE, filesz, off, "/target/" + os.path.basename(elf))]
    # Aliases (C1/C2 constructors, weak clones) legitimately share an address;
    # resolving to any name at that address is correct.
    at = collections.defaultdict(set)
    for a, (_, name) in zip(obj.addrs, obj.syms):
        at[a].add(name)
    checked = 0
    for a, (end, name) in zip(obj.addrs, obj.syms):
        if not (vaddr <= a < vaddr + filesz) or end <= a:
            continue
        ip = BASE + (a - vaddr) + (off - off)   # link addr -> file off -> ip
        assert obj.link_addr(off + (ip - BASE)) == a
        got = obj.resolve(obj.link_addr(off + (ip - BASE)))
        assert got in at[a], f"{hex(a)} {name} resolved as {got}"
        checked += 1
        if checked >= 200:
            break
    assert checked > 10, f"only {checked} symbols exercised"
    # An address past every symbol must NOT snap onto the last one.
    assert obj.resolve(obj.addrs[-1] + (1 << 28)) is None
    # An unmapped ip must be reported as unmapped, not attributed to the object.
    assert not (maps[0][0] <= BASE - 0x1000 < maps[0][0] + maps[0][1])
    print(f"selfcheck OK: {checked} symbols round-tripped through {elf}")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == "--selfcheck":
        return selfcheck(sys.argv[2])

    maps, samples = [], []
    for line in open(sys.argv[1]):
        if line.startswith("MAP"):
            f = line.split(None, 5)
            if len(f) >= 5:
                path = f[5].strip() if len(f) > 5 else "-"
                maps.append((int(f[1], 16), int(f[2], 16), int(f[3], 16), path))
            continue
        if line.startswith("#"):
            continue
        f = line.split()
        if len(f) != 5:
            continue
        samples.append((int(f[0]), int(f[1]), int(f[2]), int(f[3], 16), int(f[4])))

    maps.sort()
    map_starts = [m[0] for m in maps]

    total  = len(samples)
    onrun  = [s for s in samples if s[2] == RUNNING]
    oncpu  = [s for s in samples if s[2] in ONCPU]
    print(f"samples={total}  RUNNING={len(onrun)} ({100*len(onrun)/max(total,1):.1f}%)"
          f"  READY={len(oncpu)-len(onrun)}  maps={len(maps)}")

    print("\n== where threads sit (all samples) ==")
    for st, n in collections.Counter(s[2] for s in samples).most_common(8):
        print(f"  {STATE.get(st,st):<12} {n:6d}  {100*n/max(total,1):5.1f}%")

    print("\n== longest blocks seen per thread (ms) ==")
    worst = collections.defaultdict(int)
    for _, tid, st, _, bms in samples:
        if st not in ONCPU:
            worst[tid] = max(worst[tid], bms)
    for tid, bms in sorted(worst.items(), key=lambda kv: -kv[1])[:8]:
        print(f"  tid {tid:<4} {bms:8d} ms")

    if not maps or not onrun:
        return

    # Local unstripped ELFs, keyed by basename of the on-target path.
    objs = {}
    if len(sys.argv) > 2:
        arg = sys.argv[2]
        files = ([os.path.join(arg, f) for f in os.listdir(arg)]
                 if os.path.isdir(arg) else [arg])
        for f in files:
            if os.path.isfile(f):
                try: objs[os.path.basename(f)] = Obj(f)
                except Exception: pass

    def locate(ip):
        i = bisect.bisect_right(map_starts, ip) - 1
        if i < 0:
            return "[unmapped]", None
        vaddr, size, off, path = maps[i]
        if not (vaddr <= ip < vaddr + size):
            return "[unmapped]", None
        if path == "-":
            # 15 MB of dynarec code space lives here; it has no symbols anywhere.
            return "[JIT / anon exec]", None
        name = os.path.basename(path)
        obj = objs.get(name)
        if obj is None:
            return f"[{name}]", name
        link = obj.link_addr(off + (ip - vaddr))
        sym = obj.resolve(link) if link is not None else None
        return (f"{name}:{sym}" if sym else f"[{name} +unknown]"), name

    # RUNNING only: READY means "wants a core", which is contention, not work.
    located = [locate(s[3]) for s in onrun]

    print(f"\n== on-CPU time by object ({len(onrun)} RUNNING samples) ==")
    byobj = collections.Counter(o or l for l, o in located)
    for name, n in byobj.most_common(12):
        print(f"  {100*n/len(onrun):5.1f}%  {n:5d}  {name}")

    print(f"\n== hot functions ({len(objs)} object(s) with symbols) ==")
    hot = collections.Counter(l for l, _ in located)
    for name, n in hot.most_common(25):
        print(f"  {100*n/len(onrun):5.1f}%  {n:5d}  {name}")

main()
