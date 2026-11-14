"""Reads the formatted VHD: prints the MBR entry and the exFAT boot sector's layout, finds the non-zero megabytes
over the whole disk, and writes the head that holds them all as the image for the console."""
import struct
import sys

vhd, out = sys.argv[1], sys.argv[2]
BLOCK = 1 << 20
with open(vhd, "rb") as file:
    file.seek(0, 2)
    size = file.tell() - 512
    file.seek(0)
    mbr = file.read(512)
    entry = mbr[446:462]
    start, count = struct.unpack_from("<II", entry, 8)
    print(f"MBR: signature {mbr[510]:02X}{mbr[511]:02X}, entry 0 type {entry[4]:02X} start {start} sectors {count}"
          f" (ends at {start + count} of {size // 512})")
    file.seek(start * 512)
    boot = file.read(512)
    (volume_length, fat_offset, fat_length, heap_offset, clusters, root, serial, revision, flags, bps_shift,
     spc_shift, fats) = struct.unpack_from("<QIIIIIIHHBBB", boot, 72)
    print(f"exFAT: {boot[3:11]} volume length {volume_length}, FAT at {fat_offset} for {fat_length} sectors, heap at"
          f" {heap_offset}, {clusters} clusters of {1 << spc_shift} sectors, root cluster {root}, serial {serial:08X},"
          f" revision {revision >> 8}.{revision & 255}, flags {flags:04X}, {1 << bps_shift} bytes a sector, {fats} FATs")
    file.seek(0)
    ranges = []
    offset = 0
    while offset < size:
        chunk = file.read(min(BLOCK, size - offset))
        if chunk.count(0) != len(chunk):
            if ranges and ranges[-1][1] == offset:
                ranges[-1][1] = offset + len(chunk)
            else:
                ranges.append([offset, offset + len(chunk)])
        offset += len(chunk)
    for first, end in ranges:
        print(f"non-zero: {first:#x}..{end:#x} (sectors {first // 512}..{end // 512})")
    head = ranges[-1][1]
    if head > 32 << 20:
        sys.exit(f"the last non-zero megabyte ends at {head:#x}: more than 32 MB")
    file.seek(0)
    with open(out, "wb") as image:
        image.write(file.read(head))
    print(f"{out}: {head} bytes ({head // 512} sectors)")
