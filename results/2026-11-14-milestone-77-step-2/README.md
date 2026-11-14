# Milestone 77, step 2: the exFAT core on the PC

2026-11-14. The exFAT core, its formatter and the PC harness of docs/scope/77-exfat.md, step 2. Nothing here ran on
the console; the judges are exfatprogs 1.2.9 (`mkfs.exfat`, `fsck.exfat`), fuse-exfat 1.4.0 and Windows 11.

## What was built

- `include/xsdk/exfat.h`: the API. A volume lives over two sector callbacks (`read`, `write`) and a clock (`now`), with
  no kernel calls, so the same code builds into the console's module (step 3) and into the harness. Paths are the
  console's 8-bit names, taken as Latin-1.
- `src/exfat.c`: mount (boot sector fields, the boot region checksum, the root chain, the bitmap, the up-case table
  and its checksum, the free count), lookup through the up-case table, directory walks over entry sets with their
  checksums, reads straight into the caller's buffer in sector multiples with one bounce sector for the ends, and the
  writes: create, remove, rename and move, write and extend (contiguous while the next clusters are free, a FAT chain
  after), truncate and grow, attributes and times, directory growth. Metadata goes through a 64 x 4 KB cache written
  back in the order FAT, bitmap, directories; an entry is written and flushed before its clusters are freed, and a
  new set is flushed before the old one is marked unused, so a cut power leaks clusters rather than damages entries.
  VolumeDirty is set on the first change and cleared by sync.
- `src/exfat_format.c`: the formatter, written against the Windows-formatted stick of step 1
  (../2026-11-14-milestone-77-step-1/recipe). MBR with one type 07 partition from 1 MB to 1 MB short of the last
  whole megabyte (Windows left that megabyte too), FAT at 1 MB into the partition rounded up to a cluster, the heap
  at the next megabyte, bitmap from cluster 2, the up-case table after it, the root directory after that holding the
  label (when given), the bitmap and the up-case entries; cluster sizes 4 KB to 256 MB, 32 KB to 32 GB, 128 KB above.
  The MBR is written last. An MBR holds 2^32 sectors at most, so a drive over 2 TB with 512-byte sectors, which
  Windows formats GPT, is refused (`EXFAT_UNSUPPORTED`) rather than mimicked.
