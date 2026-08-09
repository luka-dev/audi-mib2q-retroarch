#!/usr/bin/env python3
"""Patch SystemScreenFactory.java: add `case RA_ID` -> RaScreen.build (bare TRANSPARENT empty screen).
The builder + input handling live in RaScreen (same package). Idempotent. RA_ID must match the SMCodec DHS."""
import re, sys

RA_ID = 250
SF = "src/SystemScreenFactory.java"

CASE = f'''            case {RA_ID}:
                return RaScreen.build(this, j);
'''

def main():
    s = open(SF).read()
    if f"case {RA_ID}:" in s:
        print("already patched"); return
    s2 = re.sub(r'(protected Screen createScreen\(int i, int j\) \{\s*\n\s*switch \(i\) \{\n)',
                r'\1' + CASE, s, count=1)
    if s2 == s: sys.exit("FAIL: could not find createScreen switch head")
    open(SF, "w").write(s2)
    print(f"patched: +case {RA_ID} -> RaScreen.build(this, j)")

if __name__ == "__main__":
    main()
