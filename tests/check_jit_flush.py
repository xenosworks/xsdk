#!/usr/bin/env python3
"""Check that the PowerPC C object contains cache maintenance; range/order tests run separately."""

import re
import subprocess
import sys


def main():
    disassembly = subprocess.check_output([sys.argv[1], "-d", "--disassemble=xsdk_code_flush", sys.argv[2]], text=True)
    ops = re.findall(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2}\s+){4}([a-z.]+)", disassembly, re.MULTILINE)
    ops = ["sync" if op == "hwsync" else op for op in ops]
    if not all(op in ops for op in ("dcbst", "icbi", "isync")) or ops.count("sync") < 2:
        sys.exit("FAIL: cache flush object lacks dcbst, two sync barriers, icbi or isync")
    print("PASS: compiled C cache flush contains dcbst, sync, icbi and isync")


if __name__ == "__main__":
    main()
