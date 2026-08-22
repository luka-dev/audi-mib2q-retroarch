#!/usr/bin/env python3
"""Per-object hot-function report for ra_prof samples.

Each sampled ip is attributed to the mapping that contains it, then resolved
against that object's own symbol table at (ip - object_base). Objects without
a supplied symbol file are still reported, just without function names.

  ra_prof_hot.py samples.txt [name=path/to/unstripped ...]
"""
import bisect, collections, subprocess, sys

ONCPU = {1, 2}

def symbols(path):
    out = subprocess.run(["nm", "-n", "--defined-only", path],
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1].lower() in "tw":
            try: syms.append((int(p[0], 16), p[2]))
            except ValueError: pass
    syms.sort()
    return [s[0] for s in syms], [s[1] for s in syms]

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    symfiles = {}
    for a in sys.argv[2:]:
        k, _, v = a.partition("=")
        symfiles[k] = symbols(v)

    maps, samples = [], []
    for line in open(sys.argv[1]):
        if line.startswith("MAP"):
            f = line.split(maxsplit=5)
            maps.append((int(f[1], 16), int(f[2], 16),
                         (f[5].strip() if len(f) > 5 else "-")))
        elif not line.startswith("#"):
            f = line.split()
            if len(f) == 5:
                samples.append((int(f[2]), int(f[3], 16)))

    # An object is mapped in several pieces; its base is the lowest one.
    base = {}
    for v, _s, p in maps:
        name = p.split("/")[-1]
        base[name] = min(base.get(name, v), v)
    starts = sorted(set(m[0] for m in maps))
    span = {}
    for v, s, p in maps:
        span.setdefault(v, (s, p.split("/")[-1]))

    def owner(ip):
        i = bisect.bisect_right(starts, ip) - 1
        if i >= 0:
            b = starts[i]
            s, n = span[b]
            if ip < b + s:
                return n
        return "(anon/JIT)"

    on = [ip for st, ip in samples if st in ONCPU]
    print(f"on-CPU samples: {len(on)}")
    per_obj = collections.Counter(owner(ip) for ip in on)

    for name, n in per_obj.most_common(6):
        print(f"\n== {name}  {100*n/max(len(on),1):.1f}%  ({n} samples) ==")
        if name not in symfiles:
            print("   (no symbols supplied)")
            continue
        addrs, names = symfiles[name]
        hot = collections.Counter()
        for ip in on:
            if owner(ip) != name:
                continue
            off = ip - base[name]
            j = bisect.bisect_right(addrs, off) - 1
            hot[names[j] if j >= 0 else "?"] += 1
        for fn, c in hot.most_common(12):
            print(f"   {100*c/n:5.1f}%  {c:5d}  {fn}")

main()