- `src/exfat_private.h` (layout constants, the three checksums) and `src/exfat_upcase.h` (Windows's compressed
  up-case table, 2918 words, checksum E619D30D, read off the stick's cluster 3).
- `tests/exfat_host/exfat_host.c`: one command per run on an image (`info ls stat get put write mkdir rm mv truncate
  attrib format`), through pread/pwrite; `EXFAT_HOST_FAIL_AFTER=n` fails the n-th device write and every one after;
  `EXFAT_HOST_SECTOR` sets the sector size.
- `tests/exfat_host/run.py`: the harness, root in WSL (loop devices, fuse): `make exfat_check`, or
  `wsl.exe -d Debian -u root --exec bash -c 'cd /mnt/z/Xbox-360-SDK && python3 tests/exfat_host/run.py
  build/host/exfat_host'`. Its work directory is under /var/tmp, since WSL's /tmp is RAM.
- Makefile: `src/exfat.c` and `src/exfat_format.c` in SDK_SOURCES (so the cross build compiles them),
  `build/host/exfat_host`, `exfat_check`.

## The harness (harness.log)

232 checks passed, 0 failed, in 56 s (the full run, 4 GB file and 2.5 TB volumes included).

- mkfs.exfat images at 4 KB (default for 256 MB), 32 KB and 128 KB clusters: a tree written through fuse-exfat
  (16 sizes from 0 to 3 MB + 5 around sector and cluster edges, three directory levels, a 255-character name, mixed
  case, Latin-1 names, spaces and dots) reads back through ours byte for byte, with a case-insensitive lookup and the
  modified time within 2 s of Linux's.
- Ours writes the same tree with chunk sizes 1 MB, 777, 4096 and 65536, two files grown a cluster at a time in turns
  (both chained; a file written in one go stays contiguous), then renames, moves a directory, renames a file by case
  only, removes a file and an empty directory, refuses removing a non-empty one, a duplicate mkdir, a bad name and
  moving a directory into itself, truncates down, up and to zero, sets attributes, and adds 300 files so a directory
  grows past its first cluster. VolumeDirty is clear afterwards, `fsck.exfat -n` is clean, ours reads it all back,
  fuse-exfat reads it all back and sees the modified time, and ours reads a file Linux adds.
- Power cuts: a 3 MB `put` with the device failing after write 1, 2, 3, 5, 8, 13, 21 and 34 (write 1 is the dirty
  flag). Each leaves the volume marked dirty, `fsck.exfat -y` repairs it with no corrupt or checksum report and
  leaves it clean, every earlier file is intact, and the cut file, when its entry exists, holds exactly what was
  written.
- Corruptions on a populated image: a bad boot checksum refuses the mount; a FAT chain looping inside a file reads
  up to the file's size and stops; a looping root chain refuses the mount; a first cluster past the end fails the
  read as corrupt while the entry still lists; a wrong name hash makes the lookup miss while the entry still lists;
  a wrong set checksum makes the entry skip. `fsck.exfat` rejects each image too.
- A file of 4 GiB + 1 MiB: 1 MB written at 0, truncated up to 4 GiB + 1 MiB, 1 MB written at 4 GiB (the 4 GiB gap
  filled with zeros through the bounce sector, 3.3 s in all); ours and fuse-exfat read it back byte for byte, fsck
  clean.
- A 2.5 TB mkfs.exfat volume (sparse): 20.9 M clusters, a tree written by ours, fsck clean, fuse reads it back.
- The formatter at 128 MB (4 KB clusters), 8 GB (32 KB), the stick's 120,176,640 sectors (128 KB) and 2.5 TB with
  4 KB sectors (128 KB): the backup boot region equals the main one, `fsck.exfat` is clean, ours mounts it, fuse-exfat
  writes a tree that ours reads back byte for byte, ours writes a file that fuse reads, fsck clean after each. Each
  format takes under 0.1 s (only the structures are written). The stick-sized image's boot sector bytes 0 to 119
  equal recipe/image_boot.bin apart from the serial, its signature too, and its MBR bytes 446 to 511 equal
  recipe/image_mbr.bin. A 2.5 TB image with 512-byte sectors is refused as unsupported.

## Windows (windows_vhd_check.txt)

The 128 MB image the formatter made, wrapped as a fixed VHD and attached with diskpart: Windows mounted it as E:
(exFAT, MBR type 7 at 1 MB, label XSDK, serial 643F-C9ED), wrote 200,000 bytes and a folder with a text file, read
the bytes back equal, and `chkdsk E:` found no problems. Only this image went through Windows.

## Departures from the scope's step 2 text, for the user

- No break builds. The scope lists breaks (a wrong name hash, a wrong set checksum, a missed dirty flag as code
  variants); the standing rule since 2026-11-11 is no intentional break builds, so none were made. The same
  detections are exercised from the other side: images with a wrong hash or checksum, and the dirty flag checked set
  after every cut and clear after every sync.
- The formatter's signature is `xsdk_exfat_format(device, label, serial)`: the sector count travels in the device,
  the serial comes from the caller (step 5's title picks one). The root's label, bitmap and up-case entries carry no
  time fields, so "the current time in the root entries" has nothing to write.
- 2.5 TB: an MBR with 512-byte sectors cannot hold it, so that check runs with 4 KB sectors and the 512-byte case is
  checked to be refused; docs/scope/77-exfat.md says so now.

## Build

`make clean && make -j32` in WSL: the whole SDK, xsdk_test and the probes build with the two new sources in
libxsdk.a with no warnings; tools/xenia_scan.py lists no skipped instruction in an exFAT function.
