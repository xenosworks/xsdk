# Milestone 77, step 1: the kernel seam on the console

The user's RGH console, kernel 2.0.17559.0, the stick in USB0 (`\Device\Mass0`: 120,176,640 sectors of 512 bytes,
58,680 MB). Three runs through the console agent, all passing. The user authorized formatting this stick
(2026-11-14, "You can format the USB0 currently connected then test it with the probe").

| folder | title | result |
|---|---|---|
| fat32-before | `probe_exfat_seam.xex` on the stick as it was: console-formatted FAT32, no MBR | 17 passed, 0 failed |
| format | `probe_exfat_write.xex` writing `exfat_image.bin` onto the stick | 15 passed, 0 failed |
| exfat-after | `probe_exfat_seam.xex` on the exFAT stick | 21 passed, 0 failed |

## The image

Windows's own exFAT format, not a generator of ours. In an elevated PowerShell: `recipe/vhd.py` makes a fixed VHD
of the stick's sector count (Windows refuses sparse VHDs, so the file is the full 57 GB for a minute),
`recipe/diskpart.txt` attaches it and runs `create partition primary` and `format fs=exfat quick label=XSDK`,
`recipe/scan.py` prints the layout (`recipe/scan.log`) and cuts the head that holds every non-zero megabyte: 5 MB,
sectors 0 to 10240. The image is `local/m77/exfat_image.bin` (not in the repo); its MBR and boot sector are
`recipe/image_mbr.bin` and `recipe/image_boot.bin`. exfatprogs isn't in the WSL Debian (sudo wants a password), and
7-Zip reads FAT images but not exFAT, which is why Windows made the image.

The layout, Windows's defaults for a 57 GB volume: an MBR with one type 07 partition at sector 2048 for 120,172,544
sectors (2048 sectors unused after it); exFAT revision 1.0, FAT at 2048 for 3840 sectors, cluster heap at 6144,
469,400 clusters of 256 sectors (128 KB), root cluster 4, serial 643FC9ED, one FAT.

## What the runs established

- A title can format the stick: `IoDismountVolumeByName` detaches the FAT volume (`MountedOrSelfDevice` reads NULL
  after it), write IRPs through `IoSynchronousFsdRequest` to the Mass0 device object take 64 KB chunks at sector
  offsets, and the 5 MB read back identical (`format/probe_exfat_write.log`).
- The console's sector 0 and sector 2048 (`exfat-after/sector0.bin`, `boot.bin`) are byte-identical to the image's
  (`cmp` against `recipe/image_mbr.bin` and `image_boot.bin`): the probe's sector reads see what the PC wrote.
- With an exFAT stick nothing mounts by itself. Opening `\Device\Mass0\` fails with C000014F and attaches nothing.
  Opening `\Device\Mass0` succeeds and attaches the raw volume (driver 0x80043224), which stays after the close and
  turns later path opens into C000003A. The module's poll has to dismount a raw volume first
  (plugins/exfat/kernel_seam.md, sections 6 and 8).
- IOCTL 0x74004 reports the whole stick, start 0 and a 64-bit length 0xE53800000: the class driver reads no MBR, a
  file system does.
- On the FAT32 stick the FAT volume was mounted before the probe opened anything (the dashboard's doing), and a
  device open took its refcount to 1 while held.
- The kernel's words at the boot hook, the mount chain, `IoDeviceObjectType` and the FAT driver table match the
  image the seam note was read from; the .text padding is empty; protection reads 0x20 for the padding and the mount
  chain, 2 for .rdata, 4 for .data.

## Left in step 1

The user looks at the dashboard once with the exFAT stick in: whether it offers to format it.
