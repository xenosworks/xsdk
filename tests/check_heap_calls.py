#!/usr/bin/env python3
"""Reject a heap acceptance test whose malloc/free calls GCC has removed."""

import re
import subprocess
import sys


def main():
    relocations = subprocess.check_output([sys.argv[1], "-r", sys.argv[2]], text=True)
    calls = set(re.findall(r"R_PPC_REL24\s+(malloc|free)(?=\s|$)", relocations))
    if calls != {"malloc", "free"}:
        sys.exit("FAIL: heap test object lacks malloc/free calls; it cannot test page return")
    print("PASS: compiled heap test retains malloc and free calls")


if __name__ == "__main__":
    main()
