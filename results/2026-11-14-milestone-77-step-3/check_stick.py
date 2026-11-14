"""Checks the files exfat_check.xex left on the stick, once the stick is on the PC.

Usage: python check_stick.py E:\\  (the stick's drive root)

exfat_check\\verify.bin is 1 MiB of chunk 4097; exfat_check\\big.bin is 4 GiB + 4096 bytes: chunks 0 to 4095 whole and
the first 4096 bytes of chunk 4096 (the title cut it from 4 GiB + 1 MiB). Word j of chunk k, big-endian, is
(j * 0x01010101) ^ (k * 0x9E3779B1 + 0x7F4A7C15), as tests/exfat_check.c fills it. Run chkdsk on the drive as well:
    chkdsk E:
"""
import os
import sys

CHUNK_BYTES = 1 << 20
WORDS = CHUNK_BYTES // 4
MASK = 0xFFFFFFFF

base = int.from_bytes(b''.join(((j * 0x01010101) & MASK).to_bytes(4, 'big') for j in range(WORDS)), 'big')


def chunk(k):
    w = (k * 0x9E3779B1 + 0x7F4A7C15) & MASK
    return (base ^ int.from_bytes(w.to_bytes(4, 'big') * WORDS, 'big')).to_bytes(CHUNK_BYTES, 'big')


def main():
    root = sys.argv[1]
    folder = os.path.join(root, 'exfat_check')
    failures = 0

    path = os.path.join(folder, 'verify.bin')
    data = open(path, 'rb').read()
    ok = data == chunk(4097)
    print(f"verify.bin: {len(data)} bytes, {'identical' if ok else 'DIFFERENT'}")
    failures += not ok

    path = os.path.join(folder, 'big.bin')
    size = os.path.getsize(path)
    expected = (4096 << 20) + 4096
    print(f"big.bin: {size} bytes, expected {expected}: {'ok' if size == expected else 'WRONG'}")
    failures += size != expected
    bad = 0
    with open(path, 'rb') as f:
        for k in range(4096):
            if f.read(CHUNK_BYTES) != chunk(k):
                bad += 1
                if bad <= 5:
                    print(f"  chunk {k} differs")
        tail = f.read()
        if tail != chunk(4096)[:4096]:
            bad += 1
            print("  the 4096-byte tail differs")
    print(f"big.bin: {bad} bad pieces")
    failures += bad != 0

    names = sorted(os.listdir(folder))
    print(f"exfat_check holds: {names}")
    failures += names != ['big.bin', 'verify.bin']
    print("RESULT", "all identical" if not failures else f"{failures} FAILURES")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
