# Milestone 77: exFAT on USB drives, from the kernel

Agreed on 2026-11-13. The user wants USB drives formatted exFAT to work on the console as FATX
and FAT32 ones do, for Aurora, FTP servers and titles, read and written, with files over 4 GB; and the driver
loaded by the kernel itself at boot, from the NAND, through an xeBuild patch, so no title and no DashLaunch plugin
has to run it. USB drives first; the internal drive is a milestone of its own afterwards.
Added on 2026-11-14, agreed: the console formats a stick exFAT itself (step 2's formatter, step 5's title).

## Why

- FATX holds a file's size in 32 bits, so a file stops at 4 GB, and a volume at 2 TB; the FAT32 the kernel mounts on
  USB sticks (HARDWARE.md, "Listing a folder on a FAT32 USB stick") stops at 4 GB too. Disc images, PS2 and GameCube
  dumps and emulator packs pass 4 GB and are split today (the PAK parts the user copies by FTP).
- exFAT lifts both limits, is what every desktop OS mounts natively, and keeps a contiguous file without a chain
  walk: the allocation bitmap allocates, the FAT is read only for fragmented files. Its write engine is a fifth of
  NTFS's (the assessment of 2026-11-13: no payoff from NTFS on a drive the console writes).
- The kernel's USB mass-storage path reads about 20 MB/s (docs/scope/69-usb-ethernet.md, "Why"); exFAT adds no
  lookups to a contiguous read, so it runs at the same speed as FATX.

## Decisions (the user, 2026-11-13)

