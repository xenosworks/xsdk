#!/usr/bin/env python3
"""Builds the kernel patch that loads the exFAT module at boot, as an xeBuild add-on (milestone 77 step 4).

Assembles plugins/exfat/kernel_patch.S at the stub's address in the kernel's .text padding with the toolchain's as
and ld, and writes xeBuild's patch records: for each, the address without its top bit, the word count and the words,
as xeBuild/about_patches.S gives the format; an add-on carries no 0xFFFFFFFF end, xeBuild appends it. Two records:
the stub, and the `bl stub` that replaces the `bl sub_800C8260` at the boot point. Every patched word is checked
against the kernel image first: the boot point must hold that bl and the stub's room must be zeros, so a kernel this
wasn't read from is refused.

Kernel 2.0.17559.0: ExpSystemInitThread's `bl sub_800C8260` at 0x800613dc (plugins/exfat/kernel_seam.md, section 7),
the first call after xam's load (the bl XexLoadImage at 0x800613b0) and after the point where xeBuild's DashLaunch
patch loads launch.xex, whose stub returns to 0x800613dc; a system image of the module's size loaded before launch.xex
stops the boot in DashLaunch's hooks (results/2026-11-14-milestone-77-step-4, runs 24 to 28). sub_800C8260 takes no
arguments. XexLoadImage is at 0x8007d7c0. The padding runs from 0x801616c8 to 0x8016ffff; xeBuild's XeLL add-ons
(xl_both, xl_hdd, xl_usb) take it up to 0x801621e8, so the stub sits at 0x80163000.
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path

KERNEL_BASE = 0x80040000
KERNEL_IMAGE_BYTES = 0x1B0000
HOOK = 0x800613DC
XEX_LOAD_IMAGE = 0x8007D7C0
SUB_800C8260 = 0x800C8260
STUB = 0x80163000
PADDING = range(0x801616C8, 0x80170000)
XELL_ADDONS_END = 0x801621E8


def fail(message):
    sys.exit(f"exfat_patch: {message}")


def branch(source, target, link):
    offset = target - source
    if not -0x2000000 <= offset < 0x2000000 or offset & 3:
        fail(f"no branch from {source:#010x} to {target:#010x}")
    return 0x48000000 | offset & 0x03FFFFFC | link


def assemble(toolchain, source, build, defsyms):
    build.mkdir(parents=True, exist_ok=True)
    obj = build / "kernel_patch.o"
    elf = build / "kernel_patch.elf"
    binary = build / "kernel_patch.bin"
    run = lambda *command: subprocess.run(command, check=True)
    run(f"{toolchain}as", "-a32", "-mregnames", "-o", obj, source)
    run(f"{toolchain}ld", f"-Ttext={STUB:#x}", "-e", "stub", f"--defsym=XexLoadImage={XEX_LOAD_IMAGE:#x}",
        f"--defsym=sub_800C8260={SUB_800C8260:#x}", *(f"--defsym={d}" for d in defsyms), "-o", elf,
        obj)
    run(f"{toolchain}objcopy", "-O", "binary", "-j", ".text", elf, binary)
    listing = subprocess.run([f"{toolchain}objdump", "-d", "-j", ".text", elf], check=True, capture_output=True,
        text=True).stdout
    (build / "kernel_patch.lst").write_text(listing)
    code = binary.read_bytes()
    code += b"\0" * (-len(code) % 4)
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--toolchain", required=True, help="the powerpc-xsdk-elf- prefix of as, ld, objcopy, objdump")
    parser.add_argument("--kernel", required=True, type=Path, help="the 17559 kernel image, laid out at 0x80040000")
    parser.add_argument("--build", required=True, type=Path, help="where the object, ELF and listing go")
    parser.add_argument("--defsym", action="append", default=[], metavar="NAME=ADDRESS",
        help="another kernel function a variant of the stub calls, for ld")
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="the add-on patch, for xeBuild's 17559/bin")
    args = parser.parse_args()

    kernel = args.kernel.read_bytes()
    if len(kernel) != KERNEL_IMAGE_BYTES:
        fail(f"{args.kernel}: {len(kernel)} bytes, not a 17559 kernel image of {KERNEL_IMAGE_BYTES:#x}")
    word = lambda address: struct.unpack_from(">I", kernel, address - KERNEL_BASE)[0]
    expected = branch(HOOK, SUB_800C8260, 1)
    if word(HOOK) != expected:
        fail(f"{HOOK:#010x} holds {word(HOOK):08x}, not the bl sub_800C8260 {expected:08x}")
    if word(0x800613B0) != branch(0x800613B0, XEX_LOAD_IMAGE, 1):
        fail(f"0x800613b0 holds {word(0x800613B0):08x}, not a bl XexLoadImage")

    code = assemble(args.toolchain, args.source, args.build, args.defsym)
    end = STUB + len(code)
    if STUB < XELL_ADDONS_END or end > PADDING.stop:
        fail(f"the stub {STUB:#010x}-{end:#010x} leaves the free padding {XELL_ADDONS_END:#x}-{PADDING.stop:#x}")
    room = kernel[STUB - KERNEL_BASE:end - KERNEL_BASE]
    if room.count(0) != len(room):
        fail(f"the kernel's {STUB:#010x}-{end:#010x} isn't zeros")

    records = [(STUB, code), (HOOK, struct.pack(">I", branch(HOOK, STUB, 1)))]
    out = b"".join(struct.pack(">II", address & 0x7FFFFFFF, len(data) // 4) + data for address, data in records)
    args.output.write_bytes(out)
    for address, data in records:
        words = " ".join(f"{w:08x}" for w, in struct.iter_unpack(">I", data))
        print(f"exfat_patch: {address:#010x} {len(data) // 4} words: {words}")
    print(f"exfat_patch: {args.output}, {len(out)} bytes; the stub {STUB:#010x}-{end:#010x}")


if __name__ == "__main__":
    main()
