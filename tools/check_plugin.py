#!/usr/bin/env python3
"""Fails a plugin whose code holds a floating-point or VMX instruction, from its `objdump -d` on stdin.

The kernel may run a plugin's code in a DPC, which has no floating-point context (xcn:docs/INTERNALS.md:122-124), so
plugins build with neither (-msoft-float -mno-altivec, xsdk_add_plugin); this catches assembly or a library that
brings one in anyway.
"""

import re
import sys

# Every FPU mnemonic starts with f, lf or stf, or moves the FPSCR; every VMX and VMX128 one with v, lv or stv, or moves
# the VSCR or touches a data stream.
FLOAT = re.compile(r"(f[a-z]+|lf[a-z]+|stf[a-z]+|m[ft]fs[a-z0-9]*|mcrfs)\.?")
VECTOR = re.compile(r"(v[a-z0-9]+|lv[a-z0-9]+|stv[a-z0-9]+|m[ft]vscr|dss[a-z]*|dst[a-z]*)\.?")
FUNCTION = re.compile(r"^[0-9a-f]+ <(.+)>:$")
INSTRUCTION = re.compile(r"^\s*([0-9a-f]+):\t(?:[0-9a-f]{2} ){4}\t(\S+)\s*(.*)$")


def main():
    function = None
    count = 0
    found = []
    for line in sys.stdin:
        match = FUNCTION.match(line.rstrip("\n"))
        if match:
            function = match.group(1)
            continue
        match = INSTRUCTION.match(line.rstrip("\n"))
        if not match:
            continue
        count += 1
        if FLOAT.fullmatch(match.group(2)) or VECTOR.fullmatch(match.group(2)):
            found.append(f"{function}: {match.group(1)}: {match.group(2)} {match.group(3)}")
    if not count:
        sys.exit("check_plugin: no instructions read; pipe in objdump -d's output")
    for text in found:
        print(f"check_plugin: {text}")
    if found:
        sys.exit(f"check_plugin: {len(found)} floating-point or VMX instructions; a plugin may have none")
    print(f"check_plugin: {count} instructions, no floating point or VMX")


if __name__ == "__main__":
    main()
