"""Makes a sparse fixed VHD of the given sector count, for Windows to format: zeros plus the 512-byte footer."""
import os
import struct
import subprocess
import sys
import time

path, sectors = sys.argv[1], int(sys.argv[2])
size = sectors * 512

# Virtual Hard Disk Image Format Specification, fixed disk footer (big-endian).
total = min(sectors, 65535 * 16 * 255)
if total >= 65535 * 16 * 63:
    per_track, heads = 255, 16
    cylinder_heads = total // 255
else:
    per_track = 17
    cylinder_heads = total // 17
    heads = max(4, (cylinder_heads + 1023) // 1024)
    if cylinder_heads >= heads * 1024 or heads > 16:
        per_track, heads = 31, 16
        cylinder_heads = total // 31
    if cylinder_heads >= heads * 1024:
        per_track, heads = 63, 16
        cylinder_heads = total // 63
cylinders = cylinder_heads // heads
footer = bytearray(struct.pack(">8sIIQI4sI4sQQHBBII16sB",
    b"conectix", 2, 0x10000, 0xFFFFFFFFFFFFFFFF, int(time.time()) - 946684800, b"xsdk", 0x10000, b"Wi2k",
    size, size, cylinders, heads, per_track, 2, 0, os.urandom(16), 0))
footer += bytes(512 - len(footer))
footer[64:68] = struct.pack(">I", (~sum(footer)) & 0xFFFFFFFF)

with open(path, "wb"):
    pass
subprocess.run(["fsutil", "sparse", "setflag", path], check=True)
with open(path, "r+b") as file:
    file.truncate(size)
    file.seek(size)
    file.write(footer)
print(f"{path}: {size} bytes + footer, geometry {cylinders}x{heads}x{per_track}")
