"""Checks an xeBuild image for milestone 77 step 4: our two patch records against build/exfat.bin, the module file in
the flash file system against build/exfat.xex, and which 16 KB blocks differ from an earlier image."""
import struct
import sys

PAGE, SPARE, BLOCK = 0x200, 0x10, 0x4000


def raw_of(path):
    data = open(path, "rb").read()
    if len(data) % (PAGE + SPARE) == 0:
        data = b"".join(data[i:i + PAGE] for i in range(0, len(data), PAGE + SPARE))
    return data


def records(blob, start):
    out, p = [], start
    while True:
        address = struct.unpack_from(">I", blob, p)[0]
        if address == 0xFFFFFFFF:
            return out, p
        n = struct.unpack_from(">I", blob, p + 4)[0]
        out.append((address, blob[p + 8:p + 8 + 4 * n]))
        p += 8 + 4 * n


image, addon, module, previous = sys.argv[1:5]
raw = raw_of(image)
print(f"image: {len(raw):#x} raw bytes, header {raw[:2].hex()}")
cf1 = struct.unpack_from(">I", raw, 0xC)[0]
patch_set = cf1 + 0x10000
recs, end = records(raw, patch_set + 0x60)
print(f"patch set at {patch_set:#x}: {len(recs)} records, end marker at {end:#x}")
ours = {address: words for address, words in records(open(addon, "rb").read() + b"\xff\xff\xff\xff", 0)[0]}
for address, words in recs:
    if address in (0x163000, 0x613DC, 0x61388, 0x613CC, 0x10BF40):
        tag = "OURS, matches build/exfat.bin" if ours.get(address) == words else ("OURS, DIFFERS" if address in ours else "xeBuild's")
        print(f"  {address:08x} x{len(words) // 4}: {words[:12].hex()}... {tag}")
missing = [a for a in ours if a not in dict(recs)]
print("our records present:", "all" if not missing else f"MISSING {[hex(a) for a in missing]}")
print("old hook record 0x61388 present:", any(a == 0x61388 for a, _ in recs))

xex = open(module, "rb").read()
chunks = [xex[i:i + BLOCK] for i in range(0, len(xex), BLOCK)]
hits = [raw.find(c) for c in chunks]
print(f"module {len(xex)} bytes in {len(chunks)} blocks; block offsets in the image: {[hex(h) for h in hits]}")
print("module file present:", "all blocks found" if all(h >= 0 for h in hits) else "SOME BLOCKS MISSING")
old = raw_of(previous)
diff = [i // BLOCK for i in range(0, min(len(raw), len(old)), BLOCK) if raw[i:i + BLOCK] != old[i:i + BLOCK]]
runs, start = [], None
for b in diff + [None]:
    if start is None:
        start = prev = b
    elif b is not None and b == prev + 1:
        prev = b
    else:
        runs.append((start, prev))
        start = prev = b
print(f"blocks differing from {previous.split(chr(92))[-1]}: {len(diff)} of {len(raw) // BLOCK}:",
      ", ".join(f"{s * BLOCK:#x}-{(e + 1) * BLOCK:#x}" for s, e in runs))
