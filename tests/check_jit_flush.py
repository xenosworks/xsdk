#!/usr/bin/env python3
"""Check the cache maintenance instructions even when a console happens to see unflushed writes."""

import re
import subprocess
import sys


def main():
    disassembly = subprocess.check_output([sys.argv[1], "-d", sys.argv[2]], text=True)
    body = disassembly.split("<xsdk_jit_flush_lines>:", 1)[1]
    ops = re.findall(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2}\s+){4}([a-z.]+)", body, re.MULTILINE)
    maintenance = [op for op in ops if op in ("dcbst", "icbi", "sync", "hwsync", "isync")]
    maintenance = ["sync" if op == "hwsync" else op for op in maintenance]
    if maintenance != ["dcbst", "sync", "icbi", "sync", "isync"] or ops.count("cmpw") != 2:
        sys.exit("FAIL: cache flush must walk both ranges and order dcbst, sync, icbi, sync, isync")
    if len(re.findall(r"addi\s+r[35],r[35],128", body)) != 2:
        sys.exit("FAIL: cache flush must advance both walks by 128 bytes")
    print("PASS: compiled cache flush walks 128-byte lines and orders dcbst, sync, icbi, sync, isync")


if __name__ == "__main__":
    main()
