#!/usr/bin/env python3
"""Run jit.c's actual C loops on the host, replacing only cache instructions with trace writes."""

import ctypes
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OPCODES = {"dcbst": 1, "sync": 2, "icbi": 3, "isync": 4}
ASM = re.compile(r'__asm__\s+volatile\("(dcbst|icbi) 0, %0"\s*:\s*:\s*"r"\(([^)]+)\)\s*:\s*"memory"\);'
                 r'|__asm__\s+volatile\("(sync|isync)"\s*:\s*:\s*:\s*"memory"\);')
HARNESS = """
#include <stdint.h>
#include <stdlib.h>
unsigned char xsdk_jit_start[1], xsdk_jit_end[1];
struct Trace { uintptr_t operation, address; } trace[64];
unsigned trace_count;
static void record(uintptr_t operation, uintptr_t address)
{
    if (trace_count == 64) abort();
    trace[trace_count++] = (struct Trace){operation, address};
}
"""


def instrument(match):
    op = match[1] or match[3]
    address = match[2] or "0"
    return f"record({OPCODES[op]}, {address});"


def main():
    source = Path(sys.argv[2]).read_text(encoding="utf-8")
    source = ASM.sub(instrument, source)
    if "__asm__" in source:
        sys.exit("FAIL: unrecognised cache instruction; update the trace substitution")
    directory = ROOT / "build/host"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="jit-ranges-", dir=directory) as temporary:
        path = Path(temporary)
        (path / "jit.c").write_text(HARNESS + source, encoding="utf-8")
        library = path / "jit.so"
        subprocess.run([sys.argv[1], "-shared", "-fPIC", "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "include"), str(path / "jit.c"), "-o", str(library)], check=True)
        dll = ctypes.CDLL(str(library))
        try:
            dll.xsdk_code_flush.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            dll.xsdk_code_flush.restype = None
            count = ctypes.c_uint.in_dll(dll, "trace_count")
            trace = ((ctypes.c_size_t * 2) * 64).in_dll(dll, "trace")
            maximum = (1 << (ctypes.sizeof(ctypes.c_void_p) * 8)) - 1
            cases = [("zero/null", 0, 0, []), ("zero/unaligned", 0x1001, 0, []),
                     ("zero/end", maximum, 0, []), ("one byte", 0x1000, 1, [0x1000]),
                     ("aligned line", 0x1000, 128, [0x1000]),
                     ("129 bytes", 0x1000, 129, [0x1000, 0x1080]),
                     ("crossing", 0x107F, 2, [0x1000, 0x1080]),
                     ("unaligned end", 0x1001, 127, [0x1000]),
                     ("three lines", 0x107F, 130, [0x1000, 0x1080, 0x1100]),
                     ("last byte", maximum, 1, [maximum - 127]),
                     ("last line", maximum - 127, 128, [maximum - 127])]
            for name, start, size, lines in cases:
                count.value = 0
                dll.xsdk_code_flush(start, size)
                actual = [tuple(trace[index]) for index in range(count.value)]
                expected = ([(1, line) for line in lines] + [(2, 0)] +
                            [(3, line) for line in lines] + [(2, 0), (4, 0)]) if lines else []
                if actual != expected:
                    sys.exit(f"FAIL: cache range {name}: got {actual}, expected {expected}")
        finally:
            # Windows keeps a loaded DLL locked even when a range assertion failed.
            if sys.platform == "win32":
                import _ctypes
                _ctypes.FreeLibrary(dll._handle)
        print(f"PASS: C cache loop trace: {len(cases)} exact ranges and barrier order")


if __name__ == "__main__":
    main()