1. **USB drives first.** The internal drive as exFAT (`\Device\Harddisk0\Partition1`, the system partitions staying
   FATX, a PC formatter at the console's partition offset) is milestone 78, if 77 holds up.
2. **Read and write from the start,** the writes proven on the PC against fsck.exfat before the console writes a
   byte.
3. **A kernel-resident module, not a title:** the system DLL build of milestone 69 (branch m69-plugins 2f14850,
   `elf2xex --dll`, `libxsdk_plugin.a`, soft float, no AltiVec), loaded in the end by a kernel patch from the flash
   file system. During development the milestone 69 loader title loads it (local/usbnet/plugin_loader), so the
   driver and the kernel patch aren't debugged at the same time, as milestone 69 kept its driver and its plugin apart.
4. **The exFAT core is plain C with no kernel calls,** over two callbacks that read and write sectors, built and
   tested in WSL against images; the kernel glue around it is thin and tested on the console only.
5. **Code in xsdk:** `src/exfat.c` (the core, which xsdk's own file calls gain nothing from but which a PC tool can
   share), `plugins/exfat/` (the module, its build and the patch set), `tests/exfat_host/` (the PC harness). Open
   below, as milestone 69 left the NIC's place open.
6. **The console formats sticks exFAT itself** (agreed 2026-11-14): a formatter in the core (step 2) and a title
   that runs it on a Mass device the user picks (step 5). A dashboard prompt and a store or Aurora menu entry stay
   out.

## Sources

- **The format:** Microsoft's exFAT specification, published in 2019 under its Open Invention Network pledge
  (learn.microsoft.com/windows/win32/fileio/exfat-specification), the authority on every structure and checksum.
  exfatprogs (`mkfs.exfat`, `fsck.exfat`, GPL-2.0) and Linux's `fs/exfat` (GPL-2.0) as references for behaviour,
  cited as `linux:fs/exfat/<file>:<line>`, and as the judges of our images; no code taken from them, as milestone 69
  took facts and not code from `r8152.c`. Windows's chkdsk is the second judge.
- **The kernel's I/O manager,** as Xenia names its exports (xenos-toolchain/reference, xboxkrnl_table.inc):
  `IoCreateDevice` 0x37, `IoDeleteDevice` 0x39, `IoBuildSynchronousFsdRequest` 0x32, `IoCallDriver` 0x33,
  `IoCompleteRequest` 0x35, `IoSynchronousFsdRequest` 0x4C, `IoSynchronousDeviceIoControlRequest` 0x4B,
  `IoCheckShareAccess` 0x34, `IoSetShareAccess` 0x47, `IoRemoveShareAccess` 0x45, `IoDismountVolume` 0x3B,
  `IoInvalidDeviceRequest` 0x41, `ObReferenceObjectByName` 0x111, `ExAllocatePoolWithTag` 0xA. It is the original
  Xbox's I/O manager (device, driver and file objects, IRPs with stack locations, a driver's 14 major functions;
  public through xboxdevwiki and nxdk's headers) carried to the 360. Object names are 8-bit ANSI strings
  (xenia:src/xenia/kernel/util/shim_utils.h:438-442, xboxkrnl_io.cc:99-117). The 17559 layouts and the mount path
  come from the user's labelled kernel (Z:\networking-xbox\xboxkrnl.17559.exe.i64) in step 1; this session's IDA
  can't open that database, so step 1 runs in the user's.
- **Names on the console:** USB drives are `\Device\Mass0` to `Mass2` and open as folders only with a closing
  backslash (HARDWARE.md, "Drive roots open as folders only with their closing backslash",
  results/2026-10-20-milestone-54); xsdk maps `usb0:` there (src/path.c). The kernel's FAT driver leaves
  `next_entry_offset` and `file_index` unwritten in a directory query (HARDWARE.md, "Listing a folder on a FAT32 USB
  stick"); ours writes them.
- **xeBuild,** the user's ground: the module is a file in its flash file system list and the patches a kernel patch
  set in its format, as it places DashLaunch's `launch.xex` in the flash file system and its patches load it. Step 1
  reads the exact format and hook from the patch set's source before ours is written.
- **Milestone 69's plugin facts:** local/usbnet/nic_interface.md for how the kernel's own drivers are shaped,
  local/usbnet/build_usbnet_nic.sh for the base-address rule (a gap between hud.xex and ximecore.xex, the end checked
  below 0x91680000), local/usbnet/check_dpc.py for the no-floating-point check, local/usbnet/plugin_loader for
  loading a system DLL from a title (`XexLoadImage` looked up at run time, the loader's process type set to system
  for the call).

## Steps

1. **The kernel's file-system seam** (reverse engineering, then a probe title). From the labelled 17559 kernel: how
   a volume is mounted on `\Device\Mass<n>`, the function that tries FATX, then FAT, and what it answers when both
   refuse, which is where a third file system goes; the layouts of the device, driver, IRP, stack location and file
   object in 17559, the major-function slots, and what the FAT driver does in each (create, close, cleanup, read,
   write, query and set information, directory control, query volume information, flush, file-system control); how a
   mounted volume reads and writes its device (read and write IRPs with 64-bit byte offsets; the IOCTL that gives the
   sector size and capacity) and how it learns the stick was pulled (removal to dismount); and the boot: the point
   after the file systems and Usbd are up where a patch can load a module before xam.xex, the path `XexLoadImage`
   takes for a flash file (`\Device\Flash\<name>`, or the bare name), and how xeBuild's DashLaunch patches do it.
   The seam is one of two: a hook in the kernel's mount code, which the kernel patch gives for free and a run-time
   patch gives during development, as DashLaunch hooks the kernel at run time; or the module watching the Mass
   devices on a timer, reading sector 0 and mounting itself where the kernel left the device unmounted. The hook is
   preferred: it has no window in which the kernel answers "unrecognized" before we mount. Step 1 decides.
   A probe title then confirms the listing on the console: it references `\Device\Mass0` by name, logs the device
   object and its driver's dispatch table against the layouts, reads sector 0 of a stick through an IRP, and records
   what the console does with an exFAT stick today: the status of opening `\Device\Mass0\`, and whether the dashboard
   offers to format it (the user looks once).
   Check: the layouts, the seam, the boot point and the flash path written up with addresses, as nic_interface.md
   was, beside the code; the probe's sector 0 matches the stick's boot sector read on the PC; the probe refuses any
   kernel but 17559.
2. **The exFAT core on the PC** (`src/exfat.c`, `tests/exfat_host/` in WSL). Mount: the boot sector and its checksum
   sectors, the bitmap, the up-case table and its checksum, the root directory, the volume's dirty flag; a volume
   failing any check is refused. Read: directory walks over entry sets (file, stream extension, file names, the set
   checksum), lookups through the up-case table, contiguous files read without the FAT and fragmented ones through
   it, times with their UTC offsets as FILETIMEs, attributes and sizes. Write: create and delete files and folders,
   write and extend (contiguous while the next cluster is free, a FAT chain after), truncate, rename and move, set
   times and attributes, grow directories, the name hash of each entry set, the dirty flag set on the first write and
   cleared on flush and dismount; data before bitmap and FAT before the directory entry, so a power cut leaks clusters
   rather than corrupts entries. Names: the console's 8-bit names map to UTF-16 as Latin-1; an entry with other
   characters lists with `?` in their place and can't be opened, a limit, not a fault. File data moves between the
   caller's buffer and the device in sector multiples, with one bounce sector for the ends; a fixed metadata cache
   of 256 KB holds directory, bitmap, FAT and up-case sectors.
   Format: `xsdk_exfat_format(device, label, serial)` writes what Windows writes for a stick of that size, through
   the same two callbacks: an MBR with one type 07 partition from 1 MB to 1 MB short of the last whole megabyte, as
   Windows left the stick, and on it the main and backup boot regions with their checksum sectors, one FAT, the
   bitmap, the up-case table and a root directory holding the label, the bitmap and the up-case entries; Windows's
   cluster sizes (4 KB to 256 MB, 32 KB to 32 GB, 128 KB above), the FAT and the heap on megabyte boundaries as
   Windows places them (results/2026-11-14-milestone-77-step-1/recipe/scan.log), the caller's serial. Only the
   structures are written, so a format takes seconds at any size. An MBR partition holds 2^32 sectors at most, so a
   drive over 2 TB with 512-byte sectors, which Windows would make GPT, is refused rather than mimicked.
   Check, in the harness: images from `mkfs.exfat` at each cluster size it picks, trees written by Linux read back
   through ours byte for byte; trees written by ours read back by Linux, `fsck.exfat` clean after every write test; a
   file over 4 GB written and read; a sparse image over 2 TB for the 64-bit arithmetic; images corrupted on purpose
   (a bad checksum, a FAT chain that loops, a cluster past the end) refused or contained. Each break, a wrong name
   hash, a wrong set checksum, a missed dirty flag, fails `fsck.exfat` or the comparison. The formatter: images
   it makes at each cluster size and at 2.5 TB with 4 KB sectors (the MBR's limit, above) pass `fsck.exfat`, mount
   read-write under Linux and take a tree of files back out byte for byte, and one mounts in Windows through a
   fixed VHD (the recipe above), which writes and reads a file on it; its boot sector fields for the stick's size
   equal the Windows-formatted `recipe/image_boot.bin` apart from the serial. exfatprogs 1.2.9 and exfat-fuse 1.4.0
   are in the WSL Debian (installed 2026-11-14); the harness runs as root for loop devices and fuse.
3. **The driver on the console, loaded by a title.** First the plugin build of branch m69-plugins (2f14850) comes to
   main, with the base-address check as the module's build rule; the module's base is 0x91580000, above the NIC
   plugin's image, its end checked below 0x91680000. Then the module: a driver object with step 1's major functions,
   a volume device per mounted stick from `IoCreateDevice`, attached where the kernel's FAT driver attaches its own;
   the core's two callbacks as read and write IRPs to the raw device; opens with share access through the kernel's
   share calls; directory queries in the class XAM and xsdk use, every field written; volume information with the
   size, the free space and `exFAT` as the name; dismount when the stick is pulled; the metadata cache in the
   module's own bss, no title heap. FATX and FAT32 sticks still go to the kernel's drivers. The module logs, as the
   NIC's does, to a file on the hard drive.
   Check, with an exFAT stick made on the PC: a test title through xsdk's file calls, each check able to fail: open,
   read, write, append past 4 GB, seek, truncate, rename, delete, mkdir, rmdir, list, stat, free space; then the
   stick on the PC, `fsck.exfat` clean and the written files byte-identical. Then the real consumers: Aurora's file
   manager lists the stick and copies to and from it; Aurora's FTP uploads and downloads a file over 4 GB, the speed
   logged against FATX on the same stick. Pulling the stick while mounted doesn't crash; plugging it back mounts it
   again. A stick with its boot sector corrupted on purpose is refused with an error, not mounted. Breaks: a build
   with the name hash wrong fails `fsck.exfat`; one that skips the dirty flag fails the clean-after-dismount check.
4. **Loaded by the kernel from the NAND.** The module as a file in xeBuild's flash file system list and a kernel
   patch set in xeBuild's format: at the boot point (step 1's, moved behind xam's and launch.xex's loads on
   2026-11-15, plugins/exfat/kernel_seam.md section 7), a stub that calls `XexLoadImage` on the module's flash
   path as a system DLL and carries on when that fails, so a NAND without the file still boots. xsdk's tools build
   the patch (the toolchain's assembler, a script that emits the format) and check the patched words against the
   kernel image's bytes at those addresses. The user builds the NAND with xeBuild and flashes it.
   Check: a boot with the exFAT stick plugged in, no title run and no DashLaunch plugin loaded: Aurora sees the
   stick, step 3's checks pass again on that boot, the FTP speed holds; a boot with the file removed from the flash
   file system list boots and treats the stick as unformatted. Done when all of it passes on one clean console run
   from a NAND boot, the breaks failing (results/<date>-milestone-77/).
5. **Formatting from the console** (`exfat_format.xex`, after step 3; it needs nothing of step 4). A title that
   lists the Mass devices holding media, each with its size and what its first sector is (FATX, FAT, exFAT, an
   MBR and its first partition's type, or nothing), lets the user pick one with the pad and hold A for three
   seconds to confirm, then dismounts whatever the kernel has on it (`IoDismountVolumeByName`), runs step 2's
   formatter through write IRPs to the device object as the step 1 writer did (64 KB at a time, sector
   offsets), reads the structures back and compares, and reports on screen and in its log. `exfat_format.txt`
   beside the title, holding a device name and a label, formats that device without the pad, for console-agent
   runs; without it and without a pad the title only lists. The title is launched like any other and the stick
   mounts under step 3's module at once.
   Check: the stick in USB0, formatted by the title, passes step 3's test title and lists in Aurora; on the PC,
   `fsck.exfat` is clean and Windows mounts it and writes a file (the user plugs it in once); its MBR and boot
   sector fields match what Windows chose for the same stick on 2026-11-14, serial and times apart; a format of
   a stick holding files leaves none behind. The listing-only path refuses to write when neither a pad nor the
   file says which device, and a device name in the file that holds no media is refused with a log line.

## Open

- Whether the kernel's mount code has a seam a module can take without patching code (step 1); if not, step 3 hooks
  at run time as DashLaunch does, and step 4's patch takes the hook over.
- Where the code lives: xsdk, as proposed, or its own repository, the question milestone 69 left open for the NIC.
- Whether the kernel's mass-storage driver addresses past 2 TB; a drive over 2 TB would show. Not promised.
- What the dashboard does with an exFAT stick during step 3, before our module is loaded: if it offers to format,
  the user declines once; after step 4 our mount comes first.
- Names outside Latin-1 (above); whether anything on the console needs them.

## Left out

NTFS; the internal drive, its boot order and its PC formatter (milestone 78); a dashboard prompt offering the
exFAT format when a stick is unrecognized, and a store or Aurora menu entry for the formatter (step 5's title is
launched like any other); TexFAT; memory units and the disc drive; XAM's "Xbox 360
storage" folder on the stick, which works through the file system and needs nothing of ours; the NIC module's
move to the same flash load, one more file in the list and one more name in the stub, a step to close milestone 69
with and not this one.
