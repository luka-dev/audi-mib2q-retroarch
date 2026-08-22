#!/usr/bin/env python3
"""Aggregate ra_prof samples into a flat CPU profile + a blocking report.

Note it is a FLAT profile, not a true flamegraph: DCMD_PROC_TIDSTATUS gives one
instruction pointer per thread, not a call stack. Walking stacks would need
target-side unwinding. In practice the flat profile answers "which function
burns the CPU" and the blocking report answers "what stalls the audio", which
are the two questions this port keeps asking.

  ra_prof_report.py samples.txt [unstripped-binary]
"""
import subprocess, sys, collections

# QNX thread states (sys/neutrino.h)
STATE = {0:"DEAD",1:"RUNNING",2:"READY",3:"STOPPED",4:"SEND",5:"RECEIVE",
         6:"REPLY",7:"STACK",8:"WAITTHREAD",9:"WAITPAGE",10:"SIGSUSPEND",
         11:"SIGWAITINFO",12:"NANOSLEEP",13:"MUTEX",14:"CONDVAR",15:"JOIN",
         16:"INTR",17:"SEM",18:"WAITCTX"}
ONCPU = {1, 2}

def load_symbols(binary):
    """name -> (start, end), from the unstripped ELF."""
    out = subprocess.run(["nm", "-n", "--defined-only", binary],
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1].lower() in "tw":
            try: syms.append((int(parts[0], 16), parts[2]))
            except ValueError: pass
    syms.sort()
    return syms

def resolve(syms, addr):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best = syms[mid][1]; lo = mid + 1
        else:
            hi = mid - 1
    return best or "?"

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    samples, maps = [], []
    for line in open(sys.argv[1]):
        if line.startswith("MAP"):
            f = line.split()
            maps.append((int(f[1], 16), int(f[2], 16)))
            continue
        if line.startswith("#"):
            continue
        f = line.split()
        if len(f) != 5:
            continue
        samples.append((int(f[0]), int(f[1]), int(f[2]), int(f[3], 16), int(f[4])))

    total = len(samples)
    oncpu = [s for s in samples if s[2] in ONCPU]
    print(f"samples={total}  on-CPU={len(oncpu)} ({100*len(oncpu)/max(total,1):.1f}%)  maps={len(maps)}")

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

    if len(sys.argv) > 2 and oncpu:
        syms = load_symbols(sys.argv[2])
        print(f"\n== hot functions ({len(syms)} symbols loaded) ==")
        hot = collections.Counter(resolve(syms, s[3]) for s in oncpu)
        for name, n in hot.most_common(25):
            print(f"  {100*n/len(oncpu):5.1f}%  {n:5d}  {name}")

main()
